#include "item.hpp"

#include <utility>

#include <qt_windows.h>
#include <shellapi.h>

#include <qimage.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qmetaobject.h>
#include <qnamespace.h>
#include <qobject.h>
#include <qpixmap.h>
#include <qpointer.h>
#include <qquickitem.h>
#include <qquickwindow.h>
#include <qsize.h>
#include <qstring.h>
#include <qtimer.h>
#include <qtypes.h>
#include <quuid.h>

#include "../../../core/logcat.hpp"
#include "../../appicon.hpp"
#include "../../redact.hpp"
#include "hook.hpp"

namespace qs::windows::services::systray {

namespace {

QS_LOGGING_CATEGORY(logTrayItem, "quickshell.windows.systray", QtWarningMsg);

constexpr int FALLBACK_ICON_SIZE = 32;
constexpr ULONGLONG PENDING_MAX_MS = 4000;
constexpr int REPLAY_DELAY_MS = 150;

} // namespace

QPixmap TrayIconImage::requestPixmap(const QString& /*id*/, QSize* size, const QSize& requestedSize) {
	auto pixmap = QPixmap::fromImage(this->image);

	if (!pixmap.isNull() && requestedSize.width() > 0 && requestedSize.height() > 0
	    && pixmap.size() != requestedSize)
	{
		pixmap = pixmap.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	}

	if (size != nullptr) *size = pixmap.size();
	return pixmap;
}

SystemTrayItem::SystemTrayItem(HWND hwnd, UINT uid, const QUuid& guid, QObject* parent)
    : QObject(parent)
    , hwnd(hwnd)
    , uid(uid)
    , guid(guid) {}

bool SystemTrayItem::matches(HWND hwnd, UINT uid, const QUuid& guid) const {
	if (!guid.isNull() && guid == this->guid) return true;
	return hwnd != nullptr && hwnd == this->hwnd && uid == this->uid;
}

bool SystemTrayItem::ownerAlive() const { return IsWindow(this->hwnd) != 0; }

void SystemTrayItem::update(const TrayIconMessage& message) {
	auto hadCallback = this->hasCallback();

	if (message.hwnd != nullptr) {
		this->hwnd = message.hwnd;
		this->uid = message.uid;
	}

	if ((message.flags & NIF_GUID) != 0) this->guid = message.guid;
	if ((message.flags & NIF_MESSAGE) != 0) {
		this->callbackMessage = message.callbackMessage;
	} else if (!hadCallback && message.message == NIM_MODIFY && !message.seeded
	           && message.callbackMessage >= WM_USER && message.callbackMessage <= 0xffff)
	{
		this->callbackMessage = message.callbackMessage;
		auto version = message.version;
		if (version == NOTIFYICON_VERSION || version == NOTIFYICON_VERSION_4) this->version = version;
	}

	if (message.message == NIM_SETVERSION) this->version = message.version;

	if ((message.flags & NIF_ICON) != 0) {
		this->iconRevision++;
		this->requestedIconRevision.reset();
		this->imageIsFallback = false;
		this->image.image = message.icon;
		this->updateIconSource();
	}

	if ((message.flags & NIF_TIP) != 0) this->setTip(message.tip);

	if ((message.flags & NIF_STATE) != 0 && (message.stateMask & NIS_HIDDEN) != 0) {
		this->bStatus = (message.state & NIS_HIDDEN) != 0 ? Status::Passive : Status::Active;
	}

	if (!hadCallback && this->hasCallback() && this->pending != Pending::None) {
		QTimer::singleShot(REPLAY_DELAY_MS, this, &SystemTrayItem::replayPending);
	}
}

void SystemTrayItem::refreshFromExplorer(const TrayIconMessage& message) {
	if (!message.icon.isNull() && message.icon != this->image.image) {
		this->iconRevision++;
		this->requestedIconRevision.reset();
		this->imageIsFallback = false;
		this->image.image = message.icon;
		this->updateIconSource();
	}

	if (message.tip != this->tip) this->setTip(message.tip);
}

void SystemTrayItem::setTip(const QString& tip) {
	this->tip = tip;

	auto text = tip;
	text.remove(u'\r');

	auto newline = text.indexOf(u'\n');
	this->bTooltipTitle = newline == -1 ? text : text.first(newline);
	this->bTooltipDescription = newline == -1 ? QString() : text.sliced(newline + 1);
}

void SystemTrayItem::setIdentity(const QString& id, const QString& title, const QString& exePath) {
	this->bId = id;
	this->bTitle = title;
	auto executableChanged = this->exePath != exePath;
	this->exePath = exePath;

	if (executableChanged) {
		this->iconRevision++;
		this->requestedIconRevision.reset();
		if (this->imageIsFallback) this->image.image = QImage();
	}

	if (this->image.image.isNull()) this->updateIconSource();
}

void SystemTrayItem::updateIconSource() {
	if (this->image.image.isNull() && !this->exePath.isEmpty()) {
		this->requestFallbackIcon();
	}

	if (this->image.image.isNull()) {
		this->bIcon = QString();
		return;
	}

	this->image.imageChanged();
	this->bIcon = this->image.url();
}

void SystemTrayItem::requestFallbackIcon() {
	if (this->requestedIconRevision == this->iconRevision) return;

	this->requestedIconRevision = this->iconRevision;
	auto revision = this->iconRevision;
	auto guard = QPointer<SystemTrayItem>(this);

	requestShellIcon(
	    this->exePath,
	    QSize(FALLBACK_ICON_SIZE, FALLBACK_ICON_SIZE),
	    [guard, revision] {
		    auto* item = guard.data();
		    return item == nullptr || item->iconRevision != revision
		        || !item->image.image.isNull();
	    },
	    [guard, revision](QImage icon) {
		    if (icon.isNull()) return;

		    auto* item = guard.data();
		    if (item == nullptr) return;

		    QMetaObject::invokeMethod(
		        item,
		        [guard, revision, icon = std::move(icon)]() mutable {
			        auto* target = guard.data();
			        if (target == nullptr || target->iconRevision != revision
			            || !target->image.image.isNull())
			        {
				        return;
			        }

			        target->image.image = std::move(icon);
			        target->imageIsFallback = true;
			        target->updateIconSource();
		        },
		        Qt::QueuedConnection
		    );
	    }
	);
}

void SystemTrayItem::allowForeground() const {
	DWORD pid = 0;
	GetWindowThreadProcessId(this->hwnd, &pid);
	if (pid != 0) AllowSetForegroundWindow(pid);
}

void SystemTrayItem::send(UINT event, const POINT* anchor) const {
	if (this->callbackMessage == 0 || !this->ownerAlive()) return;

	WPARAM wParam = 0;
	LPARAM lParam = 0;

	if (this->version >= NOTIFYICON_VERSION_4) {
		POINT point {};
		if (anchor) point = *anchor;
		else GetCursorPos(&point);
		wParam = MAKEWPARAM(static_cast<WORD>(point.x), static_cast<WORD>(point.y));
		lParam = MAKELPARAM(static_cast<WORD>(event), static_cast<WORD>(this->uid));
	} else {
		wParam = this->uid;
		lParam = event;
	}

	SendNotifyMessageW(this->hwnd, this->callbackMessage, wParam, lParam);
}

void SystemTrayItem::activate() {
	auto now = GetTickCount64();
	auto doubleClick = this->lastActivate != 0 && now - this->lastActivate <= GetDoubleClickTime();
	this->lastActivate = doubleClick ? 0 : now;

	if (!this->hasCallback()) {
		this->hold(doubleClick && this->pending == Pending::Activate ? Pending::DoubleClick
		                                                            : Pending::Activate);
		return;
	}

	this->sendActivate(doubleClick);
}

void SystemTrayItem::secondaryActivate() {
	if (!this->hasCallback()) {
		this->hold(Pending::SecondaryActivate);
		return;
	}

	this->sendSecondaryActivate();
}

void SystemTrayItem::scroll(qint32 /*delta*/, bool /*horizontal*/) const {}

void SystemTrayItem::display(QObject* parentWindow, qint32 relativeX, qint32 relativeY) {
	// Version 4 carries the screen anchor in its callback. Legacy owners
	// receive no coordinates and may place their menu at the cursor.
	this->displayAt.reset();

	// QML hands over the window wrapper; its content item knows the native window.
	auto* content =
	    parentWindow ? parentWindow->property("contentItem").value<QQuickItem*>() : nullptr;
	auto* window = content ? content->window() : nullptr;

	if (window) {
		auto ratio = window->devicePixelRatio();
		POINT point {qRound(relativeX * ratio), qRound(relativeY * ratio)};
		if (ClientToScreen(reinterpret_cast<HWND>(window->winId()), &point)) {
			this->displayAt = point;
		}
	}

	qCDebug(logTrayItem) << "Opening the menu of"
	                     << qs::windows::logId(this->bId.value()) << "from" << parentWindow
	                     << "at" << relativeX << relativeY << "window" << window << "cursor target"
	                     << (this->displayAt ? QPoint(this->displayAt->x, this->displayAt->y) : QPoint());

	if (!this->hasCallback()) {
		this->hold(Pending::Display);
		return;
	}

	this->sendDisplay();
}

void SystemTrayItem::sendActivate(bool doubleClick) {
	this->allowForeground();
	this->send(doubleClick ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN);
	this->send(WM_LBUTTONUP);
	if (this->version >= NOTIFYICON_VERSION) this->send(NIN_SELECT);
}

void SystemTrayItem::sendSecondaryActivate() {
	this->allowForeground();
	this->send(WM_MBUTTONDOWN);
	this->send(WM_MBUTTONUP);
}

void SystemTrayItem::sendDisplay() {
	auto* anchor = this->displayAt ? &*this->displayAt : nullptr;
	if (anchor && this->version < NOTIFYICON_VERSION_4 && this->ownerAlive())
		SetCursorPos(anchor->x, anchor->y);
	this->allowForeground();
	this->send(WM_RBUTTONDOWN, anchor);
	this->send(WM_RBUTTONUP, anchor);
	if (this->version >= NOTIFYICON_VERSION) this->send(WM_CONTEXTMENU, anchor);
}

void SystemTrayItem::hold(Pending action) {
	if (!TrayHook::running()) {
		qCWarning(logTrayItem) << "Tray click refused: callback missing and hook unavailable"
		                       << TrayHook::capability().value("reason").toString();
		return;
	}

	this->pending = action;
	this->pendingAt = GetTickCount64();
	if (this->onCallbackNeeded) this->onCallbackNeeded();

	QTimer::singleShot(static_cast<int>(PENDING_MAX_MS), this, [this, at = this->pendingAt]() {
		if (this->pending == Pending::None || this->pendingAt != at || this->hasCallback()) return;

		qCInfo(logTrayItem) << "Tray icon" << qs::windows::logId(this->bId.value())
		                    << "can't be clicked yet: its app didn't add it again when asked";
		this->pending = Pending::None;
	});
}

void SystemTrayItem::replayPending() {
	auto action = this->pending;
	this->pending = Pending::None;
	if (action == Pending::None || GetTickCount64() - this->pendingAt > PENDING_MAX_MS) return;

	switch (action) {
	case Pending::Activate: this->sendActivate(false); break;
	case Pending::DoubleClick:
		this->sendActivate(false);
		this->sendActivate(true);
		break;
	case Pending::SecondaryActivate: this->sendSecondaryActivate(); break;
	case Pending::Display: this->sendDisplay(); break;
	case Pending::None: break;
	}
}

} // namespace qs::windows::services::systray

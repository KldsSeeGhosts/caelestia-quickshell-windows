#include "keyboard.hpp"
#include <algorithm>
#include <utility>
#include <vector>

#include <qcoreapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qpointer.h>
#include <qstring.h>
#include <qvariantmap.h>

#include "../../core/logcat.hpp"
#include "../system_surface.hpp"
#include "../util.hpp"
#include "../window_tracker.hpp"
#include "keyboard_layout_name.hpp"
#include "keyboard_layout_switch.hpp"

namespace qs::windows::sys {

namespace {
QS_LOGGING_CATEGORY(logKeyboard, "quickshell.windows.keyboard", QtWarningMsg);

constexpr int LAYOUT_POLL_MS = 500;

QString localeNameFromLangId(LANGID langId) {
	auto lcid = MAKELCID(langId, SORT_DEFAULT);

	wchar_t buffer[LOCALE_NAME_MAX_LENGTH] {};
	if (LCIDToLocaleName(lcid, buffer, LOCALE_NAME_MAX_LENGTH, 0) == 0) return QString();
	return QString::fromWCharArray(buffer);
}

QString localeNameFromHkl(HKL hkl) {
	return localeNameFromLangId(LOWORD(reinterpret_cast<quintptr>(hkl))); // NOLINT
}

QString localeInfo(const QString& localeName, LCTYPE type) {
	if (localeName.isEmpty()) return QString();

	wchar_t buffer[256] {};
	auto name = localeName.toStdWString();
	auto size = GetLocaleInfoEx(name.c_str(), type, buffer, 256);
	if (size <= 0) return QString();
	return QString::fromWCharArray(buffer);
}

QString displayNameForLocale(const QString& localeName) {
	auto displayName = localeInfo(localeName, LOCALE_SLOCALIZEDDISPLAYNAME);
	if (!displayName.isEmpty()) return displayName;

	auto languageName = localeInfo(localeName, LOCALE_SENGLISHLANGUAGENAME);
	if (!languageName.isEmpty()) return languageName;

	return localeName;
}

// Upstream's XKB descriptions look like "English (US)". Build the same short
// form from the localized language name and the ISO region, never a table.
QString shortNameForLocale(const QString& localeName) {
	auto language = localeInfo(localeName, LOCALE_SLOCALIZEDLANGUAGENAME);
	auto region = localeInfo(localeName, LOCALE_SISO3166CTRYNAME);
	if (language.isEmpty()) return displayNameForLocale(localeName);
	if (region.isEmpty()) return language;
	return QStringLiteral("%1 (%2)").arg(language, region);
}

std::vector<HKL> installedLayouts() {
	auto count = GetKeyboardLayoutList(0, nullptr);
	if (count <= 0) return {};

	std::vector<HKL> layouts(static_cast<size_t>(count));
	auto got = GetKeyboardLayoutList(count, layouts.data());
	layouts.resize(static_cast<size_t>(std::max(got, 0)));
	return layouts;
}

bool isApplicationWindow(HWND hwnd) {
	if (hwnd == nullptr || IsWindow(hwnd) == FALSE) return false;
	if (isOwnProcessWindow(hwnd)) return false;
	return !system_surface::isShellSurface(system_surface::observe(hwnd));
}

// The tracker retains the last application while shell UI owns the foreground,
// and only reconciles after its own events. The window is checked again here:
// it must still be the recorded window (a recycled handle is rejected) and be
// visible, not minimized and not cloaked, or a background app that minimized
// or closed itself would receive the switch.
bool isRetainedApplication(const TrackedWindow* retained) {
	if (retained == nullptr) return false;

	auto* hwnd = retained->hwnd();
	auto state = observeLayoutTarget(hwnd);
	if (!isLiveRetainedTarget(state, {retained->frameProcessId(), retained->frameThreadId()})) {
		return false;
	}
	return isApplicationWindow(hwnd);
}

// The single target used for both observing and switching. WindowTracker keeps
// the last application while shell UI (launcher, session, task switcher) owns
// the foreground; using it here prevents the popout from reading or changing
// the shell's own thread layout. Shell windows are never accepted.
HWND intendedLayoutTarget() {
	auto* foreground = GetForegroundWindow();
	auto* retained = WindowTracker::instance()->activeWindow();

	return resolveLayoutTarget(
	    foreground,
	    isApplicationWindow(foreground),
	    retained != nullptr ? retained->hwnd() : nullptr,
	    isRetainedApplication(retained)
	);
}

bool postLayoutRequest(HWND target, HKL hkl, DWORD& error) {
	if (PostMessageW(
	        target,
	        WM_INPUTLANGCHANGEREQUEST,
	        INPUTLANGCHANGE_FORWARD,
	        reinterpret_cast<LPARAM>(hkl) // NOLINT
	    )
	    != FALSE)
	{
		return true;
	}

	error = GetLastError();
	return false;
}

// The window and thread that really take input for the validated top-level
// target. This is for hosted UWP input, where the frame (ApplicationFrameHost)
// and the app's CoreWindow are on different threads, and for classic windows
// whose focus is a child. Observation and switching both use this one result.
LayoutInputTarget intendedInputTarget() {
	auto* top = intendedLayoutTarget();
	if (top == nullptr) return {};

	static const LayoutInputQueries queries {
	    [](HWND hwnd, DWORD* process) { return GetWindowThreadProcessId(hwnd, process); },
	    [](DWORD thread, HWND& focus, HWND& active) {
		    GUITHREADINFO info {};
		    info.cbSize = sizeof(info);
		    if (GetGUIThreadInfo(thread, &info) == FALSE) return false;
		    focus = info.hwndFocus;
		    active = info.hwndActive;
		    return true;
	    },
	    [](HWND hwnd) { return GetAncestor(hwnd, GA_ROOTOWNER); },
	    [](HWND hwnd) {
		    wchar_t name[256] {};
		    auto length = GetClassNameW(hwnd, name, 256);
		    return length > 0 ? QString::fromWCharArray(name, length) : QString();
	    },
	    [](HWND frame) {
		    return FindWindowExW(frame, nullptr, L"Windows.UI.Core.CoreWindow", nullptr);
	    },
	    [](HWND parent, HWND hwnd) { return IsChild(parent, hwnd) != FALSE; },
	    [](HWND hwnd) { return GetWindow(hwnd, GW_OWNER); },
	};

	return resolveLayoutInput(top, queries);
}

HKL foregroundLayout() {
	auto input = intendedInputTarget();
	if (input.window == nullptr || input.threadId == 0) return nullptr;

	return GetKeyboardLayout(input.threadId);
}

} // namespace

KeyboardLayoutWatcher* KeyboardLayoutWatcher::instance() {
	static QPointer<KeyboardLayoutWatcher> watcher; // NOLINT
	if (watcher.isNull()) watcher = new KeyboardLayoutWatcher(QCoreApplication::instance());
	return watcher.data();
}

KeyboardLayoutWatcher::KeyboardLayoutWatcher(QObject* parent): QObject(parent) {
	this->pollTimer.setInterval(LAYOUT_POLL_MS);
	QObject::connect(&this->pollTimer, &QTimer::timeout, this, &KeyboardLayoutWatcher::check);

	QObject::connect(
	    WindowTracker::instance(),
	    &WindowTracker::foregroundChanged,
	    this,
	    &KeyboardLayoutWatcher::check
	);

	this->check();
	// Compatibility fallback, not the intended end state. It is tracked in
	// the shared architecture notes, which
	// records why no event is used yet: HSHELL_LANGUAGE through
	// RegisterShellHookWindow only fires for the foreground shell window, and a
	// global WH_SHELL hook needs an injected DLL per architecture. Until that
	// is resolved this bounded, read-only poll also observes lock toggles,
	// newly installed layouts and switches inside the foreground window.
	this->pollTimer.start();
}

void KeyboardLayoutWatcher::check() {
	auto layouts = installedLayouts();

	if (layouts != this->mLayouts) {
		this->mLayouts = std::move(layouts);

		qCDebug(logKeyboard) << "Installed keyboard layouts changed:" << this->mLayouts.size();
		emit this->layoutsChanged();
	}

	const auto capsLock = (GetKeyState(VK_CAPITAL) & 1) != 0;
	const auto numLock = (GetKeyState(VK_NUMLOCK) & 1) != 0;
	if (capsLock != this->mCapsLock || numLock != this->mNumLock) {
		this->mCapsLock = capsLock;
		this->mNumLock = numLock;
		emit this->locksChanged();
	}

	auto* current = foregroundLayout();
	// A lost target (nullptr) is published too, so the popout does not keep
	// showing the previous app's layout as active.
	if (current == this->mCurrent) return;

	this->mCurrent = current;
	emit this->currentChanged();
}

Keyboard::Keyboard(QObject* parent): QObject(parent) {
	QTimer::singleShot(0, this, &Keyboard::initDeferred);
}

void Keyboard::initDeferred() {
	auto* watcher = KeyboardLayoutWatcher::instance();
	QObject::connect(watcher, &KeyboardLayoutWatcher::currentChanged, this, &Keyboard::refresh);
	QObject::connect(watcher, &KeyboardLayoutWatcher::locksChanged, this, &Keyboard::locksChanged);

	QObject::connect(
	    watcher,
	    &KeyboardLayoutWatcher::layoutsChanged,
	    this,
	    &Keyboard::refreshLayoutList
	);

	this->refreshLayoutList();
	emit this->locksChanged();
}

Keyboard::~Keyboard() = default;

void Keyboard::setActive(bool active) {
	if (this->mActive == active) return;
	this->mActive = active;
	emit this->activeChanged();
	emit this->locksChanged();

	if (active) this->refresh();
}

void Keyboard::refreshLayoutList() {
	QList<KeyboardLayout> layouts;
	QStringList codes;
	QVariantList published;

	// Both lookups are read-only. Entries are keyed by TSF profile identity, so
	// IMEs without an HKL, or sharing one, are listed separately; the registry
	// fallback names layouts TSF gives no description for.
	auto entries = buildInputEntries(queryInputProfiles(), KeyboardLayoutWatcher::instance()->layouts());
	auto registryLayouts = queryLayoutRegistryEntries();

	for (const auto& input: entries) {
		KeyboardLayout layout;
		layout.hkl = input.hkl;
		layout.id = input.id;
		layout.code = localeNameFromLangId(input.langid);
		layout.name = displayNameForLocale(layout.code);
		layout.shortName = shortNameForLocale(layout.code);
		layout.layoutName = resolveLayoutName(input, registryLayouts, &layoutNameFromRegistry);

		if (layout.name.isEmpty()) {
			layout.name = layout.layoutName.isEmpty() ? layout.code : layout.layoutName;
		}
		if (layout.shortName.isEmpty()) layout.shortName = layout.name;
		if (!layout.code.isEmpty() && !codes.contains(layout.code)) codes.append(layout.code);

		QVariantMap entry;
		entry.insert(QStringLiteral("id"), layout.id);
		entry.insert(QStringLiteral("code"), layout.code);
		entry.insert(QStringLiteral("name"), layout.name);
		entry.insert(QStringLiteral("shortName"), layout.shortName);
		entry.insert(QStringLiteral("layoutName"), layout.layoutName);

		layouts.append(layout);
		published.append(entry);
	}

	this->mLayouts = std::move(layouts);
	Qt::beginPropertyUpdateGroup();
	this->bLayoutCodes = codes;
	this->bLayouts = published;
	Qt::endPropertyUpdateGroup();
	this->refresh();
}

void Keyboard::refresh() {
	auto* hkl = KeyboardLayoutWatcher::instance()->current();

	auto index = layoutIndexOf(this->mLayouts, hkl, [](const KeyboardLayout& layout) {
		return layout.hkl;
	});

	// No target (hkl == nullptr) clears the index and the legacy code and name
	// together, in one update, so a lost target never leaves a stale layout.
	auto code = hkl != nullptr ? localeNameFromHkl(hkl) : QString();
	auto updateLegacy = this->mActive && (hkl == nullptr || !code.isEmpty())
	                 && code != this->bCurrentLayoutCode.value();

	Qt::beginPropertyUpdateGroup();
	if (index != this->bCurrentLayoutIndex.value()) this->bCurrentLayoutIndex = index;
	if (updateLegacy) {
		this->bCurrentLayoutCode = code;
		this->bCurrentLayoutName = code.isEmpty() ? QString() : displayNameForLocale(code);
	}
	Qt::endPropertyUpdateGroup();
}

QString Keyboard::activateLayoutAt(int index) {
	if (index < 0 || index >= static_cast<int>(this->mLayouts.size())) {
		qCWarning(logKeyboard) << "activateLayoutAt: no installed layout at index" << index;
		return QStringLiteral("no-target");
	}

	auto* hkl = this->mLayouts.at(index).hkl;
	if (hkl == nullptr) {
		// A text service whose language has no installed layout to carry it.
		qCWarning(logKeyboard) << "activateLayoutAt: no switchable handle for" << this->mLayouts.at(index).id;
		return QStringLiteral("unsupported");
	}

	auto outcome = postLayoutSwitch(intendedInputTarget().window, hkl, &postLayoutRequest);
	if (outcome.result == QStringLiteral("denied")) {
		qCWarning(logKeyboard) << "Could not request a keyboard layout switch, error" << outcome.error;
	}
	return outcome.result;
}

void Keyboard::activateLayout(const QString& code) {
	for (const auto& layout: this->mLayouts) {
		if (layout.code != code || layout.hkl == nullptr) continue;

		auto outcome = postLayoutSwitch(intendedInputTarget().window, layout.hkl, &postLayoutRequest);
		if (outcome.result != QStringLiteral("requested")) {
			qCWarning(logKeyboard) << "Keyboard layout switch was not requested:" << outcome.result
			                       << "error" << outcome.error;
		}
		return;
	}

	qCWarning(logKeyboard) << "activateLayout: no installed layout matches" << code;
}

} // namespace qs::windows::sys

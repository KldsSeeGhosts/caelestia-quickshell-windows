#include "virtual_desktops.hpp"
#include <algorithm>
#include <utility>

#include <qbytearray.h>
#include <qcoreapplication.h>
#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qstring.h>
#include <qthread.h>
#include <qtimer.h>
#include <qtypes.h>

#include "desktop_index.hpp"
#include "native_desktop_accessor.hpp"
#include "startup.hpp"
#include "redact.hpp"
#include "services/message_window.hpp"

#include <objbase.h>

namespace qs::windows {

namespace {
Q_LOGGING_CATEGORY(logDesktops, "quickshell.windows.desktops", QtWarningMsg);

template <typename R, typename... P, typename... A>
bool sehCall(R (*fn)(P...), R& out, A... args) {
	__try {
		out = fn(args...);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

constexpr const wchar_t* DESKTOPS_KEY =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops";
constexpr const wchar_t* SESSION_KEY_FORMAT =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\SessionInfo\\%1\\VirtualDesktops";

constexpr qsizetype MAX_DESKTOPS = 20;
constexpr int CREATION_TIMEOUT_MS = 1000;
constexpr int CLOAK_REFRESH_DELAY_MS = 250;

bool readBinary(HKEY key, const wchar_t* name, QByteArray& out) {
	DWORD type = 0;
	DWORD size = 0;
	if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) return false;
	if (type != REG_BINARY || size == 0) return false;

	out.resize(static_cast<qsizetype>(size));
	auto* data = reinterpret_cast<BYTE*>(out.data());
	return RegQueryValueExW(key, name, nullptr, nullptr, data, &size) == ERROR_SUCCESS;
}

QString readString(HKEY key, const wchar_t* name) {
	DWORD type = 0;
	DWORD size = 0;
	if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) return {};
	if (type != REG_SZ || size < sizeof(wchar_t)) return {};

	auto buffer = QByteArray(static_cast<qsizetype>(size), '\0');
	auto* data = reinterpret_cast<BYTE*>(buffer.data());
	if (RegQueryValueExW(key, name, nullptr, nullptr, data, &size) != ERROR_SUCCESS) return {};

	auto* chars = reinterpret_cast<const wchar_t*>(buffer.constData());
	return QString::fromWCharArray(chars, static_cast<qsizetype>(size / sizeof(wchar_t))).trimmed()
	    .remove(QChar('\0'));
}

GUID guidAt(const QByteArray& bytes, qsizetype offset) {
	GUID guid {};
	memcpy(&guid, bytes.constData() + offset, sizeof(GUID));
	return guid;
}

bool guidIsNull(const GUID& guid) {
	static const GUID null {};
	return IsEqualGUID(guid, null);
}

bool isOwnWindow(HWND hwnd) {
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	return pid == GetCurrentProcessId();
}

} // namespace

RegistryWatcher::RegistryWatcher(QList<QString> subkeys, QObject* parent)
    : QThread(parent)
    , subkeys(std::move(subkeys)) {
	this->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

RegistryWatcher::~RegistryWatcher() {
	this->stop();
	if (this->stopEvent != nullptr) CloseHandle(this->stopEvent);
}

void RegistryWatcher::stop() {
	if (!this->isRunning()) return;
	SetEvent(this->stopEvent);
	this->wait();
}

void RegistryWatcher::run() {
	QList<HKEY> keys;
	QList<HANDLE> events;
	QList<bool> armed;

	for (const auto& subkey: this->subkeys) {
		HKEY key = nullptr;
		auto path = subkey.toStdWString();
		if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_NOTIFY, &key) != ERROR_SUCCESS) {
			qCDebug(logDesktops) << "Not watching missing registry key" << subkey;
			continue;
		}

		keys.append(key);
		events.append(CreateEventW(nullptr, FALSE, FALSE, nullptr));
		armed.append(false);
	}

	if (keys.isEmpty()) {
		qCWarning(logDesktops) << "No virtual desktop registry keys to watch.";
		return;
	}

	QList<HANDLE> handles;
	handles.append(this->stopEvent);
	handles.append(events);

	while (true) {
		for (qsizetype i = 0; i < keys.length(); i++) {
			if (armed[i]) continue;
			auto status = RegNotifyChangeKeyValue(
			    keys[i],
			    TRUE,
			    REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
			    events[i],
			    TRUE
			);
			armed[i] = status == ERROR_SUCCESS;
		}

		auto result = WaitForMultipleObjects(
		    static_cast<DWORD>(handles.length()),
		    handles.constData(),
		    FALSE,
		    INFINITE
		);

		if (result == WAIT_OBJECT_0 || result == WAIT_FAILED) break;

		auto index = static_cast<qsizetype>(result - WAIT_OBJECT_0 - 1);
		if (index >= 0 && index < armed.length()) armed[index] = false;
		emit this->changed();
	}

	for (auto* key: keys) RegCloseKey(key);
	for (auto* event: events) CloseHandle(event);
}

VirtualDesktops* VirtualDesktops::instance() {
	static auto* instance = new VirtualDesktops(); // NOLINT
	return instance;
}

#ifdef QS_TEST
VirtualDesktops* VirtualDesktops::createForTest(TestHooks hooks) {
	return new VirtualDesktops(std::move(hooks));
}

VirtualDesktops::VirtualDesktops(TestHooks hooks)
    : chordSender(std::move(hooks.chordSender))
    , testMode(true)
    , accessor(std::move(hooks.accessor)) {
	this->mDesktops = std::move(hooks.desktops);
	this->testAvailability = hooks.availability;
	this->initCommandObservation();
	if (this->mDesktops.isEmpty()) {
		this->mCurrent = -1;
	} else {
		this->mCurrent = qBound(qsizetype(0), hooks.current, this->mDesktops.length() - 1);
	}

	this->updateAccessorExports();
}

void VirtualDesktops::installAccessorForTest(Accessor accessor) {
	this->accessor = std::move(accessor);
	this->updateAccessorExports();
	this->setAccessorLoaded(true);
	this->testAvailability = DesktopAvailability::Available;
}

void VirtualDesktops::loseAccessorForTest() {
	this->testAvailability = DesktopAvailability::Disconnected;
	this->setAccessorLoaded(false);
}

void VirtualDesktops::destroyForTest() { delete this; }
#endif

VirtualDesktops::VirtualDesktops() {
	this->initCommandObservation();
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	ProcessIdToSessionId(GetCurrentProcessId(), &this->sessionId);
	this->refresh(true);

	this->cloakTimer.setSingleShot(true);
	this->cloakTimer.setInterval(CLOAK_REFRESH_DELAY_MS);
	QObject::connect(&this->cloakTimer, &QTimer::timeout, this, [this]() { this->refresh(); });

	startup::afterFirstFrame(this, [this]() {
		this->loadAccessor();
		this->refresh();
		if (this->accessorLoaded()) emit this->accessorReady();
	});

	auto sessionKey = QString::fromWCharArray(SESSION_KEY_FORMAT).arg(this->sessionId);
	this->watcher = new RegistryWatcher({QString::fromWCharArray(DESKTOPS_KEY), sessionKey}, this);
	QObject::connect(this->watcher, &RegistryWatcher::changed, this, [this]() {
		this->refresh(true);
	});
	this->watcher->start();

	if (auto* app = QCoreApplication::instance()) {
		QObject::connect(app, &QCoreApplication::aboutToQuit, this->watcher, &RegistryWatcher::stop);
	}
}

VirtualDesktops::~VirtualDesktops() {
	if (this->watcher != nullptr) this->watcher->stop();
	if (!this->testMode) shutdownNativeDesktopAccessor();
}

template <typename R, typename... P, typename... A>
R VirtualDesktops::call(R fallback, R (*fn)(P...), A... args) const {
	if (fn == nullptr) return fallback;

	R result = fallback;
	if (sehCall(fn, result, args...)) {
		if (this->nativeAccessor && nativeDesktopAvailability() != DesktopAvailability::Available) {
			this->setAccessorLoaded(false);
		}
		return result;
	}

	this->accessorFailed();
	return fallback;
}

void VirtualDesktops::accessorFailed() const {
	if (!this->nativeAccessor) {
		this->setAccessorLoaded(false);
		return;
	}

	qCWarning(logDesktops) << "Native desktop provider raised an exception; invalidating interfaces"
	                       << "and entering bounded recovery";
	// Defense in depth for faults outside a native leaf guard, including
	// local COM cleanup. Never let dispatch re-enable the stale provider.
	invalidateNativeDesktopAccessor(E_UNEXPECTED);
	this->setAccessorLoaded(false);
}

void VirtualDesktops::applyNativeSnapshot() {
	this->applyingNativeSnapshot = true;
	if (this->nativeAccessor)
		this->setAccessorLoaded(nativeDesktopAvailability() == DesktopAvailability::Available);
	this->refresh(true);
	this->applyingNativeSnapshot = false;
	emit this->windowDesktopsChanged();
	this->notifyCapability();
}

void VirtualDesktops::loadAccessor() {
	services::ServiceMessageWindow::instance()
	    ->addHandler(RegisterWindowMessageW(L"TaskbarCreated"), this, [this](WPARAM, LPARAM) {
		    invalidateNativeDesktopAccessor();
		    if (this->nativeAccessor) this->setAccessorLoaded(false);
	    });
	if (qEnvironmentVariable("CAELESTIA_WINDOWS_HYPR") == "1"
	    && qEnvironmentVariable("CAELESTIA_WINDOWS_HYPR_DISPATCH") == "1")
	{
		this->nativeAccessor = true;
		if (loadNativeDesktopAccessor(this->accessor, this, [this] {
			    this->updateAccessorExports();
			    this->setAccessorLoaded(true);
			    this->refresh(true);
			    emit this->accessorReady();
		    }))
		{
			this->updateAccessorExports();
			this->setAccessorLoaded(true);
		}
	}
	// Public queries share the private worker when enabled. Otherwise create
	// only a public manager, without bypassing a failed acquisition's backoff.
	initializeNativeDesktopQueries(this, [this] { this->applyNativeSnapshot(); });
	this->notifyCapability();
}

QString VirtualDesktops::readDesktopName(const GUID& id) const {
	if (guidIsNull(id)) return {};

	HKEY key = nullptr;
	auto sub = QString::fromWCharArray(DESKTOPS_KEY) + "\\Desktops\\" + guidToString(id);
	auto wsub = sub.toStdWString();
	if (RegOpenKeyExW(HKEY_CURRENT_USER, wsub.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
		return {};
	}

	auto name = readString(key, L"Name");
	RegCloseKey(key);
	return name;
}

bool VirtualDesktops::readRegistry(QList<GUID>& ids, GUID& current) const {
	HKEY key = nullptr;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, DESKTOPS_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS) {
		return false;
	}

	QByteArray blob;
	if (readBinary(key, L"VirtualDesktopIDs", blob)) {
		for (qsizetype offset = 0; offset + static_cast<qsizetype>(sizeof(GUID)) <= blob.length();
		     offset += static_cast<qsizetype>(sizeof(GUID)))
		{
			ids.append(guidAt(blob, offset));
		}
	}

	QByteArray currentBytes;
	HKEY sessionKey = nullptr;
	auto sessionPath = QString::fromWCharArray(SESSION_KEY_FORMAT).arg(this->sessionId).toStdWString();
	if (RegOpenKeyExW(HKEY_CURRENT_USER, sessionPath.c_str(), 0, KEY_READ, &sessionKey)
	    == ERROR_SUCCESS)
	{
		readBinary(sessionKey, L"CurrentVirtualDesktop", currentBytes);
		RegCloseKey(sessionKey);
	}

	if (currentBytes.length() < static_cast<qsizetype>(sizeof(GUID))) {
		readBinary(key, L"CurrentVirtualDesktop", currentBytes);
	}

	if (currentBytes.length() >= static_cast<qsizetype>(sizeof(GUID))) {
		current = guidAt(currentBytes, 0);
	}

	RegCloseKey(key);
	return true;
}

void VirtualDesktops::noteCloakChange() {
	if (!this->cloakTimer.isActive()) this->cloakTimer.start();
}

void VirtualDesktops::refresh(bool rereadNames) {
	if (!this->applyingNativeSnapshot && (!this->testMode || this->nativeAccessor))
		requestNativeDesktopRefresh();
#ifdef QS_TEST
	if (this->testMode) {
		auto count = this->accessorLoaded()
		               ? this->call(-1, this->accessor.getDesktopCount)
		               : -1;
		if (count < 0) return;

		auto changed = count != this->mDesktops.length();
		if (changed) {
			this->mDesktops.resize(count);
			emit this->desktopsChanged();
		}

		if (count == 0) {
			if (this->mCurrent != -1) {
				this->mCurrent = -1;
				emit this->currentChanged();
			}
		} else if (this->mCurrent < 0 || this->mCurrent >= count) {
			this->mCurrent = qBound(qsizetype(0), this->mCurrent, count - 1);
			emit this->currentChanged();
		}
		if (count > 0 && this->accessor.getCurrentDesktopNumber) {
			const auto current = this->call(-1, this->accessor.getCurrentDesktopNumber);
			if (current >= 0 && current < count && current != this->mCurrent) {
				this->mCurrent = current;
				emit this->currentChanged();
			}
		}
		this->observeSwitch();
		return;
	}
#endif

	QList<GUID> ids;
	GUID current {};

	if (!this->readRegistry(ids, current)) {
		qCWarning(logDesktops) << "Unable to read the virtual desktop registry keys.";
	}

	const auto& a = this->accessor;
	bool healthyEmpty = false;
	if (this->accessorLoaded()) {
		auto count = this->call(-1, a.getDesktopCount);
		if (count < 0 && this->nativeAccessor) {
			this->setAccessorLoaded(false);
		}
		if (count > 0) {
			ids.resize(count);
			if (a.getDesktopIdByNumber != nullptr) {
				for (auto i = 0; i < count; i++) {
					if (this->nativeAccessor || guidIsNull(ids[i])) {
						ids[i] = this->call(GUID {}, a.getDesktopIdByNumber, i);
					}
				}
			}
		}
		healthyEmpty = count == 0;
		if (healthyEmpty) ids.clear();
	}

	if (ids.isEmpty() && !healthyEmpty) ids.append(GUID {});

	auto sameIds = !rereadNames && ids.length() == this->mDesktops.length();
	for (qsizetype i = 0; sameIds && i < ids.length(); i++) {
		sameIds = IsEqualGUID(ids[i], this->mDesktops[i].id);
	}

	QList<Desktop> desktops;
	desktops.reserve(ids.length());

	for (qsizetype i = 0; i < ids.length(); i++) {
		auto name = sameIds ? this->mDesktops[i].name : this->readDesktopName(ids[i]);
		desktops.append(Desktop {.id = ids[i], .name = name});
	}

	auto currentIndex = qsizetype(-1);
	for (qsizetype i = 0; i < desktops.length(); i++) {
		if (IsEqualGUID(desktops[i].id, current)) {
			currentIndex = i;
			break;
		}
	}

	if (this->accessorLoaded()) {
		auto number = this->call(-1, a.getCurrentDesktopNumber);
		if (number >= 0 && number < desktops.length()) {
			currentIndex = number;
			current = desktops[number].id;
		}
	}

	if (currentIndex == -1 && !desktops.isEmpty()) currentIndex = 0;

	auto listChanged = desktops.length() != this->mDesktops.length();
	if (!listChanged) {
		for (qsizetype i = 0; i < desktops.length(); i++) {
			if (!IsEqualGUID(desktops[i].id, this->mDesktops[i].id)
			    || desktops[i].name != this->mDesktops[i].name)
			{
				listChanged = true;
				break;
			}
		}
	}

	auto currentChanged = currentIndex != this->mCurrent || !IsEqualGUID(current, this->mCurrentId);

	this->mDesktops = desktops;
	this->mCurrent = currentIndex;
	this->mCurrentId = current;

	if (listChanged) {
		qCDebug(logDesktops) << "Desktop list changed:" << desktops.length() << "desktops";
		emit this->desktopsChanged();
	}

	if (currentChanged) {
		qCDebug(logDesktops) << "Current desktop:" << currentIndex;
		emit this->currentChanged();
	}
	this->observeSwitch();
}

qsizetype VirtualDesktops::indexOf(const GUID& id) const {
	return desktopIndexOf(this->mDesktops, id);
}

GUID VirtualDesktops::windowDesktopId(HWND hwnd) const { return nativeWindowDesktopId(hwnd); }

qsizetype VirtualDesktops::windowDesktopIndex(HWND hwnd) const {
	auto index = this->indexOf(this->windowDesktopId(hwnd));
	if (index != -1) return index;

	const auto& a = this->accessor;
	if (this->accessorLoaded() && a.getWindowDesktopNumber != nullptr) {
		auto number = this->call(-1, a.getWindowDesktopNumber, hwnd);
		if (number >= 0 && number < this->count()) return number;
	}

	return -1;
}

bool VirtualDesktops::isWindowOnCurrent(HWND hwnd) const {
	return nativeWindowOnCurrentDesktop(hwnd) != 0;
}

void VirtualDesktops::sendShortcut(WORD key, int times) const {
	// Unreachable by design. Desktop control must never synthesize Ctrl+Win
	// chords: an unmodified Win+D degrades into the show-desktop effect. The
	// hook stays as a tripwire so tests can inject a recorder and prove no
	// operation sends a chord.
	if (this->chordSender) this->chordSender(key, times);
}

void VirtualDesktops::updateAccessorExports() {
	const auto& a = this->accessor;
	this->setPinExports(
	    a.isPinnedWindow != nullptr,
	    a.pinWindow != nullptr,
	    a.unPinWindow != nullptr
	);
	this->setDesktopControlExports(a.goToDesktopNumber != nullptr, a.createDesktop != nullptr);
}

void VirtualDesktops::warnAccessorOnly(const QString& operation) const {
	if (this->warnedOperations.contains(operation)) return;
	this->warnedOperations.insert(operation);
	qCWarning(logDesktops) << "Cannot" << operation
	                       << "without a compatible desktop-control provider; refusing (further uses are"
	                       << "not logged)";
}

void VirtualDesktops::retryAccessor() {
	if (this->nativeAccessor && retryNativeDesktopAccessor()) this->setAccessorLoaded(true);
}

DesktopAvailability VirtualDesktops::availability() const {
	if (this->testMode) {
		return this->accessorLoaded() ? DesktopAvailability::Available : this->testAvailability;
	}
	if (qEnvironmentVariable("CAELESTIA_WINDOWS_HYPR") != "1"
	    || qEnvironmentVariable("CAELESTIA_WINDOWS_HYPR_DISPATCH") != "1")
		return DesktopAvailability::Disabled;
	if (this->nativeAccessor) {
		const auto state = nativeDesktopAvailability();
		return state == DesktopAvailability::Available && !this->accessorLoaded()
		    ? DesktopAvailability::Disconnected : state;
	}
	return this->accessorLoaded() ? DesktopAvailability::Available : DesktopAvailability::MissingProvider;
}

QVariantMap VirtualDesktops::capability() const {
	const auto state = this->availability();
	return {{QStringLiteral("availability"), desktopAvailabilityName(state)},
	        {QStringLiteral("reason"), desktopAvailabilityReason(state)},
	        {QStringLiteral("provider"), QStringLiteral("build-gated-private-com")},
	        {QStringLiteral("operations"),
	         QVariantMap {{QStringLiteral("switch"), this->supports(DesktopOperation::Switch)},
	                      {QStringLiteral("create"), this->supports(DesktopOperation::Create)},
	                      {QStringLiteral("move"), this->supports(DesktopOperation::Move)},
	                      {QStringLiteral("remove"), this->supports(DesktopOperation::Remove)},
	                      {QStringLiteral("pin"), this->supports(DesktopOperation::Pin)}}}};
}

bool VirtualDesktops::supports(DesktopOperation operation) const {
	if (this->availability() != DesktopAvailability::Available || !this->accessorLoaded()) return false;
	switch (operation) {
	case DesktopOperation::Switch: return this->accessor.goToDesktopNumber != nullptr;
	case DesktopOperation::Create: return this->accessor.createDesktop != nullptr;
	case DesktopOperation::Move: return this->accessor.moveWindowToDesktopNumber != nullptr;
	case DesktopOperation::Remove: return this->accessor.removeDesktop != nullptr;
	case DesktopOperation::Pin: return this->pinSupported();
	}
	return false;
}

void VirtualDesktops::initCommandObservation() {
	this->creationTimer.setSingleShot(true);
	this->creationTimer.setInterval(CREATION_TIMEOUT_MS);
	QObject::connect(&this->creationTimer, &QTimer::timeout, this, [this] {
		this->finishCreation(DesktopCommandOutcome::TimedOut, QStringLiteral("observation-timeout"));
	});
	QObject::connect(this, &VirtualDesktops::desktopsChanged, this,
	                 &VirtualDesktops::observeCreation, Qt::QueuedConnection);
	this->switchDeadline.setSingleShot(true);
	this->switchDeadline.setInterval(1500);
	QObject::connect(&this->switchDeadline, &QTimer::timeout, this, [this] {
		this->finishSwitch(DesktopCommandOutcome::TimedOut, QStringLiteral("observation-timeout"));
	});
	QObject::connect(this, &AccessorState::accessorStateChanged, this, [this] {
		this->notifyCapability();
		if (!this->accessorLoaded()) {
			this->finishSwitch(DesktopCommandOutcome::Denied, this->commandFailureReason());
			this->creationTimer.stop();
			this->finishCreation(DesktopCommandOutcome::Denied, this->commandFailureReason());
		}
	});
}

void VirtualDesktops::notifyCapability() {
	const auto value = this->capability();
	if (value == this->emittedCapability) return;
	this->emittedCapability = value;
	emit this->capabilityChanged();
}

QString VirtualDesktops::commandFailureReason() const {
	if (nativeDesktopLastError() == HRESULT_FROM_WIN32(ERROR_TIMEOUT))
		return QStringLiteral("provider-timeout");
	if (nativeDesktopLastError() == HRESULT_FROM_WIN32(ERROR_BUSY))
		return QStringLiteral("provider-busy");
	const auto reason = desktopAvailabilityReason(this->availability());
	if (!reason.isEmpty()) return reason;
	if (this->nativeAccessor && nativeDesktopLastError() == E_ACCESSDENIED)
		return QStringLiteral("permission-denied");
	return QStringLiteral("command-rejected");
}

void VirtualDesktops::publishCommand(DesktopCommandResult result) {
	const auto snapshot = desktopCommandMap(result);
	this->mCommandResult = std::move(result);
	emit this->commandResult(snapshot);
	emit this->lastCommandChanged();
}

void VirtualDesktops::refuseCommand(const QString& operation, const QString& reason) {
	this->publishCommand(DesktopRefusal {operation, reason});
}

void VirtualDesktops::finishSwitch(DesktopCommandOutcome outcome, const QString& reason) {
	if (!this->pendingSwitch) return;
	const auto id = this->pendingSwitch->id;
	this->pendingSwitch.reset();
	this->switchDeadline.stop();
	this->publishCommand(DesktopRequestResult {id, QStringLiteral("switch"), outcome, reason});
}

void VirtualDesktops::observeSwitch() {
	if (!this->pendingSwitch) return;
	const auto& pending = *this->pendingSwitch;
	if (pending.index == this->mCurrent && pending.index < this->count()
	    && IsEqualGUID(pending.desktop, this->mDesktops[pending.index].id))
		this->finishSwitch(DesktopCommandOutcome::Observed);
}

bool VirtualDesktops::switchTo(qsizetype index) {
	this->retryAccessor();
	if (!this->supports(DesktopOperation::Switch)) {
		this->warnAccessorOnly("switch desktops");
		this->refuseCommand(QStringLiteral("switch"),
		    this->availability() == DesktopAvailability::Available
		        ? QStringLiteral("operation-unsupported") : this->commandFailureReason());
		return false;
	}
	if (index < 0 || index >= this->count()) {
		this->refuseCommand(QStringLiteral("switch"), QStringLiteral("invalid-target"));
		return false;
	}

	this->finishSwitch(DesktopCommandOutcome::Superseded, QStringLiteral("newer-request"));
	if (index == this->mCurrent) {
		this->publishCommand(DesktopRequestResult {++this->nextRequest, QStringLiteral("switch"),
		                                         DesktopCommandOutcome::Observed, {}});
		return true;
	}

	if (this->call(-1, this->accessor.goToDesktopNumber, static_cast<int>(index)) == -1) {
		qCWarning(logDesktops) << "GoToDesktopNumber" << index << "failed";
		this->refuseCommand(QStringLiteral("switch"), this->commandFailureReason());
		return false;
	}

	const auto id = ++this->nextRequest;
	this->pendingSwitch = PendingSwitch {id, index, this->mDesktops[index].id};
	this->switchDeadline.start();
	this->publishCommand(DesktopRequestResult {id, QStringLiteral("switch"), DesktopCommandOutcome::Accepted, {}});
	this->refresh();
	return true;
}

void VirtualDesktops::ensureCount(qsizetype count, std::function<void(bool)> done) {
	this->retryAccessor();
	if (!this->supports(DesktopOperation::Create)) {
		this->warnAccessorOnly("create desktops");
		this->refuseCommand(QStringLiteral("create"),
		    this->availability() == DesktopAvailability::Available
		        ? QStringLiteral("operation-unsupported") : this->commandFailureReason());
		done(false);
		return;
	}

	if (count > MAX_DESKTOPS || (!this->testMode && count > this->mWorkspaceLimit)) {
		qCWarning(logDesktops) << "Refusing desktop creation above configured limit" << this->mWorkspaceLimit;
		this->refuseCommand(QStringLiteral("create"), QStringLiteral("workspace-limit"));
		done(false);
		return;
	}

	if (this->count() >= count) {
		this->publishCommand(DesktopRequestResult {++this->nextRequest, QStringLiteral("create"),
		                                         DesktopCommandOutcome::Observed, {}});
		done(true);
		return;
	}

	const auto id = ++this->nextRequest;
	this->creationWaiters.append({count, id, std::move(done)});
	auto idle = this->creationTarget == 0;
	this->creationTarget = std::max(this->creationTarget, count);
	if (idle) this->createNext();
	if (std::any_of(this->creationWaiters.cbegin(), this->creationWaiters.cend(),
	                [id](const auto& waiter) { return waiter.id == id; })) {
		this->publishCommand(DesktopRequestResult {id, QStringLiteral("create"), DesktopCommandOutcome::Accepted, {}});
	}
}

void VirtualDesktops::createNext() {
	if (this->count() >= this->creationTarget) {
		this->finishCreation();
		return;
	}

	this->creationBefore = this->count();

	if (!this->accessorLoaded() || this->accessor.createDesktop == nullptr) {
		this->warnAccessorOnly("create desktops");
		this->creationTimer.stop();
		this->finishCreation(DesktopCommandOutcome::Denied, this->commandFailureReason());
		return;
	}

	if (this->call(-1, this->accessor.createDesktop) == -1) {
		qCWarning(logDesktops) << "CreateDesktop failed";
		this->finishCreation(DesktopCommandOutcome::Denied, this->commandFailureReason());
		return;
	}

	this->creationTimer.start();
	this->refresh();
}

void VirtualDesktops::observeCreation() {
	if (this->creationTarget == 0) return;
	if (!this->accessorLoaded() || this->accessor.createDesktop == nullptr) {
		this->warnAccessorOnly("create desktops");
		this->creationTimer.stop();
		this->finishCreation(DesktopCommandOutcome::Denied, this->commandFailureReason());
		return;
	}
	if (this->count() > this->creationBefore) {
		this->creationTimer.stop();
		this->createNext();
		return;
	}
}

void VirtualDesktops::finishCreation(DesktopCommandOutcome failure, const QString& reason) {
	auto waiters = std::exchange(this->creationWaiters, {});
	this->creationTarget = 0;

	for (auto& [count, id, done]: waiters) {
		const auto ok = this->accessorLoaded() && this->accessor.createDesktop != nullptr
		             && this->count() >= count;
		this->publishCommand(DesktopRequestResult {id, QStringLiteral("create"),
		                                         ok ? DesktopCommandOutcome::Observed : failure,
		                                         ok ? QString() : reason});
		done(ok);
	}
}

bool VirtualDesktops::removeDesktop(qsizetype index, qsizetype fallback) {
	const auto& a = this->accessor;
	if (!this->accessorLoaded() || a.removeDesktop == nullptr) {
		this->warnAccessorOnly("remove desktops");
		return false;
	}

	if (index < 0 || index >= this->count() || fallback < 0 || fallback >= this->count()
	    || index == fallback || this->count() < 2)
	{
		return false;
	}

	if (this->call(-1, a.removeDesktop, static_cast<int>(index), static_cast<int>(fallback)) == -1) {
		qCWarning(logDesktops) << "RemoveDesktop" << index << "failed";
		return false;
	}

	this->refresh();
	return true;
}

bool VirtualDesktops::moveWindow(HWND hwnd, qsizetype index) {
	this->retryAccessor();
	if (hwnd == nullptr || index < 0 || index >= this->count()) {
		this->refuseCommand(QStringLiteral("move"), QStringLiteral("invalid-target"));
		return false;
	}
	auto result = [this](bool ok, const QString& reason) {
		if (ok) this->publishCommand(DesktopUnobserved {QStringLiteral("move")});
		else this->refuseCommand(QStringLiteral("move"), reason);
		return ok;
	};

	if (isOwnWindow(hwnd)) {
		const auto ok = moveNativeDesktopOwnWindow(hwnd, this->mDesktops[index].id);
		if (ok) {
			const auto error = nativeDesktopLastError();
			auto reason = error == E_ACCESSDENIED ? QStringLiteral("permission-denied")
			                                     : QStringLiteral("command-rejected");
			if (error == HRESULT_FROM_WIN32(ERROR_TIMEOUT) || error == HRESULT_FROM_WIN32(ERROR_BUSY))
				reason = this->commandFailureReason();
			return result(*ok, reason);
		}
	}

	if (this->accessorLoaded()) {
		const auto ok = this->call(-1, this->accessor.moveWindowToDesktopNumber, hwnd, static_cast<int>(index)) != -1;
		return result(ok, this->commandFailureReason());
	}

	qCWarning(logDesktops) << "Moving another application's window to a desktop needs"
	                       << "a working desktop provider";
	return result(false, this->commandFailureReason());
}

bool VirtualDesktops::pinWindow(HWND hwnd, bool pinned) {
	auto& a = this->accessor;
	if (!this->pinSupported()) return false;
	return this->call(-1, pinned ? a.pinWindow : a.unPinWindow, hwnd) != -1;
}

bool VirtualDesktops::isWindowPinned(HWND hwnd) const {
	const auto& a = this->accessor;
	if (!this->pinSupported()) return false;
	return this->call(-1, a.isPinnedWindow, hwnd) == 1;
}

QString VirtualDesktops::guidToString(const GUID& guid) {
	wchar_t buffer[40] {};
	StringFromGUID2(guid, buffer, 40);
	return QString::fromWCharArray(buffer);
}

} // namespace qs::windows

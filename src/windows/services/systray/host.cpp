#include "host.hpp"
#include <algorithm>
#include <array>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <qt_windows.h>
#include <shellapi.h>

#include <qcoreapplication.h>
#include <qfileinfo.h>
#include <qhash.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qnamespace.h>
#include <qobject.h>
#include <qstring.h>
#include <qstringbuilder.h>
#include <qtenvironmentvariables.h>
#include <quuid.h>

#include "../../../core/logcat.hpp"
#include "../../redact.hpp"
#include "../../startup.hpp"
#include "../../supervisor_bridge.hpp"
#include "hook.hpp"
#include "item.hpp"

namespace qs::windows::services::systray {

namespace {

QS_LOGGING_CATEGORY(logTrayHost, "quickshell.windows.systray", QtWarningMsg);

constexpr ULONGLONG DELETION_MEMORY_MS = 10000;

constexpr int RESYNC_DELAY_MS = 300;
constexpr ULONGLONG SNAPSHOT_MIN_GAP_MS = 2000;
constexpr int CONFIRM_DELAY_MS = 2000;
constexpr int RECOVER_DELAY_MS = 1500;
constexpr ULONGLONG ANNOUNCE_COOLDOWN_MS = 15000;
constexpr ULONGLONG CLICK_ANNOUNCE_GAP_MS = 2000;
constexpr std::array BRIEF_RETRY_MS {1500, 3000, 6000};

struct KnownIcon {
	QUuid guid;
	const char* id;
	const char* title;
	Category::Enum category;
	bool skip;
};

const KnownIcon* knownIcon(const QUuid& guid) {
	// NOLINTBEGIN(cert-err58-cpp)
	static const KnownIcon icons[] = {
	    {QUuid("{7820ae73-23e3-4229-82c1-e41cb67d5b9c}"), "Windows.Volume", "Volume", Category::Hardware, true},
	    {QUuid("{7820ae74-23e3-4229-82c1-e41cb67d5b9c}"), "Windows.Network", "Network", Category::Hardware, true},
	    {QUuid("{7820ae75-23e3-4229-82c1-e41cb67d5b9c}"), "Windows.Power", "Power", Category::Hardware, true},
	    {QUuid("{7820ae76-23e3-4229-82c1-e41cb67d5b9c}"), "Windows.Health", "Security and Maintenance", Category::SystemServices, false},
	    {QUuid("{7820ae77-23e3-4229-82c1-e41cb67d5b9c}"), "Windows.Location", "Location", Category::SystemServices, false},
	    {QUuid("{7820ae78-23e3-4229-82c1-e41cb67d5b9c}"), "Windows.Hardware", "Safely Remove Hardware", Category::Hardware, false},
	    {QUuid("{7820ae81-23e3-4229-82c1-e41cb67d5b9c}"), "Windows.Update", "Windows Update", Category::SystemServices, false},
	    {QUuid("{7820ae82-23e3-4229-82c1-e41cb67d5b9c}"), "Windows.Microphone", "Microphone", Category::Hardware, false},
	    {QUuid("{7820ae83-23e3-4229-82c1-e41cb67d5b9c}"), "Windows.MeetNow", "Meet Now", Category::Communications, false},
	};
	// NOLINTEND(cert-err58-cpp)

	if (guid.isNull()) return nullptr;

	for (const auto& icon: icons) {
		if (icon.guid == guid) return &icon;
	}

	return nullptr;
}

QString exePathOf(HWND hwnd) {
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == 0) return {};

	auto* process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (process == nullptr) return {};

	wchar_t path[MAX_PATH] {};
	DWORD size = MAX_PATH;
	auto ok = QueryFullProcessImageNameW(process, 0, path, &size);
	CloseHandle(process);

	return ok ? QString::fromWCharArray(path, static_cast<qsizetype>(size)) : QString();
}

QString fileDescription(const QString& path) {
	if (path.isEmpty()) return {};

	auto native = path.toStdWString();
	DWORD unused = 0;
	auto size = GetFileVersionInfoSizeW(native.c_str(), &unused);
	if (size == 0) return {};

	std::vector<char> data(size);
	if (!GetFileVersionInfoW(native.c_str(), 0, size, data.data())) return {};

	struct Translation {
		WORD language;
		WORD codePage;
	};

	Translation* translation = nullptr;
	UINT length = 0;
	if (!VerQueryValueW(
	        data.data(),
	        L"\\VarFileInfo\\Translation",
	        reinterpret_cast<void**>(&translation), // NOLINT
	        &length
	    )
	    || length < sizeof(Translation))
	{
		return {};
	}

	auto key = QStringLiteral("\\StringFileInfo\\%1%2\\FileDescription")
	               .arg(translation->language, 4, 16, QChar(u'0'))
	               .arg(translation->codePage, 4, 16, QChar(u'0'))
	               .toStdWString();

	wchar_t* value = nullptr;
	if (!VerQueryValueW(data.data(), key.c_str(), reinterpret_cast<void**>(&value), &length) // NOLINT
	    || value == nullptr || length == 0)
	{
		return {};
	}

	return QString::fromWCharArray(value).trimmed();
}

} // namespace

TrayHost* TrayHost::instance() {
	static auto* host = new TrayHost(); // NOLINT
	return host;
}

#ifdef QS_TEST
TrayHost* TrayHost::createForTest() { return new TrayHost(); }
#endif

TrayHost::~TrayHost() {
	this->stopWorkers();

	{
		std::lock_guard lock(this->waitState->mutex);
		this->waitState->host = nullptr;
	}

	auto clean = true;
	for (auto& watch: this->ownerWatches) {
		// Blocking unregistration means the callback has finished, so its
		// context and the process handle can go. A failed unregister leaves a
		// live callback, so leak both rather than risk a use-after-free.
		if (watch.wait != nullptr
		    && !UnregisterWaitEx(watch.wait, INVALID_HANDLE_VALUE))
		{
			clean = false;
			continue;
		}

		delete watch.context;
		if (watch.process != nullptr) CloseHandle(watch.process);
	}

	if (clean && !this->watchesLost) delete this->waitState;
}

TrayHost::TrayHost() {
	this->waitState = new WaitState;
	this->waitState->host = this;
	this->ownerSweepTimer.setInterval(10000);
	QObject::connect(&this->ownerSweepTimer, &QTimer::timeout, this, &TrayHost::sweepUnwatchedOwners);

	this->snapshotTimer.setSingleShot(true);
	QObject::connect(&this->snapshotTimer, &QTimer::timeout, this, &TrayHost::startSnapshot);
	this->pollTimer.setSingleShot(true);
	QObject::connect(&this->pollTimer, &QTimer::timeout, this, &TrayHost::startSnapshot);

	this->recoverTimer.setSingleShot(true);
	this->recoverTimer.setInterval(RECOVER_DELAY_MS);
	QObject::connect(&this->recoverTimer, &QTimer::timeout, this, &TrayHost::recoverCallbacks);

	QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &TrayHost::stopWorkers);
	QObject::connect(
	    SupervisorBridge::instance(),
	    &SupervisorBridge::explorerSettled,
	    this,
	    [this](quint64 /*pid*/, qint64 /*hwnd*/, int epoch, bool /*suppressed*/) {
		    if (epoch > 1) this->onExplorerRestarted();
	    }
	);

	this->sink = [this](TrayIconMessage message) {
		QMetaObject::invokeMethod(
		    this,
		    [this, message = std::move(message)]() {
			    if (!this->stopping) this->apply(message);
		    },
		    Qt::QueuedConnection
		);
	};

	auto missedTraffic = [this]() {
		QMetaObject::invokeMethod(
		    this,
		    [this]() {
			    if (!this->stopping && this->transport == Transport::Mirror
			        && this->observerState == ComObserverState::Live)
			    {
				    this->scheduleSnapshot(RESYNC_DELAY_MS);
			    }
		    },
		    Qt::QueuedConnection
		);
	};

	auto mode = qEnvironmentVariable("QS_WINDOWS_TRAY_HOOK");

	if (mode == QStringLiteral("0")) {
		qCInfo(logTrayHost) << "QS_WINDOWS_TRAY_HOOK=0: not hooking the system tray";
		return;
	}

	this->configuredTransport = mode == QStringLiteral("com") ? Transport::ComHybrid
	                          : mode == QStringLiteral("brief") ? Transport::Brief
	                                                           : Transport::Mirror;
	this->transport = this->configuredTransport;
	this->hookState = TrayHookState::Starting;

	startup::afterFirstFrame(this, [this, missedTraffic]() {
		if (this->stopping) return;
		this->transportsStarted = true;
		auto brief = this->transport != Transport::Mirror;

		TrayHook::start(this->sink, missedTraffic, brief, [this](TrayHookState state) {
			QMetaObject::invokeMethod(
			    this,
			    [this, state]() {
				    if (this->stopping || this->hookState == state) return;
				    this->hookState = state;
				    emit this->hookCapabilityChanged();
			    },
			    Qt::QueuedConnection
			);
		});

		this->beginSnapshot();
		this->observer.start(this->sink, [this](ComObserverState state) {
			QMetaObject::invokeMethod(
			    this,
			    [this, state]() {
				    if (!this->stopping) this->onObserverState(state);
			    },
			    Qt::QueuedConnection
			);
		});
	});
}

QVariantMap TrayHost::hookCapability() const {
	auto capability = trayHookCapability(this->hookState);
	const char* name = "disabled";
	switch (this->transport) {
	case Transport::Disabled: break;
	case Transport::ComHybrid: name = "com-hybrid"; break;
	case Transport::Brief: name = "brief"; break;
	case Transport::Mirror: name = "mirror"; break;
	}
	capability.insert(QStringLiteral("transport"), QString::fromLatin1(name));
	capability.insert(
	    QStringLiteral("comObserver"),
	    QString::fromLatin1(comObserverStateName(this->observerState))
	);
	return capability;
}

void TrayHost::stopWorkers() {
	if (this->stopping) return;
	this->stopping = true;
	this->pollTimer.stop();
	this->snapshotTimer.stop();
	this->recoverTimer.stop();

	if (this->titlesWorker.joinable()) this->titlesWorker.request_stop();
	if (this->transportsStarted) {
		this->observer.stop();
		TrayHook::stop();
		this->observerState = ComObserverState::Inactive;
		this->hookState = TrayHookState::Stopped;
	}
	if (this->titlesWorker.joinable()) this->titlesWorker.join();
}

void TrayHost::onExplorerRestarted() {
	if (this->stopping || !this->transportsStarted) return;

	this->pollTimer.stop();
	this->snapshotTimer.stop();
	this->briefRetries = 0;
	this->startSnapshot();

	for (auto* item: this->mItems) item->tracking.announcedAt = 0;
	this->recoverTimer.start();
}

void TrayHost::apply(const TrayIconMessage& message) {
	if (message.hwnd == nullptr && message.guid.isNull()) return;

	auto* item = this->find(message.hwnd, message.uid, message.guid);

	switch (message.message) {
	case NIM_ADD:
	case NIM_MODIFY:
		if (message.seeded) this->snapshotReported++;

		if (item == nullptr) {
			if (!message.seeded || !this->deletedRecently(message)) this->add(message);
		} else if (!message.seeded) {
			item->update(message);
			if (message.fromObserver) {
				item->tracking.seenByExplorer = true;
				item->tracking.seenThisSnapshot = true;
				item->tracking.missedSnapshots = 0;
			} else {
				item->tracking.hookUpdatedAt = GetTickCount64();
			}
			// A GUID icon can come back from a restarted app with a new window
			// and process, so move the exit watch to the new owner.
			this->watchOwner(item);
		} else {
			item->tracking.seenByExplorer = true;
			item->tracking.seenThisSnapshot = true;
			item->tracking.missedSnapshots = 0;

			if (item->tracking.hookUpdatedAt < this->snapshotStartedAt) {
				item->refreshFromExplorer(message);
			}
		}
		break;
	case NIM_SETVERSION:
		if (item != nullptr) {
			item->update(message);
			item->tracking.hookUpdatedAt = GetTickCount64();
		}
		break;
	case NIM_DELETE:
		if (item != nullptr) this->remove(item);
		this->rememberDeletion(message.hwnd, message.uid, message.guid);
		break;
	default: break;
	}
}

void TrayHost::add(const TrayIconMessage& message) {
	if (message.hwnd == nullptr || !IsWindow(message.hwnd)) return;

	const auto* known = knownIcon(message.guid);
	if (known != nullptr && known->skip) return;

	auto exePath = message.exePath.isEmpty() ? exePathOf(message.hwnd) : message.exePath;
	auto exeName = QFileInfo(exePath).completeBaseName();

	auto id = known != nullptr ? QString::fromLatin1(known->id) : exeName;
	if (id.isEmpty()) id = QStringLiteral("unknown");

	auto title = known != nullptr ? QString::fromLatin1(known->title) : this->titles.value(exePath);
	if (title.isEmpty()) title = exeName;

	auto* item = new SystemTrayItem(message.hwnd, message.uid, message.guid, this);
	if (known != nullptr) item->setCategory(known->category);
	item->update(message);
	item->setIdentity(this->uniqueId(id, message.uid), title, exePath);

	if (message.fromObserver || message.seeded) {
		item->tracking.seenByExplorer = true;
		item->tracking.seenThisSnapshot = true;
	} else {
		item->tracking.hookUpdatedAt = GetTickCount64();
	}

	item->onCallbackNeeded = [this, item]() {
		auto now = GetTickCount64();
		if (now - item->tracking.announcedAt >= CLICK_ANNOUNCE_GAP_MS) item->tracking.announcedAt = 0;
		this->recoverCallbacks();
	};

	qCDebug(logTrayHost) << "Added tray icon" << qs::windows::logId(item->bindableId().value())
	                     << (message.seeded ? "from explorer's list" : "")
	                     << (item->hasCallback() ? "" : "(no callback message yet)");

	this->mItems.append(item);
	this->watchOwner(item);
	emit this->itemAdded(item);

	if (!item->hasCallback()) this->recoverTimer.start();
	if (known == nullptr) this->requestTitle(exePath);
}

void TrayHost::remove(SystemTrayItem* item) {
	qCDebug(logTrayHost) << "Removed tray icon" << qs::windows::logId(item->bindableId().value());

	this->unwatchOwner(item);
	this->mItems.removeOne(item);
	emit this->itemRemoved(item);
	item->deleteLater();
}

void TrayHost::watchOwner(SystemTrayItem* item) {
	auto* hwnd = item->ownerWindow();
	DWORD pid = 0;
	auto thread = GetWindowThreadProcessId(hwnd, &pid);

	auto existing = this->ownerWatches.find(item);
	if (existing != this->ownerWatches.end()) {
		// Same process still running: the wait already covers this icon.
		if (existing->pid == pid && WaitForSingleObject(existing->process, 0) == WAIT_TIMEOUT) return;
	}
	auto fallback = this->unwatchedOwners.constFind(item);
	if (fallback != this->unwatchedOwners.cend() && fallback->hwnd == hwnd
	    && fallback->pid == pid && fallback->thread == thread)
		return;
	this->unwatchOwner(item);

	auto sweep = [this, item, hwnd, pid, thread] {
		this->unwatchedOwners.insert(item, {hwnd, pid, thread});
		if (!this->ownerSweepTimer.isActive()) this->ownerSweepTimer.start();
	};

	auto process = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
	if (process == nullptr) {
		// Elevated or protected owners may deny SYNCHRONIZE. Their HWND
		// and owning thread still tell us when the icon can no longer work.
		if (pid) qCDebug(logTrayHost) << "Polling the tray owner window, process wait unavailable:"
		                             << GetLastError();
		sweep();
		return;
	}

	auto token = ++this->nextOwnerToken;
	auto* context = new OwnerWaitContext {this->waitState, token};

	HANDLE wait = nullptr;
	if (!RegisterWaitForSingleObject(
	        &wait,
	        process,
	        &TrayHost::ownerExited,
	        context,
	        INFINITE,
	        WT_EXECUTEONLYONCE
	    ))
	{
		qCWarning(logTrayHost) << "Can't register the owner-process wait of"
		                      << qs::windows::logId(item->bindableId().value()) << ":"
		                      << GetLastError();
		CloseHandle(process);
		delete context;
		sweep();
		return;
	}

	this->ownerWatches.insert(item, {process, wait, pid, token, context});
}

void TrayHost::unwatchOwner(SystemTrayItem* item) {
	if (this->unwatchedOwners.remove(item) && this->unwatchedOwners.isEmpty())
		this->ownerSweepTimer.stop();
	auto it = this->ownerWatches.find(item);
	if (it == this->ownerWatches.end()) return;

	auto watch = *it;
	this->ownerWatches.erase(it);

	// Blocking unregistration is what makes it safe to free the context and
	// close the process handle: when it returns, no callback is running.
	auto unregistered = watch.wait == nullptr || UnregisterWaitEx(watch.wait, INVALID_HANDLE_VALUE);

	if (!unregistered) {
		// A live callback could still read the context, and closing the process
		// handle under a pending wait is undefined. Leak both and keep the
		// state alive for whatever callback still runs.
		qCWarning(logTrayHost) << "Can't unregister an owner-process wait:" << GetLastError();
		this->watchesLost = true;
		return;
	}

	delete watch.context;
	if (watch.process != nullptr) CloseHandle(watch.process);
}

void TrayHost::sweepUnwatchedOwners() {
	QList<SystemTrayItem*> expired;
	for (const auto& [item, owner]: this->unwatchedOwners.asKeyValueRange()) {
		DWORD pid = 0;
		auto thread = GetWindowThreadProcessId(owner.hwnd, &pid);
		if (!thread || thread != owner.thread || pid != owner.pid) expired.append(item);
	}
	for (auto* item: expired) {
		this->rememberDeletion(item->ownerWindow(), item->iconId(), item->iconGuid());
		this->remove(item);
	}
}

void CALLBACK TrayHost::ownerExited(PVOID context, BOOLEAN /*timedOut*/) {
	auto* wait = static_cast<OwnerWaitContext*>(context);

	std::lock_guard lock(wait->state->mutex);
	auto* host = wait->state->host;
	if (host == nullptr) return;

	QMetaObject::invokeMethod(
	    host,
	    [host, token = wait->token]() { host->onOwnerExited(token); },
	    Qt::QueuedConnection
	);
}

void TrayHost::onOwnerExited(quint64 token) {
	for (auto it = this->ownerWatches.begin(); it != this->ownerWatches.end(); ++it) {
		if (it->token != token) continue;

		auto* item = it.key();
		qCDebug(logTrayHost) << "Owner process of" << qs::windows::logId(item->bindableId().value())
		                     << "exited; removing its tray icon";
		this->remove(item);
		return;
	}
}

void TrayHost::rememberDeletion(HWND hwnd, UINT uid, const QUuid& guid) {
	auto now = GetTickCount64();
	this->deletions.removeIf([&](const Deletion& deletion) {
		return now - deletion.at > DELETION_MEMORY_MS;
	});
	this->deletions.append({hwnd, uid, guid, now});
}

void TrayHost::beginSnapshot() {
	this->snapshotRunning = true;
	this->snapshotAgain = false;
	this->snapshotStartedAt = GetTickCount64();
	this->snapshotReported = 0;

	for (auto* item: this->mItems) {
		item->tracking.seenThisSnapshot = false;
	}
}

void TrayHost::startSnapshot() {
	if (this->stopping || !this->transportsStarted) return;
	if (this->snapshotRunning) {
		this->snapshotAgain = true;
		return;
	}

	this->beginSnapshot();
	this->observer.resync();
}

void TrayHost::scheduleSnapshot(int delayMs) {
	if (this->snapshotTimer.isActive()) return;

	auto sinceLast = GetTickCount64() - this->snapshotDoneAt;
	auto gap = sinceLast >= SNAPSHOT_MIN_GAP_MS ? 0 : SNAPSHOT_MIN_GAP_MS - sinceLast;

	this->snapshotTimer.start(std::max(delayMs, static_cast<int>(gap)));
}

void TrayHost::onObserverState(ComObserverState state) {
	auto changed = this->observerState != state;
	this->observerState = state;

	if (state == ComObserverState::Live || state == ComObserverState::StubNoOp
	    || state == ComObserverState::Unavailable)
	{
		auto live = state == ComObserverState::Live;
		this->pollTimer.stop();
		if (!live) this->snapshotTimer.stop();

		if (this->configuredTransport == Transport::ComHybrid) {
			auto transport = live ? Transport::ComHybrid : Transport::Mirror;
			if (this->transport != transport) {
				this->transport = transport;
				TrayHook::setBrief(live);
				changed = true;
			}
			if (!live && !this->fallbackLogged) {
				this->fallbackLogged = true;
				qCInfo(logTrayHost) << "Tray COM observer is" << comObserverStateName(state)
				                    << "; using persistent tray mirror";
			}
		} else if (this->configuredTransport == Transport::Brief) {
			if (state != ComObserverState::Unavailable) this->briefRetries = 0;
			else if (this->observer.canRetry() && !this->snapshotAgain
			         && this->briefRetries < BRIEF_RETRY_MS.size())
				this->pollTimer.start(BRIEF_RETRY_MS[this->briefRetries++]);
		}

		this->onSnapshotDone(live);
	}

	if (changed) emit this->hookCapabilityChanged();
}

void TrayHost::onSnapshotDone(bool ok) {
	this->snapshotRunning = false;
	this->snapshotDoneAt = GetTickCount64();

	auto confirm = false;

	if (ok && this->snapshotReported > 0) {
		auto items = this->mItems;
		for (auto* item: items) {
			auto& tracking = item->tracking;

			if (tracking.seenThisSnapshot || !tracking.seenByExplorer
			    || tracking.hookUpdatedAt >= this->snapshotStartedAt
			    || item->bindableStatus().value() == Status::Passive)
			{
				continue;
			}

			if (++tracking.missedSnapshots >= 2) {
				this->rememberDeletion(item->ownerWindow(), item->iconId(), item->iconGuid());
				this->remove(item);
			} else {
				confirm = true;
			}
		}
	}

	if (confirm || this->snapshotAgain) this->scheduleSnapshot(CONFIRM_DELAY_MS);
}

void TrayHost::recoverCallbacks() {
	auto missing = std::ranges::any_of(this->mItems, [](SystemTrayItem* item) {
		return !item->hasCallback();
	});

	if (!missing) return;

	// Explorer's icon list has no callback messages. Ask the owners to add
	// their icons again so the hook sees them.
	auto now = GetTickCount64();
	std::vector<HWND> owners;

	for (auto* item: this->mItems) {
		if (item->hasCallback() || now - item->tracking.announcedAt < ANNOUNCE_COOLDOWN_MS) continue;
		item->tracking.announcedAt = now;
		owners.push_back(item->ownerWindow());
	}

	if (!owners.empty()) {
		qCDebug(logTrayHost) << "Asking" << owners.size()
		                     << "tray icon owners to add their icons again";
		TrayHook::announceTo(std::move(owners));
	}
}

void TrayHost::requestTitle(const QString& exePath) {
	if (this->stopping || exePath.isEmpty() || this->titles.contains(exePath)) return;
	if (this->titleQueue.contains(exePath)) return;

	this->titleQueue.append(exePath);
	if (!this->titlesWorker.joinable()) this->readTitles();
}

void TrayHost::readTitles() {
	this->titlesWorker = std::jthread([this, paths = this->titleQueue](std::stop_token stop) {
		SetThreadDescription(GetCurrentThread(), L"qs tray titles");

		QHash<QString, QString> found;
		for (const auto& path: paths) {
			if (stop.stop_requested()) return;
			found.insert(path, fileDescription(path));
		}

		QMetaObject::invokeMethod(
		    this,
		    [this, found = std::move(found)]() {
			    if (!this->stopping) this->onTitles(found);
		    },
		    Qt::QueuedConnection
		);
	});
}

void TrayHost::onTitles(const QHash<QString, QString>& found) {
	this->titlesWorker.join();

	for (const auto& [path, title]: found.asKeyValueRange()) {
		this->titleQueue.removeOne(path);
		this->titles.insert(path, title);
		if (title.isEmpty()) continue;

		for (auto* item: this->mItems) {
			if (item->executable() == path && knownIcon(item->iconGuid()) == nullptr) {
				item->setTitle(title);
			}
		}
	}

	if (!this->titleQueue.isEmpty()) this->readTitles();
}

SystemTrayItem* TrayHost::find(HWND hwnd, UINT uid, const QUuid& guid) const {
	for (auto* item: this->mItems) {
		if (item->matches(hwnd, uid, guid)) return item;
	}

	return nullptr;
}

QString TrayHost::uniqueId(const QString& base, UINT uid) const {
	auto taken = [this](const QString& id) {
		for (auto* item: this->mItems) {
			if (item->bindableId().value() == id) return true;
		}
		return false;
	};

	if (!taken(base)) return base;

	QString id = base % u':' % QString::number(uid);
	for (auto n = 2; taken(id); n++) {
		id = base % u':' % QString::number(uid) % u'-' % QString::number(n);
	}

	return id;
}

bool TrayHost::deletedRecently(const TrayIconMessage& message) {
	auto now = GetTickCount64();

	for (const auto& deletion: this->deletions) {
		if (now - deletion.at > DELETION_MEMORY_MS) continue;
		if (!message.guid.isNull() && message.guid == deletion.guid) return true;
		if (message.hwnd == deletion.hwnd && message.uid == deletion.uid) return true;
	}

	return false;
}

} // namespace qs::windows::services::systray

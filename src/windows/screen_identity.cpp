#include "screen_identity.hpp"
#include <algorithm>
#include <utility>
#include <vector>

#include <qdir.h>
#include <qcryptographichash.h>
#include <qdatetime.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qguiapplication.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qloggingcategory.h>
#include <qmap.h>
#include <qpointer.h>
#include <qsavefile.h>
#include <qscreen_platform.h>
#include <qset.h>
#include <qstandardpaths.h>
#include <qt_windows.h>

#include "redact.hpp"

namespace qs::windows {
namespace {

Q_LOGGING_CATEGORY(logScreenIdentity, "quickshell.windows.screen", QtInfoMsg);

constexpr auto RESERVATIONS_FILE = "screen-names.json";

QString pathId(const QString& devicePath) {
	return QStringLiteral("path:") + devicePath.toCaseFolded();
}

QString targetBase(const LUID& adapterId, quint32 target) {
	return QStringLiteral("target:%1:%2:%3")
	    .arg(adapterId.HighPart)
	    .arg(adapterId.LowPart)
	    .arg(target);
}

QString sourceBase(const QString& device) {
	return QStringLiteral("source:") + device.toCaseFolded();
}

QString sessionId(quint64 token, const QString& base) {
	return QStringLiteral("session:%1:%2").arg(token).arg(base);
}

QString profileDir(QStandardPaths::StandardLocation location) {
	return QStandardPaths::writableLocation(location) + QStringLiteral("/caelestia");
}

QString defaultReservationsPath() {
	auto isolated = qEnvironmentVariable("CAELESTIA_STATE_DIR");
	auto stateDir = isolated.isEmpty() ? profileDir(QStandardPaths::GenericStateLocation) : isolated;
	return stateDir + QLatin1Char('/') + QLatin1StringView(RESERVATIONS_FILE);
}

QString defaultMonitorConfigDir() {
	auto isolated = qEnvironmentVariable("CAELESTIA_CONFIG_DIR");
	auto configDir =
	    isolated.isEmpty() ? profileDir(QStandardPaths::GenericConfigLocation) : isolated;
	return configDir + QStringLiteral("/monitors");
}

// Deterministic fallback used when persistence is disabled (fixtures) and for
// session-local screens. Plain labels are reserved first; only a genuine
// duplicate gains " (2)", " (3)"...
void assignUniqueNames(QList<DisplayIdentity>& identities) {
	using Index = qsizetype;
	auto byIdentity = [&identities](Index left, Index right) {
		const auto& a = identities[left];
		const auto& b = identities[right];
		auto aId = a.stableId.isEmpty() ? a.deviceName : a.stableId;
		auto bId = b.stableId.isEmpty() ? b.deviceName : b.stableId;
		auto order = aId.compare(bId, Qt::CaseInsensitive);
		if (order != 0) return order < 0;
		return a.deviceName.compare(b.deviceName, Qt::CaseInsensitive) < 0;
	};

	QMap<QString, QList<Index>> groups;
	QSet<QString> used;
	for (Index index = 0; index < identities.size(); ++index) {
		auto& identity = identities[index];
		if (identity.friendlyName.isEmpty()) identity.friendlyName = identity.deviceName;
		if (identity.uniqueName.isEmpty()) {
			groups[identity.friendlyName.toCaseFolded()].append(index);
		} else {
			used.insert(identity.uniqueName.toCaseFolded());
		}
	}

	for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
		auto indices = it.value();
		std::sort(indices.begin(), indices.end(), byIdentity);
		auto& first = identities[indices.first()];
		first.uniqueName = first.friendlyName;
		used.insert(first.uniqueName.toCaseFolded());
	}

	for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
		auto indices = it.value();
		std::sort(indices.begin(), indices.end(), byIdentity);
		for (Index index = 1; index < indices.size(); ++index) {
			auto& identity = identities[indices[index]];
			auto suffix = 2;
			auto candidate = QStringLiteral("%1 (%2)").arg(identity.friendlyName).arg(suffix);
			while (used.contains(candidate.toCaseFolded())) {
				candidate = QStringLiteral("%1 (%2)").arg(identity.friendlyName).arg(++suffix);
			}
			identity.uniqueName = candidate;
			used.insert(candidate.toCaseFolded());
		}
	}
}

QString deviceNameFor(QScreen* screen) {
	auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
	if (native == nullptr) return {};
	MONITORINFOEXW info {};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfoW(native->handle(), &info)) return {};
	return QString::fromWCharArray(info.szDevice);
}

QList<DisplayIdentity> queryIdentities() {
	QList<DisplayIdentity> identities;
	// The topology may change between the size query and the data query.
	for (int attempt = 0; attempt != 3; ++attempt) {
		UINT32 pathCount = 0;
		UINT32 modeCount = 0;
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
			break;
		std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
		std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
		auto status = QueryDisplayConfig(
		    QDC_ONLY_ACTIVE_PATHS,
		    &pathCount,
		    paths.data(),
		    &modeCount,
		    modes.data(),
		    nullptr
		);
		if (status == ERROR_INSUFFICIENT_BUFFER) continue;
		if (status != ERROR_SUCCESS) break;
		paths.resize(pathCount);
		for (const auto& path: paths) {
			DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
			source.header = {
			    DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
			    sizeof(source),
			    path.sourceInfo.adapterId,
			    path.sourceInfo.id
			};
			DISPLAYCONFIG_TARGET_DEVICE_NAME target {};
			target.header = {
			    DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME,
			    sizeof(target),
			    path.targetInfo.adapterId,
			    path.targetInfo.id
			};
			if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS
			    || DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS)
				continue;
			auto targetPath = QString::fromWCharArray(target.monitorDevicePath);
			auto persistent = !targetPath.isEmpty();
			identities.append({
			    QString::fromWCharArray(source.viewGdiDeviceName),
			    targetPath,
			    QString::fromWCharArray(target.monitorFriendlyDeviceName).trimmed(),
			    persistent ? pathId(targetPath)
			               : targetBase(path.targetInfo.adapterId, path.targetInfo.id),
			    {},
			    persistent,
			});
		}
		break;
	}

	// Some virtual/older drivers do not supply DisplayConfig friendly metadata.
	// EDD_GET_DEVICE_INTERFACE_NAME gives a target path, not the reused GDI name.
	for (DWORD index = 0;; ++index) {
		DISPLAY_DEVICEW adapter {};
		adapter.cb = sizeof(adapter);
		if (!EnumDisplayDevicesW(nullptr, index, &adapter, 0)) break;
		if (!(adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
		auto device = QString::fromWCharArray(adapter.DeviceName);
		for (DWORD monitorIndex = 0;; ++monitorIndex) {
			DISPLAY_DEVICEW monitor {};
			monitor.cb = sizeof(monitor);
			if (!EnumDisplayDevicesW(
			        adapter.DeviceName,
			        monitorIndex,
			        &monitor,
			        EDD_GET_DEVICE_INTERFACE_NAME
			    ))
				break;
			if (!(monitor.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;
			auto path = QString::fromWCharArray(monitor.DeviceID);
			bool found = false;
			for (auto& identity: identities) {
				if (identity.deviceName.compare(device, Qt::CaseInsensitive) != 0) continue;
				found = true;
				if (!identity.devicePath.isEmpty()
				    && identity.devicePath.compare(path, Qt::CaseInsensitive) != 0)
					continue;
				if (!path.isEmpty()) {
					identity.devicePath = path;
					identity.stableId = pathId(path);
					identity.persistent = true;
				}
				if (identity.friendlyName.isEmpty())
					identity.friendlyName = QString::fromWCharArray(monitor.DeviceString).trimmed();
			}
			if (!found) {
				auto persistent = !path.isEmpty();
				identities.append({
				    device,
				    path,
				    QString::fromWCharArray(monitor.DeviceString).trimmed(),
				    persistent ? pathId(path) : sourceBase(device),
				    {},
				    persistent,
				});
			}
		}
	}

	// Always prefer an interface path supplied by either API. Only identities
	// with no path at all stay session-local.
	for (auto& identity: identities) {
		if (!identity.devicePath.isEmpty()) {
			identity.stableId = pathId(identity.devicePath);
			identity.persistent = true;
		} else {
			if (identity.stableId.isEmpty()) identity.stableId = sourceBase(identity.deviceName);
			identity.persistent = false;
		}
	}
	return identities;
}

} // namespace

ScreenIdentityRegistry* ScreenIdentityRegistry::instance() {
	static QPointer<ScreenIdentityRegistry> registry;
	if (!registry)
		registry = new ScreenIdentityRegistry(
		    queryIdentities,
		    deviceNameFor,
		    qGuiApp,
		    defaultReservationsPath(),
		    defaultMonitorConfigDir()
		);
	return registry;
}

ScreenIdentityRegistry::ScreenIdentityRegistry(
    Source source,
    DeviceName deviceName,
    QObject* parent,
    QString reservationsPath,
    QString monitorConfigDir
)
    : QObject(parent)
    , source(std::move(source))
    , deviceName(std::move(deviceName))
    , reservationsPath(std::move(reservationsPath))
    , monitorConfigDir(std::move(monitorConfigDir)) {
	this->loadReservations();
	this->settleTimer.setSingleShot(true);
	this->settleTimer.setInterval(250);
	connect(&this->settleTimer, &QTimer::timeout, this, &ScreenIdentityRegistry::refresh);
	if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
		app->installNativeEventFilter(this);
		for (auto* screen: app->screens()) this->watch(screen);
		connect(app, &QGuiApplication::screenAdded, this, [this](QScreen* screen) {
			this->watch(screen);
			this->scheduleRefresh();
		});
		connect(app, &QGuiApplication::screenRemoved, this, [this](QScreen* screen) {
			// Consumers undo native state and drop their own while the id still
			// resolves to this screen.
			if (auto state = this->states.constFind(screen); state != this->states.cend())
				emit this->identityRetired(state->identity.stableId, screen);
			this->states.remove(screen);
			this->sessions.remove(screen);
			// Never answer with a screen that is about to be destroyed, before
			// the coalesced refresh below rebuilds the indexes.
			this->screensById.removeIf([screen](auto entry) { return entry.value() == screen; });
			this->screensByName.removeIf([screen](auto entry) { return entry.value() == screen; });
			emit this->screenRemoved(screen);
			this->scheduleRefresh();
		});
	}
	this->refresh();
}

ScreenIdentityRegistry::~ScreenIdentityRegistry() {
	if (auto* app = QCoreApplication::instance()) app->removeNativeEventFilter(this);
}

void ScreenIdentityRegistry::watch(QScreen* screen) {
	connect(screen, &QScreen::geometryChanged, this, &ScreenIdentityRegistry::scheduleRefresh);
	connect(
	    screen,
	    &QScreen::logicalDotsPerInchChanged,
	    this,
	    &ScreenIdentityRegistry::scheduleRefresh
	);
	connect(
	    screen,
	    &QScreen::physicalDotsPerInchChanged,
	    this,
	    &ScreenIdentityRegistry::scheduleRefresh
	);
}

void ScreenIdentityRegistry::scheduleRefresh() {
	// Run after Qt has consumed WM_DISPLAYCHANGE/WM_DPICHANGED. Requery once
	// more after the driver settles, including identity-only topology changes.
	this->settleTimer.start();
	if (this->refreshPending) return;
	this->refreshPending = true;
	QTimer::singleShot(0, this, [this]() {
		this->refreshPending = false;
		this->refresh();
	});
}

bool ScreenIdentityRegistry::nativeEventFilter(const QByteArray&, void* message, qintptr*) {
	auto* msg = static_cast<MSG*>(message);
	if (msg->message == WM_DISPLAYCHANGE || msg->message == WM_DEVICECHANGE
	    || msg->message == WM_DPICHANGED || msg->message == WM_SETTINGCHANGE)
		this->scheduleRefresh();
	return false;
}

void ScreenIdentityRegistry::refresh() {
	if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) return;
	auto displays = this->source();
	for (const auto& display: displays) {
		auto name = display.friendlyName.trimmed();
		if (!display.devicePath.isEmpty() && !name.isEmpty())
			this->namesByTargetPath.insert(display.devicePath.toCaseFolded(), name);
	}
	auto screens = QGuiApplication::screens();
	QList<DisplayIdentity> identities;
	QList<QPair<QString, QScreen*>> retired;
	identities.reserve(screens.size());
	for (auto* screen: screens) {
		auto device = this->deviceName(screen);
		DisplayIdentity identity {device, {}, {}};
		for (const auto& display: displays) {
			if (device.isEmpty() || display.deviceName.compare(device, Qt::CaseInsensitive) != 0)
				continue;
			identity = display;
			break;
		}
		identity.friendlyName = identity.friendlyName.trimmed();
		if (identity.friendlyName.isEmpty()) {
			// A GDI source alone cannot prove this is still the same target.
			// Retain known metadata by target path, including across a failed
			// query or a QScreen/GDI-source reassignment.
			if (!identity.devicePath.isEmpty())
				identity.friendlyName = this->namesByTargetPath.value(identity.devicePath.toCaseFolded());
			if (identity.friendlyName.isEmpty())
				identity.friendlyName =
				    identity.devicePath.isEmpty() ? this->unresolvedName(screen) : identity.devicePath;
		}

		if (!identity.devicePath.isEmpty()) {
			// The interface path is the only persistent identity.
			identity.stableId = pathId(identity.devicePath);
			identity.persistent = true;
			this->sessions.remove(screen);
		} else {
			auto base = identity.stableId;
			if (base.startsWith(QStringLiteral("session:"))) base = base.mid(8);
			if (base.isEmpty() || base.startsWith(QStringLiteral("path:")))
				base = device.isEmpty() ? sourceBase(identity.friendlyName) : sourceBase(device);
			auto& session = this->sessions[screen];
			if (session.base != base) {
				session.base = base;
				session.token = ++this->nextSessionToken;
			}
			identity.stableId = sessionId(session.token, base);
			identity.persistent = false;
		}

		// A screen that now has another identity (a session-only screen that gains
		// an interface path, a reused QScreen behind another target) is a removal
		// followed by an addition. Never a rename.
		auto previous = this->states.constFind(screen);
		if (previous != this->states.cend() && previous->identity.stableId != identity.stableId)
			retired.append({previous->identity.stableId, screen});
		identities.append(identity);
	}

	if (this->reservationsPath.isEmpty()) {
		assignUniqueNames(identities);
	} else {
		this->assignReservedNames(identities);
	}

	// The old ids still resolve to their screens here, so consumers can undo
	// native state (a dimmed gamma ramp) before anything is republished.
	for (const auto& [id, screen]: retired) emit this->identityRetired(id, screen);

	QList<QScreen*> changed;
	QList<QScreen*> renamed;
	this->screensById.clear();
	this->screensByName.clear();
	for (auto index = 0; index < screens.size(); ++index) {
		auto* screen = screens[index];
		const auto& identity = identities[index];
		auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
		auto monitor = reinterpret_cast<quintptr>(native ? native->handle() : nullptr);
		State next {
		    identity,
		    monitor,
		    screen->geometry(),
		    screen->devicePixelRatio(),
		    screen->physicalDotsPerInch(),
		    screen->logicalDotsPerInch()
		};
		if (!this->states.contains(screen) || this->states[screen] != next) {
			if (!this->states.contains(screen) || this->states[screen].identity != identity)
				renamed.append(screen);
			this->states.insert(screen, next);
			changed.append(screen);
		}
		this->screensById.insert(identity.stableId, screen);
		this->screensByName.insert(identity.uniqueName.toCaseFolded(), screen);
	}
	// Publish the complete topology before any consumer queries another screen.
	for (auto* screen: renamed) emit this->identityChanged(screen);
	for (auto* screen: changed) emit this->screenChanged(screen);
}

QString ScreenIdentityRegistry::reservationKey(const QString& persistentId) {
	return QStringLiteral("sha256:")
	     + QString::fromLatin1(
	           QCryptographicHash::hash(persistentId.toUtf8(), QCryptographicHash::Sha256).toHex()
	     );
}

namespace {

// A stored key is already hashed, or a raw "path:" key written before keys were
// hashed. Anything else (for example a session id) is never a reservation.
QString storedKey(const QString& key) {
	if (key.startsWith(QStringLiteral("path:"))) return ScreenIdentityRegistry::reservationKey(key);
	if (key.startsWith(QStringLiteral("sha256:")) && key.size() == 7 + 64) return key;
	return {};
}

} // namespace

void ScreenIdentityRegistry::assignReservedNames(QList<DisplayIdentity>& identities) {
	// A session-local name lasts exactly as long as its session id. A surviving
	// monitor keeps it however its neighbours come and go; only a new session id
	// is allocated a name.
	QSet<QString> liveSessions;
	for (const auto& identity: std::as_const(identities))
		if (!identity.persistent) liveSessions.insert(identity.stableId);
	this->sessionNames.removeIf([&liveSessions](auto entry) {
		return !liveSessions.contains(entry.key());
	});

	QSet<QString> used;
	for (const auto& reservation: std::as_const(this->reservations))
		used.insert(reservation.name.toCaseFolded());
	for (const auto& name: std::as_const(this->sessionNames)) used.insert(name.toCaseFolded());

	QList<qsizetype> persistent;
	for (qsizetype index = 0; index < identities.size(); ++index)
		if (identities[index].persistent) persistent.append(index);
	std::sort(persistent.begin(), persistent.end(), [&identities](qsizetype left, qsizetype right) {
		return identities[left].stableId.compare(identities[right].stableId, Qt::CaseInsensitive) < 0;
	});

	const auto today = QDate::currentDate().toString(Qt::ISODate);
	auto changed = false;
	for (const auto index: persistent) {
		auto& identity = identities[index];
		auto key = reservationKey(identity.stableId);
		auto reservation = this->reservations.find(key);
		if (reservation != this->reservations.end()) {
			identity.uniqueName = reservation->name;
			if (reservation->label.isEmpty()) reservation->label = identity.friendlyName;
			if (reservation->lastSeen != today) {
				reservation->lastSeen = today;
				changed = true;
			}
			continue;
		}

		auto candidate = this->allocateName(identity.friendlyName, used);

		Reservation next;
		next.name = candidate;
		next.label = identity.friendlyName;
		next.lastSeen = today;
		// Only a generated suffix can refer to a legacy shared layer. A genuine
		// label ending in " (2)" is never treated as a collision suffix.
		if (candidate.compare(identity.friendlyName, Qt::CaseInsensitive) != 0
		    && this->hasLegacyLayer(identity.friendlyName))
			next.legacyLabel = identity.friendlyName;
		this->reservations.insert(key, next);
		identity.uniqueName = candidate;
		used.insert(candidate.toCaseFolded());
		changed = true;
	}

	if (changed) this->saveReservations();

	// Session-local screens never reserve a persistent name. They avoid every
	// reserved name, including reservations for disconnected monitors, so a
	// reconnect cannot collide with a persistent screen.
	this->assignSessionNames(identities);
}

void ScreenIdentityRegistry::assignSessionNames(QList<DisplayIdentity>& identities) {
	QSet<QString> used;
	for (const auto& reservation: std::as_const(this->reservations))
		used.insert(reservation.name.toCaseFolded());
	for (const auto& name: std::as_const(this->sessionNames)) used.insert(name.toCaseFolded());

	QList<qsizetype> sessions;
	for (qsizetype index = 0; index < identities.size(); ++index)
		if (!identities[index].persistent) sessions.append(index);
	std::sort(sessions.begin(), sessions.end(), [&identities](qsizetype left, qsizetype right) {
		return identities[left].stableId.compare(identities[right].stableId, Qt::CaseInsensitive) < 0;
	});
	for (const auto index: sessions) {
		auto& identity = identities[index];
		auto existing = this->sessionNames.constFind(identity.stableId);
		if (existing != this->sessionNames.cend()) {
			identity.uniqueName = existing.value();
			continue;
		}
		identity.uniqueName = this->allocateName(identity.friendlyName, used);
		used.insert(identity.uniqueName.toCaseFolded());
		this->sessionNames.insert(identity.stableId, identity.uniqueName);
	}
}

QString ScreenIdentityRegistry::allocateName(const QString& label, QSet<QString>& used) {
	auto base = label;
	if (base.isEmpty()) base = QStringLiteral("Display");
	auto candidate = base;
	if (!used.contains(candidate.toCaseFolded())) return candidate;
	for (auto suffix = 2;; ++suffix) {
		candidate = QStringLiteral("%1 (%2)").arg(base).arg(suffix);
		if (!used.contains(candidate.toCaseFolded())) return candidate;
	}
}

bool ScreenIdentityRegistry::hasLegacyLayer(const QString& label) const {
	if (this->monitorConfigDir.isEmpty() || label.isEmpty()) return false;
	return QFileInfo(this->monitorConfigDir + QLatin1Char('/') + label).isDir();
}

ScreenIdentityRegistry::FileState
ScreenIdentityRegistry::readReservationFile(QJsonObject& reservations) const {
	QFile file(this->reservationsPath);
	if (!file.exists()) return FileState::Missing;
	if (!file.open(QIODevice::ReadOnly)) return FileState::Unreadable;
	QJsonParseError error;
	auto document = QJsonDocument::fromJson(file.readAll(), &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) return FileState::Unreadable;
	const auto value = document.object().value(QStringLiteral("reservations"));
	if (!value.isUndefined() && !value.isObject()) return FileState::Unreadable;
	reservations = value.toObject();
	return FileState::Ok;
}

void ScreenIdentityRegistry::loadReservations() {
	this->reservations.clear();
	this->reservationsWritable = true;
	if (this->reservationsPath.isEmpty()) return;

	QJsonObject stored;
	switch (this->readReservationFile(stored)) {
	case FileState::Missing: return;
	case FileState::Unreadable:
		// Whatever is in there is the user's. Keep it byte for byte and run on
		// in-memory names for this session.
		this->reservationsWritable = false;
		qCWarning(logScreenIdentity) << "Screen name reservations are unreadable; keeping names in "
		                                "memory only";
		return;
	case FileState::Ok: break;
	}

	auto migrated = false;
	for (auto it = stored.begin(); it != stored.end(); ++it) {
		auto key = storedKey(it.key());
		if (key.isEmpty()) continue;
		if (it.key().startsWith(QStringLiteral("path:"))) migrated = true;
		auto object = it.value().toObject();
		Reservation reservation;
		reservation.name = object.value(QStringLiteral("name")).toString();
		reservation.label = object.value(QStringLiteral("label")).toString();
		reservation.legacyLabel = object.value(QStringLiteral("legacyLabel")).toString();
		reservation.lastSeen = object.value(QStringLiteral("lastSeen")).toString();
		for (const auto& copied: object.value(QStringLiteral("legacyCopied")).toArray())
			reservation.legacyCopied.insert(copied.toString());
		// Keys iterate in sorted order, so a hashed entry replaces the raw
		// entry it was migrated from.
		if (!reservation.name.isEmpty()) this->reservations.insert(key, reservation);
	}
	// Rewrite raw interface paths as hashes right away.
	if (migrated) this->saveReservations();
}

void ScreenIdentityRegistry::saveReservations() {
	if (this->reservationsPath.isEmpty() || !this->reservationsWritable) return;

	// The plugin may have recorded a completed legacy copy while this process
	// was running. Merge those flags instead of overwriting them. If the file
	// has become unreadable meanwhile, leave it alone.
	QJsonObject current;
	if (this->readReservationFile(current) == FileState::Unreadable) {
		qCWarning(logScreenIdentity) << "Screen name reservations are unreadable; not saving";
		return;
	}
	auto copiedByReservation = QHash<QString, QSet<QString>>();
	for (auto it = current.begin(); it != current.end(); ++it) {
		auto key = storedKey(it.key());
		if (key.isEmpty()) continue;
		auto copied = QSet<QString>();
		for (const auto& value: it.value().toObject().value(QStringLiteral("legacyCopied")).toArray())
			copied.insert(value.toString());
		if (!copied.isEmpty()) copiedByReservation[key].unite(copied);
	}

	auto root = QJsonObject();
	root.insert(QStringLiteral("version"), 1);
	auto reservations = QJsonObject();
	for (auto it = this->reservations.cbegin(); it != this->reservations.cend(); ++it) {
		auto object = QJsonObject();
		object.insert(QStringLiteral("name"), it->name);
		object.insert(QStringLiteral("label"), it->label);
		if (!it->lastSeen.isEmpty()) object.insert(QStringLiteral("lastSeen"), it->lastSeen);
		if (!it->legacyLabel.isEmpty()) object.insert(QStringLiteral("legacyLabel"), it->legacyLabel);
		auto copied = it->legacyCopied;
		copied.unite(copiedByReservation.value(it.key()));
		if (!copied.isEmpty()) {
			auto array = QJsonArray();
			auto sorted = copied.values();
			std::sort(sorted.begin(), sorted.end());
			for (const auto& suffix: sorted) array.append(suffix);
			object.insert(QStringLiteral("legacyCopied"), array);
		}
		reservations.insert(it.key(), object);
	}
	root.insert(QStringLiteral("reservations"), reservations);

	QDir().mkpath(QFileInfo(this->reservationsPath).absolutePath());
	QSaveFile file(this->reservationsPath);
	if (!file.open(QIODevice::WriteOnly)) {
		qCWarning(logScreenIdentity) << "Cannot write screen name reservations";
		return;
	}
	file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	if (!file.commit()) qCWarning(logScreenIdentity) << "Cannot commit screen name reservations";
}

QScreen* ScreenIdentityRegistry::screenByName(const QString& name) const {
	return this->screensByName.value(name.toCaseFolded());
}

QScreen* ScreenIdentityRegistry::screenById(const QString& id) const {
	return this->screensById.value(id);
}

QString ScreenIdentityRegistry::unresolvedName(QScreen* screen) const {
	if (screen == nullptr) return QStringLiteral("Unresolved display");
	const auto existing = this->unresolvedNames.constFind(screen);
	if (existing != this->unresolvedNames.cend()) return existing.value();
	// Pending-screen queries and refreshes must use the same lifetime token.
	// Do not key this by a missing/reused GDI name or reuse a destroyed screen's id.
	auto name = QStringLiteral("Unresolved display %1").arg(++this->nextUnresolvedId);
	this->unresolvedNames.insert(screen, name);
	QObject::connect(screen, &QObject::destroyed, this, [this, screen]() {
		this->unresolvedNames.remove(screen);
	});
	return name;
}

DisplayIdentity ScreenIdentityRegistry::identity(QScreen* screen) const {
	const auto state = this->states.constFind(screen);
	if (state != this->states.cend()) return state->identity;
	auto device = screen ? this->deviceName(screen) : QString();
	auto name = this->unresolvedName(screen);
	auto id = device.isEmpty() ? QStringLiteral("session:pending:") + name
	                           : QStringLiteral("session:pending:") + sourceBase(device);
	return {device, {}, name, id, name, false};
}

QString displayName(const DisplayIdentity& identity) {
	return identity.uniqueName.isEmpty() ? identity.friendlyName : identity.uniqueName;
}

QString screenName(QScreen* screen) {
	if (screen == nullptr) return {};
	return displayName(ScreenIdentityRegistry::instance()->identity(screen));
}

QScreen* screenByName(const QString& name) {
	// Fixtures without a GUI must not construct the native registry.
	if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) return nullptr;
	return ScreenIdentityRegistry::instance()->screenByName(name);
}

} // namespace qs::windows

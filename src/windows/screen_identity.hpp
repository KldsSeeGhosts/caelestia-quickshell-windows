#pragma once

#include <functional>

#include <qabstractnativeeventfilter.h>
#include <qhash.h>
#include <qjsonobject.h>
#include <qlist.h>
#include <qobject.h>
#include <qrect.h>
#include <qscreen.h>
#include <qset.h>
#include <qstring.h>
#include <qtimer.h>

namespace qs::windows {

struct DisplayIdentity {
	QString deviceName;   // GDI source, e.g. \\.\DISPLAY1. Not a monitor identity.
	QString devicePath;   // DisplayConfig target/EnumDisplayDevices interface path.
	QString friendlyName; // Display label only. Never use it as a routing key.
	// Persistent per-target identity: the interface path supplied by either
	// DisplayConfig or EnumDisplayDevices. When neither supplies a path this is
	// an explicitly session-local id; it is never persisted or carried across a
	// reconnect. Opaque and internal; never expose or log it raw.
	QString stableId;
	// Unique, stable QML-facing name. Reserved per persistent stableId in the
	// profile's screen-names.json, so adding or removing an identical monitor
	// never renames an existing one.
	QString uniqueName;
	bool persistent = false;
	bool operator==(const DisplayIdentity&) const = default;
};

// Qt can reuse a QScreen/GDI source for another physical monitor without
// refreshing QScreen::name(). Never cache monitor metadata by GDI source alone.
class ScreenIdentityRegistry
    : public QObject
    , public QAbstractNativeEventFilter {
	Q_OBJECT

public:
	using Source = std::function<QList<DisplayIdentity>()>;
	using DeviceName = std::function<QString(QScreen*)>;

	static ScreenIdentityRegistry* instance();
	// reservationsPath empty disables persistence (used by fixtures). Production
	// uses the profile state dir and the profile monitor-config dir for legacy
	// layer metadata.
	explicit ScreenIdentityRegistry(
	    Source source,
	    DeviceName deviceName,
	    QObject* parent = nullptr,
	    QString reservationsPath = {},
	    QString monitorConfigDir = {}
	);
	~ScreenIdentityRegistry() override;

	[[nodiscard]] DisplayIdentity identity(QScreen* screen) const;
	// On-disk key of a persistent id: an unsalted SHA-256, so the file never
	// holds a raw interface path. Stable across runs, unlike the logging ids.
	[[nodiscard]] static QString reservationKey(const QString& persistentId);
	// Resolve a unique QML-facing name or an opaque stable id to the live
	// screen. These are lookups in the identity registry, never a scan of
	// display labels.
	[[nodiscard]] QScreen* screenByName(const QString& name) const;
	[[nodiscard]] QScreen* screenById(const QString& id) const;
	void refresh();
	bool nativeEventFilter(const QByteArray& type, void* message, qintptr* result) override;

signals:
	void identityChanged(QScreen* screen);
	// Identity, geometry or scale changed, including a reused QScreen.
	void screenChanged(QScreen* screen);
	// A screen left the topology. Session-local state keyed by its id must not
	// carry over to a replacement behind the same GDI source.
	void screenRemoved(QScreen* screen);
	// An id stops naming this screen: the screen left the topology, or it now has
	// another identity (a session-only screen that gained an interface path is a
	// removal followed by an addition, never a rename). Emitted while the id still
	// resolves to the live screen, so a consumer can undo native state on it (a
	// dimmed gamma ramp) and must then drop everything it keyed by this id,
	// including work still in flight.
	void identityRetired(const QString& id, QScreen* screen);

private:
	struct State {
		DisplayIdentity identity;
		quintptr monitor;
		QRect geometry;
		qreal dpr;
		qreal physicalDpi;
		qreal logicalDpi;
		bool operator==(const State&) const = default;
	};

	struct Reservation {
		QString name;
		QString label;
		QString legacyLabel;
		QSet<QString> legacyCopied;
		QString lastSeen; // ISO date; for diagnostics only, never used to decide anything.
	};

	struct SessionState {
		QString base;
		quint64 token = 0;
	};

	void watch(QScreen* screen);
	void scheduleRefresh();
	[[nodiscard]] QString unresolvedName(QScreen* screen) const;
	void loadReservations();
	void saveReservations();
	enum class FileState { Missing, Ok, Unreadable };
	// Reads the reservation file's "reservations" object. Unreadable means the
	// file exists but cannot be read or parsed: it is never overwritten.
	[[nodiscard]] FileState readReservationFile(QJsonObject& reservations) const;
	void assignReservedNames(QList<DisplayIdentity>& identities);
	void assignSessionNames(QList<DisplayIdentity>& identities);
	[[nodiscard]] QString allocateName(const QString& label, QSet<QString>& used);
	[[nodiscard]] bool hasLegacyLayer(const QString& label) const;
	Source source;
	DeviceName deviceName;
	QString reservationsPath;
	QString monitorConfigDir;
	// Keyed by reservationKey(stableId).
	QHash<QString, Reservation> reservations;
	// False after an unreadable reservation file: names then live in memory for
	// this session only and the user's file stays untouched.
	bool reservationsWritable = true;
	// Names of session-local ids, kept for as long as each session id survives.
	QHash<QString, QString> sessionNames;
	QHash<QScreen*, State> states;
	QHash<QScreen*, SessionState> sessions;
	QHash<QString, QScreen*> screensById;
	QHash<QString, QScreen*> screensByName;
	QHash<QString, QString> namesByTargetPath;
	mutable QHash<QScreen*, QString> unresolvedNames;
	mutable quint64 nextUnresolvedId = 0;
	quint64 nextSessionToken = 0;
	QTimer settleTimer;
	bool refreshPending = false;
};

// The QML-facing name of an identity: the unique name, or the label while the
// registry has not reserved one. QuickshellScreenInfo::name() is this value.
[[nodiscard]] QString displayName(const DisplayIdentity& identity);
// Unique QML-facing name (label plus a collision suffix when needed). Use it
// for config selection and display, never for native routing.
[[nodiscard]] QString screenName(QScreen* screen);
// Resolve a QML-facing name to the live screen without comparing labels.
[[nodiscard]] QScreen* screenByName(const QString& name);

} // namespace qs::windows

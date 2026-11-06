#pragma once

#include <functional>
#include <optional>

#include <qlist.h>
#include <qobject.h>
#include <qset.h>
#include <qstring.h>
#include <qt_windows.h>
#include <qtclasshelpermacros.h>
#include <qthread.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include "accessor_state.hpp"
#include "desktop_contract.hpp"

struct IVirtualDesktopManager;

namespace qs::windows {

class RegistryWatcher: public QThread {
	Q_OBJECT;

public:
	explicit RegistryWatcher(QList<QString> subkeys, QObject* parent = nullptr);
	~RegistryWatcher() override;
	Q_DISABLE_COPY_MOVE(RegistryWatcher);

	void stop();

signals:
	void changed();

protected:
	void run() override;

private:
	QList<QString> subkeys;
	HANDLE stopEvent = nullptr;
};

class VirtualDesktops: public AccessorState {
	Q_OBJECT;
	Q_PROPERTY(QVariantMap capability READ capability NOTIFY capabilityChanged);
	Q_PROPERTY(QVariantMap lastCommand READ lastCommand NOTIFY lastCommandChanged);

public:
	struct Desktop {
		GUID id {};
		QString name;
	};

	// The accessor table is public so tests can inject a fake without loading a
	// DLL. The in-process COM provider uses the same table. Every desktop-changing
	// operation requires accessorLoaded(); there is no
	// synthetic-input fallback.
	struct Accessor {
		HMODULE module = nullptr;
		int (*getCurrentDesktopNumber)() = nullptr;
		int (*getDesktopCount)() = nullptr;
		GUID (*getDesktopIdByNumber)(int) = nullptr;
		int (*getWindowDesktopNumber)(HWND) = nullptr;
		int (*goToDesktopNumber)(int) = nullptr;
		int (*moveWindowToDesktopNumber)(HWND, int) = nullptr;
		int (*isWindowOnCurrentVirtualDesktop)(HWND) = nullptr;
		int (*createDesktop)() = nullptr;
		int (*removeDesktop)(int, int) = nullptr;
		int (*registerPostMessageHook)(HWND, UINT) = nullptr;
		int (*unregisterPostMessageHook)(HWND) = nullptr;
		int (*isPinnedWindow)(HWND) = nullptr;
		int (*pinWindow)(HWND) = nullptr;
		int (*unPinWindow)(HWND) = nullptr;
		void (*restart)() = nullptr;
	};

	// Injection point for tests. Production never installs a sender: desktop
	// control must not synthesize keyboard chords.
	using ChordSender = std::function<void(WORD key, int times)>;

	static VirtualDesktops* instance();

	[[nodiscard]] const QList<Desktop>& desktops() const { return this->mDesktops; }
	[[nodiscard]] qsizetype count() const { return this->mDesktops.length(); }
	[[nodiscard]] qsizetype currentIndex() const { return this->mCurrent; }
	[[nodiscard]] qsizetype indexOf(const GUID& id) const;

	[[nodiscard]] GUID windowDesktopId(HWND hwnd) const;
	[[nodiscard]] qsizetype windowDesktopIndex(HWND hwnd) const;
	[[nodiscard]] bool isWindowOnCurrent(HWND hwnd) const;

	[[nodiscard]] int workspaceLimit() const { return this->mWorkspaceLimit; }
	void setWorkspaceLimit(int count) { this->mWorkspaceLimit = qBound(1, count, 20); }
	void retryAccessor();
	bool switchTo(qsizetype index);
	void ensureCount(qsizetype count, std::function<void(bool)> done);
	bool removeDesktop(qsizetype index, qsizetype fallback);
	bool moveWindow(HWND hwnd, qsizetype index);
	bool pinWindow(HWND hwnd, bool pinned);
	[[nodiscard]] bool isWindowPinned(HWND hwnd) const;
	[[nodiscard]] DesktopAvailability availability() const;
	[[nodiscard]] bool supports(DesktopOperation operation) const;
	[[nodiscard]] QVariantMap capability() const;
	[[nodiscard]] QVariantMap lastCommand() const { return desktopCommandMap(this->mCommandResult); }
	void refuseCommand(const QString& operation, const QString& reason);
	// Shared ERM-607 result surface for synchronous compatibility dispatch too.
	void publishCommand(DesktopCommandResult result);

	void refresh(bool rereadNames = false);
	void noteCloakChange();

	static QString guidToString(const GUID& guid);

#ifdef QS_TEST
	// Seed without registry/DLL access. Installation uses the same AccessorState
	// export checks and deferred notifications as production and tiling fakes.
	struct TestHooks {
		Accessor accessor;
		ChordSender chordSender;
		QList<Desktop> desktops;
		qsizetype current = 0;
		DesktopAvailability availability = DesktopAvailability::MissingProvider;
	};

	static VirtualDesktops* createForTest(TestHooks hooks);
	void installAccessorForTest(Accessor accessor);
	void loseAccessorForTest();
	void destroyForTest();
#endif

signals:
	void desktopsChanged();
	void currentChanged();
	void accessorReady();
	void capabilityChanged();
	void lastCommandChanged();
	void commandResult(QVariantMap result);

private:
	explicit VirtualDesktops();
#ifdef QS_TEST
	explicit VirtualDesktops(TestHooks hooks);
#endif
	~VirtualDesktops() override;
	Q_DISABLE_COPY_MOVE(VirtualDesktops);

	[[nodiscard]] IVirtualDesktopManager* manager() const;
	void loadAccessor();
	void installListener();
	bool readRegistry(QList<GUID>& ids, GUID& current) const;
	[[nodiscard]] QString readDesktopName(const GUID& id) const;
	void createNext();
	void observeCreation();
	void finishCreation(
	    DesktopCommandOutcome failure = DesktopCommandOutcome::Denied,
	    const QString& reason = QStringLiteral("command-rejected")
	);
	// Unreachable by design: desktop control never synthesizes input. Kept so a
	// test can inject a recording sender and fail if a future path sends a
	// chord (or if one is reintroduced through this hook).
	void sendShortcut(WORD key, int times) const;
	void updateAccessorExports();
	void warnAccessorOnly(const QString& operation) const;
	void initCommandObservation();
	void observeSwitch();
	void finishSwitch(DesktopCommandOutcome outcome, const QString& reason = {});
	[[nodiscard]] QString commandFailureReason() const;

	template <typename R, typename... P, typename... A>
	R call(R fallback, R (*fn)(P...), A... args) const;
	void accessorFailed() const;

	static LRESULT CALLBACK listenerProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

	QList<Desktop> mDesktops;
	qsizetype mCurrent = -1;
	GUID mCurrentId {};
	DWORD sessionId = 0;

	mutable IVirtualDesktopManager* mManager = nullptr;
	mutable bool managerTried = false;
	RegistryWatcher* watcher = nullptr;
	HWND listener = nullptr;
	QTimer cloakTimer;
	QTimer creationTimer;
	qsizetype creationTarget = 0;
	qsizetype creationBefore = 0;
	struct CreationWaiter {
		qsizetype count;
		quint64 id;
		std::function<void(bool)> done;
	};
	QList<CreationWaiter> creationWaiters;

	mutable ULONGLONG accessorRestartedAt = 0;

	mutable QSet<QString> warnedOperations;
	ChordSender chordSender;
	bool testMode = false;
	mutable Accessor accessor;
	bool nativeAccessor = false;
	int mWorkspaceLimit = 5;
	DesktopAvailability testAvailability = DesktopAvailability::MissingProvider;
	DesktopCommandResult mCommandResult;
	struct PendingSwitch {
		quint64 id;
		qsizetype index;
		GUID desktop;
	};
	std::optional<PendingSwitch> pendingSwitch;
	QTimer switchDeadline;
	quint64 nextRequest = 0;
};

} // namespace qs::windows

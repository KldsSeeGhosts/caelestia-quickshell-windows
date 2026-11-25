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
	// provider. The in-process COM provider uses the same table. Every
	// private desktop-changing operation requires accessorLoaded(); public
	// own-window moves remain independent. There is no synthetic-input fallback.
	struct Accessor {
		int (*getCurrentDesktopNumber)() = nullptr;
		int (*getDesktopCount)() = nullptr;
		GUID (*getDesktopIdByNumber)(int) = nullptr;
		int (*getWindowDesktopNumber)(HWND) = nullptr;
		int (*goToDesktopNumber)(int) = nullptr;
		int (*moveWindowToDesktopNumber)(HWND, int) = nullptr;
		int (*isWindowOnCurrentVirtualDesktop)(HWND) = nullptr;
		int (*createDesktop)() = nullptr;
		int (*removeDesktop)(int, int) = nullptr;
		int (*isPinnedWindow)(HWND) = nullptr;
		int (*pinWindow)(HWND) = nullptr;
		int (*unPinWindow)(HWND) = nullptr;
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
	[[nodiscard]] quint64 switchOrder() const { return this->mSwitchOrder; }
	[[nodiscard]] quint64 workspaceIntentOrder() const { return this->mWorkspaceIntentOrder; }
	[[nodiscard]] quint64 beginWorkspaceIntent() { return ++this->mWorkspaceIntentOrder; }
	[[nodiscard]] bool isDesktopCurrentlyActive(qsizetype index, const GUID& targetGuid) const;
	bool switchTo(
	    qsizetype index,
	    const std::function<bool()>& shouldProceed = {},
	    std::function<void(qsizetype)> onObserved = {},
	    std::optional<quint64> expectedIntent = std::nullopt,
	    std::optional<GUID> targetDesktopId = std::nullopt,
	    std::optional<GUID> expectedOriginDesktopId = std::nullopt
	);
	void ensureCount(qsizetype count, std::function<void(bool)> done);
	void ensureCountWithReason(qsizetype count, std::function<void(bool, const QString&)> done);
	bool removeDesktop(qsizetype index, qsizetype fallback);
	bool moveWindow(
	    HWND hwnd,
	    qsizetype index,
	    quint64* pendingSequence = nullptr,
	    QString* failureReason = nullptr,
	    const std::function<void(quint64)>& onPendingSequence = {},
	    const std::function<bool()>& shouldProceed = {},
	    GUID* targetDesktopId = nullptr,
	    std::optional<GUID> requestedTargetId = std::nullopt
	);
	bool pinWindow(HWND hwnd, bool pinned);
	[[nodiscard]] std::optional<bool> isWindowPinned(HWND hwnd) const;
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
	// Seed without registry or COM access. Installation uses the same AccessorState
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
	void windowDesktopsChanged();
	void accessorReady();
	void capabilityChanged();
	void lastCommandChanged();
	void commandResult(QVariantMap result);
	void moveWindowCompleted(quint64 sequence, quintptr hwnd, bool succeeded, QString reason);

private:
#ifdef QS_TEST
	friend struct NativeDesktopFacadeTestHook;
#endif
	explicit VirtualDesktops();
#ifdef QS_TEST
	explicit VirtualDesktops(TestHooks hooks);
#endif
	~VirtualDesktops() override;
	Q_DISABLE_COPY_MOVE(VirtualDesktops);

	void loadAccessor();
	void bindNativeDesktopQueries();
	void onNativeMoveCompleted(
	    quint64 sequence,
	    HWND hwnd,
	    bool succeeded,
	    DesktopAvailability availability,
	    HRESULT error
	);
	void applyNativeSnapshot();
	void notifyCapability();
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
	[[nodiscard]] static QString failureReasonForError(DesktopAvailability availability, HRESULT error);
	[[nodiscard]] QString commandFailureReason() const;

	template <typename R, typename... P, typename... A>
	R call(R fallback, R (*fn)(P...), A... args) const;
	void accessorFailed() const;

	QList<Desktop> mDesktops;
	qsizetype mCurrent = -1;
	GUID mCurrentId {};
	DWORD sessionId = 0;

	bool applyingNativeSnapshot = false;
	quint64 appliedWindowRevision = 0;
	RegistryWatcher* watcher = nullptr;
	QTimer cloakTimer;
	QTimer creationTimer;
	qsizetype creationTarget = 0;
	qsizetype creationBefore = 0;
	struct CreationWaiter {
		qsizetype count;
		quint64 id;
		std::function<void(bool, const QString&)> done;
	};
	QList<CreationWaiter> creationWaiters;

	mutable QSet<QString> warnedOperations;
	ChordSender chordSender;
	bool testMode = false;
	mutable Accessor accessor;
	bool nativeAccessor = false;
	int mWorkspaceLimit = 5;
	DesktopAvailability testAvailability = DesktopAvailability::MissingProvider;
	DesktopCommandResult mCommandResult;
	QVariantMap emittedCapability;
	struct PendingSwitch {
		quint64 id;
		qsizetype index;
		GUID desktop;
		quint64 intentOrder = 0;
		std::function<void(qsizetype)> onObserved;
		quint64 minObservationSequence = 0;
		std::optional<GUID> expectedOrigin;
	};
	std::optional<PendingSwitch> pendingSwitch;
	QTimer switchDeadline;
	quint64 nextRequest = 0;
	quint64 mSwitchOrder = 0;
	quint64 mWorkspaceIntentOrder = 0;
	quint64 mLastNativeSwitchSequence = 0;
	bool mHasUnobservedNativeSwitch = false;
};

} // namespace qs::windows

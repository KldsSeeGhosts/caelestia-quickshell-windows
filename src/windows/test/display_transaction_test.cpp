#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

#include <qbytearray.h>
#include <qsignalspy.h>
#include <qtest.h>

#include "../system/displays.hpp"

using namespace std::chrono_literals;
using namespace qs::windows::sys;

namespace qs::windows::sys {
struct DisplaysTestAccess {
	static std::unique_ptr<Displays> facade(const std::shared_ptr<DisplayTransaction>& owner) {
		return std::unique_ptr<Displays>(new Displays(owner));
	}
	static bool apply(Displays& facade, DisplayTopology topology) {
		return facade.applyLayoutChange(std::move(topology));
	}
	static bool unconfirmedJob(Displays& facade, DisplayTransaction::Job job) {
		return facade.runJob(std::move(job), false);
	}
	static void sync(Displays& facade) { facade.syncTransaction(); }
	static QTimer& timer(Displays& facade) { return facade.countdownTimer; }
};
} // namespace qs::windows::sys

namespace {
// Gates bound every wait. No scheduler sleep, display enumeration, window,
// device, topology, HDR, mode or power API is invoked by these fixtures.
struct Gate {
	std::mutex mutex;
	std::condition_variable changed;
	bool closed = false;
	int entries = 0;
	void arm() {
		std::lock_guard lock(this->mutex);
		this->closed = true;
	}
	void open() {
		std::lock_guard lock(this->mutex);
		this->closed = false;
		this->changed.notify_all();
	}
	bool enter() {
		std::unique_lock lock(this->mutex);
		++this->entries;
		this->changed.notify_all();
		return this->changed.wait_for(lock, 5s, [this] { return !this->closed; });
	}
	bool reached(int count) {
		std::unique_lock lock(this->mutex);
		return this->changed.wait_for(lock, 3s, [this, count] { return this->entries >= count; });
	}
};

DisplayTopology layout(int marker) {
	DisplayTopology topology;
	for (UINT32 id: {1u, 2u}) {
		DISPLAYCONFIG_PATH_INFO path {};
		path.flags = DISPLAYCONFIG_PATH_ACTIVE;
		path.sourceInfo.id = id;
		path.sourceInfo.modeInfoIdx = id - 1;
		path.targetInfo.id = id + 10;
		path.targetInfo.targetAvailable = TRUE;
		path.targetInfo.rotation = DISPLAYCONFIG_ROTATION_IDENTITY;
		path.targetInfo.scaling = DISPLAYCONFIG_SCALING_IDENTITY;
		path.targetInfo.refreshRate = {60, 1};
		path.targetInfo.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
		topology.paths.push_back(path);
		DISPLAYCONFIG_MODE_INFO mode {};
		mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
		mode.id = id;
		mode.sourceMode.width = 1920;
		mode.sourceMode.height = 1080;
		mode.sourceMode.position.x = marker + static_cast<int>(id - 1) * 1920;
		topology.modes.push_back(mode);
	}
	for (auto& path: topology.paths) {
		path.targetInfo.modeInfoIdx = static_cast<UINT32>(topology.modes.size());
		DISPLAYCONFIG_MODE_INFO mode {};
		mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
		mode.id = path.targetInfo.id;
		auto& signal = mode.targetMode.targetVideoSignalInfo;
		signal.pixelRate = 148500000;
		signal.activeSize = {1920, 1080};
		signal.totalSize = {2200, 1125};
		signal.vSyncFreq = {60, 1};
		signal.hSyncFreq = {67500, 1};
		signal.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
		topology.modes.push_back(mode);
	}
	return topology;
}

QByteArray bytes(const DisplayTopology& topology) {
	QByteArray result(
	    reinterpret_cast<const char*>(topology.paths.data()),
	    static_cast<qsizetype>(topology.paths.size() * sizeof(DISPLAYCONFIG_PATH_INFO))
	);
	result.append(
	    reinterpret_cast<const char*>(topology.modes.data()),
	    static_cast<qsizetype>(topology.modes.size() * sizeof(DISPLAYCONFIG_MODE_INFO))
	);
	return result;
}

struct Native {
	Gate snapshotGate;
	Gate applyGate;
	Gate saveGate;
	Gate restoreGate;
	Gate beforeApplyGate;
	Gate deadlineGate;
	std::mutex mutex;
	DisplayTopology current = layout(0);
	DisplayTopology saved = current;
	DisplayTopology lastRestore;
	std::atomic<int> snapshots = 0;
	std::atomic<int> applies = 0;
	std::atomic<int> saves = 0;
	std::atomic<int> restores = 0;
	std::atomic<bool> snapshotFails = false;
	std::atomic<bool> applyFails = false;
	std::atomic<bool> saveFails = false;
	std::atomic<bool> restoreFails = false;
	std::atomic<bool> restoreDropsMonitor = false;
	std::atomic<bool> restoreObservationFails = false;

	DisplayTransaction::Operations operations(const std::shared_ptr<Native>& self) {
		return {
		    [self](DisplayTopology& out) {
			    ++self->snapshots;
			    if (!self->snapshotGate.enter() || self->snapshotFails) return false;
			    std::lock_guard lock(self->mutex);
			    out = self->current;
			    return true;
		    },
		    [self](DisplayTopology topology) -> QString {
			    ++self->applies;
			    if (!self->applyGate.enter()) return QStringLiteral("apply gate timed out");
			    std::lock_guard lock(self->mutex);
			    // Even an API failure can have changed native state.
			    self->current = std::move(topology);
			    return self->applyFails ? QStringLiteral("apply failed") : QString();
		    },
		    [self](DisplayTopology topology) -> QString {
			    ++self->restores;
			    if (!self->restoreGate.enter()) return QStringLiteral("restore gate timed out");
			    return restoreDisplayTopology(
			        std::move(topology),
			        [self](DisplayTopology& supplied) -> QString {
				        std::lock_guard lock(self->mutex);
				        self->lastRestore = supplied;
				        if (self->restoreFails) return QStringLiteral("original monitor unavailable");
				        self->current = supplied;
				        self->saved = supplied;
				        if (self->restoreDropsMonitor) self->current.paths.pop_back();
				        return {};
			        },
			        [self](DisplayTopology& observed) {
				        std::lock_guard lock(self->mutex);
				        observed = self->current;
				        return !self->restoreObservationFails;
			        }
			    );
		    },
		    [self]() -> QString {
			    ++self->saves;
			    if (!self->saveGate.enter()) return QStringLiteral("save gate timed out");
			    std::lock_guard lock(self->mutex);
			    if (self->saveFails) return QStringLiteral("save denied");
			    self->saved = self->current;
			    return {};
		    },
		    [self] { self->beforeApplyGate.enter(); },
		    [self] { self->deadlineGate.enter(); },
		};
	}
	QByteArray currentBytes() {
		std::lock_guard lock(this->mutex);
		return bytes(this->current);
	}
	QByteArray restoreBytes() {
		std::lock_guard lock(this->mutex);
		return bytes(this->lastRestore);
	}
	QByteArray savedBytes() {
		std::lock_guard lock(this->mutex);
		return bytes(this->saved);
	}
};

struct Fixture {
	std::shared_ptr<Native> native = std::make_shared<Native>();
	std::shared_ptr<DisplayTransaction> owner;
	explicit Fixture(DisplayTransaction::Clock::duration timeout = 15s)
	    : owner(DisplayTransaction::create(native->operations(native), timeout)) {}
	~Fixture() {
		this->native->snapshotGate.open();
		this->native->applyGate.open();
		this->native->saveGate.open();
		this->native->restoreGate.open();
		this->native->beforeApplyGate.open();
		this->native->deadlineGate.open();
		if (this->owner) this->owner->shutdown(1s);
	}
};
} // namespace

class DisplayTransactionTest: public QObject {
	Q_OBJECT
private slots:
	void initialApplyOutlivesFacade() {
		Fixture f;
		auto facade = DisplaysTestAccess::facade(f.owner);
		f.native->applyGate.arm();
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
		QVERIFY(f.native->applyGate.reached(1));
		facade.reset();
		QCOMPARE(f.native->restores.load(), 0);
		f.native->applyGate.open();
		// Completion/recovery must run even while the GUI event loop is blocked.
		QVERIFY(f.native->restoreGate.reached(1));
		QTRY_VERIFY(!f.owner->state().busy);
		QVERIFY(!f.owner->state().pending);
		QCOMPARE(f.native->applies.load(), 1);
		QCOMPARE(f.native->restores.load(), 1);
		QCOMPARE(f.native->currentBytes(), bytes(layout(0)));
	}

	void workerRetainsOwnerAfterLastExternalReference() {
		Fixture f;
		std::weak_ptr<DisplayTransaction> weak = f.owner;
		auto facade = DisplaysTestAccess::facade(f.owner);
		f.native->applyGate.arm();
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
		QVERIFY(f.native->applyGate.reached(1));
		facade.reset();
		f.owner.reset();
		QVERIFY(!weak.expired());
		f.native->applyGate.open();
		QVERIFY(f.native->restoreGate.reached(1));
		f.owner = weak.lock();
		QVERIFY(f.owner);
		QTRY_VERIFY(!f.owner->state().busy);
		QCOMPARE(f.native->restores.load(), 1);
		QCOMPARE(f.native->currentBytes(), bytes(layout(0)));
	}

	void replacementFacade_data() {
		QTest::addColumn<bool>("newFirst");
		QTest::newRow("destroy-then-recreate") << false;
		QTest::newRow("replace-then-retire") << true;
	}
	void replacementFacade() {
		QFETCH(bool, newFirst);
		Fixture f;
		auto oldFacade = DisplaysTestAccess::facade(f.owner);
		f.native->applyGate.arm();
		QVERIFY(DisplaysTestAccess::apply(*oldFacade, layout(100)));
		QVERIFY(f.native->applyGate.reached(1));
		std::unique_ptr<Displays> replacement;
		if (newFirst) {
			replacement = DisplaysTestAccess::facade(f.owner);
			oldFacade->keepChanges();
			oldFacade->revertChanges();
			QVERIFY(!DisplaysTestAccess::apply(*oldFacade, layout(200)));
		}
		oldFacade.reset();
		if (!newFirst) replacement = DisplaysTestAccess::facade(f.owner);
		QVERIFY(!DisplaysTestAccess::apply(*replacement, layout(200)));
		f.native->applyGate.open();
		QTRY_VERIFY(!f.owner->state().busy);
		QCOMPARE(f.native->restores.load(), 1);
		QCOMPARE(f.native->saves.load(), 0);
		// A stale release must not end the replacement's next transaction.
		QVERIFY(DisplaysTestAccess::apply(*replacement, layout(300)));
		QTRY_VERIFY(!f.owner->state().busy);
		replacement->keepChanges();
		QTRY_VERIFY(!f.owner->state().pending);
		replacement.reset();
		QCOMPARE(f.native->restores.load(), 1);
	}

	void secondApplyPreservesBaselineAndDeadline_data() {
		QTest::addColumn<bool>("releaseDuringApply");
		QTest::newRow("release-during-second-apply") << true;
		QTest::newRow("release-after-second-apply") << false;
	}
	void secondApplyPreservesBaselineAndDeadline() {
		QFETCH(bool, releaseDuringApply);
		Fixture f(3s);
		auto facade = DisplaysTestAccess::facade(f.owner);
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
		QTRY_VERIFY(!f.owner->state().busy);
		const auto deadline = f.owner->state().deadline;
		f.native->applyGate.arm();
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(200)));
		QVERIFY(f.native->applyGate.reached(2));
		QCOMPARE(f.native->snapshots.load(), 1);
		QVERIFY(f.owner->state().deadline == deadline);
		if (releaseDuringApply) facade.reset();
		f.native->applyGate.open();
		QTRY_VERIFY(!f.owner->state().busy);
		if (!releaseDuringApply) {
			QVERIFY(f.owner->state().deadline == deadline);
			facade.reset();
			QVERIFY(f.native->restoreGate.reached(1));
			QTRY_VERIFY(!f.owner->state().busy);
		}
		QCOMPARE(f.native->restoreBytes(), bytes(layout(0)));
		QCOMPARE(f.native->restores.load(), 1);
	}

	void facadeTeardownAfterApplyRestoresOnce() {
		Fixture f;
		auto facade = DisplaysTestAccess::facade(f.owner);
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
		QTRY_VERIFY(!f.owner->state().busy);
		QVERIFY(f.owner->state().pending);
		facade.reset();
		QVERIFY(f.native->restoreGate.reached(1));
		QTRY_VERIFY(!f.owner->state().busy);
		QVERIFY(!f.owner->state().pending);
		QCOMPARE(f.native->restores.load(), 1);
		QCOMPARE(f.native->currentBytes(), bytes(layout(0)));
	}

	void timeoutDuringSecondApply() {
		Fixture f(300ms);
		auto facade = DisplaysTestAccess::facade(f.owner);
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
		QTRY_VERIFY_WITH_TIMEOUT(!f.owner->state().busy, 1000);
		f.native->applyGate.arm();
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(200)));
		QVERIFY(f.native->applyGate.reached(2));
		// Wait on the real monotonic deadline, not a test-only clock branch.
		QTRY_COMPARE_WITH_TIMEOUT(f.owner->state().secondsLeft, 0, 1000);
		f.native->applyGate.open();
		QTRY_VERIFY(!f.owner->state().busy);
		QVERIFY(!f.owner->state().pending);
		facade.reset();
		QCOMPARE(f.native->restores.load(), 1);
		QCOMPARE(f.native->restoreBytes(), bytes(layout(0)));
	}

	void successfulKeepDoesNotRestore() {
		Fixture f;
		auto facade = DisplaysTestAccess::facade(f.owner);
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
		QTRY_VERIFY(!f.owner->state().busy);
		facade->keepChanges();
		facade->keepChanges();
		QTRY_VERIFY(!f.owner->state().busy);
		QVERIFY(!f.owner->state().pending);
		facade->revertChanges();
		facade.reset();
		QVERIFY(f.owner->shutdown());
		QCOMPARE(f.native->saves.load(), 1);
		QCOMPARE(f.native->restores.load(), 0);
		QCOMPARE(f.native->currentBytes(), bytes(layout(100)));
	}

	void failedSaveRetainsRecovery_data() {
		QTest::addColumn<bool>("releaseDuringSave");
		QTest::newRow("retry-save") << false;
		QTest::newRow("lease-ends-during-save") << true;
	}
	void failedSaveRetainsRecovery() {
		QFETCH(bool, releaseDuringSave);
		Fixture f;
		auto facade = DisplaysTestAccess::facade(f.owner);
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
		QTRY_VERIFY(!f.owner->state().busy);
		f.native->saveFails = true;
		f.native->saveGate.arm();
		facade->keepChanges();
		QVERIFY(f.native->saveGate.reached(1));
		facade->keepChanges();
		QCOMPARE(f.native->saves.load(), 1);
		QVERIFY(f.owner->state().pending);
		if (releaseDuringSave) facade.reset();
		f.native->saveGate.open();
		QTRY_VERIFY(!f.owner->state().busy);
		if (releaseDuringSave) {
			QVERIFY(!f.owner->state().pending);
			QCOMPARE(f.native->restores.load(), 1);
			QCOMPARE(f.native->restoreBytes(), bytes(layout(0)));
		} else {
			QVERIFY(f.owner->state().pending);
			DisplaysTestAccess::sync(*facade);
			QCOMPARE(facade->lastError(), QStringLiteral("save denied"));
			QVERIFY(facade->pendingConfirmation());
			f.native->saveFails = false;
			facade->keepChanges();
			QTRY_VERIFY(!f.owner->state().busy);
			QVERIFY(!f.owner->state().pending);
			QCOMPARE(f.native->saves.load(), 2);
			QCOMPARE(f.native->restores.load(), 0);
		}
	}

	void failedSavePreservesOriginalTimeout() {
		Fixture f(300ms);
		auto facade = DisplaysTestAccess::facade(f.owner);
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
		QTRY_VERIFY(!f.owner->state().busy);
		const auto deadline = f.owner->state().deadline;
		f.native->saveFails = true;
		f.native->restoreGate.arm();
		facade->keepChanges();
		QTRY_VERIFY(!f.owner->state().busy);
		QVERIFY(f.owner->state().pending);
		QVERIFY(f.owner->state().deadline == deadline);
		QVERIFY(f.native->restoreGate.reached(1));
		f.native->restoreGate.open();
		QTRY_VERIFY(!f.owner->state().busy);
		QVERIFY(!f.owner->state().pending);
		QCOMPARE(f.native->saves.load(), 1);
		QCOMPARE(f.native->restores.load(), 1);
	}

	void leaseEndsDuringSuccessfulSave() {
		Fixture f;
		auto facade = DisplaysTestAccess::facade(f.owner);
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
		QTRY_VERIFY(!f.owner->state().busy);
		f.native->saveGate.arm();
		facade->keepChanges();
		QVERIFY(f.native->saveGate.reached(1));
		facade.reset();
		f.native->saveGate.open();
		QTRY_VERIFY(!f.owner->state().busy);
		QVERIFY(!f.owner->state().pending);
		QCOMPARE(f.native->saves.load(), 1);
		QCOMPARE(f.native->restores.load(), 0);
		QCOMPARE(f.native->currentBytes(), bytes(layout(100)));
		QCOMPARE(f.native->savedBytes(), bytes(layout(100)));
	}

	void slowSuccessfulKeepDoesNotWriteRollback() {
		Fixture f(300ms);
		auto facade = DisplaysTestAccess::facade(f.owner);
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
		QTRY_VERIFY(!f.owner->state().busy);
		const auto originalDeadline = f.owner->state().deadline;
		QVERIFY(originalDeadline.has_value());
		f.native->saveGate.arm();
		facade->keepChanges();
		QVERIFY(f.native->saveGate.reached(1));
		QTRY_VERIFY_WITH_TIMEOUT(DisplayTransaction::Clock::now() >= *originalDeadline, 1000);
		f.native->saveGate.open();
		QTRY_VERIFY(!f.owner->state().busy);
		QVERIFY(!f.owner->state().pending);
		QCOMPARE(f.native->saves.load(), 1);
		QCOMPARE(f.native->restores.load(), 0);
		QCOMPARE(f.native->currentBytes(), bytes(layout(100)));
	}

	void keepOrderingAcrossReloadAndExpiry_data() {
		QTest::addColumn<bool>("saveSucceeds");
		QTest::addColumn<bool>("originalExpires");
		QTest::addColumn<int>("reloadOrder");
		for (const bool success: {false, true}) {
			for (const bool expires: {false, true}) {
				for (const int reload: {0, 1, 2}) {
					const auto name = QStringLiteral("%1-%2-%3")
					                      .arg(success ? "save-ok" : "save-failed")
					                      .arg(expires ? "expired" : "before-deadline")
					                      .arg(
					                          reload == 0   ? "same-facade"
					                          : reload == 1 ? "replace-then-retire"
					                                        : "destroy-then-recreate"
					                      );
					QTest::newRow(qPrintable(name)) << success << expires << reload;
				}
			}
		}
	}
	void keepOrderingAcrossReloadAndExpiry() {
		QFETCH(bool, saveSucceeds);
		QFETCH(bool, originalExpires);
		QFETCH(int, reloadOrder);
		Fixture f(originalExpires ? 300ms : 15s);
		auto facade = DisplaysTestAccess::facade(f.owner);
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
		QTRY_VERIFY(!f.owner->state().busy);
		const auto originalDeadline = f.owner->state().deadline;
		QVERIFY(originalDeadline.has_value());
		f.native->saveFails = !saveSucceeds;
		f.native->saveGate.arm();
		facade->keepChanges();
		QVERIFY(f.native->saveGate.reached(1));
		QVERIFY(!f.owner->state().deadline.has_value());
		QVERIFY(f.owner->state().pending);

		if (reloadOrder == 1) {
			auto replacement = DisplaysTestAccess::facade(f.owner);
			facade.reset();
			facade = std::move(replacement);
		} else if (reloadOrder == 2) {
			facade.reset();
			facade = DisplaysTestAccess::facade(f.owner);
		}
		if (originalExpires)
			QTRY_VERIFY_WITH_TIMEOUT(DisplayTransaction::Clock::now() >= *originalDeadline, 1000);
		QCOMPARE(f.native->restores.load(), 0);
		facade->keepChanges(); // Busy repeat cannot start another save.
		QCOMPARE(f.native->saves.load(), 1);
		f.native->saveGate.open();
		QTRY_VERIFY(!f.owner->state().busy);
		const bool resumePreview = !saveSucceeds && !originalExpires && reloadOrder == 0;
		QCOMPARE(f.owner->state().pending, resumePreview);
		// Observe the actual replacement/same facade timer, not a manual sync.
		QTRY_COMPARE(facade->pendingConfirmation(), resumePreview);
		QTRY_VERIFY(!facade->busy());
		QCOMPARE(f.native->saves.load(), 1);
		QCOMPARE(f.native->snapshots.load(), 1);
		if (saveSucceeds) {
			QCOMPARE(f.native->restores.load(), 0);
			QCOMPARE(f.native->currentBytes(), bytes(layout(100)));
			QCOMPARE(f.native->savedBytes(), bytes(layout(100)));
		} else {
			if (resumePreview) {
				QCOMPARE(f.native->restores.load(), 0);
				QVERIFY(f.owner->state().deadline == originalDeadline);
				QCOMPARE(f.owner->state().error, QStringLiteral("save denied"));
				facade->revertChanges();
				QTRY_VERIFY(!f.owner->state().busy);
			}
			QCOMPARE(f.native->restores.load(), 1);
			QCOMPARE(f.native->restoreBytes(), bytes(layout(0)));
			QCOMPARE(f.native->currentBytes(), bytes(layout(0)));
		}
		facade.reset();
		QVERIFY(f.owner->shutdown());
		QCOMPARE(f.native->restores.load(), saveSucceeds ? 0 : 1);
	}

	void acceptedKeepDuringBoundedShutdown_data() {
		QTest::addColumn<bool>("saveSucceeds");
		QTest::newRow("save-success") << true;
		QTest::newRow("save-failure") << false;
	}
	void acceptedKeepDuringBoundedShutdown() {
		QFETCH(bool, saveSucceeds);
		Fixture f;
		auto facade = DisplaysTestAccess::facade(f.owner);
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
		QTRY_VERIFY(!f.owner->state().busy);
		f.native->saveFails = !saveSucceeds;
		f.native->saveGate.arm();
		facade->keepChanges();
		QVERIFY(f.native->saveGate.reached(1));
		const auto start = DisplayTransaction::Clock::now();
		QVERIFY(!f.owner->shutdown(20ms));
		QVERIFY(DisplayTransaction::Clock::now() - start < 500ms);
		facade.reset();
		f.native->saveGate.open();
		QTRY_VERIFY(!f.owner->state().busy);
		QVERIFY(f.owner->shutdown());
		QVERIFY(!f.owner->state().pending);
		QCOMPARE(f.native->saves.load(), 1);
		QCOMPARE(f.native->restores.load(), saveSucceeds ? 0 : 1);
		QCOMPARE(f.native->currentBytes(), bytes(layout(saveSucceeds ? 100 : 0)));
	}

	void facadeTimerObservesTimeoutAcrossWorkerGap() {
		Fixture f(500ms);
		auto facade = DisplaysTestAccess::facade(f.owner);
		auto& timer = DisplaysTestAccess::timer(*facade);
		timer.setInterval(10);
		QSignalSpy confirmation(facade.get(), &Displays::pendingConfirmationChanged);
		f.native->deadlineGate.arm();
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
		QTRY_VERIFY(facade->pendingConfirmation());
		QVERIFY(f.native->deadlineGate.reached(1));
		QVERIFY(!f.owner->state().busy);
		QCOMPARE(f.owner->state().secondsLeft, 0);
		QVERIFY(f.owner->state().deadline.has_value());
		QSignalSpy ticks(&timer, &QTimer::timeout);
		QTRY_VERIFY_WITH_TIMEOUT(ticks.count() > 0, 500);
		QVERIFY(timer.isActive());
		QVERIFY(facade->pendingConfirmation());
		facade->keepChanges(); // An already-expired Keep cannot be accepted.
		QCOMPARE(f.native->saves.load(), 0);
		f.native->deadlineGate.open();
		// No DisplaysTestAccess::sync: the real facade timer must publish completion.
		QTRY_VERIFY_WITH_TIMEOUT(!facade->pendingConfirmation(), 1000);
		QTRY_VERIFY(!facade->busy());
		QVERIFY(!timer.isActive());
		QCOMPARE(confirmation.count(), 2);
		QCOMPARE(f.native->restores.load(), 1);
		QCOMPARE(f.native->currentBytes(), bytes(layout(0)));
	}

	void firstApplyDoesNotPublishPrematureConfirmation() {
		Fixture f;
		auto facade = DisplaysTestAccess::facade(f.owner);
		auto& timer = DisplaysTestAccess::timer(*facade);
		timer.setInterval(10);
		QSignalSpy ticks(&timer, &QTimer::timeout);
		QSignalSpy confirmation(facade.get(), &Displays::pendingConfirmationChanged);
		f.native->applyGate.arm();
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
		QVERIFY(f.native->applyGate.reached(1));
		QVERIFY(facade->busy());
		QVERIFY(!facade->pendingConfirmation());
		QTRY_VERIFY(ticks.count() > 0);
		QVERIFY(!facade->pendingConfirmation());
		QCOMPARE(confirmation.count(), 0);
		f.native->applyGate.open();
		QTRY_VERIFY(facade->pendingConfirmation());
		QCOMPARE(confirmation.count(), 1);
	}

	void cancellationBeforeDispatchAvoidsUnneededWrites_data() {
		QTest::addColumn<bool>("alreadyPending");
		QTest::newRow("before-first-native-call") << false;
		QTest::newRow("before-second-native-call") << true;
	}
	void cancellationBeforeDispatchAvoidsUnneededWrites() {
		QFETCH(bool, alreadyPending);
		Fixture f;
		auto facade = DisplaysTestAccess::facade(f.owner);
		if (alreadyPending) {
			QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
			QTRY_VERIFY(!f.owner->state().busy);
		}
		f.native->beforeApplyGate.arm();
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(200)));
		QVERIFY(f.native->beforeApplyGate.reached(alreadyPending ? 2 : 1));
		facade.reset();
		f.native->beforeApplyGate.open();
		QTRY_VERIFY(!f.owner->state().busy);
		QVERIFY(!f.owner->state().pending);
		QCOMPARE(f.native->applies.load(), alreadyPending ? 1 : 0);
		QCOMPARE(f.native->restores.load(), alreadyPending ? 1 : 0);
		QCOMPARE(f.native->currentBytes(), bytes(layout(0)));
	}

	void failedRestoreRetainsExactBaseline() {
		Fixture f;
		auto facade = DisplaysTestAccess::facade(f.owner);
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
		QTRY_VERIFY(!f.owner->state().busy);
		f.native->restoreFails = true;
		f.native->restoreGate.arm();
		facade->revertChanges();
		QVERIFY(f.native->restoreGate.reached(1));
		facade.reset();
		auto replacement = DisplaysTestAccess::facade(f.owner);
		f.native->restoreGate.open();
		QTRY_VERIFY(!f.owner->state().busy);
		QVERIFY(f.owner->state().pending);
		DisplaysTestAccess::sync(*replacement);
		QVERIFY(replacement->pendingConfirmation());
		QCOMPARE(replacement->confirmSecondsLeft(), 0);
		QCOMPARE(replacement->lastError(), QStringLiteral("original monitor unavailable"));
		QCOMPARE(f.native->restores.load(), 1);
		QCOMPARE(f.native->currentBytes(), bytes(layout(100)));
		QVERIFY(!DisplaysTestAccess::apply(*replacement, layout(200)));
		QVERIFY(replacement->lastError().contains(QStringLiteral("revertChanges()")));
		QVERIFY(replacement->lastError().contains(QStringLiteral("unconfirmed")));
		replacement->keepChanges();
		QVERIFY(replacement->lastError().contains(QStringLiteral("revertChanges()")));
		QCOMPARE(f.owner->state().error, QStringLiteral("original monitor unavailable"));
		QCOMPARE(f.native->saves.load(), 0);
		// A monitor returning permits explicit retry using the original snapshot.
		f.native->restoreFails = false;
		replacement->revertChanges();
		QTRY_VERIFY(!f.owner->state().busy);
		QVERIFY(!f.owner->state().pending);
		QCOMPARE(f.native->restores.load(), 2);
		QCOMPARE(f.native->restoreBytes(), bytes(layout(0)));
	}

	void cancelAndTimeoutDoNotDuplicate_data() {
		QTest::addColumn<bool>("timeout");
		QTest::newRow("cancel") << false;
		QTest::newRow("timeout") << true;
	}
	void cancelAndTimeoutDoNotDuplicate() {
		QFETCH(bool, timeout);
		Fixture f(timeout ? 300ms : 15s);
		auto facade = DisplaysTestAccess::facade(f.owner);
		f.native->restoreGate.arm();
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
		QTRY_VERIFY(!f.owner->state().busy);
		if (!timeout) facade->revertChanges();
		QVERIFY(f.native->restoreGate.reached(1));
		facade->revertChanges();
		facade->keepChanges();
		facade.reset();
		f.native->restoreGate.open();
		QTRY_VERIFY(!f.owner->state().busy);
		QVERIFY(!f.owner->state().pending);
		QCOMPARE(f.native->restores.load(), 1);
		QCOMPARE(f.native->saves.load(), 0);
	}

	void failedApplyRecoversPartialWrite() {
		Fixture f;
		auto facade = DisplaysTestAccess::facade(f.owner);
		f.native->applyFails = true;
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
		QTRY_VERIFY(!f.owner->state().busy);
		QVERIFY(!f.owner->state().pending);
		QCOMPARE(f.owner->state().error, QStringLiteral("apply failed"));
		QCOMPARE(f.native->restores.load(), 1);
		QCOMPARE(f.native->currentBytes(), bytes(layout(0)));
	}

	void snapshotFailureDoesNotWrite() {
		Fixture f;
		auto facade = DisplaysTestAccess::facade(f.owner);
		QSignalSpy failures(facade.get(), &Displays::changeFailed);
		f.native->snapshotFails = true;
		QVERIFY(!DisplaysTestAccess::apply(*facade, layout(100)));
		QTRY_VERIFY(!f.owner->state().busy);
		QVERIFY(!f.owner->state().pending);
		QVERIFY(!f.owner->state().error.isEmpty());
		QCOMPARE(f.native->applies.load(), 0);
		QCOMPARE(f.native->restores.load(), 0);
		QCOMPARE(failures.count(), 1);
	}

	void unobservedRestorationRetainsRecovery_data() {
		QTest::addColumn<bool>("missingMonitor");
		QTest::newRow("monitor-disappeared") << true;
		QTest::newRow("read-back-failed") << false;
	}
	void unobservedRestorationRetainsRecovery() {
		QFETCH(bool, missingMonitor);
		Fixture f;
		auto facade = DisplaysTestAccess::facade(f.owner);
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
		QTRY_VERIFY(!f.owner->state().busy);
		f.native->restoreDropsMonitor = missingMonitor;
		f.native->restoreObservationFails = !missingMonitor;
		facade->revertChanges();
		QTRY_VERIFY(!f.owner->state().busy);
		QVERIFY(f.owner->state().pending);
		QCOMPARE(
		    f.owner->state().error,
		    QStringLiteral("Restoring the previous display layout could not be confirmed")
		);
		QCOMPARE(f.native->restores.load(), 1);
		f.native->restoreDropsMonitor = false;
		f.native->restoreObservationFails = false;
		facade->revertChanges();
		QTRY_VERIFY(!f.owner->state().busy);
		QVERIFY(!f.owner->state().pending);
		QCOMPARE(f.native->restoreBytes(), bytes(layout(0)));
		QCOMPARE(f.native->restores.load(), 2);
	}

	void topologyComparisonUsesMeaningNotArrayOffsets() {
		const auto original = layout(0);
		auto reordered = original;
		std::ranges::reverse(reordered.paths);
		std::ranges::reverse(reordered.modes);
		for (auto& path: reordered.paths) {
			path.sourceInfo.modeInfoIdx = 3 - path.sourceInfo.modeInfoIdx;
			path.targetInfo.modeInfoIdx = 3 - path.targetInfo.modeInfoIdx;
		}
		QVERIFY(sameDisplayTopology(original, reordered));
		reordered.paths[0].targetInfo.refreshRate = {60000, 1000};
		QVERIFY(sameDisplayTopology(original, reordered));
		reordered.modes[3].sourceMode.position.x += 1;
		QVERIFY(!sameDisplayTopology(original, reordered));
	}

	void topologyComparisonRejectsChangedState_data() {
		QTest::addColumn<int>("change");
		QTest::newRow("geometry") << 0;
		QTest::newRow("rotation") << 1;
		QTest::newRow("refresh") << 2;
		QTest::newRow("target-timing") << 3;
		QTest::newRow("invalid-source-mode") << 4;
		QTest::newRow("target-identity") << 5;
	}
	void topologyComparisonRejectsChangedState() {
		QFETCH(int, change);
		const auto original = layout(0);
		auto changed = original;
		switch (change) {
		case 0: changed.modes[0].sourceMode.width += 1; break;
		case 1: changed.paths[0].targetInfo.rotation = DISPLAYCONFIG_ROTATION_ROTATE90; break;
		case 2: changed.paths[0].targetInfo.refreshRate.Numerator += 1; break;
		case 3: changed.modes[2].targetMode.targetVideoSignalInfo.pixelRate += 1; break;
		case 4: changed.paths[0].sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID; break;
		case 5: changed.paths[0].targetInfo.id += 1; break;
		default: QFAIL("invalid test row");
		}
		QVERIFY(!sameDisplayTopology(original, changed));
	}

	void unconfirmedJobCannotStrandExistingPreview() {
		Fixture f;
		auto facade = DisplaysTestAccess::facade(f.owner);
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
		QTRY_VERIFY(!f.owner->state().busy);
		auto gate = std::make_shared<Gate>();
		gate->arm();
		QVERIFY(DisplaysTestAccess::unconfirmedJob(*facade, [gate] {
			return gate->enter() ? QString() : QStringLiteral("job gate timed out");
		}));
		QVERIFY(gate->reached(1));
		facade.reset();
		gate->open();
		QTRY_VERIFY(!f.owner->state().busy);
		QCOMPARE(f.native->snapshots.load(), 1);
		QCOMPARE(f.native->restores.load(), 1);
		QCOMPARE(f.native->restoreBytes(), bytes(layout(0)));
	}

	void failedNonConfirmingJobDoesNotCancelPreview_data() {
		QTest::addColumn<bool>("leaseEnds");
		QTest::newRow("live-lease-preserves-preview") << false;
		QTest::newRow("ended-lease-still-restores") << true;
	}
	void failedNonConfirmingJobDoesNotCancelPreview() {
		QFETCH(bool, leaseEnds);
		Fixture f;
		auto facade = DisplaysTestAccess::facade(f.owner);
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
		QTRY_VERIFY(!f.owner->state().busy);
		const auto deadline = f.owner->state().deadline;
		auto gate = std::make_shared<Gate>();
		gate->arm();
		QVERIFY(DisplaysTestAccess::unconfirmedJob(*facade, [gate] {
			return gate->enter() ? QStringLiteral("non-confirming setting failed")
			                     : QStringLiteral("setting gate timed out");
		}));
		QVERIFY(gate->reached(1));
		if (leaseEnds) facade.reset();
		gate->open();
		QTRY_VERIFY(!f.owner->state().busy);
		QCOMPARE(f.owner->state().error, QStringLiteral("non-confirming setting failed"));
		QCOMPARE(f.native->restores.load(), leaseEnds ? 1 : 0);
		QCOMPARE(f.owner->state().pending, !leaseEnds);
		if (!leaseEnds) {
			QVERIFY(f.owner->state().deadline == deadline);
			QCOMPARE(f.native->currentBytes(), bytes(layout(100)));
			facade->keepChanges();
			QTRY_VERIFY(!f.owner->state().busy);
			QVERIFY(!f.owner->state().pending);
			QCOMPARE(f.native->saves.load(), 1);
			QCOMPARE(f.native->restores.load(), 0);
		} else {
			QCOMPARE(f.native->restoreBytes(), bytes(layout(0)));
		}
	}

	void shutdownIsBoundedAndLateApplyRestores() {
		Fixture f;
		auto facade = DisplaysTestAccess::facade(f.owner);
		f.native->applyGate.arm();
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
		QVERIFY(f.native->applyGate.reached(1));
		const auto start = DisplayTransaction::Clock::now();
		QVERIFY(!f.owner->shutdown(20ms));
		QVERIFY(DisplayTransaction::Clock::now() - start < 500ms);
		QVERIFY(!DisplaysTestAccess::apply(*facade, layout(200)));
		facade.reset();
		f.native->applyGate.open();
		QTRY_VERIFY(!f.owner->state().busy);
		QVERIFY(f.owner->shutdown());
		QCOMPARE(f.native->restores.load(), 1);
		QCOMPARE(f.native->currentBytes(), bytes(layout(0)));
	}

	void shutdownPendingAndFailedRecovery_data() {
		QTest::addColumn<bool>("restoreFails");
		QTest::newRow("pending-confirmation") << false;
		QTest::newRow("monitor-gone") << true;
	}
	void shutdownPendingAndFailedRecovery() {
		QFETCH(bool, restoreFails);
		Fixture f;
		auto facade = DisplaysTestAccess::facade(f.owner);
		QVERIFY(DisplaysTestAccess::apply(*facade, layout(100)));
		QTRY_VERIFY(!f.owner->state().busy);
		f.native->restoreFails = restoreFails;
		QCOMPARE(f.owner->shutdown(1s), !restoreFails);
		QCOMPARE(f.owner->state().pending, restoreFails);
		QCOMPARE(f.native->restores.load(), 1);
		if (restoreFails) QVERIFY(!f.owner->state().error.isEmpty());
		facade.reset();
		QCOMPARE(f.native->restores.load(), 1);
	}
};

QTEST_GUILESS_MAIN(DisplayTransactionTest)
#include "display_transaction_test.moc"

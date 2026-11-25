#include <memory>
#include <optional>

#include <qcoreapplication.h>
#include <qsignalspy.h>
#include <qtest.h>

#include "../hyprland/dispatch_refusals.hpp"
#include "../hyprland/dispatcher.hpp"
#include "../tiling_result.hpp"
#include "../window_tracker.hpp"

using qs::hyprland::ipc::Dispatcher;
using qs::hyprland::ipc::PinRequest;
using qs::windows::FloatingChangeResult;
using qs::windows::TrackedWindow;
using qs::windows::WindowTracker;

namespace qs::windows {

WindowTracker::WindowTracker() {}
WindowTracker::~WindowTracker() {}

WindowTracker* WindowTracker::createForTest() { return new WindowTracker(); }
void WindowTracker::destroyForTest(WindowTracker* tracker) { delete tracker; }

TrackedWindow::TrackedWindow(WindowTracker* tracker, HWND hwnd)
    : QObject(tracker)
    , tracker(tracker)
    , mHwnd(hwnd)
    , fullscreenOwner(this->fullscreenOps, hwnd) {
	QObject::connect(this, &TrackedWindow::exePathChanged, this, &TrackedWindow::imagePathChanged);
}

TrackedWindow::~TrackedWindow() = default;

} // namespace qs::windows

namespace {

struct PinFake {
	bool supported = true;
	std::optional<bool> state = false;
	bool mutationSucceeds = true;
	int queries = 0;
	int mutations = 0;
};

struct FloatingFake {
	bool active = true;
	FloatingChangeResult result = FloatingChangeResult::Changed;
	int calls = 0;
};

struct DispatcherFixture {
	WindowTracker* tracker = nullptr;
	TrackedWindow* window = nullptr;
	PinFake pin;
	FloatingFake floating;
	Dispatcher::TestHooks hooks;
	std::unique_ptr<Dispatcher> dispatcher;

	DispatcherFixture() {
		this->tracker = WindowTracker::createForTest();
		this->window = TrackedWindow::createForTest(
		    this->tracker, reinterpret_cast<HWND>(static_cast<quintptr>(1))
		);

		this->hooks.tracker = this->tracker;
		this->hooks.pinSupported = [this] { return this->pin.supported; };
		this->hooks.queryPin = [this](HWND) {
			this->pin.queries++;
			return this->pin.state;
		};
		this->hooks.setPin = [this](HWND, bool pinned) {
			this->pin.mutations++;
			if (!this->pin.mutationSucceeds) return false;
			this->pin.state = pinned;
			return true;
		};
		this->hooks.setFloating = [this](TrackedWindow*, bool, bool)
		    -> std::optional<FloatingChangeResult> {
			if (!this->floating.active) return std::nullopt;
			this->floating.calls++;
			return this->floating.result;
		};
		this->dispatcher = std::make_unique<Dispatcher>(this->hooks);
	}

	~DispatcherFixture() { WindowTracker::destroyForTest(this->tracker); }
};

class DispatchRefusalTest: public QObject {
	Q_OBJECT;

private slots:
	void pinUnsupportedReportsOnceWithoutMutation() {
		DispatcherFixture fixture;
		fixture.pin.supported = false;
		QSignalSpy refused(fixture.tracker, &WindowTracker::windowActionRefused);
		QSignalSpy errors(fixture.tracker, &WindowTracker::lastActionErrorChanged);

		fixture.dispatcher->pinForTest(fixture.window, PinRequest::Toggle);

		QCOMPARE(refused.size(), 1);
		QCOMPARE(refused.first().at(0).toString(), QStringLiteral("pin"));
		QCOMPARE(
		    refused.first().at(1).toString(),
		    QStringLiteral("Window pinning is not available on this system")
		);
		QCOMPARE(errors.size(), 1);
		QCOMPARE(fixture.pin.queries, 0);
		QCOMPARE(fixture.pin.mutations, 0);
	}

	void pinQueryFailureReportsOnceWithoutMutation_data() {
		QTest::addColumn<int>("request");
		QTest::newRow("pin") << static_cast<int>(PinRequest::Pin);
		QTest::newRow("unpin") << static_cast<int>(PinRequest::Unpin);
		QTest::newRow("toggle") << static_cast<int>(PinRequest::Toggle);
	}

	void pinQueryFailureReportsOnceWithoutMutation() {
		QFETCH(int, request);
		DispatcherFixture fixture;
		fixture.pin.state = std::nullopt;
		QSignalSpy refused(fixture.tracker, &WindowTracker::windowActionRefused);

		fixture.dispatcher->pinForTest(fixture.window, static_cast<PinRequest>(request));

		QCOMPARE(refused.size(), 1);
		QCOMPARE(refused.first().at(0).toString(), QStringLiteral("pin"));
		QCOMPARE(
		    refused.first().at(1).toString(),
		    QStringLiteral("Could not read the window pin state")
		);
		QCOMPARE(
		    fixture.tracker->lastActionError(),
		    QStringLiteral("pin:Could not read the window pin state")
		);
		QCOMPARE(fixture.pin.queries, 1);
		QCOMPARE(fixture.pin.mutations, 0);
	}

	void pinMutationFailureReportsOnce_data() {
		QTest::addColumn<int>("request");
		QTest::addColumn<bool>("initiallyPinned");
		QTest::addColumn<bool>("elevated");
		QTest::addColumn<QString>("reason");

		QTest::newRow("pin") << static_cast<int>(PinRequest::Pin) << false << false
		                     << QStringLiteral("Failed to pin window");
		QTest::newRow("unpin") << static_cast<int>(PinRequest::Unpin) << true << false
		                       << QStringLiteral("Failed to pin window");
		QTest::newRow("elevated pin") << static_cast<int>(PinRequest::Pin) << false << true
		                              << QStringLiteral("Windows refused to pin an elevated window");
	}

	void pinMutationFailureReportsOnce() {
		QFETCH(int, request);
		QFETCH(bool, initiallyPinned);
		QFETCH(bool, elevated);
		QFETCH(QString, reason);
		DispatcherFixture fixture;
		fixture.pin.state = initiallyPinned;
		fixture.pin.mutationSucceeds = false;
		fixture.window->setElevatedForTest(elevated);
		QSignalSpy refused(fixture.tracker, &WindowTracker::windowActionRefused);

		fixture.dispatcher->pinForTest(fixture.window, static_cast<PinRequest>(request));

		QCOMPARE(refused.size(), 1);
		QCOMPARE(refused.first().at(0).toString(), QStringLiteral("pin"));
		QCOMPARE(refused.first().at(1).toString(), reason);
		QCOMPARE(refused.first().at(2).toBool(), elevated);
		QCOMPARE(fixture.pin.queries, 1);
		QCOMPARE(fixture.pin.mutations, 1);
	}

	void confirmedPinNoopReportsNothingAndDoesNotMutate_data() {
		QTest::addColumn<int>("request");
		QTest::addColumn<bool>("initiallyPinned");
		QTest::newRow("pin already pinned") << static_cast<int>(PinRequest::Pin) << true;
		QTest::newRow("unpin already unpinned") << static_cast<int>(PinRequest::Unpin) << false;
	}

	void confirmedPinNoopReportsNothingAndDoesNotMutate() {
		QFETCH(int, request);
		QFETCH(bool, initiallyPinned);
		DispatcherFixture fixture;
		fixture.pin.state = initiallyPinned;
		QSignalSpy refused(fixture.tracker, &WindowTracker::windowActionRefused);

		fixture.dispatcher->pinForTest(fixture.window, static_cast<PinRequest>(request));

		QCOMPARE(refused.size(), 0);
		QCOMPARE(fixture.tracker->lastActionError(), QString());
		QCOMPARE(fixture.pin.queries, 1);
		QCOMPARE(fixture.pin.mutations, 0);
	}

	void successfulPinReportsNothing() {
		DispatcherFixture fixture;
		fixture.pin.state = false;
		QSignalSpy refused(fixture.tracker, &WindowTracker::windowActionRefused);

		fixture.dispatcher->pinForTest(fixture.window, PinRequest::Pin);

		QCOMPARE(refused.size(), 0);
		QCOMPARE(fixture.pin.mutations, 1);
		QCOMPARE(fixture.pin.state, std::optional<bool>(true));
	}

	void inactiveTilingReportsOnce() {
		DispatcherFixture fixture;
		fixture.floating.active = false;
		QSignalSpy refused(fixture.tracker, &WindowTracker::windowActionRefused);

		fixture.dispatcher->toggleFloatingForTest(fixture.window, QStringLiteral("toggle"));

		QCOMPARE(refused.size(), 1);
		QCOMPARE(refused.first().at(0).toString(), QStringLiteral("togglefloating"));
		QCOMPARE(refused.first().at(1).toString(), QStringLiteral("Tiling is disabled"));
		QCOMPARE(
		    fixture.tracker->lastActionError(),
		    QStringLiteral("togglefloating:Tiling is disabled")
		);
		QCOMPARE(fixture.floating.calls, 0);
	}

	void scopeExcludedFloatingReportsDistinctRefusal() {
		DispatcherFixture fixture;
		fixture.floating.result = FloatingChangeResult::ScopeExcluded;
		QSignalSpy refused(fixture.tracker, &WindowTracker::windowActionRefused);

		fixture.dispatcher->toggleFloatingForTest(fixture.window, QStringLiteral("tile"));

		QCOMPARE(refused.size(), 1);
		QCOMPARE(refused.first().at(0).toString(), QStringLiteral("togglefloating"));
		QCOMPARE(
		    refused.first().at(1).toString(), QStringLiteral("Window is excluded from tiling")
		);
		QCOMPARE(
		    fixture.tracker->lastActionError(),
		    QStringLiteral("togglefloating:Window is excluded from tiling")
		);
		QCOMPARE(fixture.floating.calls, 1);
	}

	void failedFloatingReportsOnce_data() {
		QTest::addColumn<bool>("elevated");
		QTest::addColumn<QString>("reason");
		QTest::newRow("plain") << false << QStringLiteral("Window cannot change floating state");
		QTest::newRow("elevated")
		    << true << QStringLiteral("Windows prevents tiling an elevated window");
	}

	void failedFloatingReportsOnce() {
		QFETCH(bool, elevated);
		QFETCH(QString, reason);
		DispatcherFixture fixture;
		fixture.floating.result = FloatingChangeResult::Failed;
		fixture.window->setElevatedForTest(elevated);
		QSignalSpy refused(fixture.tracker, &WindowTracker::windowActionRefused);

		fixture.dispatcher->toggleFloatingForTest(fixture.window, QStringLiteral("toggle"));

		QCOMPARE(refused.size(), 1);
		QCOMPARE(refused.first().at(0).toString(), QStringLiteral("togglefloating"));
		QCOMPARE(refused.first().at(1).toString(), reason);
		QCOMPARE(refused.first().at(2).toBool(), elevated);
		QCOMPARE(fixture.floating.calls, 1);
	}

	void acceptedNoopAndDeferredFloatingReportNothing_data() {
		QTest::addColumn<int>("result");
		QTest::newRow("changed") << static_cast<int>(FloatingChangeResult::Changed);
		QTest::newRow("noop") << static_cast<int>(FloatingChangeResult::Noop);
		QTest::newRow("deferred") << static_cast<int>(FloatingChangeResult::Deferred);
	}

	void acceptedNoopAndDeferredFloatingReportNothing() {
		QFETCH(int, result);
		DispatcherFixture fixture;
		fixture.floating.result = static_cast<FloatingChangeResult>(result);
		QSignalSpy refused(fixture.tracker, &WindowTracker::windowActionRefused);

		fixture.dispatcher->toggleFloatingForTest(fixture.window, QStringLiteral("tile"));

		QCOMPARE(refused.size(), 0);
		QCOMPARE(fixture.tracker->lastActionError(), QString());
		QCOMPARE(fixture.floating.calls, 1);
	}

	void moveRefusalReportsReasonAndElevation_data() {
		QTest::addColumn<QString>("code");
		QTest::addColumn<bool>("elevated");
		QTest::addColumn<bool>("expectedElevated");
		QTest::addColumn<QString>("expectedReason");

		QTest::newRow("command rejected plain")
		    << QStringLiteral("command-rejected") << false << false
		    << QStringLiteral("Windows refused to move this window to that workspace");
		QTest::newRow("command rejected elevated")
		    << QStringLiteral("command-rejected") << true << true
		    << QStringLiteral("Windows refused to move an elevated window");
		QTest::newRow("permission denied plain")
		    << QStringLiteral("permission-denied") << false << false
		    << QStringLiteral("Windows denied permission to move this window");
		QTest::newRow("permission denied elevated")
		    << QStringLiteral("permission-denied") << true << true
		    << QStringLiteral("Windows refused to move an elevated window");
		QTest::newRow("invalid target does not blame elevation")
		    << QStringLiteral("invalid-target") << true << false
		    << QStringLiteral("The target workspace is not available");
		QTest::newRow("workspace limit")
		    << QStringLiteral("workspace-limit") << true << false
		    << QStringLiteral("Workspace limit reached");
		QTest::newRow("provider timeout")
		    << QStringLiteral("provider-timeout") << false << false
		    << QStringLiteral("Windows desktop control did not respond in time");
		QTest::newRow("provider unavailable")
		    << QStringLiteral("provider-unavailable") << false << false
		    << QStringLiteral("Moving windows between workspaces is not available on this system");
		QTest::newRow("invalid window")
		    << QStringLiteral("invalid-window") << false << false
		    << QStringLiteral("The target window is no longer available");
	}

	void moveRefusalReportsReasonAndElevation() {
		QFETCH(QString, code);
		QFETCH(bool, elevated);
		QFETCH(bool, expectedElevated);
		QFETCH(QString, expectedReason);
		DispatcherFixture fixture;
		QSignalSpy refused(fixture.tracker, &WindowTracker::windowActionRefused);

		qs::hyprland::ipc::reportMoveRefusal(fixture.tracker, code, elevated);

		QCOMPARE(refused.size(), 1);
		QCOMPARE(refused.first().at(0).toString(), QStringLiteral("move"));
		QCOMPARE(refused.first().at(1).toString(), expectedReason);
		QCOMPARE(refused.first().at(2).toBool(), expectedElevated);
		QCOMPARE(
		    fixture.tracker->lastActionError(),
		    QStringLiteral("move:") + expectedReason
		);
	}

	void lateMoveRefusalIgnoresReplacementWindowAndDoesNotProbeElevation() {
		DispatcherFixture fixture;
		const auto hwnd = reinterpret_cast<HWND>(static_cast<quintptr>(0x42));
		auto* original = TrackedWindow::createForTest(fixture.tracker, hwnd);
		fixture.tracker->registerWindowForTest(original);
		fixture.tracker->notePendingUserMoveForTest(11, original);

		// Close the original window and recycle the HWND for a replacement.
		fixture.tracker->unregisterWindowForTest(original);
		delete original;
		auto* replacement = TrackedWindow::createForTest(fixture.tracker, hwnd);
		fixture.tracker->registerWindowForTest(replacement);

		QSignalSpy replacementRefused(replacement, &TrackedWindow::actionRefused);
		QSignalSpy trackerRefused(fixture.tracker, &WindowTracker::windowActionRefused);
		int elevationChecks = 0;

		fixture.tracker->completePendingUserMoveForTest(
		    11,
		    reinterpret_cast<quintptr>(hwnd),
		    false,
		    QStringLiteral("permission-denied"),
		    [](const TrackedWindow*) { return true; },
		    [&elevationChecks](HWND) {
			    ++elevationChecks;
			    return true;
		    }
		);

		QCOMPARE(elevationChecks, 0);
		QCOMPARE(replacementRefused.size(), 0);
		QCOMPARE(replacement->lastActionError(), QString());
		QCOMPARE(trackerRefused.size(), 1);
		QCOMPARE(trackerRefused.first().at(0).toString(), QStringLiteral("move"));
		QCOMPARE(
		    trackerRefused.first().at(1).toString(),
		    QStringLiteral("Windows denied permission to move this window")
		);
		QCOMPARE(trackerRefused.first().at(2).toBool(), false);
	}

	void lateMoveSuccessPreservesNewerRefusalAndClearsOlderError() {
		DispatcherFixture fixture;
		fixture.tracker->registerWindowForTest(fixture.window);

		// Start pending move A, then record a newer refusal B before A finishes.
		fixture.tracker->notePendingUserMoveForTest(21, fixture.window);
		fixture.window->reportActionRefusedForTest(
		    QStringLiteral("move"),
		    qs::hyprland::ipc::moveRefusalReason(QStringLiteral("provider-busy"), false),
		    false
		);
		const auto busyError = fixture.window->lastActionError();
		QVERIFY(!busyError.isEmpty());

		fixture.tracker->completePendingUserMoveForTest(
		    21,
		    reinterpret_cast<quintptr>(fixture.window->hwnd()),
		    true,
		    QString(),
		    [](const TrackedWindow*) { return true; },
		    [](HWND) { return false; }
		);

		QCOMPARE(fixture.window->lastActionError(), busyError);
		QCOMPARE(fixture.tracker->lastActionError(), busyError);

		// A later move C started after refusal B clears it on late success.
		fixture.tracker->notePendingUserMoveForTest(22, fixture.window);
		fixture.tracker->completePendingUserMoveForTest(
		    22,
		    reinterpret_cast<quintptr>(fixture.window->hwnd()),
		    true,
		    QString(),
		    [](const TrackedWindow*) { return true; },
		    [](HWND) { return false; }
		);

		QCOMPARE(fixture.window->lastActionError(), QString());
		QCOMPARE(fixture.tracker->lastActionError(), QString());
	}

	void lateMoveFailureDoesNotOverwriteNewerSuccess() {
		DispatcherFixture fixture;
		fixture.tracker->registerWindowForTest(fixture.window);

		// Move A is pending, then move B succeeds synchronously and clears errors
		// before move A's queued failure callback runs.
		fixture.tracker->notePendingUserMoveForTest(31, fixture.window);
		fixture.window->clearActionErrorForTest();

		QSignalSpy windowRefused(fixture.window, &TrackedWindow::actionRefused);
		QSignalSpy trackerRefused(fixture.tracker, &WindowTracker::windowActionRefused);
		int elevationChecks = 0;

		fixture.tracker->completePendingUserMoveForTest(
		    31,
		    reinterpret_cast<quintptr>(fixture.window->hwnd()),
		    false,
		    QStringLiteral("permission-denied"),
		    [](const TrackedWindow*) { return true; },
		    [&elevationChecks](HWND) {
			    ++elevationChecks;
			    return true;
		    }
		);

		QCOMPARE(elevationChecks, 0);
		QCOMPARE(windowRefused.size(), 0);
		QCOMPARE(trackerRefused.size(), 0);
		QCOMPARE(fixture.window->lastActionError(), QString());
		QCOMPARE(fixture.tracker->lastActionError(), QString());
	}

	void lateMoveFailureDoesNotOverwriteTrackerOnlyRefusal() {
		DispatcherFixture fixture;
		fixture.tracker->registerWindowForTest(fixture.window);

		// Move A is pending, then a newer move targets an invalid workspace and
		// records a tracker-only refusal before move A's failure callback runs.
		fixture.tracker->notePendingUserMoveForTest(41, fixture.window);
		qs::hyprland::ipc::reportMoveRefusal(fixture.tracker, QStringLiteral("invalid-target"), false);
		const auto invalidTargetError = fixture.tracker->lastActionError();
		QCOMPARE(invalidTargetError, QStringLiteral("move:The target workspace is not available"));

		QSignalSpy windowRefused(fixture.window, &TrackedWindow::actionRefused);
		QSignalSpy trackerRefused(fixture.tracker, &WindowTracker::windowActionRefused);
		int elevationChecks = 0;

		fixture.tracker->completePendingUserMoveForTest(
		    41,
		    reinterpret_cast<quintptr>(fixture.window->hwnd()),
		    false,
		    QStringLiteral("permission-denied"),
		    [](const TrackedWindow*) { return true; },
		    [&elevationChecks](HWND) {
			    ++elevationChecks;
			    return true;
		    }
		);

		QCOMPARE(elevationChecks, 0);
		QCOMPARE(windowRefused.size(), 0);
		QCOMPARE(trackerRefused.size(), 0);
		QCOMPARE(fixture.tracker->lastActionError(), invalidTargetError);
	}

	void lateMoveSuccessClearsOwnErrorDespiteUnrelatedRefusal() {
		DispatcherFixture fixture;
		fixture.tracker->registerWindowForTest(fixture.window);
		auto* other = TrackedWindow::createForTest(
		    fixture.tracker, reinterpret_cast<HWND>(static_cast<quintptr>(0x99))
		);
		fixture.tracker->registerWindowForTest(other);

		// Window A has a prior error, starts move A, then an unrelated window B
		// reports a refusal before move A succeeds.
		fixture.window->reportActionRefusedForTest(
		    QStringLiteral("move"),
		    qs::hyprland::ipc::moveRefusalReason(QStringLiteral("command-rejected"), false),
		    false
		);
		QVERIFY(!fixture.window->lastActionError().isEmpty());
		fixture.tracker->notePendingUserMoveForTest(51, fixture.window);

		other->reportActionRefusedForTest(
		    QStringLiteral("close"),
		    QStringLiteral("access-denied"),
		    false
		);
		const auto otherError = other->lastActionError();
		QCOMPARE(fixture.tracker->lastActionError(), otherError);

		fixture.tracker->completePendingUserMoveForTest(
		    51,
		    reinterpret_cast<quintptr>(fixture.window->hwnd()),
		    true,
		    QString(),
		    [](const TrackedWindow*) { return true; },
		    [](HWND) { return false; }
		);

		QCOMPARE(fixture.window->lastActionError(), QString());
		QCOMPARE(other->lastActionError(), otherError);
		QCOMPARE(fixture.tracker->lastActionError(), otherError);
	}

	void reentrantSameWindowMoveDuringSuccessPreservesInnerPendingRefusal() {
		DispatcherFixture fixture;
		fixture.tracker->registerWindowForTest(fixture.window);

		// Outer move A captures generations before moveWindow(), then during A's
		// synchronous commandResult publication, inner move B starts pending on
		// the SAME window.
		const auto outerWindowGen = fixture.window->actionErrorGeneration();
		const auto outerTrackerGen = fixture.tracker->actionErrorGeneration();

		const auto innerWindowGen = fixture.window->actionErrorGeneration();
		const auto innerTrackerGen = fixture.tracker->actionErrorGeneration();
		QVERIFY(fixture.window->finishMoveRequestForTest(
		    true, 61, QString(), innerWindowGen, innerTrackerGen, [](HWND) { return false; }
		));

		// Outer move A now finishes its synchronous success without bumping
		// generation or dropping B's pending registration.
		QVERIFY(fixture.window->finishMoveRequestForTest(
		    true, 0, QString(), outerWindowGen, outerTrackerGen, [](HWND) { return false; }
		));

		QSignalSpy windowRefused(fixture.window, &TrackedWindow::actionRefused);
		QSignalSpy trackerRefused(fixture.tracker, &WindowTracker::windowActionRefused);
		fixture.tracker->completePendingUserMoveForTest(
		    61,
		    reinterpret_cast<quintptr>(fixture.window->hwnd()),
		    false,
		    QStringLiteral("permission-denied"),
		    [](const TrackedWindow*) { return true; },
		    [](HWND) { return false; }
		);

		QCOMPARE(windowRefused.size(), 1);
		QCOMPARE(trackerRefused.size(), 1);
		QCOMPARE(
		    fixture.window->lastActionError(),
		    QStringLiteral("move:Windows denied permission to move this window")
		);
	}

	void lateMoveCompletionIgnoresUntrackedMove() {
		DispatcherFixture fixture;
		fixture.tracker->registerWindowForTest(fixture.window);
		QSignalSpy trackerRefused(fixture.tracker, &WindowTracker::windowActionRefused);

		fixture.tracker->completePendingUserMoveForTest(
		    99,
		    reinterpret_cast<quintptr>(fixture.window->hwnd()),
		    false,
		    QStringLiteral("permission-denied"),
		    [](const TrackedWindow*) { return true; },
		    [](HWND) { return true; }
		);

		QCOMPARE(trackerRefused.size(), 0);
		QCOMPARE(fixture.tracker->lastActionError(), QString());
	}
};

} // namespace

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	DispatchRefusalTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "dispatch_refusal_test.moc"

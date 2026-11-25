#include <memory>
#include <optional>

#include <qcoreapplication.h>
#include <qsignalspy.h>
#include <qtest.h>

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

	void acceptedAndNoopFloatingReportNothing_data() {
		QTest::addColumn<int>("result");
		QTest::newRow("changed") << static_cast<int>(FloatingChangeResult::Changed);
		QTest::newRow("noop") << static_cast<int>(FloatingChangeResult::Noop);
	}

	void acceptedAndNoopFloatingReportNothing() {
		QFETCH(int, result);
		DispatcherFixture fixture;
		fixture.floating.result = static_cast<FloatingChangeResult>(result);
		QSignalSpy refused(fixture.tracker, &WindowTracker::windowActionRefused);

		fixture.dispatcher->toggleFloatingForTest(fixture.window, QStringLiteral("toggle"));

		QCOMPARE(refused.size(), 0);
		QCOMPARE(fixture.tracker->lastActionError(), QString());
		QCOMPARE(fixture.floating.calls, 1);
	}

	void deferredFloatingReportsDeferredRefusal() {
		DispatcherFixture fixture;
		fixture.floating.result = FloatingChangeResult::Deferred;
		QSignalSpy refused(fixture.tracker, &WindowTracker::windowActionRefused);

		fixture.dispatcher->toggleFloatingForTest(fixture.window, QStringLiteral("tile"));

		QCOMPARE(refused.size(), 1);
		QCOMPARE(refused.first().at(0).toString(), QStringLiteral("togglefloating"));
		QCOMPARE(
		    refused.first().at(1).toString(),
		    QStringLiteral("Tiling is deferred until the window can be placed")
		);
		QCOMPARE(
		    fixture.tracker->lastActionError(),
		    QStringLiteral("togglefloating:Tiling is deferred until the window can be placed")
		);
		QCOMPARE(fixture.floating.calls, 1);
	}
};

} // namespace

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	DispatchRefusalTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "dispatch_refusal_test.moc"

#include <qcoreapplication.h>
#include <qsignalspy.h>
#include <qtest.h>

#include "../hyprland/dispatch_refusals.hpp"
#include "../window_tracker.hpp"

using qs::hyprland::ipc::PinDispatchResult;
using qs::windows::WindowTracker;

WindowTracker::WindowTracker() {}
WindowTracker::~WindowTracker() {}

WindowTracker* WindowTracker::createForTest() { return new WindowTracker(); }
void WindowTracker::destroyForTest(WindowTracker* tracker) { delete tracker; }

namespace {

class DispatchRefusalTest: public QObject {
	Q_OBJECT;

private slots:
	void pinRefusals_data() {
		QTest::addColumn<int>("result");
		QTest::addColumn<bool>("elevated");
		QTest::addColumn<QString>("reason");

		QTest::newRow("unsupported")
		    << static_cast<int>(PinDispatchResult::Unsupported) << false
		    << QStringLiteral("Window pinning is not available on this system");
		QTest::newRow("failed")
		    << static_cast<int>(PinDispatchResult::Failed) << false
		    << QStringLiteral("Failed to pin window");
		QTest::newRow("elevated failed")
		    << static_cast<int>(PinDispatchResult::Failed) << true
		    << QStringLiteral("Windows refused to pin an elevated window");
	}

	void pinRefusals() {
		QFETCH(int, result);
		QFETCH(bool, elevated);
		QFETCH(QString, reason);

		auto* tracker = WindowTracker::createForTest();
		QSignalSpy refused(tracker, &WindowTracker::windowActionRefused);
		QSignalSpy errors(tracker, &WindowTracker::lastActionErrorChanged);

		qs::hyprland::ipc::reportPinRefusal(
		    tracker, static_cast<PinDispatchResult>(result), elevated
		);

		QCOMPARE(refused.size(), 1);
		QCOMPARE(refused.first().at(0).toString(), QStringLiteral("pin"));
		QCOMPARE(refused.first().at(1).toString(), reason);
		QCOMPARE(refused.first().at(2).toBool(), elevated);
		QCOMPARE(tracker->lastActionError(), QStringLiteral("pin:") + reason);
		QCOMPARE(errors.size(), 1);
		WindowTracker::destroyForTest(tracker);
	}

	void successfulPinDoesNotReport() {
		auto* tracker = WindowTracker::createForTest();
		QSignalSpy refused(tracker, &WindowTracker::windowActionRefused);
		qs::hyprland::ipc::reportPinRefusal(tracker, PinDispatchResult::Succeeded, false);
		QCOMPARE(refused.size(), 0);
		QCOMPARE(tracker->lastActionError(), QString());
		WindowTracker::destroyForTest(tracker);
	}

	void disabledFloatingReportsRefusal() {
		auto* tracker = WindowTracker::createForTest();
		QSignalSpy refused(tracker, &WindowTracker::windowActionRefused);

		qs::hyprland::ipc::reportToggleFloatingDisabled(tracker, true);

		QCOMPARE(refused.size(), 1);
		QCOMPARE(refused.first().at(0).toString(), QStringLiteral("togglefloating"));
		QCOMPARE(refused.first().at(1).toString(), QStringLiteral("Tiling is disabled"));
		QCOMPARE(refused.first().at(2).toBool(), true);
		QCOMPARE(
		    tracker->lastActionError(),
		    QStringLiteral("togglefloating:Tiling is disabled")
		);
		WindowTracker::destroyForTest(tracker);
	}

	void failedFloatingReportsRefusal_data() {
		QTest::addColumn<bool>("elevated");
		QTest::addColumn<QString>("reason");
		QTest::newRow("plain") << false << QStringLiteral("Window cannot change floating state");
		QTest::newRow("elevated")
		    << true << QStringLiteral("Windows prevents tiling an elevated window");
	}

	void failedFloatingReportsRefusal() {
		QFETCH(bool, elevated);
		QFETCH(QString, reason);

		auto* tracker = WindowTracker::createForTest();
		QSignalSpy refused(tracker, &WindowTracker::windowActionRefused);

		qs::hyprland::ipc::reportToggleFloatingFailure(tracker, elevated);

		QCOMPARE(refused.size(), 1);
		QCOMPARE(refused.first().at(0).toString(), QStringLiteral("togglefloating"));
		QCOMPARE(refused.first().at(1).toString(), reason);
		QCOMPARE(refused.first().at(2).toBool(), elevated);
		QCOMPARE(tracker->lastActionError(), QStringLiteral("togglefloating:") + reason);
		WindowTracker::destroyForTest(tracker);
	}
};

} // namespace

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	DispatchRefusalTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "dispatch_refusal_test.moc"

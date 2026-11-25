#include <qtest.h>
#include <qt_windows.h>

#include "../window_tracker.hpp"

using namespace qs::windows;

namespace {

class WindowActionTest: public QObject {
	Q_OBJECT

private slots:
	void accessDeniedIsReportedForAnUnelevatedWindow() {
		QVERIFY(window_action_detail::shouldReportFailure(ERROR_ACCESS_DENIED, false));
		QCOMPARE(
		    window_action_detail::refusalReason(ERROR_ACCESS_DENIED, false),
		    QStringLiteral("access-denied")
		);
	}

	void staleWindowHandlesUseTheirOwnReason() {
		QVERIFY(window_action_detail::shouldReportFailure(ERROR_INVALID_WINDOW_HANDLE, false));
		QVERIFY(window_action_detail::shouldReportFailure(ERROR_INVALID_WINDOW_HANDLE, true));
		QCOMPARE(
		    window_action_detail::refusalReason(ERROR_INVALID_WINDOW_HANDLE, false),
		    QStringLiteral("stale-window")
		);
		QCOMPARE(
		    window_action_detail::refusalReason(ERROR_INVALID_WINDOW_HANDLE, true),
		    QStringLiteral("stale-window")
		);
	}

	void unrelatedFailuresStaySilentEvenOnAnElevatedWindow() {
		QVERIFY(!window_action_detail::shouldReportFailure(ERROR_INVALID_PARAMETER, true));
		QCOMPARE(window_action_detail::refusalReason(ERROR_INVALID_PARAMETER, true), QString());
	}

	void recycledHwndOwnerMismatchIsStale() {
		QVERIFY(window_action_detail::trackedOwnerMatches(true, 1234, 55, 1234, 55));
		QVERIFY(!window_action_detail::trackedOwnerMatches(true, 4321, 55, 1234, 55));
		QVERIFY(!window_action_detail::trackedOwnerMatches(true, 1234, 56, 1234, 55));
		QVERIFY(!window_action_detail::trackedOwnerMatches(false, 1234, 55, 1234, 55));
		QCOMPARE(
		    window_action_detail::trackedOwnerFailureReason(
		        window_action_detail::trackedOwnerMatches(true, 4321, 55, 1234, 55)
		    ),
		    QStringLiteral("stale-window")
		);
		QVERIFY(window_action_detail::trackedOwnerFailureReason(true).isEmpty());
		QCOMPARE(
		    window_action_detail::refusalReason(ERROR_INVALID_WINDOW_HANDLE, false),
		    QStringLiteral("stale-window")
		);
	}

	void fullscreenAccessDenialPropagatesOnALiveWindow() {
		QCOMPARE(
		    window_action_detail::fullscreenFailureReason(true, true, false),
		    QStringLiteral("access-denied")
		);
		QCOMPARE(
		    window_action_detail::fullscreenFailureReason(true, true, true),
		    QStringLiteral("elevated-window")
		);
		QCOMPARE(
		    window_action_detail::fullscreenFailureReason(false, false, true),
		    QStringLiteral("stale-window")
		);
		QVERIFY(window_action_detail::fullscreenFailureReason(false, true, true).isEmpty());
	}

	void elevatedWindowsUseTheStableElevatedReasonOnlyForAccessDenied() {
		QVERIFY(window_action_detail::shouldReportFailure(ERROR_ACCESS_DENIED, true));
		QCOMPARE(
		    window_action_detail::refusalReason(ERROR_ACCESS_DENIED, true),
		    QStringLiteral("elevated-window")
		);
		QCOMPARE(
		    window_action_detail::errorText(QStringLiteral("move"), QStringLiteral("elevated-window")),
		    QStringLiteral("move:elevated-window")
		);
	}
};

} // namespace

QTEST_GUILESS_MAIN(WindowActionTest)

#include "window_action_test.moc"

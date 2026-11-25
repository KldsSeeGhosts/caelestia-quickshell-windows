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

	void unrelatedFailuresStaySilentWithoutElevationEvidence() {
		QVERIFY(!window_action_detail::shouldReportFailure(ERROR_INVALID_WINDOW_HANDLE, false));
		QCOMPARE(
		    window_action_detail::refusalReason(ERROR_INVALID_WINDOW_HANDLE, false),
		    QString()
		);
	}

	void elevatedWindowsUseTheStableElevatedReason() {
		QVERIFY(window_action_detail::shouldReportFailure(ERROR_INVALID_WINDOW_HANDLE, true));
		QCOMPARE(
		    window_action_detail::refusalReason(ERROR_INVALID_WINDOW_HANDLE, true),
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

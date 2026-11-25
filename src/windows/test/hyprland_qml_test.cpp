#include <qcoreapplication.h>
#include <qsignalspy.h>
#include <qtest.h>

#include "../hyprland/qml.hpp"
#include "../tiling.hpp"
#include "../window_tracker.hpp"

namespace qs::windows::test {
void setTilingManagerActive(bool active);
}

using qs::hyprland::ipc::HyprlandIpcQml;
using qs::windows::WindowTracker;

class HyprlandQmlTest: public QObject {
	Q_OBJECT;

private slots:
	void tilingActiveReflectsManagerAndForwardsNotify() {
		HyprlandIpcQml qml;
		QSignalSpy changes(&qml, &HyprlandIpcQml::tilingActiveChanged);

		qs::windows::test::setTilingManagerActive(false);
		QCOMPARE(qml.tilingActive(), false);

		qs::windows::test::setTilingManagerActive(true);
		emit WindowTracker::instance()->tilingActiveChanged();
		QCOMPARE(qml.tilingActive(), true);
		QCOMPARE(changes.size(), 1);

		qs::windows::test::setTilingManagerActive(false);
		emit WindowTracker::instance()->tilingActiveChanged();
		QCOMPARE(qml.tilingActive(), false);
		QCOMPARE(changes.size(), 2);
	}

	void windowActionRefusalForwardsToQml() {
		HyprlandIpcQml qml;
		QSignalSpy refused(&qml, &HyprlandIpcQml::actionRefused);
		QSignalSpy errors(&qml, &HyprlandIpcQml::lastActionErrorChanged);

		WindowTracker::instance()->reportActionRefusal(
		    QStringLiteral("pin"),
		    QStringLiteral("Window pinning is not available on this system"),
		    false
		);

		QCOMPARE(refused.size(), 1);
		QCOMPARE(refused.first().at(0).toString(), QStringLiteral("pin"));
		QCOMPARE(
		    refused.first().at(1).toString(),
		    QStringLiteral("Window pinning is not available on this system")
		);
		QCOMPARE(refused.first().at(2).toBool(), false);
		QCOMPARE(
		    qml.lastActionError(),
		    QStringLiteral("pin:Window pinning is not available on this system")
		);
		QCOMPARE(errors.size(), 1);
	}
};

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	HyprlandQmlTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "hyprland_qml_test.moc"

#include <qtest.h>

#include "../system/wallpaper_target.hpp"

using namespace qs::windows;
using namespace qs::windows::sys::wallpaper_target;

class TestWallpaperTarget: public QObject {
	Q_OBJECT

private slots:
	void normalization() {
		QCOMPARE(
		    normalizedPath(QStringLiteral("\\\\?\\DISPLAY#AbC")),
		    QStringLiteral("//?/display#abc")
		);
		QCOMPARE(normalizedPath(QString::fromUtf8("\\Ä\\Ω")), QString::fromUtf8("/ä/ω"));
		QCOMPARE(targetId({}), QString());
		QCOMPARE(
		    targetId(QStringLiteral("ABC")),
		    QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
		);
		QCOMPARE(
		    targetId(QStringLiteral("\\\\?\\DISPLAY#AbC")),
		    targetId(QStringLiteral("//?/display#abc"))
		);
		QCOMPARE(targetId(QString::fromUtf8("\\Ä\\Ω")), targetId(QString::fromUtf8("/ä/ω")));
		QCOMPARE(targetId(QStringLiteral("path-a")).size(), 64);
		QVERIFY(targetId(QStringLiteral("path-a")) != targetId(QStringLiteral("path-b")));
	}

	void exactRoutingAndRemoval() {
		QList<DisplayIdentity> live {
		    {QStringLiteral("DISPLAY1"),
		     QStringLiteral("\\\\?\\Primary"),
		     QStringLiteral("Same label"),
		     {},
		     QStringLiteral("Same label"),
		     true},
		    {QStringLiteral("DISPLAY2"),
		     QStringLiteral("\\\\?\\Secondary"),
		     QStringLiteral("Same label"),
		     {},
		     QStringLiteral("Same label (2)"),
		     true}
		};
		QCOMPARE(
		    devicePath(live, QStringLiteral("same LABEL (2)")),
		    QStringLiteral("\\\\?\\Secondary")
		);
		QVERIFY(devicePath(live, {}).isEmpty());
		QVERIFY(devicePath(live, QStringLiteral("DISPLAY2")).isEmpty());
		QVERIFY(devicePath(live, QStringLiteral("missing")).isEmpty());
		live.removeLast();
		QVERIFY(devicePath(live, QStringLiteral("Same label (2)")).isEmpty());
	}

	void rejectsAmbiguityAndMissingPath() {
		QList<DisplayIdentity> live {
		    {{}, QStringLiteral("\\\\?\\Target"), {}, {}, QStringLiteral("screen"), true},
		    {{}, QStringLiteral("other"), {}, {}, QStringLiteral("SCREEN"), true}
		};
		QVERIFY(devicePath(live, QStringLiteral("screen")).isEmpty());
		live[1].uniqueName = QStringLiteral("other");
		live[1].devicePath = QStringLiteral("//?/target");
		QVERIFY(devicePath(live, QStringLiteral("screen")).isEmpty());
		live.removeLast();
		live[0].devicePath.clear();
		QVERIFY(devicePath(live, QStringLiteral("screen")).isEmpty());
		QVERIFY(devicePath({}, QStringLiteral("screen")).isEmpty());
	}

	void comSpellingMatchesOnlyExactTarget() {
		QStringList paths {QStringLiteral("\\\\?\\Primary"), QStringLiteral("\\\\?\\SeCoNdArY")};
		QCOMPARE(
		    monitorPath(paths, QStringLiteral("//?/secondary")),
		    QStringLiteral("\\\\?\\SeCoNdArY")
		);
		QVERIFY(monitorPath(paths, QStringLiteral("//?/removed")).isEmpty());
		QVERIFY(monitorPath(paths, {}).isEmpty());
		QVERIFY(monitorPath({}, QStringLiteral("//?/secondary")).isEmpty());
		paths.append(QStringLiteral("//?/SECONDARY"));
		QVERIFY(monitorPath(paths, QStringLiteral("//?/secondary")).isEmpty());
	}
};

QTEST_GUILESS_MAIN(TestWallpaperTarget)
#include "wallpaper_target_test.moc"

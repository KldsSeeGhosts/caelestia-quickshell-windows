#include <qtest.h>

#include "../desktop_embedding.hpp"

class DesktopEmbeddingTest: public QObject {
	Q_OBJECT;

private slots:
	void desktopSwitchWithUnchangedHost_data() {
		QTest::addColumn<bool>("visible");
		QTest::newRow("visible background") << true;
		QTest::newRow("hidden background") << false;
	}

	void desktopSwitchWithUnchangedHost() {
		QFETCH(bool, visible);
		const int workerW = 0;
		int rechecks = 0;
		int placementChecks = 0;
		int presentResets = 0;

		qs::windows::reconcileEmbeddedPanel(
		    &workerW,
		    [&] {
			    ++rechecks;
			    return &workerW;
		    },
		    [&] { ++placementChecks; },
		    [&] { return visible; },
		    [&] { ++presentResets; }
		);

		QCOMPARE(rechecks, 1);
		QCOMPARE(placementChecks, 1);
		QCOMPARE(presentResets, visible ? 1 : 0);
	}
};

QTEST_GUILESS_MAIN(DesktopEmbeddingTest)
#include "desktop_embedding_test.moc"

#include <qcoreapplication.h>
#include <qhash.h>
#include <qtest.h>

#include "../fullscreen_app_state.hpp"

using qs::windows::fullscreen_app::State;

class FullscreenAppStateTest: public QObject {
	Q_OBJECT;

private slots:
	void normalForegroundRecoversMissingClose() {
		State state;
		auto opened = state.open(false);
		QVERIFY(state.lowered());
		QVERIFY(state.settle(opened.generation, false));
		QVERIFY(!state.lowered());
		QVERIFY(!state.pending());
		QVERIFY(!state.reconcile(opened.generation, true));
	}

	void fullscreenForegroundStaysLowered() {
		State state;
		auto opened = state.open(false);
		QVERIFY(!state.settle(opened.generation, true));
		QVERIFY(state.lowered());
	}

	void staleNormalForegroundCannotEndNewEpisode() {
		State state;
		auto old = state.open(false);
		state.close();
		(void)state.open(false);
		QVERIFY(!state.settle(old.generation, false));
		QVERIFY(state.lowered());
	}

	void unconfirmedOpeningDoesNotLowerForNormalApp() {
		State state;
		auto opened = state.open(true);
		QVERIFY(!state.settle(opened.generation, false));
		QVERIFY(!state.lowered());
		QVERIFY(state.settle(opened.generation, true));
		QVERIFY(state.lowered());
	}

	void perMonitorReplayKeepsOtherMonitorAndReservationsUnchanged() {
		QHash<quintptr, State> monitors;
		constexpr quintptr left = 1;
		constexpr quintptr right = 2;
		monitors[left].seed(true); // shell starts during a left-monitor fullscreen
		monitors[right].seed(false);
		QVERIFY(monitors[left].lowered());
		QVERIFY(!monitors[right].lowered());
		monitors[left].close();
		for (int transition = 0; transition < 5; transition++) {
			int lowers = 0;
			int raises = 0;
			// Four reserved edges report each transition. They must produce
			// only one effective stack change, never a reservation change.
			for (int edge = 0; edge < 4; edge++) {
				auto before = monitors[left].lowered();
				(void)monitors[left].open(false);
				lowers += before != monitors[left].lowered();
				QVERIFY(!monitors[right].lowered());
			}
			for (int edge = 0; edge < 4; edge++) {
				auto before = monitors[left].lowered();
				monitors[left].close();
				raises += before != monitors[left].lowered();
				QVERIFY(!monitors[right].lowered());
			}
			QCOMPARE(lowers, 1);
			QCOMPARE(raises, 1);
		}
		// Two simultaneous episodes: a close on the left monitor must not raise the right.
		(void)monitors[left].open(false);
		auto pending = monitors[right].open(true);
		monitors[left].close();
		QVERIFY(monitors[right].reconcile(pending.generation, true));
		QVERIFY(monitors[right].lowered());
		QVERIFY(!monitors[left].lowered());
	}

	void lateRegistrationCannotOverrideCallback() {
		State state;
		state.close();
		state.seed(true);
		QVERIFY(!state.lowered());
		(void)state.open(false);
		state.seed(false);
		QVERIFY(state.lowered());
	}

	void switcherThenFullscreenAppLowers() {
		State state;

		// Alt+Tab host is foreground when the opening notification arrives.
		auto opened = state.open(true);
		QVERIFY(!opened.lower);
		QVERIFY(state.pending());
		QVERIFY(!state.lowered());

		// The switcher is still up at the first retry.
		QVERIFY(!state.reconcile(opened.generation, false));
		QVERIFY(state.pending());

		// The target fullscreen application settles into the foreground.
		QVERIFY(state.reconcile(opened.generation, true));
		QVERIFY(!state.pending());
		QVERIFY(state.lowered());
	}

	void switcherThenCloseNeverLowers() {
		State state;

		auto opened = state.open(true);
		QVERIFY(!opened.lower);
		QVERIFY(state.pending());

		// The switcher was dismissed back to the same normal application, so
		// Explorer sends a close before the pending open is reconciled.
		state.close();
		QVERIFY(!state.pending());
		QVERIFY(!state.lowered());

		QVERIFY(!state.reconcile(opened.generation, true));
		QVERIFY(!state.lowered());
	}

	void confirmedFullscreenForegroundLowersImmediately() {
		State state;

		auto opened = state.open(false);
		QVERIFY(opened.lower);
		QVERIFY(!state.pending());
		QVERIFY(state.lowered());

		// A later close raises again.
		state.close();
		QVERIFY(!state.lowered());
	}

	void retriesStayPendingUntilAFullscreenAppSettles() {
		State state;

		auto opened = state.open(true);
		for (int retry = 0; retry < 2; retry++) {
			QVERIFY(!state.reconcile(opened.generation, false));
			QVERIFY(state.pending());
		}

		QVERIFY(state.reconcile(opened.generation, true));
		QVERIFY(!state.pending());
		QVERIFY(state.lowered());
	}

	void newEpisodeRetriesAfterRapidCloseReopen() {
		State state;

		// open@0, close@400, open@490. The old 500 ms retry must not be
		// suppressed by, or suppress, the new episode's retries.
		auto first = state.open(true);
		state.close();
		auto second = state.open(true);
		QVERIFY(second.generation != first.generation);

		// The first episode's retry at 500 ms is stale.
		QVERIFY(!state.reconcile(first.generation, true));

		// Foreground change at 510 ms: geometry has not settled yet.
		QVERIFY(!state.reconcile(second.generation, false));
		QVERIFY(state.pending());

		// The window settles at 600 ms; the new episode's 150 ms retry at
		// 640 ms honors it.
		QVERIFY(state.reconcile(second.generation, true));
		QVERIFY(!state.pending());
		QVERIFY(state.lowered());
	}
};

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	FullscreenAppStateTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "fullscreen_app_state_test.moc"

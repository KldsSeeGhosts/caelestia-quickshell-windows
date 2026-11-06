#include <qtest.h>
#include <qt_windows.h>

#include "../input_settle.hpp"

using qs::windows::hotkeys::ChordRequest;
using qs::windows::hotkeys::classifyChordRequest;
using qs::windows::hotkeys::InputSettle;
using Result = InputSettle::Result;

class InputSettleTest: public QObject {
	Q_OBJECT;

private slots:
	void emptyArmRunsImmediately() {
		InputSettle settle;
		QCOMPARE(settle.arm({}), Result::Empty);
		QVERIFY(!settle.pending());
		QCOMPARE(settle.held(), size_t(0));
	}

	void settlesOnlyAfterEveryHeldKeyIsReleased() {
		InputSettle settle;
		QCOMPARE(settle.arm({VK_LWIN, VK_LSHIFT, 'O'}), Result::Waiting);
		QVERIFY(settle.pending());
		QCOMPARE(settle.held(), size_t(3));

		QCOMPARE(settle.release('O'), Result::Waiting);
		QVERIFY(settle.pending());
		QCOMPARE(settle.release(VK_LWIN), Result::Waiting);
		QVERIFY(settle.pending());

		// The last release is the completion event, not a timer.
		QCOMPARE(settle.release(VK_LSHIFT), Result::Settled);
		QVERIFY(!settle.pending());
		QCOMPARE(settle.held(), size_t(0));
	}

	void duplicateKeysCountOnce() {
		InputSettle settle;
		QCOMPARE(settle.arm({VK_LSHIFT, VK_LSHIFT, VK_LWIN}), Result::Waiting);
		QCOMPARE(settle.held(), size_t(2));
		QCOMPARE(settle.release(VK_LSHIFT), Result::Waiting);
		QCOMPARE(settle.release(VK_LWIN), Result::Settled);
	}

	void repeatOfHeldKeyDoesNotCancel() {
		InputSettle settle;
		QCOMPARE(settle.arm({VK_LWIN, 'O'}), Result::Waiting);
		QCOMPARE(settle.press('O'), Result::Waiting);
		QCOMPARE(settle.release('O'), Result::Waiting);
		QCOMPARE(settle.release(VK_LWIN), Result::Settled);
	}

	void anotherKeyCancels() {
		InputSettle settle;
		QCOMPARE(settle.arm({VK_LWIN, 'O'}), Result::Waiting);
		QCOMPARE(settle.press('A'), Result::Cancelled);
		QVERIFY(!settle.pending());

		// A later release cannot revive the cancelled request.
		QCOMPARE(settle.release('O'), Result::Cancelled);
		QCOMPARE(settle.release(VK_LWIN), Result::Cancelled);
	}

	void rePressedKeyCancels() {
		InputSettle settle;
		QCOMPARE(settle.arm({VK_LWIN, 'O'}), Result::Waiting);
		QCOMPARE(settle.release('O'), Result::Waiting);
		QCOMPARE(settle.press('O'), Result::Cancelled);
		QVERIFY(!settle.pending());
	}

	void cancelDropsEveryHeldKey() {
		InputSettle settle;
		QCOMPARE(settle.arm({VK_LWIN, 'O'}), Result::Waiting);
		QCOMPARE(settle.cancel(), Result::Cancelled);
		QVERIFY(!settle.pending());
		QCOMPARE(settle.held(), size_t(0));
	}

	void unrelatedReleaseDoesNotComplete() {
		InputSettle settle;
		QCOMPARE(settle.arm({VK_LWIN, 'O'}), Result::Waiting);
		QCOMPARE(settle.release('A'), Result::Waiting);
		QVERIFY(settle.pending());
		QCOMPARE(settle.release(VK_LWIN), Result::Waiting);
		QCOMPARE(settle.release('O'), Result::Settled);
	}

	void armsAgainAfterCompletion() {
		InputSettle settle;
		(void)settle.arm({'O'});
		QCOMPARE(settle.release('O'), Result::Settled);
		QCOMPARE(settle.arm({VK_LWIN}), Result::Waiting);
		QCOMPARE(settle.release(VK_LWIN), Result::Settled);
	}

	void mouseButtonPressCancels() {
		InputSettle settle;
		QCOMPARE(settle.arm({VK_LWIN, 'O'}), Result::Waiting);
		QCOMPARE(settle.press(VK_LBUTTON), Result::Cancelled);
		QVERIFY(!settle.pending());
	}

	void directRequestSupersedesPendingWait() {
		// A keybind dispatch waits; the same dispatch asking again keeps the
		// wait; a direct IPC call never waits and cancels the pending one.
		QCOMPARE(classifyChordRequest(true, false), ChordRequest::Wait);
		QCOMPARE(classifyChordRequest(true, true), ChordRequest::AlreadyWaiting);
		QCOMPARE(classifyChordRequest(false, false), ChordRequest::SendNow);
		QCOMPARE(classifyChordRequest(false, true), ChordRequest::SendNow);
	}
};

QTEST_GUILESS_MAIN(InputSettleTest)

#include "input_settle_test.moc"

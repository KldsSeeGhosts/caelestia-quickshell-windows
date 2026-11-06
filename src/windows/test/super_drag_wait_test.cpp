#include <qtest.h>

#include "../super_drag_wait.hpp"

using qs::windows::DragPlacement;
using qs::windows::DragWait;
using qs::windows::DragWaitKind;
using qs::windows::DragWaitResult;

namespace {

constexpr DWORD TIMEOUT_MS = 3000;

RECT rect(LONG left, LONG top, LONG right, LONG bottom) {
	return RECT {.left = left, .top = top, .right = right, .bottom = bottom};
}

DragWait waitFor(DragWaitKind kind) {
	DragWait wait;
	wait.begin(kind, 100, TIMEOUT_MS);
	return wait;
}

} // namespace

class SuperDragWaitTest: public QObject {
	Q_OBJECT;

private slots:
	void restoreCompletesOnlyWhenUnmaximized() {
		auto wait = waitFor(DragWaitKind::Restore);
		QCOMPARE(qs::windows::dragRestoreWait(wait, true, 100), DragWaitResult::Waiting);
		QCOMPARE(qs::windows::dragRestoreWait(wait, false, 100), DragWaitResult::Done);
	}

	void restoreDeadlineReportsFailureNeverCompletion() {
		auto wait = waitFor(DragWaitKind::Restore);
		QCOMPARE(qs::windows::dragRestoreWait(wait, true, 100 + TIMEOUT_MS), DragWaitResult::Failed);
		QCOMPARE(qs::windows::dragRestoreWait(wait, true, 100 + TIMEOUT_MS + 5000), DragWaitResult::Failed);

		// An observation that arrives after the deadline still wins over it.
		QCOMPARE(qs::windows::dragRestoreWait(wait, false, 100 + TIMEOUT_MS + 5000), DragWaitResult::Done);
	}

	void placementWaitsOnlyWhileUnanswered() {
		auto wait = waitFor(DragWaitKind::Placement);
		QCOMPARE(qs::windows::dragPlacementWait(wait, true, 100), DragWaitResult::Waiting);
		QCOMPARE(qs::windows::dragPlacementWait(wait, false, 100), DragWaitResult::Done);
	}

	void placementDeadlineReportsFailureNeverCompletion() {
		// A hung target never answers: the deadline fails the drag.
		auto wait = waitFor(DragWaitKind::Placement);
		QCOMPARE(qs::windows::dragPlacementWait(wait, true, 100 + TIMEOUT_MS), DragWaitResult::Failed);

		// An answer that arrives after the deadline still wins over it.
		QCOMPARE(qs::windows::dragPlacementWait(wait, false, 100 + TIMEOUT_MS + 5000), DragWaitResult::Done);
	}

	// Esc on a maximized drag restores the placement captured at drag start.
	// The target's state event wakes the worker, so the restore is sent on the
	// answer rather than at the deadline, and only after any placement that was
	// already in flight has answered.
	void cancelRestoreSendsOnTheRestoreAnswerNotTheDeadline() {
		auto wait = waitFor(DragWaitKind::Placement);

		// Still maximized: wait, but the answer at 150 sends at once.
		QCOMPARE(
		    qs::windows::dragCancelStep(false, false, false, wait, 100),
		    qs::windows::DragCancelStep::WaitForRestore
		);
		QCOMPARE(
		    qs::windows::dragCancelStep(false, true, false, wait, 150),
		    qs::windows::DragCancelStep::SendRestore
		);

		// A placement from before Esc must answer first.
		QCOMPARE(
		    qs::windows::dragCancelStep(false, true, true, wait, 150),
		    qs::windows::DragCancelStep::WaitForRestore
		);
		QCOMPARE(
		    qs::windows::dragCancelStep(false, true, false, wait, 160),
		    qs::windows::DragCancelStep::SendRestore
		);
	}

	// The saved placement may be maximized. Completion is its own answer, so a
	// successful restore that leaves the window maximized is not held until the
	// deadline as "still maximized".
	void cancelCompletesOnAMatchingMaximizedPlacement() {
		auto wait = waitFor(DragWaitKind::Placement);

		QCOMPARE(
		    qs::windows::dragCancelStep(true, true, true, wait, 200),
		    qs::windows::DragCancelStep::WaitForAnswer
		);
		QCOMPARE(
		    qs::windows::dragCancelStep(true, true, false, wait, 250),
		    qs::windows::DragCancelStep::Done
		);
		QCOMPARE(
		    qs::windows::dragCancelStep(true, true, false, wait, 100 + TIMEOUT_MS),
		    qs::windows::DragCancelStep::Done
		);
	}

	void restoredPlacementMustMatchTheCapturedOne() {
		WINDOWPLACEMENT saved {};
		saved.length = sizeof(saved);
		saved.showCmd = SW_SHOWMAXIMIZED;
		saved.rcNormalPosition = rect(100, 100, 900, 700);

		// Maximized stays maximized: the same show command and normal frame.
		auto same = saved;
		QVERIFY(qs::windows::dragPlacementRestored(saved, same, true));

		auto moved = saved;
		moved.rcNormalPosition = rect(120, 100, 920, 700);
		QVERIFY(!qs::windows::dragPlacementRestored(saved, moved, true));

		auto restored = saved;
		restored.showCmd = SW_SHOWNORMAL;
		QVERIFY(!qs::windows::dragPlacementRestored(saved, restored, true));

		QVERIFY(!qs::windows::dragPlacementRestored(saved, same, false));
	}

	void placementsCoalesceWhileOneIsOutstanding() {
		DragPlacement placement;
		QVERIFY(placement.submit(1));
		QVERIFY(placement.pending);

		// Newer targets are not queued behind the outstanding one.
		QVERIFY(!placement.submit(2));
		QVERIFY(!placement.submit(3));
		QVERIFY(placement.dirty);
		QCOMPARE(placement.seq, 1u);

		QVERIFY(placement.answer(1));
		QVERIFY(!placement.pending);
		QVERIFY(placement.dirty);

		// The coalesced target goes out as one request.
		QVERIFY(placement.submit(4));
		QVERIFY(!placement.dirty);
	}

	void answerToAnEarlierRequestIsIgnored() {
		DragPlacement placement;
		QVERIFY(placement.submit(7));
		QVERIFY(!placement.answer(6));
		QVERIFY(placement.pending);
		QVERIFY(placement.answer(7));
		QVERIFY(!placement.answer(7));
	}

	void refusedUnchangedAnswerUnblocksTheNextPlacement() {
		// Already at its 500 px minimum, the window keeps its geometry when asked
		// for 400. The synchronous answer is that unchanged rect; it answers the
		// request even though nothing moved and no location event was emitted.
		auto origin = rect(0, 0, 500, 300);
		qs::windows::DragLimits limits;
		limits.minWidth = 136;

		DragPlacement placement;
		auto requested = qs::windows::dragResizedRect(origin, false, false, -100, 0, limits);
		QCOMPARE(requested.right, 400L);
		QVERIFY(placement.submit(1));

		// The cursor keeps moving while the request is outstanding.
		QVERIFY(!placement.submit(2));

		QVERIFY(placement.answer(1));
		qs::windows::dragLearnLimits(limits, requested, origin);
		QCOMPARE(limits.minWidth, 500L);

		// The next placement goes out at once instead of waiting until mouse-up.
		auto reversed = qs::windows::dragResizedRect(origin, false, false, 50, 0, limits);
		QCOMPARE(reversed.right, 550L);
		QVERIFY(placement.submit(3));
		QCOMPARE(placement.seq, 3u);
	}

	void resizeReversesOutwardAfterAMinimumSizeRefusal() {
		auto origin = rect(0, 0, 500, 300);
		qs::windows::DragLimits limits;
		limits.minWidth = 136;

		// Inward by 100: the target refuses and stays at 500.
		auto requested = qs::windows::dragResizedRect(origin, false, false, -100, 0, limits);
		QCOMPARE(requested.right, 400L);
		qs::windows::dragLearnLimits(limits, requested, origin);
		QCOMPARE(limits.minWidth, 500L);

		// Reversed to 50 px outward of the start: a real placement is sent.
		auto reversed = qs::windows::dragResizedRect(origin, false, false, 50, 0, limits);
		QCOMPARE(reversed.right, 550L);
		QVERIFY(!EqualRect(&reversed, &origin));
	}

	void resizeReversalAfterAClampMeasuresFromTheOriginalStart() {
		auto origin = rect(0, 0, 800, 300);
		qs::windows::DragLimits limits;

		// Asked for 400, the target clamps to 500.
		auto requested = qs::windows::dragResizedRect(origin, false, false, -400, 0, limits);
		QCOMPARE(requested.right, 400L);
		qs::windows::dragLearnLimits(limits, requested, rect(0, 0, 500, 300));
		QCOMPARE(limits.minWidth, 500L);

		// Reversing to 200 px inward of the start asks for 600, not 500.
		auto reversed = qs::windows::dragResizedRect(origin, false, false, -200, 0, limits);
		QCOMPARE(reversed.right, 600L);

		// Still clamped by what was learned when pushed further in.
		auto again = qs::windows::dragResizedRect(origin, false, false, -400, 0, limits);
		QCOMPARE(again.right, 500L);
	}

	void resizeReversalKeepsTheAnchoredEdge() {
		// Left and top edges are grabbed: right and bottom stay at 900 and 700.
		auto origin = rect(100, 100, 900, 700);
		qs::windows::DragLimits limits;

		auto requested = qs::windows::dragResizedRect(origin, true, true, 400, 300, limits);
		QCOMPARE(requested.left, 500L);
		QCOMPARE(requested.top, 400L);

		// The target keeps 500x400 at least.
		qs::windows::dragLearnLimits(limits, requested, rect(400, 300, 900, 700));
		QCOMPARE(limits.minWidth, 500L);
		QCOMPARE(limits.minHeight, 400L);

		auto reversed = qs::windows::dragResizedRect(origin, true, true, 200, 100, limits);
		QCOMPARE(reversed.left, 300L);
		QCOMPARE(reversed.right, 900L);
		QCOMPARE(reversed.top, 200L);
		QCOMPARE(reversed.bottom, 700L);
	}

	void restoreUsesTheMaximizedFrameAndGrabOffsets() {
		auto normal = rect(200, 100, 1000, 700);
		auto maximized = rect(0, 0, 1000, 1000);

		// Grabbed at 70% across and 500 px down the maximized frame: the
		// restored 800x600 frame keeps that point under the cursor.
		auto restored = qs::windows::dragRestoredRect(normal, maximized, POINT {.x = 700, .y = 500});
		QCOMPARE(restored.left, 140L);
		QCOMPARE(restored.top, 200L);
		QCOMPARE(restored.right, 940L);
		QCOMPARE(restored.bottom, 800L);

		// Reading the frame after the restore moved it (400..1000) would clamp
		// the ratio to 50% and lose the grab position.
		auto afterRestore = qs::windows::dragRestoredRect(normal, rect(400, 0, 1000, 1000), POINT {.x = 700, .y = 500});
		QCOMPARE(afterRestore.left, 300L);
	}
};

QTEST_GUILESS_MAIN(SuperDragWaitTest)

#include "super_drag_wait_test.moc"

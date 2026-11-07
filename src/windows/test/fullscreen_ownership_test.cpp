#include <qcoreapplication.h>
#include <qtest.h>

#include "../fullscreen_ownership.hpp"

using namespace qs::windows::fullscreen_ownership;

namespace {
constexpr RECT FULL {0, 0, 1920, 1080};
constexpr RECT OTHER {1920, 0, 3840, 1080};

// No HWND is created or used. All calls, including property and process checks,
// go through this fake. The native adapter is compiled, never instantiated.
struct FakeOps final: Ops {
	HWND hwnd = reinterpret_cast<HWND>(1);
	Snapshot state {
	    .identity = {.process = 42, .thread = 7, .created = {123, 456}},
	    .placement =
	        {
	            .length = sizeof(WINDOWPLACEMENT),
	            .flags = WPF_SETMINPOSITION,
	            .showCmd = SW_SHOWNORMAL,
	            .ptMinPosition = {12, 34},
	            .ptMaxPosition = {56, 78},
	            .rcNormalPosition = {100, 120, 900, 720},
	        },
	    .rect = {100, 120, 900, 720},
	    .style = WS_OVERLAPPEDWINDOW | WS_VISIBLE,
	    .visible = true,
	};
	const void* marker = nullptr;
	bool alive = true;
	bool readable = true;
	bool markAllowed = true;
	int writes = 0;
	int styleWrites = 0;
	int placementWrites = 0;
	int frameWrites = 0;
	bool failStyle = false;
	bool failPlacement = false;
	bool failFrame = false;
	bool recycleAfterStyle = false;
	bool appFullscreenAfterStyle = false;
	bool hideAfterStyle = false;
	bool minimizeAfterStyle = false;
	bool changePlacementOnMark = false;
	bool recycleOnMark = false;

	bool read(HWND window, Snapshot& out) override {
		if (window != this->hwnd || !this->alive || !this->readable) return false;
		out = this->state;
		return true;
	}
	bool mark(HWND window, const Identity& identity, const void* token) override {
		if (!this->markAllowed || this->marker || !this->alive || window != this->hwnd
		    || !(identity == this->state.identity))
			return false;
		this->marker = token;
		if (this->changePlacementOnMark) {
			this->state.placement.rcNormalPosition = {210, 240, 1010, 840};
			this->state.rect = this->state.placement.rcNormalPosition;
			this->state.style |= WS_DISABLED;
		}
		if (this->recycleOnMark) this->marker = nullptr;
		return true;
	}
	bool matches(HWND window, const Identity& identity, const void* token) override {
		return this->alive && window == this->hwnd && this->marker == token
		    && identity == this->state.identity;
	}
	void unmark(HWND window, const Identity& identity, const void* token) override {
		if (this->matches(window, identity, token)) this->marker = nullptr;
	}
	bool style(HWND window, const Identity& identity, const void* token, LONG_PTR value) override {
		if (!this->matches(window, identity, token) || this->failStyle) return false;
		this->writes++;
		this->styleWrites++;
		this->state.style = value;
		if (this->recycleAfterStyle) this->marker = nullptr; // same process/thread, new HWND
		if (this->appFullscreenAfterStyle) {
			this->state.rect = FULL;
			this->state.fullscreen = true;
		}
		if (this->hideAfterStyle) {
			this->state.visible = false;
			this->state.placement.showCmd = SW_HIDE;
		}
		if (this->minimizeAfterStyle) {
			this->state.minimized = true;
			this->state.placement.showCmd = SW_SHOWMINIMIZED;
			this->state.rect = {-32000, -32000, -31840, -31960};
		}
		return true;
	}
	bool placement(
	    HWND window,
	    const Identity& identity,
	    const void* token,
	    const WINDOWPLACEMENT& value
	) override {
		if (!this->matches(window, identity, token) || this->failPlacement) return false;
		this->writes++;
		this->placementWrites++;
		this->state.placement = value;
		this->state.minimized = value.showCmd == SW_SHOWMINIMIZED || value.showCmd == SW_MINIMIZE
		                     || value.showCmd == SW_SHOWMINNOACTIVE;
		this->state.maximized = value.showCmd == SW_SHOWMAXIMIZED;
		this->state.visible = value.showCmd != SW_HIDE;
		this->state.style &= ~(WS_VISIBLE | WS_MINIMIZE | WS_MAXIMIZE);
		if (this->state.visible) this->state.style |= WS_VISIBLE;
		if (this->state.minimized) this->state.style |= WS_MINIMIZE;
		if (this->state.maximized) this->state.style |= WS_MAXIMIZE;
		this->state.rect = value.rcNormalPosition;
		return true;
	}
	bool frame(HWND window, const Identity& identity, const void* token, const RECT* rect) override {
		if (!this->matches(window, identity, token) || this->failFrame) return false;
		this->writes++;
		this->frameWrites++;
		if (rect) this->state.rect = *rect;
		return true;
	}
};

void comparePlacement(const WINDOWPLACEMENT& actual, const WINDOWPLACEMENT& expected) {
	QCOMPARE(actual.length, expected.length);
	QCOMPARE(actual.flags, expected.flags);
	// Restore the saved geometry without activating an ordinary window.
	const auto expectedShow = expected.showCmd == SW_SHOWNORMAL || expected.showCmd == SW_RESTORE
	    ? UINT(SW_SHOWNOACTIVATE) : expected.showCmd;
	QCOMPARE(actual.showCmd, expectedShow);
	QCOMPARE(actual.ptMinPosition.x, expected.ptMinPosition.x);
	QCOMPARE(actual.ptMinPosition.y, expected.ptMinPosition.y);
	QCOMPARE(actual.ptMaxPosition.x, expected.ptMaxPosition.x);
	QCOMPARE(actual.ptMaxPosition.y, expected.ptMaxPosition.y);
	QVERIFY(EqualRect(&actual.rcNormalPosition, &expected.rcNormalPosition));
}
} // namespace

class FullscreenOwnershipTest: public QObject {
	Q_OBJECT;

private slots:
	void explicitToggleRestoresCompletePlacement_data() {
		QTest::addColumn<bool>("maximized");
		QTest::newRow("normal") << false;
		QTest::newRow("maximized") << true;
	}
	void explicitToggleRestoresCompletePlacement() {
		QFETCH(bool, maximized);
		FakeOps ops;
		ops.state.maximized = maximized;
		ops.state.placement.showCmd = maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
		if (maximized) ops.state.style |= WS_MAXIMIZE;
		auto original = ops.state;
		Owner owner(ops, ops.hwnd);
		QVERIFY(owner.enter(FULL, false));
		QVERIFY(owner.owned());
		QCOMPARE(ops.state.style & (WS_CAPTION | WS_THICKFRAME), 0);
		QVERIFY(EqualRect(&ops.state.rect, &FULL));
		QCOMPARE(owner.release(), Release::Restored);
		comparePlacement(ops.state.placement, original.placement);
		QCOMPARE(
		    ops.state.style & (WS_CAPTION | WS_THICKFRAME),
		    original.style & (WS_CAPTION | WS_THICKFRAME)
		);
		QCOMPARE(ops.state.maximized, maximized);
		QVERIFY(!owner.owned());
		QVERIFY(!ops.marker);
		auto writes = ops.writes;
		QCOMPARE(owner.release(), Release::NotOwned);
		QCOMPARE(ops.writes, writes);
	}

	void destructorAndFreshOwnerRestoreOriginal_data() {
		QTest::addColumn<bool>("maximized");
		QTest::newRow("normal") << false;
		QTest::newRow("maximized") << true;
	}
	void destructorAndFreshOwnerRestoreOriginal() {
		QFETCH(bool, maximized);
		FakeOps ops;
		ops.state.maximized = maximized;
		ops.state.placement.showCmd = maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
		if (maximized) ops.state.style |= WS_MAXIMIZE;
		auto original = ops.state;
		{
			Owner owner(ops, ops.hwnd);
			QVERIFY(owner.enter(FULL, false));
		}
		comparePlacement(ops.state.placement, original.placement);
		QCOMPARE(ops.state.style, original.style);
		QVERIFY(!ops.marker);
		// A clean restart is a fresh owner, never a recovered crash snapshot.
		{
			Owner restarted(ops, ops.hwnd);
			QVERIFY(restarted.enter(FULL, false));
		}
		comparePlacement(ops.state.placement, original.placement);
		QCOMPARE(ops.state.style, original.style);
	}

	void hiddenOrMinimizedEntryDoesNotMutate_data() {
		QTest::addColumn<bool>("hidden");
		QTest::newRow("hidden") << true;
		QTest::newRow("minimized") << false;
	}
	void hiddenOrMinimizedEntryDoesNotMutate() {
		QFETCH(bool, hidden);
		FakeOps ops;
		ops.state.visible = !hidden;
		ops.state.minimized = !hidden;
		Owner owner(ops, ops.hwnd);
		QVERIFY(!owner.enter(FULL, false));
		QCOMPARE(ops.writes, 0);
		QVERIFY(!ops.marker);
	}

	void hiddenOrMinimizedReleaseDoesNotResurrect_data() {
		QTest::addColumn<bool>("hidden");
		QTest::addColumn<bool>("maximized");
		QTest::newRow("hidden-normal") << true << false;
		QTest::newRow("hidden-maximized") << true << true;
		QTest::newRow("minimized-normal") << false << false;
		QTest::newRow("minimized-maximized") << false << true;
	}
	void hiddenOrMinimizedReleaseDoesNotResurrect() {
		QFETCH(bool, hidden);
		QFETCH(bool, maximized);
		FakeOps ops;
		ops.state.maximized = maximized;
		ops.state.placement.showCmd = maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
		auto normal = ops.state.placement.rcNormalPosition;
		Owner owner(ops, ops.hwnd);
		QVERIFY(owner.enter(FULL, false));
		ops.state.visible = !hidden;
		ops.state.minimized = !hidden;
		// GetWindowPlacement reports SW_SHOWMINIMIZED for an iconic window.
		ops.state.placement.showCmd = hidden ? SW_HIDE : SW_SHOWMINIMIZED;
		if (!hidden) ops.state.rect = {-32000, -32000, -31840, -31960};
		QCOMPARE(owner.release(), Release::Restored);
		QCOMPARE(ops.state.visible, !hidden);
		QCOMPARE(ops.state.minimized, !hidden);
		QCOMPARE(ops.state.placement.showCmd, hidden ? UINT(SW_HIDE) : UINT(SW_SHOWMINNOACTIVE));
		QVERIFY(EqualRect(&ops.state.placement.rcNormalPosition, &normal));
		QCOMPARE(ops.state.style & (WS_CAPTION | WS_THICKFRAME), LONG_PTR(WS_CAPTION | WS_THICKFRAME));
		if (!hidden && maximized) QVERIFY(ops.state.placement.flags & WPF_RESTORETOMAXIMIZED);
	}

	void lostIdentityNeverWrites_data() {
		QTest::addColumn<int>("change");
		QTest::newRow("closed") << 0;
		QTest::newRow("same-process-hwnd-reused") << 1;
		QTest::newRow("pid-reused-creation-time-changed") << 2;
		QTest::newRow("new-pid") << 3;
		QTest::newRow("new-thread") << 4;
	}
	void lostIdentityNeverWrites() {
		QFETCH(int, change);
		FakeOps ops;
		int replacementOwner = 0;
		Owner owner(ops, ops.hwnd);
		QVERIFY(owner.enter(FULL, false));
		switch (change) {
		case 0: ops.alive = false; break;
		case 1: ops.marker = &replacementOwner; break;
		case 2: ops.state.identity.created.dwLowDateTime++; break;
		case 3: ops.state.identity.process++; break;
		case 4: ops.state.identity.thread++; break;
		}
		auto writes = ops.writes;
		QCOMPARE(owner.release(), Release::IdentityLost);
		QCOMPARE(ops.writes, writes);
		QCOMPARE(owner.release(), Release::NotOwned);
		QCOMPARE(ops.writes, writes);
		if (change == 1) QCOMPARE(ops.marker, &replacementOwner);
	}

	void appFullscreenIsNeverAdopted_data() {
		QTest::addColumn<bool>("observed");
		QTest::newRow("native-geometry") << false;
		QTest::newRow("tracker-latch") << true;
	}
	void appFullscreenIsNeverAdopted() {
		QFETCH(bool, observed);
		FakeOps ops;
		ops.state.fullscreen = !observed;
		Owner owner(ops, ops.hwnd);
		QVERIFY(!owner.enter(FULL, observed));
		QCOMPARE(owner.release(), Release::NotOwned);
		QCOMPARE(ops.writes, 0);
	}

	void applicationTakeoverIsNotRestored_data() {
		QTest::addColumn<bool>("style");
		QTest::newRow("app-changed-owned-style") << true;
		QTest::newRow("app-changed-placement") << false;
	}
	void applicationTakeoverIsNotRestored() {
		QFETCH(bool, style);
		FakeOps ops;
		Owner owner(ops, ops.hwnd);
		QVERIFY(owner.enter(FULL, false));
		if (style) ops.state.style |= WS_CAPTION;
		else ops.state.rect = OTHER;
		auto writes = ops.writes;
		QCOMPARE(owner.release(), Release::ApplicationChanged);
		QCOMPARE(ops.writes, writes);
		QVERIFY(!ops.marker);
	}

	void unrelatedStyleChangesAndRetargetKeepFirstSnapshot() {
		FakeOps ops;
		ops.state.style &= ~WS_MINIMIZEBOX;
		auto original = ops.state;
		Owner owner(ops, ops.hwnd);
		QVERIFY(owner.enter(FULL, false));
		ops.state.style |= WS_MINIMIZEBOX;
		ops.state.style &= ~WS_SYSMENU;
		QVERIFY(owner.enter(OTHER, true));
		QCOMPARE(owner.release(), Release::Restored);
		QVERIFY(ops.state.style & WS_MINIMIZEBOX);
		QVERIFY(!(ops.state.style & WS_SYSMENU));
		QCOMPARE(
		    ops.state.style & (WS_CAPTION | WS_THICKFRAME),
		    original.style & (WS_CAPTION | WS_THICKFRAME)
		);
		comparePlacement(ops.state.placement, original.placement);
	}

	void restorationFailureRetainsOwnershipForRetry() {
		FakeOps ops;
		auto original = ops.state;
		Owner owner(ops, ops.hwnd);
		QVERIFY(owner.enter(FULL, false));
		ops.failPlacement = true;
		QCOMPARE(owner.release(), Release::Failed);
		QVERIFY(owner.owned());
		QVERIFY(ops.marker);
		QVERIFY(!owner.enter(OTHER, true)); // release must finish before retargeting
		ops.failPlacement = false;
		QCOMPARE(owner.release(), Release::Restored);
		comparePlacement(ops.state.placement, original.placement);
		QCOMPARE(ops.state.style, original.style);
	}

	void inaccessibleReleaseRetainsOwnershipForRetry() {
		FakeOps ops;
		Owner owner(ops, ops.hwnd);
		QVERIFY(owner.enter(FULL, false));
		auto writes = ops.writes;
		ops.readable = false;
		QCOMPARE(owner.release(), Release::Failed);
		QVERIFY(owner.owned());
		QCOMPARE(ops.writes, writes);
		ops.readable = true;
		ops.failStyle = true;
		QCOMPARE(owner.release(), Release::Failed);
		QVERIFY(owner.owned());
		QCOMPARE(ops.writes, writes);
		ops.failStyle = false;
		QCOMPARE(owner.release(), Release::Restored);
	}

	void onlyOriginallyPresentFrameBitsAreRestored() {
		FakeOps ops;
		ops.state.style &= ~WS_CAPTION;
		Owner owner(ops, ops.hwnd);
		QVERIFY(owner.enter(FULL, false));
		QCOMPARE(owner.release(), Release::Restored);
		QVERIFY(!(ops.state.style & WS_CAPTION));
		QVERIFY(ops.state.style & WS_THICKFRAME);
		ops.state.style &= ~WS_THICKFRAME;
		auto writes = ops.writes;
		QVERIFY(!owner.enter(FULL, false)); // no feature-owned frame bits
		QCOMPARE(ops.writes, writes);
	}

	void failedEntryRollsBackStyleAndPlacement() {
		FakeOps ops;
		ops.state.maximized = true;
		ops.state.placement.showCmd = SW_SHOWMAXIMIZED;
		ops.state.style |= WS_MAXIMIZE;
		auto original = ops.state;
		Owner owner(ops, ops.hwnd);
		ops.failFrame = true;
		QVERIFY(!owner.enter(FULL, false));
		// The frame refresh also failed, but the placement/style were restored.
		comparePlacement(ops.state.placement, original.placement);
		QCOMPARE(ops.state.style, original.style);
		ops.failFrame = false;
		QCOMPARE(owner.release(), Release::Restored);
		QVERIFY(!ops.marker);
	}

	void identityIsRecheckedBetweenNativeWrites() {
		FakeOps ops;
		Owner owner(ops, ops.hwnd);
		ops.recycleAfterStyle = true;
		QVERIFY(!owner.enter(FULL, false));
		QCOMPARE(ops.styleWrites, 1);
		QCOMPARE(ops.frameWrites, 0);
		QCOMPARE(ops.placementWrites, 0);
		QVERIFY(!owner.owned());
	}

	void replacementDuringReleaseDoesNotReceiveOldPlacement() {
		FakeOps ops;
		Owner owner(ops, ops.hwnd);
		QVERIFY(owner.enter(FULL, false));
		auto placements = ops.placementWrites;
		auto frames = ops.frameWrites;
		ops.recycleAfterStyle = true;
		QCOMPARE(owner.release(), Release::Failed);
		QCOMPARE(ops.placementWrites, placements);
		QCOMPARE(ops.frameWrites, frames);
		auto writes = ops.writes;
		QCOMPARE(owner.release(), Release::IdentityLost);
		QCOMPARE(ops.writes, writes);
	}

	void appFullscreenFromStyleCallbackIsNotOverwritten() {
		FakeOps ops;
		Owner owner(ops, ops.hwnd);
		ops.appFullscreenAfterStyle = true;
		QVERIFY(!owner.enter(OTHER, false));
		QCOMPARE(ops.styleWrites, 1);
		QCOMPARE(ops.frameWrites, 0);
		QCOMPARE(ops.placementWrites, 0);
		QVERIFY(EqualRect(&ops.state.rect, &FULL));
		QVERIFY(!owner.owned());
		QVERIFY(!ops.marker);
	}

	void savedSnapshotBelongsToMarkedWindow() {
		FakeOps ops;
		Owner owner(ops, ops.hwnd);
		ops.changePlacementOnMark = true;
		QVERIFY(owner.enter(FULL, false));
		QCOMPARE(owner.release(), Release::Restored);
		RECT markedNormal {210, 240, 1010, 840};
		QVERIFY(EqualRect(&ops.state.placement.rcNormalPosition, &markedNormal));
		QVERIFY(ops.state.style & WS_DISABLED);
		ops.recycleOnMark = true;
		auto writes = ops.writes;
		QVERIFY(!owner.enter(FULL, false));
		QVERIFY(!owner.owned());
		QCOMPARE(ops.writes, writes);
	}

	void visibilityFromRestoreCallbackIsPreserved_data() {
		QTest::addColumn<bool>("hide");
		QTest::newRow("hide") << true;
		QTest::newRow("minimize") << false;
	}
	void visibilityFromRestoreCallbackIsPreserved() {
		QFETCH(bool, hide);
		FakeOps ops;
		Owner owner(ops, ops.hwnd);
		QVERIFY(owner.enter(FULL, false));
		ops.hideAfterStyle = hide;
		ops.minimizeAfterStyle = !hide;
		QCOMPARE(owner.release(), Release::Restored);
		QCOMPARE(ops.state.visible, !hide);
		QCOMPARE(ops.state.minimized, !hide);
		QCOMPARE(ops.state.placement.showCmd, hide ? UINT(SW_HIDE) : UINT(SW_SHOWMINNOACTIVE));
	}

	void inaccessibleOrConflictingOwnerIsRefused() {
		FakeOps ops;
		Owner owner(ops, ops.hwnd);
		ops.readable = false;
		QVERIFY(!owner.enter(FULL, false));
		ops.readable = true;
		ops.markAllowed = false;
		QVERIFY(!owner.enter(FULL, false));
		ops.markAllowed = true;
		int otherOwner = 0;
		ops.marker = &otherOwner;
		QVERIFY(!owner.enter(FULL, false));
		QCOMPARE(ops.writes, 0);
		QCOMPARE(ops.marker, &otherOwner);
	}
};

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	FullscreenOwnershipTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "fullscreen_ownership_test.moc"

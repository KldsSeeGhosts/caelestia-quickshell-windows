// The capture exclusion decision, driven by fake Win32 calls. No window, no
// desktop and no capture is involved. capture_native_test checks the same
// logic against real windows with GetWindowDisplayAffinity.
#include <qcoreapplication.h>
#include <qtest.h>
#include <qt_windows.h>

#include "../capture_affinity.hpp"

using qs::windows::capture_affinity::Api;
using qs::windows::capture_affinity::CaptureExclusion;

namespace {

// One fake window: its affinity, and whether SetWindowDisplayAffinity is
// accepted for it. State lives in globals because Api holds plain function
// pointers, like the Win32 functions it replaces.
DWORD gAffinity = WDA_NONE;
bool gGetFails = false;
bool gSetFails = false;
int gSetCalls = 0;
DWORD gLastSet = 0xFFFFFFFF;
DWORD gError = 0;

void resetFake() {
	gAffinity = WDA_NONE;
	gGetFails = false;
	gSetFails = false;
	gSetCalls = 0;
	gLastSet = 0xFFFFFFFF;
	gError = 0;
}

BOOL fakeGet(HWND, DWORD* affinity) {
	if (gGetFails) return FALSE;
	*affinity = gAffinity;
	return TRUE;
}

BOOL fakeSet(HWND, DWORD affinity) {
	gSetCalls++;
	gLastSet = affinity;
	if (gSetFails) {
		gError = ERROR_INVALID_PARAMETER;
		return FALSE;
	}
	gAffinity = affinity;
	return TRUE;
}

DWORD fakeLastError() { return gError; }

Api fakeApi() {
	return Api {.set = &fakeSet, .get = &fakeGet, .lastError = &fakeLastError};
}

HWND window() { return reinterpret_cast<HWND>(static_cast<quintptr>(0x1234)); }

} // namespace

class CaptureAffinityTest: public QObject {
	Q_OBJECT;

private slots:
	void init() { resetFake(); }

	void excludesAndRestores() {
		CaptureExclusion exclusion;

		auto on = exclusion.apply(window(), true, fakeApi());
		QVERIFY(on.matches && on.called);
		QCOMPARE(gAffinity, static_cast<DWORD>(WDA_EXCLUDEFROMCAPTURE));
		QCOMPARE(gLastSet, static_cast<DWORD>(WDA_EXCLUDEFROMCAPTURE));

		auto off = exclusion.apply(window(), false, fakeApi());
		QVERIFY(off.matches && off.called);
		QCOMPARE(gAffinity, static_cast<DWORD>(WDA_NONE));
		QCOMPARE(gLastSet, static_cast<DWORD>(WDA_NONE));
		QCOMPARE(gSetCalls, 2);
	}

	// Every panel runs this at creation. A panel that never asked for exclusion
	// must not be written to, whatever the window type.
	void doesNotWriteWhenAlreadyInTheWantedState() {
		CaptureExclusion exclusion;

		auto off = exclusion.apply(window(), false, fakeApi());
		QVERIFY(off.matches);
		QVERIFY(!off.called);

		QVERIFY(exclusion.apply(window(), true, fakeApi()).called);
		auto again = exclusion.apply(window(), true, fakeApi());
		QVERIFY(again.matches);
		QVERIFY(!again.called);
		QCOMPARE(gSetCalls, 1);
	}

	// A reload adopts a window an earlier panel excluded. The new panel may want
	// the opposite, so the state is read from the window and not remembered.
	void readsTheWindowInsteadOfRememberingState() {
		gAffinity = WDA_EXCLUDEFROMCAPTURE;
		CaptureExclusion adopted;

		QVERIFY(adopted.apply(window(), true, fakeApi()).matches);
		QCOMPARE(gSetCalls, 0);

		auto off = adopted.apply(window(), false, fakeApi());
		QVERIFY(off.matches && off.called);
		QCOMPARE(gAffinity, static_cast<DWORD>(WDA_NONE));
	}

	// A recreated window starts with no affinity even though the property is
	// still true, and the same object applies it again.
	void reappliesToANewWindow() {
		CaptureExclusion exclusion;
		QVERIFY(exclusion.apply(window(), true, fakeApi()).called);

		gAffinity = WDA_NONE; // the new native window
		auto again = exclusion.apply(window(), true, fakeApi());
		QVERIFY(again.matches && again.called);
		QCOMPARE(gAffinity, static_cast<DWORD>(WDA_EXCLUDEFROMCAPTURE));
	}

	void failureKeepsWorkingAndReportsOnce() {
		gSetFails = true;
		CaptureExclusion exclusion;

		auto first = exclusion.apply(window(), true, fakeApi());
		QVERIFY(!first.matches);
		QVERIFY(first.called);
		QVERIFY(first.firstFailure);
		QCOMPARE(first.error, static_cast<DWORD>(ERROR_INVALID_PARAMETER));
		QCOMPARE(gAffinity, static_cast<DWORD>(WDA_NONE));

		auto second = exclusion.apply(window(), true, fakeApi());
		QVERIFY(!second.matches);
		QVERIFY(!second.firstFailure);

		// A later success is still applied.
		gSetFails = false;
		QVERIFY(exclusion.apply(window(), true, fakeApi()).matches);
		QCOMPARE(gAffinity, static_cast<DWORD>(WDA_EXCLUDEFROMCAPTURE));

		// Each object reports its own first failure.
		gAffinity = WDA_NONE;
		gSetFails = true;
		CaptureExclusion other;
		QVERIFY(other.apply(window(), true, fakeApi()).firstFailure);
	}

	// A window type that cannot report an affinity has none.
	void unreadableAffinityCountsAsNone() {
		gGetFails = true;
		CaptureExclusion exclusion;

		auto off = exclusion.apply(window(), false, fakeApi());
		QVERIFY(off.matches);
		QVERIFY(!off.called);

		auto on = exclusion.apply(window(), true, fakeApi());
		QVERIFY(on.called);
		QCOMPARE(gLastSet, static_cast<DWORD>(WDA_EXCLUDEFROMCAPTURE));
	}

	void noWindowIsANoOp() {
		CaptureExclusion exclusion;
		auto outcome = exclusion.apply(nullptr, true, fakeApi());
		QVERIFY(outcome.matches);
		QVERIFY(!outcome.called);
		QCOMPARE(gSetCalls, 0);
	}
};

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	CaptureAffinityTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "capture_affinity_test.moc"

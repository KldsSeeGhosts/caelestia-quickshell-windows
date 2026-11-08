// Capture exclusion and the window stack snapshot against real native windows,
// on a private desktop this process creates and never switches to. Nothing is
// shown on the input desktop, and the user's windows are not enumerated: a
// thread sees only the windows of the desktop it is attached to.
//
// The window class and style combinations are the ones the backend meets:
// ordinary top-level popups, a WS_EX_LAYERED window (the style the desktop
// panels force with QS_DESKTOP_LAYERED), a layered window drawn with
// UpdateLayeredWindow, and a child window. Whether the exclusion hides pixels
// from a capture needs a composed desktop and is a live check.
#include <cstdio>

#include <qcoreapplication.h>
#include <qlist.h>
#include <qrect.h>
#include <qstring.h>
#include <qtest.h>
#include <qt_windows.h>

#include "../capture_affinity.hpp"
#include "../window_stack.hpp"

using namespace qs::windows;

namespace {

constexpr auto CLASS_NAME = L"QuickshellCaptureNativeTest";

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
	return DefWindowProcW(hwnd, message, wparam, lparam);
}

void registerClasses() {
	WNDCLASSW cls {};
	cls.lpfnWndProc = &windowProc;
	cls.hInstance = GetModuleHandleW(nullptr);
	cls.lpszClassName = CLASS_NAME;
	RegisterClassW(&cls);

	// The Windows shell classes. A window of this class is a shell surface for
	// the classification, whichever process owns it.
	for (const auto* name: {L"Shell_TrayWnd", L"Progman"}) {
		cls.lpszClassName = name;
		RegisterClassW(&cls);
	}
}

struct Spec {
	const wchar_t* className = CLASS_NAME;
	const wchar_t* title = L"";
	DWORD style = WS_POPUP | WS_VISIBLE;
	DWORD exStyle = 0;
	QRect rect = QRect(0, 0, 100, 80);
	HWND parent = nullptr;
};

HWND create(const Spec& spec) {
	return CreateWindowExW(
	    spec.exStyle,
	    spec.className,
	    spec.title,
	    spec.style,
	    spec.rect.x(),
	    spec.rect.y(),
	    spec.rect.width(),
	    spec.rect.height(),
	    spec.parent,
	    nullptr,
	    GetModuleHandleW(nullptr),
	    nullptr
	);
}

DWORD affinityOf(HWND hwnd) {
	DWORD affinity = 0xFFFFFFFF;
	GetWindowDisplayAffinity(hwnd, &affinity);
	return affinity;
}

// Test windows are in this process, so the snapshot's own-process filter would
// drop them. The fixture says none is ours, and maps with a fixed 2x scale so a
// mapping that is skipped shows up as a wrong rect.
window_stack::Hooks fixtureHooks() {
	return {
	    .ownProcess = [](HWND) { return false; },
	    .toLogical =
	        [](HWND, const QRect& physical) {
		        return QRect(
		            physical.x() / 2,
		            physical.y() / 2,
		            physical.width() / 2,
		            physical.height() / 2
		        );
	        },
	};
}

QStringList titles(const QList<window_stack::Entry>& entries) {
	auto list = QStringList();
	for (const auto& entry: entries) list.append(entry.title);
	return list;
}

} // namespace

class CaptureNativeTest: public QObject {
	Q_OBJECT;

private slots:
	void initTestCase() {
		registerClasses();
		// Creating a window on a desktop that has none waits about two seconds, so
		// every test after the first would pay that. A hidden, untitled anchor
		// window keeps the desktop populated. It is never listed.
		m_anchor = create({.style = WS_POPUP});
		QVERIFY(m_anchor != nullptr);
	}

	void cleanupTestCase() {
		if (m_anchor != nullptr) DestroyWindow(m_anchor);
	}

	void cleanup() {
		for (auto* hwnd: m_windows) {
			if (IsWindow(hwnd)) DestroyWindow(hwnd);
		}
		m_windows.clear();
	}

	// ---- capture exclusion ------------------------------------------------

	void topLevelWindowRoundTripsTheAffinity() {
		auto* hwnd = make({.title = L"top", .style = WS_POPUP});
		QCOMPARE(affinityOf(hwnd), static_cast<DWORD>(WDA_NONE));

		capture_affinity::CaptureExclusion exclusion;
		auto on = exclusion.apply(hwnd, true);
		QVERIFY2(on.matches, qPrintable(QString::number(on.error)));
		QCOMPARE(affinityOf(hwnd), static_cast<DWORD>(WDA_EXCLUDEFROMCAPTURE));

		QVERIFY(!exclusion.apply(hwnd, true).called);

		QVERIFY(exclusion.apply(hwnd, false).matches);
		QCOMPARE(affinityOf(hwnd), static_cast<DWORD>(WDA_NONE));
	}

	void visibleWindowCanBeExcluded() {
		auto* hwnd = make({.title = L"visible"});
		capture_affinity::CaptureExclusion exclusion;
		QVERIFY(exclusion.apply(hwnd, true).matches);
		QCOMPARE(affinityOf(hwnd), static_cast<DWORD>(WDA_EXCLUDEFROMCAPTURE));
		QVERIFY(IsWindowVisible(hwnd));
	}

	// A recreated native window starts with no affinity, so the owner applies
	// it again to the new handle.
	void aNewWindowStartsUnexcludedAndIsExcludedAgain() {
		capture_affinity::CaptureExclusion exclusion;

		auto* first = make({.title = L"first", .style = WS_POPUP});
		QVERIFY(exclusion.apply(first, true).matches);
		DestroyWindow(first);

		auto* second = make({.title = L"second", .style = WS_POPUP});
		QCOMPARE(affinityOf(second), static_cast<DWORD>(WDA_NONE));
		QVERIFY(exclusion.apply(second, true).matches);
		QCOMPARE(affinityOf(second), static_cast<DWORD>(WDA_EXCLUDEFROMCAPTURE));
	}

	// WS_EX_LAYERED is forced on desktop panels with QS_DESKTOP_LAYERED. The
	// finding is recorded in the PR; the test pins it so a Windows change that
	// breaks the combination is noticed.
	void layeredTopLevelWindow() {
		auto* hwnd = make({.title = L"layered", .style = WS_POPUP, .exStyle = WS_EX_LAYERED});
		QVERIFY(SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA));

		capture_affinity::CaptureExclusion exclusion;
		auto on = exclusion.apply(hwnd, true);
		QVERIFY2(on.matches, qPrintable(QString::number(on.error)));
		QCOMPARE(affinityOf(hwnd), static_cast<DWORD>(WDA_EXCLUDEFROMCAPTURE));
		QVERIFY(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED);

		// The alpha is still applied after the affinity is set.
		BYTE alpha = 0;
		DWORD flags = 0;
		QVERIFY(GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags));
		QCOMPARE(alpha, static_cast<BYTE>(255));

		QVERIFY(exclusion.apply(hwnd, false).matches);
		QCOMPARE(affinityOf(hwnd), static_cast<DWORD>(WDA_NONE));
	}

	void layeredWindowWithPerPixelAlpha() {
		auto* hwnd = make({.title = L"alpha", .style = WS_POPUP, .exStyle = WS_EX_LAYERED});

		// Setting the affinity first and drawing afterwards, then the reverse.
		capture_affinity::CaptureExclusion exclusion;
		QVERIFY(exclusion.apply(hwnd, true).matches);

		auto* screen = GetDC(nullptr);
		auto* memory = CreateCompatibleDC(screen);
		auto* bitmap = CreateCompatibleBitmap(screen, 100, 80);
		auto* old = SelectObject(memory, bitmap);
		auto size = SIZE {100, 80};
		auto source = POINT {0, 0};
		auto blend = BLENDFUNCTION {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
		auto updated = UpdateLayeredWindow(hwnd, screen, nullptr, &size, memory, &source, 0, &blend, ULW_ALPHA);
		SelectObject(memory, old);
		DeleteObject(bitmap);
		DeleteDC(memory);
		ReleaseDC(nullptr, screen);

		QVERIFY(updated);
		QCOMPARE(affinityOf(hwnd), static_cast<DWORD>(WDA_EXCLUDEFROMCAPTURE));
	}

	// A panel embedded in the desktop window is a WS_CHILD. A child window cannot
	// hold a display affinity, so the call is rejected and the panel is captured
	// normally. This is the case the one debug message is for.
	void childWindowIsRejectedAndNothingBreaks() {
		auto* parent = make({.title = L"parent"});
		auto* child = make({.title = L"child", .style = WS_CHILD | WS_VISIBLE, .parent = parent});
		QVERIFY(child != nullptr);

		// A child window reports no affinity at all.
		DWORD affinity = 0;
		QVERIFY(!GetWindowDisplayAffinity(child, &affinity));

		capture_affinity::CaptureExclusion exclusion;
		auto on = exclusion.apply(child, true);
		QVERIFY(!on.matches);
		QVERIFY(on.firstFailure);
		QVERIFY(on.error != 0);
		QVERIFY(!GetWindowDisplayAffinity(child, &affinity));
		QVERIFY(IsWindowVisible(child));

		// Asking again does not report again, and "off" costs nothing.
		QVERIFY(!exclusion.apply(child, true).firstFailure);
		QVERIFY(!exclusion.apply(child, false).called);
	}

	// ---- window stack -----------------------------------------------------

	void listsWindowsTopmostFirst() {
		make({.title = L"A", .rect = QRect(0, 0, 100, 80)});
		make({.title = L"B", .rect = QRect(100, 0, 100, 80)});
		make({.title = L"C", .rect = QRect(200, 0, 100, 80)});

		QCOMPARE(titles(window_stack::snapshot(fixtureHooks())), (QStringList {"C", "B", "A"}));
	}

	void followsRestacking() {
		auto* a = make({.title = L"A"});
		make({.title = L"B"});
		make({.title = L"C"});

		SetWindowPos(a, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		QCOMPARE(titles(window_stack::snapshot(fixtureHooks())), (QStringList {"A", "C", "B"}));

		SetWindowPos(a, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		QCOMPARE(titles(window_stack::snapshot(fixtureHooks())), (QStringList {"C", "B", "A"}));
	}

	void topmostWindowsComeFirst() {
		make({.title = L"normal"});
		make({.title = L"pinned", .exStyle = WS_EX_TOPMOST});
		make({.title = L"newer normal"});

		QCOMPARE(
		    titles(window_stack::snapshot(fixtureHooks())),
		    (QStringList {"pinned", "newer normal", "normal"})
		);
	}

	void usesTheFrameBoundsAndTheMapping() {
		make({.title = L"framed", .rect = QRect(60, 40, 300, 200)});

		auto entries = window_stack::snapshot(fixtureHooks());
		QCOMPARE(entries.size(), 1);

		// The window has no border, so its frame is its rect. The fixture hook maps
		// physical pixels to logical ones at a 2x scale.
		QCOMPARE(entries.first().rect, QRect(30, 20, 150, 100));

		// The default mapping needs a Qt screen for the window's monitor. The
		// private desktop has none, so the window is dropped and not given a wrong
		// rect.
		auto hooks = fixtureHooks();
		hooks.toLogical = nullptr;
		QVERIFY(window_stack::snapshot(hooks).isEmpty());
	}

	void idMatchesTheWindowHandle() {
		auto* hwnd = make({.title = L"id"});
		auto entries = window_stack::snapshot(fixtureHooks());
		QCOMPARE(entries.size(), 1);
		QCOMPARE(entries.first().id, window_stack::windowId(hwnd));
	}

	void leavesOutWindowsThatAreNotApplications() {
		auto* listed = make({.title = L"listed"});
		auto* hidden = make({.title = L"hidden"});
		ShowWindow(hidden, SW_HIDE);
		auto* minimized = make({.title = L"minimized", .style = WS_OVERLAPPEDWINDOW | WS_VISIBLE});
		ShowWindow(minimized, SW_SHOWMINIMIZED);
		QVERIFY(IsIconic(minimized));
		make({.title = L"", .rect = QRect(0, 0, 100, 80)});
		make({.title = L"tooltip", .exStyle = WS_EX_TOOLWINDOW});
		make({.title = L"overlay", .exStyle = WS_EX_NOACTIVATE});
		make({.className = L"Shell_TrayWnd", .title = L"Taskbar"});
		make({.className = L"Progman", .title = L"Program Manager"});
		(void) listed;

		QCOMPARE(titles(window_stack::snapshot(fixtureHooks())), (QStringList {"listed"}));
	}

	void listsWhatAnApplicationForcesIntoTheTaskbar() {
		make({.title = L"forced", .exStyle = WS_EX_TOOLWINDOW | WS_EX_APPWINDOW});
		QCOMPARE(titles(window_stack::snapshot(fixtureHooks())), (QStringList {"forced"}));
	}

	// The dialog is above its owner, so a hit test finds the dialog first.
	void listsAnOwnedDialogAboveItsOwner() {
		auto* owner = make({.title = L"owner", .rect = QRect(0, 0, 400, 300)});
		make({.title = L"dialog", .rect = QRect(100, 100, 100, 80), .parent = owner});

		QCOMPARE(titles(window_stack::snapshot(fixtureHooks())), (QStringList {"dialog", "owner"}));
	}

	void neverListsTheProcessOwnWindows() {
		make({.title = L"ours"});
		QVERIFY(window_stack::snapshot().isEmpty());
	}

private:
	HWND make(const Spec& spec) {
		auto* hwnd = create(spec);
		if (hwnd != nullptr) m_windows.append(hwnd);
		return hwnd;
	}

	QList<HWND> m_windows;
	HWND m_anchor = nullptr;
};

int main(int argc, char** argv) {
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
	// The shell is per-monitor aware. Without this the process is DPI-virtualized,
	// window coordinates are scaled and DWM's frame bounds (always physical) would
	// not match the rects the fixture creates.
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	auto name = QStringLiteral("qs-capture-%1").arg(GetCurrentProcessId()).toStdWString();
	auto* original = GetThreadDesktop(GetCurrentThreadId());
	auto* desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
	if (desktop == nullptr) {
		std::fprintf(stderr, "CreateDesktop failed: %lu\n", GetLastError());
		return 2;
	}
	if (!SetThreadDesktop(desktop)) {
		std::fprintf(stderr, "SetThreadDesktop failed: %lu\n", GetLastError());
		return 2;
	}

	int result = 0;
	{
		QCoreApplication app(argc, argv);
		CaptureNativeTest test;
		result = QTest::qExec(&test, argc, argv);
	}

	SetThreadDesktop(original);
	CloseDesktop(desktop);
	return result;
}

#include "capture_native_test.moc"

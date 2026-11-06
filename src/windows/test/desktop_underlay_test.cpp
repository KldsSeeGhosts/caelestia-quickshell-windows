// #146: native layering policy, without Explorer, process discovery, config
// files or the input desktop. Two screen-sized fallback children and a real
// foreign-process child share a synthetic host on a private, never-switched
// desktop. HWND order is deliberately not called rendered-frame readiness.
#include <cstdio>
#include <cstdlib>

#include <qcoreapplication.h>
#include <qprocess.h>
#include <qtest.h>
#include <qt_windows.h>

#include "appbar.hpp"
#include "desktop_host.hpp"

namespace qs::windows {
UINT WinAppBar::taskbarCreatedMessage() {
	return RegisterWindowMessageW(L"TaskbarCreated");
}
} // namespace qs::windows
namespace qs::log {
void initLogCategoryLevel(const char*, QtMsgType) {}
} // namespace qs::log

namespace {
constexpr auto CLASS_NAME = L"QuickshellUnderlayPolicyTest";
constexpr UINT SET_VISIBLE = WM_APP + 1;

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	if (msg == SET_VISIBLE) {
		ShowWindow(hwnd, wp ? SW_SHOWNA : SW_HIDE);
		return 0;
	}
	if (msg == WM_DESTROY) PostQuitMessage(0);
	return DefWindowProcW(hwnd, msg, wp, lp);
}

void registerClass() {
	WNDCLASSW cls {};
	cls.lpfnWndProc = windowProc;
	cls.hInstance = GetModuleHandleW(nullptr);
	cls.lpszClassName = CLASS_NAME;
	// An opaque placeholder is legal before an external renderer presents.
	cls.hbrBackground = GetSysColorBrush(COLOR_DESKTOP);
	RegisterClassW(&cls);
}

HWND create(HWND parent, int x, int width) {
	return CreateWindowExW(
	    WS_EX_NOACTIVATE, CLASS_NAME, L"", WS_VISIBLE | WS_CLIPSIBLINGS
	        | (parent ? WS_CHILD : WS_POPUP),
	    x, 0, width, 100, parent, nullptr, GetModuleHandleW(nullptr), nullptr
	);
}

bool below(HWND parent, HWND window, HWND other) {
	for (auto* child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
		if (child == other) return true;
		if (child == window) return false;
	}
	return false;
}

int foreignMain(HWND parent) {
	registerClass();
	auto* window = create(parent, 0, 100);
	if (!window) return 2;
	std::printf("%lld\n", static_cast<long long>(reinterpret_cast<qintptr>(window)));
	std::fflush(stdout);
	MSG msg {};
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
	return 0;
}
} // namespace

class DesktopUnderlayTest: public QObject {
	Q_OBJECT;
public:
	QString desktopName;

private slots:
	void nativeOwnerOverridesReplaceWithoutDiscovery() {
		registerClass();
		auto* parent = create(nullptr, 0, 200);
		QVERIFY(parent);
		auto* left = create(parent, 0, 100);
		auto* right = create(parent, 100, 100);
		QVERIFY(left);
		QVERIFY(right);
		auto* host = qs::windows::DesktopHost::instance();
		host->setReconcileSourcesForTesting(false, false);
		host->adoptHostForTesting(parent);
		host->setAboveIcons({QStringLiteral("background")});
		QVERIFY(host->allowsAboveIcons(QStringLiteral("background")));
		QVERIFY(!host->allowsAboveIcons(QStringLiteral("other")));
		qs::windows::markDesktopBackground(left, true);
		qs::windows::markDesktopBackground(right, true);

		QProcess foreign;
		auto nativeDesktop = desktopName.toStdWString();
		foreign.setCreateProcessArgumentsModifier([&](QProcess::CreateProcessArguments* args) {
			args->startupInfo->lpDesktop = nativeDesktop.data();
		});
		foreign.start(QCoreApplication::applicationFilePath(), {
		    QStringLiteral("--foreign"), QString::number(reinterpret_cast<qintptr>(parent))
		});
		QVERIFY(foreign.waitForStarted());
		QTRY_VERIFY_WITH_TIMEOUT(foreign.canReadLine(), 5000);
		auto* external = reinterpret_cast<HWND>(foreign.readLine().trimmed().toLongLong());
		QVERIFY(IsWindow(external));
		QVERIFY(qs::windows::desktopHostHasForeignChild(parent, true));
		// No bridge/helper exists in this fixture. A requested Replace embed
		// must still not escape the underlay and cover the external window.
		QVERIFY(!host->allowsAboveIcons(QStringLiteral("background")));
		qs::windows::placeDesktopBackgrounds(parent);
		QVERIFY(below(parent, left, external));
		QVERIFY(below(parent, right, external));
		QVERIFY(!qs::windows::desktopBackgroundsMisordered(parent));
		auto moves = qs::windows::desktopBackgroundMovesForTesting();
		qs::windows::placeDesktopBackgrounds(parent);
		QCOMPARE(qs::windows::desktopBackgroundMovesForTesting(), moves);

		// The external child covers only the left region; host-wide order
		// does not invent a MonitorN mapping or per-monitor acknowledgement.
		RECT externalRect {}, rightRect {};
		QVERIFY(GetWindowRect(external, &externalRect));
		QVERIFY(GetWindowRect(right, &rightRect));
		RECT intersection {};
		QVERIFY(!IntersectRect(&intersection, &externalRect, &rightRect));
		auto* leftDc = GetDCEx(left, nullptr, DCX_CACHE | DCX_CLIPSIBLINGS);
		QVERIFY(leftDc);
		auto leftExposed = PtVisible(leftDc, 50, 50);
		ReleaseDC(left, leftDc);
		auto* rightDc = GetDCEx(right, nullptr, DCX_CACHE | DCX_CLIPSIBLINGS);
		QVERIFY(rightDc);
		auto rightExposed = PtVisible(rightDc, 50, 50);
		ReleaseDC(right, rightDc);
		QVERIFY(!leftExposed);
		QVERIFY(rightExposed);
		// Even before WM_PAINT, HWND presence says nothing about content:
		// both the opaque placeholder and later frames have this same order.
		QVERIFY(GetClassLongPtrW(external, GCLP_HBRBACKGROUND) != 0);

		SendMessageW(external, SET_VISIBLE, FALSE, 0);
		QVERIFY(!qs::windows::desktopHostHasForeignChild(parent, true));
		QVERIFY(host->allowsAboveIcons(QStringLiteral("background")));
		SendMessageW(external, SET_VISIBLE, TRUE, 0);
		QVERIFY(!host->allowsAboveIcons(QStringLiteral("background")));
		SendMessageW(external, WM_CLOSE, 0, 0);
		QVERIFY(foreign.waitForFinished());
		QVERIFY(host->allowsAboveIcons(QStringLiteral("background")));
		host->setEnabled(false);
		DestroyWindow(parent);
	}
};

int main(int argc, char** argv) {
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
	if (argc > 2 && qstrcmp(argv[1], "--foreign") == 0) {
		return foreignMain(reinterpret_cast<HWND>(static_cast<qintptr>(std::atoll(argv[2]))));
	}
	auto name = QStringLiteral("qs-underlay-%1").arg(GetCurrentProcessId());
	auto nativeName = name.toStdWString();
	auto* original = GetThreadDesktop(GetCurrentThreadId());
	auto* desktop = CreateDesktopW(nativeName.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
	if (!desktop || !SetThreadDesktop(desktop)) return 2;
	int result = 0;
	{
		QCoreApplication app(argc, argv);
		DesktopUnderlayTest test;
		test.desktopName = name;
		result = QTest::qExec(&test, argc, argv);
	}
	SetThreadDesktop(original);
	CloseDesktop(desktop);
	return result;
}

#include "desktop_underlay_test.moc"

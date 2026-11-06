// The foreground observer's raw event stream and its deduplicated display
// properties are checked against a fake tracker and fake window identities, so
// no live desktop, hook or foreground change is needed.

#include <qhash.h>
#include <qsignalspy.h>
#include <qstring.h>
#include <qtest.h>

#include "../foreground_observer.hpp"
#include "../system_surface.hpp"
#include "../util.hpp"
#include "../window_tracker.hpp"

namespace {

struct FakeSurface {
	QString title;
	QString process;
};

QHash<HWND, FakeSurface> fakeSurfaces;
HWND fakeOwnWindow = nullptr;
quint64 fakeSequence = 0;

HWND fakeHandle(quintptr value) { return reinterpret_cast<HWND>(value); }

void deliver(HWND hwnd, quint32 time, quint64 sequence) {
	fakeSequence = sequence;
	emit qs::windows::WindowTracker::instance()->foregroundEvent(hwnd, time, sequence);
}

} // namespace

namespace qs::windows {

// The observer only needs the tracker's foreground stream and lookup; the
// fixture supplies those without starting the real event hook.
WindowTracker::WindowTracker() = default;
WindowTracker::~WindowTracker() = default;

WindowTracker* WindowTracker::instance() {
	static WindowTracker tracker;
	return &tracker;
}

quint64 WindowTracker::eventSequence() const { return fakeSequence; }

bool isOwnProcessWindow(HWND hwnd) { return hwnd != nullptr && hwnd == fakeOwnWindow; }

namespace system_surface {

Identity observe(HWND hwnd) {
	Identity identity;
	const auto found = fakeSurfaces.constFind(hwnd);
	if (found == fakeSurfaces.constEnd()) return identity;

	identity.windowClass = QStringLiteral("Chrome_WidgetWin_1");
	identity.processName = found->process;
	identity.title = found->title;
	identity.processId = 2000;
	identity.root = true;
	identity.visible = true;
	identity.hasTitle = true;
	return identity;
}

bool isShellSurface(const Identity&) { return false; }

bool isDesktopSurface(const Identity&) { return false; }

} // namespace system_surface
} // namespace qs::windows

using namespace qs::windows;

class ForegroundObserverTest: public QObject {
	Q_OBJECT;

private slots:
	void rawEventIsEmittedForTheRetainedApplication();
};

void ForegroundObserverTest::rawEventIsEmittedForTheRetainedApplication() {
	const auto appA = fakeHandle(0xA001);
	const auto appB = fakeHandle(0xB002);
	const auto panel = fakeHandle(0xC003);
	fakeSurfaces.insert(appA, {QStringLiteral("Editor"), QStringLiteral("Code.exe")});
	fakeSurfaces.insert(appB, {QStringLiteral("Browser"), QStringLiteral("chrome.exe")});
	fakeOwnWindow = panel;

	Foreground foreground;
	QSignalSpy raw(&foreground, &Foreground::foregroundChanged);
	QSignalSpy address(&foreground, &Foreground::addressChanged);

	const auto addressA = QString::number(reinterpret_cast<quintptr>(appA), 16);

	deliver(appA, 100, 1);
	QCOMPARE(foreground.address(), addressA);
	QCOMPARE(foreground.title(), QStringLiteral("Editor"));
	QCOMPARE(foreground.appId(), QStringLiteral("code"));
	QCOMPARE(raw.count(), 1);
	QCOMPARE(address.count(), 1);

	// A second event for the same window keeps the display properties but is
	// still delivered: the guard needs every raw foreground event.
	deliver(appA, 200, 2);
	QCOMPARE(foreground.address(), addressA);
	QCOMPARE(raw.count(), 2);
	QCOMPARE(address.count(), 1);

	// The shell's own panel keeps the retained application and is still a raw
	// event for a consumer that filters by process.
	deliver(panel, 300, 3);
	QCOMPARE(foreground.address(), addressA);
	QCOMPARE(raw.count(), 3);
	QCOMPARE(address.count(), 1);

	// Returning to the retained application is the event that was previously
	// swallowed. It must reach consumers with its HWND, tick and sequence.
	deliver(appA, 400, 4);
	QCOMPARE(foreground.address(), addressA);
	QCOMPARE(raw.count(), 4);
	QCOMPARE(address.count(), 1);
	QCOMPARE(raw.constLast().at(0).toULongLong(), static_cast<qulonglong>(reinterpret_cast<quintptr>(appA)));
	QCOMPARE(raw.constLast().at(1).toUInt(), 400u);
	QCOMPARE(raw.constLast().at(2).toULongLong(), 4ull);
	QCOMPARE(foreground.sequence(), 4ull);
}

QTEST_GUILESS_MAIN(ForegroundObserverTest)

#include "foreground_observer_test.moc"

#include <qtest.h>

#include "../hyprland/dispatcher_args.hpp"
#include "../hyprland/monitor_focus.hpp"
#include "../hyprland/window_cycle.hpp"
#include "../hyprland/pin_support.hpp"
#include "../desktop_contract.hpp"

using namespace qs::hyprland::ipc;

namespace {

// The pin dispatch seam is a template, so it can be exercised without a
// tracker, a desktop accessor or Composite.
struct PinFakeDesktops {
	bool support = true;
	bool pinned = false;
	bool result = true;
	int calls = 0;

	[[nodiscard]] bool pinSupported() const { return this->support; }
	[[nodiscard]] bool isWindowPinned(int) const { return this->pinned; }
	bool pinWindow(int, bool value) {
		this->calls++;
		if (!this->result) return false;
		this->pinned = value;
		return true;
	}
};

struct MonitorFakeWindow {
	int monitor = 0;
	bool iconic = false;
	bool cloak = false;
	bool onCurrent = true;
	bool accepted = true;
	int popup = 0;
	int calls = 0;
	int activatedPopup = 0;

	int screen() const { return monitor; }
	bool minimized() const { return iconic; }
	bool cloaked() const { return cloak; }
	bool activate() {
		++calls;
		activatedPopup = popup;
		return accepted;
	}
};

struct CycleFakeWindow: QObject {
	int id;
	bool eligible = true;
	bool accepted = true;
	int calls = 0;
	explicit CycleFakeWindow(int id): id(id) {}
	bool activate() { ++calls; return accepted; }
};

} // namespace

class DispatcherTest: public QObject {
	Q_OBJECT;

private slots:
	void parsesWorkspaceTargets_data();
	void parsesWorkspaceTargets();

	void resolvesWorkspaceTargets_data();
	void resolvesWorkspaceTargets();

	void parsesMoveToWorkspaceArguments_data();
	void parsesMoveToWorkspaceArguments();

	void parsesResizeArguments_data();
	void parsesResizeArguments();

	void resolvesResizeAxes_data();
	void resolvesResizeAxes();

	void parsesPinRequests_data();
	void parsesPinRequests();

	void dispatchesPinRequests();
	void reportsPinSupportAndFailures();
	void resolvesMonitorTargets_data();
	void resolvesMonitorTargets();
	void monitorIdsFollowTheLiveModel();
	void focusesEligibleMonitorWindow_data();
	void focusesEligibleMonitorWindow();
	void focusesLastFocusedNotTopmost();
	void immediateCommandResults();
	void cyclesWindows_data();
	void cyclesWindows();
	void skipsExpiredCycleWindows();
	void rejectsCornerOnlyMonitor();
	void parsesCycleRequests_data();
	void parsesCycleRequests();

	void windowPatternNeverMatchesWithAnInvalidRegex();
};

void DispatcherTest::rejectsCornerOnlyMonitor() {
	const QList<MonitorFocusTarget> monitors {
	    {0, "Source", {0, 0, 100, 100}}, {1, "Corner", {100, 100, 100, 100}}};
	QCOMPARE(resolveMonitorTarget("r", monitors, 0), qsizetype(-1));
	QCOMPARE(resolveMonitorTarget("d", monitors, 0), qsizetype(-1));
}

void DispatcherTest::cyclesWindows_data() {
	QTest::addColumn<bool>("backward");
	QTest::addColumn<int>("scenario");
	QTest::addColumn<QList<int>>("expected");
	QTest::newRow("forward wraps without MRU bounce") << false << 0 << QList<int> {2, 3, 1, 2};
	QTest::newRow("backward wraps without MRU bounce") << true << 0 << QList<int> {3, 2, 1, 3};
	QTest::newRow("ineligible candidate skipped") << false << 1 << QList<int> {3, 1, 3, 1};
	QTest::newRow("no foreground starts at first") << false << 2 << QList<int> {1, 2, 3, 1};
	QTest::newRow("no foreground backward starts at first") << true << 2 << QList<int> {1, 3, 2, 1};
	QTest::newRow("creation deferred until reset") << false << 3 << QList<int> {2, 3, 1, 2};
	QTest::newRow("external foreground resets snapshot") << false << 4 << QList<int> {2, 2, 1, 3};
	QTest::newRow("desktop change resets snapshot") << false << 5 << QList<int> {2, 1, 3, 2};
	QTest::newRow("direction can reverse") << false << 6 << QList<int> {2, 1, 2, 1};
}

void DispatcherTest::cyclesWindows() {
	QFETCH(bool, backward);
	QFETCH(int, scenario);
	QFETCH(QList<int>, expected);
	CycleFakeWindow a(1), b(2), c(3), added(4);
	QList<CycleFakeWindow*> windows {&a, &b, &c};
	if (scenario == 1) b.eligible = false;
	auto* foreground = scenario == 2 ? nullptr : &a;
	WindowCycle<CycleFakeWindow> cycle;
	qsizetype desktop = 0;
	for (int step = 0; step < expected.size(); ++step) {
		if (step == 1) {
			// Activation can reorder the source list, but not an ongoing cycle.
			windows = {&b, &a, &c};
			if (scenario == 3) windows.prepend(&added);
			if (scenario == 4) foreground = &c;
			if (scenario == 5) ++desktop;
		}
		const auto result = cycle.cycle(
		    windows, foreground, desktop, scenario == 6 ? step % 2 != 0 : backward,
		    [](auto* window) { return window->eligible; });
		QCOMPARE(result, WindowCycleResult::Accepted);
		auto* target = expected[step] == 1 ? &a : expected[step] == 2 ? &b : &c;
		QCOMPARE(target->calls, 1);
		a.calls = b.calls = c.calls = added.calls = 0;
		foreground = target;
	}
}

void DispatcherTest::skipsExpiredCycleWindows() {
	CycleFakeWindow a(1), b(2), c(3);
	auto* destroyed = new CycleFakeWindow(4);
	QList<CycleFakeWindow*> windows {&a, &b, destroyed, &c};
	WindowCycle<CycleFakeWindow> cycle;
	auto eligible = [](auto* window) { return window->eligible; };
	QCOMPARE(cycle.cycle(windows, &a, 0, false, eligible), WindowCycleResult::Accepted);
	QCOMPARE(b.calls, 1);
	windows.removeOne(destroyed);
	delete destroyed;
	// Also reject a still-tracked but stale/ineligible native identity.
	c.eligible = false;
	QCOMPARE(cycle.cycle(windows, &b, 0, false, eligible), WindowCycleResult::Accepted);
	QCOMPARE(a.calls, 1);
	QCOMPARE(c.calls, 0);
	a.eligible = b.eligible = false;
	QCOMPARE(cycle.cycle(windows, &a, 0, false, eligible), WindowCycleResult::Stale);
	a.eligible = b.eligible = true;
	a.accepted = false;
	QCOMPARE(cycle.cycle(windows, &b, 0, false, eligible), WindowCycleResult::Denied);
	QCOMPARE(a.calls, 2);
	QCOMPARE(cycle.cycle({}, nullptr, 0, false, eligible), WindowCycleResult::Empty);
}

void DispatcherTest::parsesCycleRequests_data() {
	QTest::addColumn<bool>("lua");
	QTest::addColumn<QString>("argument");
	QTest::addColumn<QString>("key");
	QTest::addColumn<QString>("value");
	QTest::addColumn<int>("expected"); // -1 unsupported, 0 forward, 1 backward
	QTest::newRow("classic forward") << false << "" << "" << "" << 0;
	QTest::newRow("classic backward") << false << " prev " << "" << "" << 1;
	QTest::newRow("classic prev alias") << false << "p" << "" << "" << 1;
	QTest::newRow("classic tiled refused") << false << "tiled" << "" << "" << -1;
	QTest::newRow("classic floating refused") << false << "prev floating" << "" << "" << -1;
	QTest::newRow("classic history refused") << false << "hist" << "" << "" << -1;
	QTest::newRow("classic unknown refused") << false << "typo" << "" << "" << -1;
	QTest::newRow("lua default forward") << true << "" << "" << "" << 0;
	QTest::newRow("lua explicit forward") << true << "" << "next" << "true" << 0;
	QTest::newRow("lua backward") << true << "" << "next" << "false" << 1;
	QTest::newRow("lua malformed direction") << true << "" << "next" << "typo" << -1;
	QTest::newRow("lua tiled refused") << true << "" << "tiled" << "true" << -1;
	QTest::newRow("lua floating refused") << true << "" << "floating" << "false" << -1;
	QTest::newRow("lua selector refused") << true << "" << "window" << "activewindow" << -1;
	QTest::newRow("lua scalar refused") << true << "prev" << "" << "" << -1;
}

void DispatcherTest::parsesCycleRequests() {
	QFETCH(bool, lua);
	QFETCH(QString, argument);
	QFETCH(QString, key);
	QFETCH(QString, value);
	QFETCH(int, expected);
	QHash<QString, QString> table;
	if (!key.isEmpty()) table.insert(key, value);
	const auto direction = lua ? parseLuaCycle(argument, table, {}) : parseClassicCycle(argument);
	QCOMPARE(direction.has_value() ? int(*direction) : -1, expected);
	QVERIFY(!parseLuaCycle({}, {}, {"prev"}).has_value());
}

void DispatcherTest::resolvesMonitorTargets_data() {
	QTest::addColumn<QString>("target");
	QTest::addColumn<int>("focused");
	QTest::addColumn<int>("expected");
	QTest::newRow("unique name") << "Panel #2" << 0 << 1;
	QTest::newRow("duplicate model has distinct name") << "Panel" << 1 << 0;
	QTest::newRow("runtime id not list index") << "9" << 0 << 1;
	QTest::newRow("zero is not invented id") << "0" << 0 << -1;
	QTest::newRow("right greatest overlap") << "r" << 0 << 1;
	QTest::newRow("left negative origin") << "l" << 0 << 2;
	QTest::newRow("up mixed native sizes") << "u" << 0 << 3;
	QTest::newRow("down") << "d" << 3 << 0;
	QTest::newRow("direction no neighbor") << "d" << 0 << -1;
	QTest::newRow("next") << "+1" << 0 << 1;
	QTest::newRow("next wraps") << "+1" << 4 << 0;
	QTest::newRow("previous wraps") << "-1" << 0 << 4;
	QTest::newRow("large relative") << "+12" << 0 << 2;
	QTest::newRow("negative relative") << "-12" << 0 << 3;
	QTest::newRow("zero offset") << "+0" << 2 << 2;
	QTest::newRow("name without focus") << "Panel #2" << -1 << 1;
	QTest::newRow("direction without focus") << "r" << -1 << -1;
	QTest::newRow("relative without focus") << "+1" << -1 << -1;
	QTest::newRow("disconnected name") << "Removed panel" << 0 << -1;
	QTest::newRow("disconnected id") << "6" << 0 << -1;
	QTest::newRow("blank") << "" << 0 << -1;
	QTest::newRow("invalid relative") << "+x" << 0 << -1;
	QTest::newRow("overflow") << "+9223372036854775808" << 0 << -1;
	QTest::newRow("overflow id") << "9999999999999999" << 0 << -1;
	QTest::newRow("padding") << "  Panel #2  " << 0 << 1;
}

void DispatcherTest::resolvesMonitorTargets() {
	QFETCH(QString, target);
	QFETCH(int, focused);
	QFETCH(int, expected);
	const QList<MonitorFocusTarget> monitors {
	    {4, "Panel", {0, 0, 1920, 1080}},
	    {9, "Panel #2", {1920, 0, 2560, 1440}},
	    {2, "Left", {-1600, 0, 1600, 900}},
	    {12, "Above", {0, -1440, 1920, 1440}},
	    {20, "Short overlap", {1920, 900, 800, 600}},
	};
	QCOMPARE(resolveMonitorTarget(target, monitors, focused), qsizetype(expected));
}

void DispatcherTest::monitorIdsFollowTheLiveModel() {
	QList<MonitorFocusTarget> monitors {
	    {8, "First", {0, 0, 1920, 1080}}, {3, "Second", {1920, 0, 1920, 1080}}};
	std::swap(monitors[0], monitors[1]);
	QCOMPARE(resolveMonitorTarget("8", monitors, 0), qsizetype(1));
	QCOMPARE(resolveMonitorTarget("+1", monitors, 0), qsizetype(1));
	monitors.removeLast();
	QCOMPARE(resolveMonitorTarget("8", monitors, 0), qsizetype(-1));
	QCOMPARE(resolveMonitorTarget("First", monitors, 0), qsizetype(-1));
	QCOMPARE(resolveMonitorTarget("-1", monitors, 0), qsizetype(0));
	monitors.clear();
	QCOMPARE(resolveMonitorTarget("+1", monitors, 0), qsizetype(-1));
}

void DispatcherTest::focusesEligibleMonitorWindow_data() {
	QTest::addColumn<int>("scenario");
	QTest::addColumn<int>("activated"); // index, or -1 for no eligible window
	QTest::addColumn<int>("result"); // -1 empty, 0 denied, 1 accepted
	QTest::newRow("topmost eligible") << 0 << 1 << 1;
	QTest::newRow("minimized topmost skipped") << 1 << 2 << 1;
	QTest::newRow("cloaked topmost skipped") << 2 << 2 << 1;
	QTest::newRow("other desktop skipped") << 3 << 2 << 1;
	QTest::newRow("modal owner goes through existing activation") << 4 << 1 << 1;
	QTest::newRow("empty monitor only publishes focus") << 5 << -1 << -1;
	QTest::newRow("minimized only is empty") << 6 << -1 << -1;
	QTest::newRow("denied stops without trying lower window") << 7 << 1 << 0;
}

void DispatcherTest::focusesEligibleMonitorWindow() {
	QFETCH(int, scenario);
	QFETCH(int, activated);
	QFETCH(int, result);
	MonitorFakeWindow other {0}, top {1}, lower {1};
	QList<MonitorFakeWindow*> windows {&other, &top, &lower};
	switch (scenario) {
	case 1: top.iconic = true; break;
	case 2: top.cloak = true; break;
	case 3: top.onCurrent = false; break;
	case 4: top.popup = 42; break;
	case 5: top.monitor = lower.monitor = 0; break;
	case 6: top.iconic = lower.iconic = true; break;
	case 7: top.accepted = false; break;
	}
	auto focused = 0;
	auto publishes = 0;
	const auto outcome = focusMonitorWindows(
	    windows, static_cast<MonitorFakeWindow*>(nullptr), 1,
	    [](auto* window) { return window->onCurrent; }, [&]() { focused = 1; ++publishes; });
	QCOMPARE(outcome.has_value() ? int(*outcome) : -1, result);
	for (qsizetype i = 0; i < windows.size(); ++i) {
		QCOMPARE(windows[i]->calls, i == activated ? 1 : 0);
	}
	QCOMPARE(top.activatedPopup, scenario == 4 ? 42 : 0);
	QCOMPARE(focused, result == 0 ? 0 : 1);
	QCOMPARE(publishes, result == 0 ? 0 : 1);
	// The dispatch has no pending callback that can undo a later user choice.
	focused = 2;
	QCoreApplication::processEvents();
	QCOMPARE(focused, 2);
}

// An always-on-top window on the target monitor is topmost, but the user was
// working in the window last focused there: focusmonitor returns to that one.
void DispatcherTest::focusesLastFocusedNotTopmost() {
	MonitorFakeWindow pip {1}, editor {1}, other {0};
	QList<MonitorFakeWindow*> windows {&other, &pip, &editor}; // Z order, top first
	auto focused = 0;
	const auto outcome = focusMonitorWindows(
	    windows, &editor, 1, [](auto* window) { return window->onCurrent; },
	    [&]() { focused = 1; });
	QVERIFY(outcome.has_value() && *outcome);
	QCOMPARE(editor.calls, 1);
	QCOMPARE(pip.calls, 0);
	QCOMPARE(focused, 1);

	// No history on that monitor falls back to stacking order.
	MonitorFakeWindow top {2}, lower {2};
	QList<MonitorFakeWindow*> fresh {&top, &lower};
	const auto fallback = focusMonitorWindows(
	    fresh, static_cast<MonitorFakeWindow*>(nullptr), 2,
	    [](auto* window) { return window->onCurrent; }, []() {});
	QVERIFY(fallback.has_value() && *fallback);
	QCOMPARE(top.calls, 1);
	QCOMPARE(lower.calls, 0);

	// The last-focused window stopped being a candidate: the next one in
	// stacking order takes it, as before.
	MonitorFakeWindow gone {1}, next {1};
	gone.iconic = true;
	QList<MonitorFakeWindow*> stale {&next, &gone};
	const auto skipped = focusMonitorWindows(
	    stale, &gone, 1, [](auto* window) { return window->onCurrent; }, []() {});
	QVERIFY(skipped.has_value() && *skipped);
	QCOMPARE(next.calls, 1);
	QCOMPARE(gone.calls, 0);

	// A remembered window absent from the live enumeration is never promoted.
	MonitorFakeWindow absent {1}, live {1};
	QList<MonitorFakeWindow*> current {&live};
	const auto unlisted = focusMonitorWindows(
	    current, &absent, 1, [](auto* window) { return window->onCurrent; }, []() {});
	QVERIFY(unlisted.has_value() && *unlisted);
	QCOMPARE(live.calls, 1);
	QCOMPARE(absent.calls, 0);
}

void DispatcherTest::immediateCommandResults() {
	using namespace qs::windows;
	const auto accepted = desktopCommandMap(DesktopImmediateResult {
	    "focusmonitor", DesktopCommandOutcome::Accepted, {}});
	QCOMPARE(accepted.value("outcome").toString(), QStringLiteral("accepted"));
	QVERIFY(!accepted.contains("requestId"));
	const auto empty = desktopCommandMap(DesktopImmediateResult {
	    "focusmonitor", DesktopCommandOutcome::Observed, {}});
	QCOMPARE(empty.value("outcome").toString(), QStringLiteral("observed"));
	for (const auto* reason: {"invalid-target", "activation-denied", "window-enumeration-failed",
	                         "unsupported-options", "empty-scope", "stale-target"}) {
		const auto refused = desktopCommandMap(DesktopRefusal {"focusmonitor", reason});
		QCOMPARE(refused.value("operation").toString(), QStringLiteral("focusmonitor"));
		QCOMPARE(refused.value("outcome").toString(), QStringLiteral("denied"));
		QCOMPARE(refused.value("reason").toString(), QString::fromLatin1(reason));
	}
}

// `class:` / `title:` dispatcher selectors come from user keybinds. A pattern
// that is not a valid regular expression must not be evaluated as one.
void DispatcherTest::windowPatternNeverMatchesWithAnInvalidRegex() {
	auto valid = WindowPattern(QStringLiteral("^Editor.*$"));
	QVERIFY(valid.validRegex());
	QVERIFY(valid.matches(QStringLiteral("Editor - notes")));
	QVERIFY(!valid.matches(QStringLiteral("Terminal")));

	auto invalid = WindowPattern(QStringLiteral("Notes ["));
	QVERIFY(!invalid.validRegex());
	// Matches nothing as a regex, and does not match unrelated text.
	QVERIFY(!invalid.matches(QStringLiteral("Notes")));
	QVERIFY(!invalid.matches(QStringLiteral("Terminal")));
	// The literal spelling still selects a window titled exactly that.
	QVERIFY(invalid.matches(QStringLiteral("Notes [")));
}

void DispatcherTest::parsesWorkspaceTargets_data() {
	QTest::addColumn<QString>("argument");
	QTest::addColumn<int>("kind");
	QTest::addColumn<int>("index");
	QTest::addColumn<int>("delta");
	QTest::addColumn<QString>("name");

	auto number = static_cast<int>(WorkspaceTarget::Kind::Number);
	auto relative = static_cast<int>(WorkspaceTarget::Kind::Relative);
	auto previous = static_cast<int>(WorkspaceTarget::Kind::Previous);
	auto special = static_cast<int>(WorkspaceTarget::Kind::Special);
	auto name = static_cast<int>(WorkspaceTarget::Kind::Name);
	auto empty = static_cast<int>(WorkspaceTarget::Kind::Empty);
	auto invalid = static_cast<int>(WorkspaceTarget::Kind::Invalid);

	QTest::newRow("first desktop") << "1" << number << 0 << 0 << QString();
	QTest::newRow("tenth desktop") << "10" << number << 9 << 0 << QString();
	QTest::newRow("padded number") << " 3 " << number << 2 << 0 << QString();
	QTest::newRow("next") << "+1" << relative << -1 << 1 << QString();
	QTest::newRow("previous") << "-2" << relative << -1 << -2 << QString();
	QTest::newRow("r next") << "r+1" << relative << -1 << 1 << QString();
	QTest::newRow("r previous") << "r-1" << relative << -1 << -1 << QString();
	QTest::newRow("e next") << "e+1" << relative << -1 << 1 << QString();
	QTest::newRow("e previous") << "e-1" << relative << -1 << -1 << QString();
	QTest::newRow("m next") << "m+2" << relative << -1 << 2 << QString();
	QTest::newRow("previous target") << "previous" << previous << -1 << 0 << QString();
	QTest::newRow("per-monitor previous")
	    << "previous_per_monitor" << previous << -1 << 0 << QString();
	QTest::newRow("special name") << "special:sysmon" << special << -1 << 0 << QString();
	QTest::newRow("bare special") << "special" << special << -1 << 0 << QString();
	QTest::newRow("named") << "name:dev" << name << -1 << 0 << QStringLiteral("dev");
	QTest::newRow("empty") << "empty" << empty << -1 << 0 << QString();
	QTest::newRow("blank") << "" << invalid << -1 << 0 << QString();
	QTest::newRow("zero") << "0" << invalid << -1 << 0 << QString();
	QTest::newRow("negative absolute") << "-2" << relative << -1 << -2 << QString();
	QTest::newRow("text") << "desk" << invalid << -1 << 0 << QString();
	QTest::newRow("bad step") << "+x" << invalid << -1 << 0 << QString();
}

void DispatcherTest::parsesWorkspaceTargets() {
	QFETCH(QString, argument);
	QFETCH(int, kind);
	QFETCH(int, index);
	QFETCH(int, delta);
	QFETCH(QString, name);

	auto target = parseWorkspaceTarget(argument);
	QCOMPARE(static_cast<int>(target.kind), kind);
	QCOMPARE(static_cast<int>(target.index), index);
	QCOMPARE(static_cast<int>(target.delta), delta);
	QCOMPARE(target.name, name);
}

void DispatcherTest::resolvesWorkspaceTargets_data() {
	QTest::addColumn<QString>("argument");
	QTest::addColumn<int>("current");
	QTest::addColumn<int>("count");
	QTest::addColumn<int>("limit");
	QTest::addColumn<int>("previous");
	QTest::addColumn<int>("index");
	QTest::addColumn<bool>("create");

	QTest::newRow("next") << "+1" << 0 << 3 << 10 << -1 << 1 << false;
	QTest::newRow("previous") << "-1" << 0 << 3 << 10 << -1 << 0 << false;
	QTest::newRow("r creates") << "r+1" << 2 << 3 << 10 << -1 << 3 << true;
	QTest::newRow("r previous") << "r-1" << 2 << 3 << 10 << -1 << 1 << false;
	QTest::newRow("e is relative") << "e+1" << 1 << 4 << 10 << -1 << 2 << false;
	QTest::newRow("limit clamps") << "+9" << 2 << 3 << 5 << -1 << 4 << true;
	QTest::newRow("existing above limit") << "+9" << 4 << 5 << 5 << -1 << 4 << false;
	QTest::newRow("zero clamps") << "-9" << 1 << 3 << 5 << -1 << 0 << false;
	QTest::newRow("absolute") << "3" << 0 << 3 << 10 << -1 << 2 << true;
	QTest::newRow("absolute past count") << "8" << 0 << 3 << 10 << -1 << 7 << true;
	QTest::newRow("previous target") << "previous" << 2 << 3 << 10 << 0 << 0 << false;
	QTest::newRow("no previous") << "previous" << 2 << 3 << 10 << -1 << -1 << false;
	QTest::newRow("special") << "special:sysmon" << 2 << 3 << 10 << -1 << -1 << false;
	QTest::newRow("named") << "name:dev" << 2 << 3 << 10 << -1 << -1 << false;
	QTest::newRow("empty") << "empty" << 2 << 3 << 10 << -1 << -1 << false;
	QTest::newRow("invalid") << "desk" << 2 << 3 << 10 << -1 << -1 << false;
}

void DispatcherTest::resolvesWorkspaceTargets() {
	QFETCH(QString, argument);
	QFETCH(int, current);
	QFETCH(int, count);
	QFETCH(int, limit);
	QFETCH(int, previous);
	QFETCH(int, index);
	QFETCH(bool, create);

	auto target = parseWorkspaceTarget(argument);
	auto result = resolveWorkspaceTarget(target, current, count, limit, previous);
	QCOMPARE(static_cast<int>(result.index), index);
	QCOMPARE(result.create, create);
}

void DispatcherTest::parsesMoveToWorkspaceArguments_data() {
	QTest::addColumn<QString>("argument");
	QTest::addColumn<QString>("workspace");
	QTest::addColumn<QString>("window");

	QTest::newRow("plain") << "3" << "3" << QString();
	QTest::newRow("relative") << "+1" << "+1" << QString();
	QTest::newRow("home") << "special:music" << "special:music" << QString();
	QTest::newRow("selector") << "2,address:0x1234" << "2" << "address:0x1234";
	QTest::newRow("relative selector") << "r-1,class:spotify" << "r-1" << "class:spotify";
	QTest::newRow("spaces") << " 4 , address:0xab " << " 4 " << " address:0xab ";
}

void DispatcherTest::parsesMoveToWorkspaceArguments() {
	QFETCH(QString, argument);
	QFETCH(QString, workspace);
	QFETCH(QString, window);

	auto parsed = parseMoveToWorkspaceArgs(argument);
	QCOMPARE(parsed.workspace, workspace);
	QCOMPARE(parsed.window, window);
}

void DispatcherTest::parsesResizeArguments_data() {
	QTest::addColumn<QString>("argument");
	QTest::addColumn<bool>("valid");
	QTest::addColumn<bool>("exact");
	QTest::addColumn<bool>("widthValid");
	QTest::addColumn<bool>("widthPercent");
	QTest::addColumn<double>("width");
	QTest::addColumn<bool>("heightValid");
	QTest::addColumn<bool>("heightPercent");
	QTest::addColumn<double>("height");

	QTest::newRow("width only") << "10 0" << true << false << true << false << 10.0 << true << false
	                            << 0.0;
	QTest::newRow("height only") << "0 -10" << true << false << true << false << 0.0 << true << false
	                             << -10.0;
	QTest::newRow("percent") << "10% 20%" << true << false << true << true << 10.0 << true << true
	                         << 20.0;
	QTest::newRow("exact percent") << "exact 55% 70%" << true << true << true << true << 55.0 << true
	                               << true << 70.0;
	QTest::newRow("exact pixels") << "exact 800 600" << true << true << true << false << 800.0 << true
	                              << false << 600.0;
	QTest::newRow("negative fraction")
	    << "-0.5 0.25" << true << false << true << false << -0.5 << true << false << 0.25;
	QTest::newRow("missing height") << "10" << false << false << false << false << 0.0 << false
	                                << false << 0.0;
	QTest::newRow("only exact") << "exact" << false << true << false << false << 0.0 << false << false
	                            << 0.0;
	QTest::newRow("not numbers") << "a b" << false << false << false << false << 0.0 << false << false
	                             << 0.0;
	QTest::newRow("blank") << "" << false << false << false << false << 0.0 << false << false << 0.0;
	QTest::newRow("nan") << "nan 10" << false << false << false << false << 0.0 << true << false
	                     << 10.0;
	QTest::newRow("inf") << "inf 0" << false << false << false << false << 0.0 << true << false << 0.0;
	QTest::newRow("huge pixels") << "1e300 0" << false << false << false << false << 0.0 << true
	                             << false << 0.0;
	QTest::newRow("huge percent") << "100000000000% 0" << false << false << false << true << 0.0
	                              << true << false << 0.0;
}

void DispatcherTest::parsesResizeArguments() {
	QFETCH(QString, argument);
	QFETCH(bool, valid);
	QFETCH(bool, exact);
	QFETCH(bool, widthValid);
	QFETCH(bool, widthPercent);
	QFETCH(double, width);
	QFETCH(bool, heightValid);
	QFETCH(bool, heightPercent);
	QFETCH(double, height);

	auto parsed = parseResizeArguments(argument);
	QCOMPARE(parsed.valid, valid);
	QCOMPARE(parsed.exact, exact);
	QCOMPARE(parsed.width.valid, widthValid);
	QCOMPARE(parsed.width.percent, widthPercent);
	QCOMPARE(parsed.width.value, width);
	QCOMPARE(parsed.height.valid, heightValid);
	QCOMPARE(parsed.height.percent, heightPercent);
	QCOMPARE(parsed.height.value, height);
}

void DispatcherTest::resolvesResizeAxes_data() {
	QTest::addColumn<double>("value");
	QTest::addColumn<bool>("percent");
	QTest::addColumn<int>("whole");
	QTest::addColumn<int>("current");
	QTest::addColumn<bool>("exact");
	QTest::addColumn<int>("expected");

	QTest::newRow("relative pixels") << 10.0 << false << 1920 << 600 << false << 10;
	QTest::newRow("relative negative") << -10.0 << false << 1920 << 600 << false << -10;
	QTest::newRow("relative zero") << 0.0 << false << 1920 << 600 << false << 0;
	QTest::newRow("relative percent") << 10.0 << true << 1000 << 400 << false << 100;
	QTest::newRow("exact pixels") << 800.0 << false << 1920 << 500 << true << 800;
	QTest::newRow("exact percent") << 55.0 << true << 1920 << 800 << true << 1056;
	QTest::newRow("exact percent height") << 70.0 << true << 1080 << 500 << true << 756;
	QTest::newRow("exact zero keeps width") << 0.0 << false << 1920 << 800 << true << 800;
	QTest::newRow("exact zero percent keeps width") << 0.0 << true << 1920 << 800 << true << 800;
}

void DispatcherTest::resolvesResizeAxes() {
	QFETCH(double, value);
	QFETCH(bool, percent);
	QFETCH(int, whole);
	QFETCH(int, current);
	QFETCH(bool, exact);
	QFETCH(int, expected);

	ResizeValue parsed {true, percent, value};
	QCOMPARE(resizeAxis(parsed, whole, current, exact), expected);
}

void DispatcherTest::parsesPinRequests_data() {
	QTest::addColumn<QString>("argument");
	QTest::addColumn<int>("request");

	auto toggle = static_cast<int>(PinRequest::Toggle);
	auto pin = static_cast<int>(PinRequest::Pin);
	auto unpin = static_cast<int>(PinRequest::Unpin);

	QTest::newRow("empty") << "" << toggle;
	QTest::newRow("toggle") << "toggle" << toggle;
	QTest::newRow("on") << "on" << pin;
	QTest::newRow("upper on") << " ON " << pin;
	QTest::newRow("true") << "true" << pin;
	QTest::newRow("pin") << "pin" << pin;
	QTest::newRow("off") << "off" << unpin;
	QTest::newRow("false") << "false" << unpin;
	QTest::newRow("unpin") << "unpin" << unpin;
	QTest::newRow("selector is a toggle") << "address:0x1234" << toggle;
}

void DispatcherTest::parsesPinRequests() {
	QFETCH(QString, argument);
	QFETCH(int, request);

	QCOMPARE(static_cast<int>(parsePinRequest(argument)), request);
}

void DispatcherTest::dispatchesPinRequests() {
	PinFakeDesktops desktops;

	QCOMPARE(
	    static_cast<int>(dispatchPin(desktops, 1)),
	    static_cast<int>(PinDispatchResult::Succeeded)
	);
	QVERIFY(desktops.pinned);
	QCOMPARE(desktops.calls, 1);

	// Requesting the current state is idempotent and does not touch the accessor.
	QCOMPARE(
	    static_cast<int>(dispatchPin(desktops, 1, PinRequest::Pin)),
	    static_cast<int>(PinDispatchResult::Succeeded)
	);
	QCOMPARE(desktops.calls, 1);

	QCOMPARE(
	    static_cast<int>(dispatchPin(desktops, 1, PinRequest::Unpin)),
	    static_cast<int>(PinDispatchResult::Succeeded)
	);
	QVERIFY(!desktops.pinned);
	QCOMPARE(desktops.calls, 2);

	QCOMPARE(
	    static_cast<int>(dispatchPin(desktops, 1, PinRequest::Toggle)),
	    static_cast<int>(PinDispatchResult::Succeeded)
	);
	QVERIFY(desktops.pinned);
	QCOMPARE(desktops.calls, 3);
}

void DispatcherTest::reportsPinSupportAndFailures() {
	PinFakeDesktops desktops;
	desktops.support = false;
	QCOMPARE(
	    static_cast<int>(dispatchPin(desktops, 1)),
	    static_cast<int>(PinDispatchResult::Unsupported)
	);
	QCOMPARE(desktops.calls, 0);

	desktops.support = true;
	desktops.result = false;
	QCOMPARE(static_cast<int>(dispatchPin(desktops, 1)), static_cast<int>(PinDispatchResult::Failed));
	QCOMPARE(desktops.calls, 1);
	QVERIFY(!desktops.pinned);
}

QTEST_GUILESS_MAIN(DispatcherTest)

#include "dispatcher_test.moc"

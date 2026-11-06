#include <qcoreapplication.h>
#include <qjsondocument.h>
#include <qsignalspy.h>
#include <qtest.h>

// The fixture opens HotkeyManager's private members before anything else
// includes hotkeys.hpp; hotkeys.cpp brings in hotkey_defaults.hpp.
#include "hotkeys_routing_fixture.hpp"
#include "../keyboard_hook.hpp"

using qs::windows::hotkeys::HookTrigger;
using qs::windows::hotkeys::ModAlt;
using qs::windows::hotkeys::ModCtrl;
using qs::windows::hotkeys::ModShift;
using qs::windows::hotkeys::ModSuper;
using qs::windows::hotkeys::planModifierRelease;
using qs::windows::hotkeys::resolveTriggerMechanism;
using qs::windows::hotkeys::shouldRegisterHotkey;
using qs::windows::hotkeys::supervisorOwnsChord;
using qs::windows::hotkeys::swallowModifierRelease;

class RoutingShortcut: public QObject {
	Q_OBJECT;

public:
	explicit RoutingShortcut(const QString& name) {
		this->setProperty("appid", "quickshell");
		this->setProperty("name", name);
	}
	int presses = 0;
	int releases = 0;
	std::optional<qs::windows::ActivationSource> source;

public slots:
	void pressed() {
		this->presses++;
		this->source = qs::windows::currentActivationSource();
	}
	void released() { this->releases++; }
};

class HotkeysRoutingTest: public QObject {
	Q_OBJECT;

private slots:
	void cleanup() {
		auto* manager = qs::windows::hotkeys::HotkeyManager::instance();
		manager->unregisterAll();
		manager->binds.clear();
		hotkey_fixture::registerChords = false;
		hotkey_fixture::canStart = true;
	}

	void submapMembershipRoutesOnlyTheGlobalMapInitially() {
		using namespace qs::windows::hotkeys;
		auto* manager = HotkeyManager::instance();
		manager->unregisterAll();
		manager->parse(R"({"binds":[
			{"keys":"Super+R","action":"global","name":"enter"},
			{"keys":"H","submap":"resize","action":"global","name":"resize"}
		]})", "fixture");
		manager->registerAll();
		QCOMPARE(manager->triggers.size(), 1);
		QCOMPARE(hotkey_fixture::snapshot->triggers.size(), size_t(1));
		manager->unregisterAll();
	}

	void submapWindowCallbacksDeliverOnlyTheSelectedBinds() {
		using namespace qs::windows::hotkeys;
		auto* manager = HotkeyManager::instance();
		manager->parse(R"({"binds":[
			{"keys":"Super+R","action":"global","name":"global"},
			{"keys":"H","submap":"resize","action":"global","name":"resize"},
			{"keys":"H","submap":"move","action":"global","name":"move"},
			{"keys":"Ctrl","submap":"resize","onRelease":true,"action":"global","name":"tap"}
		]})", "fixture");
		RoutingShortcut global("global"), resize("resize"), move("move"), tap("tap");
		Hotkeys qml(nullptr);
		QSignalSpy submapChanges(&qml, &Hotkeys::activeSubmapChanged);
		QVERIFY(!qml.metaObject()->property(qml.metaObject()->indexOfProperty("activeSubmap")).isWritable());
		for (auto* shortcut: {&global, &resize, &move, &tap}) manager->registerShortcut(shortcut);
		manager->registerAll();
		auto deliver = [&](HookEvent event, quint32 id, quint32 serial) {
			HotkeyManager::messageWindowProc(
			    manager->messageWindow, WM_APP + 16 + event, id, serial);
		};
		const auto oldSerial = manager->serial;
		deliver(HookPressed, 0, oldSerial);
		QCOMPARE(global.presses, 1);
		QVERIFY(manager->setSubmap("resize"));
		QCOMPARE(global.releases, 1); // No held GlobalShortcut leaks across modes.
		QCOMPARE(manager->activeSubmap(), QString("resize"));
		QCOMPARE(qml.activeSubmap(), QString("resize"));
		QCOMPARE(submapChanges.size(), 1);
		QCOMPARE(manager->triggers.size(), 2);
		QCOMPARE(hotkey_fixture::snapshot->triggers.size(), size_t(2));
		deliver(HookPressed, 0, oldSerial); // Queued event from the old map.
		QCOMPARE(resize.presses, 0);
		deliver(HookPressed, 0, manager->serial);
		deliver(HookReleased, 0, manager->serial);
		deliver(HookTapped, 1, manager->serial);
		QCOMPARE(resize.presses, 1);
		QCOMPARE(resize.releases, 1);
		QCOMPARE(tap.presses, 1);
		QCOMPARE(tap.releases, 1);
		QCOMPARE(tap.source, std::optional(qs::windows::ActivationSource::Hook));
		auto refusal = QString();
		QVERIFY(!manager->setSubmap("missing", &refusal));
		QCOMPARE(refusal, QString("invalid-target"));
		QCOMPARE(manager->activeSubmap(), QString("resize"));
		QVERIFY(manager->setSubmap("move"));
		deliver(HookPressed, 0, manager->serial);
		QCOMPARE(move.presses, 1);
		QCOMPARE(resize.presses, 1);
		QVERIFY(manager->setSubmap("reset"));
		QVERIFY(manager->activeSubmap().isEmpty());
		QCOMPARE(move.releases, 1);
		QCOMPARE(manager->triggers.size(), 1);
	}

	void submapRegisteredHotkeysAndLifecycleUseTheSameWindowCallback() {
		using namespace qs::windows::hotkeys;
		auto* manager = HotkeyManager::instance();
		hotkey_fixture::registerChords = true;
		manager->parse(R"({"binds":[
			{"keys":"Ctrl+F10","action":"global","name":"global"},
			{"keys":"H","submap":"resize","action":"global","name":"resize"}
		]})", "fixture");
		RoutingShortcut resize("resize");
		manager->registerShortcut(&resize);
		manager->registerAll();
		QVERIFY(manager->setSubmap("resize"));
		QVERIFY(manager->hookActive()); // Desktop reset even with only WM_HOTKEY binds.
		QCOMPARE(manager->triggers.at(0).mechanism, Bind::Mechanism::Hotkey);
		HotkeyManager::messageWindowProc(manager->messageWindow, WM_HOTKEY, 1, 0);
		QCOMPARE(resize.presses, 1);
		QCOMPARE(resize.source, std::optional(qs::windows::ActivationSource::Hotkey));
		HotkeyManager::messageWindowProc(
		    manager->messageWindow, WM_APP + 16 + HookReset, 0, manager->serial);
		QVERIFY(manager->activeSubmap().isEmpty());
		QCOMPARE(resize.releases, 1);
		QVERIFY(manager->setSubmap("resize"));
		manager->reload(); // No config: reload must still cancel the selected map.
		QVERIFY(manager->activeSubmap().isEmpty());
	}

	void submapConflictsStayLocalAndEscapeIsNotImplicit() {
		using namespace qs::windows::hotkeys;
		auto* manager = HotkeyManager::instance();
		manager->parse(R"({"binds":[
			{"keys":"H","action":"global","name":"global"},
			{"keys":"H","submap":"resize","action":"global","name":"one"},
			{"keys":"H","submap":"resize","action":"global","name":"two"},
			{"keys":"Escape","submap":"move","action":"dispatch","dispatch":"submap reset"}
		]})", "fixture");
		manager->registerAll();
		QVERIFY(manager->setSubmap("resize"));
		QCOMPARE(manager->triggers.size(), 1);
		QCOMPARE(manager->triggers.at(0).binds.size(), 2); // Existing multi-action order.
		QCOMPARE(manager->triggers.at(0).combo.vk, uint8_t('H'));
		QVERIFY(manager->setSubmap("move"));
		QCOMPARE(manager->triggers.at(0).combo.vk, uint8_t(VK_ESCAPE));
		QVERIFY(manager->setSubmap(""));
		QVERIFY(manager->activeSubmap().isEmpty());
	}

	void submapEntryRefusesWithoutLifecycleNotifications() {
		using namespace qs::windows::hotkeys;
		auto* manager = HotkeyManager::instance();
		hotkey_fixture::registerChords = true;
		hotkey_fixture::canStart = false;
		manager->parse(R"({"binds":[
			{"keys":"Ctrl+F10","action":"global","name":"global"},
			{"keys":"H","submap":"resize","action":"global","name":"resize"}
		]})", "fixture");
		manager->registerAll();
		auto refusal = QString();
		QVERIFY(!manager->setSubmap("resize", &refusal));
		QCOMPARE(refusal, QString("operation-unsupported"));
		QVERIFY(manager->activeSubmap().isEmpty());
		QCOMPARE(manager->triggers.size(), 1);
		QCOMPARE(manager->triggers.at(0).combo.vk, uint8_t(VK_F10));
	}

	void hookTappedDeliversEveryModifierTapCallback() {
		// The hook's onModifier/postAll path delivers a lone modifier tap with
		// HookTapped. keyboard_hook_test proves the post; this proves the
		// callback it lands on, for every modifier, so a dropped or misrouted
		// tap message cannot pass while only the tap decision is tested.
		using namespace qs::windows::hotkeys;
		auto* manager = HotkeyManager::instance();
		manager->parse(R"({"binds":[
			{"keys":"Super","onRelease":true,"action":"global","name":"super"},
			{"keys":"Ctrl","onRelease":true,"action":"global","name":"ctrl"},
			{"keys":"Shift","onRelease":true,"action":"global","name":"shift"},
			{"keys":"Alt","onRelease":true,"action":"global","name":"alt"}
		]})", "fixture");
		RoutingShortcut super("super"), ctrl("ctrl"), shift("shift"), alt("alt");
		for (auto* shortcut: {&super, &ctrl, &shift, &alt}) manager->registerShortcut(shortcut);
		manager->registerAll();
		QCOMPARE(manager->triggers.size(), 4);
		auto deliver = [&](HookEvent event, quint32 id, quint32 serial) {
			HotkeyManager::messageWindowProc(
			    manager->messageWindow, WM_APP + 16 + event, id, serial);
		};

		// A stale serial (the map changed under the hook) and a trigger index
		// that no longer exists are both dropped.
		deliver(HookTapped, 0, manager->serial - 1);
		deliver(HookTapped, 99, manager->serial);
		QCOMPARE(super.presses, 0);
		QCOMPARE(ctrl.presses, 0);
		QCOMPARE(shift.presses, 0);
		QCOMPARE(alt.presses, 0);

		// The tap bind fires from HookTapped alone: a press from the same
		// trigger must not deliver it, so a dropped tap post is observable.
		for (quint32 id = 0; id < 4; ++id) deliver(HookPressed, id, manager->serial);
		QCOMPARE(super.presses, 0);
		QCOMPARE(ctrl.presses, 0);
		QCOMPARE(shift.presses, 0);
		QCOMPARE(alt.presses, 0);

		for (quint32 id = 0; id < 4; ++id) deliver(HookTapped, id, manager->serial);
		for (auto* shortcut: {&super, &ctrl, &shift, &alt}) {
			QCOMPARE(shortcut->presses, 1);
			QCOMPARE(shortcut->releases, 1);
			QCOMPARE(shortcut->source, std::optional(qs::windows::ActivationSource::Hook));
		}
		// A repeated tap message is a second tap, not a lost one.
		deliver(HookTapped, 1, manager->serial);
		QCOMPARE(ctrl.presses, 2);
		QCOMPARE(super.presses, 1);
	}

	void legacyDefaultsPreserveUserChordsAndShortcutIdentity() {
		using qs::windows::hotkeys::Bind;
		using qs::windows::hotkeys::mergeDefaults;
		auto global = [](uint8_t modifiers, uint8_t vk, const QString& name) {
			Bind bind;
			bind.action = Bind::Action::Global;
			bind.combo.mods = modifiers;
			bind.combo.vk = vk;
			bind.appid = "caelestia";
			bind.name = name;
			return bind;
		};
		const QList<Bind> defaults {
		    global(0, VK_SNAPSHOT, "screenshot"),
		    global(ModSuper | ModShift, 'S', "screenshotFreeze"),
		    global(ModSuper | ModShift, 'R', "record"),
		    global(ModSuper, 'K', "showall"),
		};
		// Raw file names must follow the same normalization as parsed binds.
		// Previously all three colon-prefixed since defaults were skipped.
		const auto introduced = qs::windows::hotkeys::introducedDefaults(
		    QJsonDocument::fromJson(R"({"binds":[
		        {"name":"caelestia:screenshot","since":"1"},
		        {"name":"caelestia:screenshotFreeze","since":"1"},
		        {"name":"caelestia:record","since":"1"},
		        {"name":"caelestia:showall"}
		    ]})").object().value("binds").toArray());
		const auto launcher = global(0, VK_SNAPSHOT, "launcher");
		QList<Bind> binds {launcher};
		mergeDefaults(binds, defaults, introduced);
		QCOMPARE(binds.size(), 3);
		QCOMPARE(binds.at(0).name, launcher.name);
		QCOMPARE(binds.at(0).combo, launcher.combo);
		QCOMPARE(binds.at(1).name, "screenshotFreeze");
		QCOMPARE(binds.at(2).name, "record");
		mergeDefaults(binds, defaults, introduced);
		QCOMPARE(binds.size(), 3); // No duplicate on another merge.

		binds.clear();
		mergeDefaults(binds, defaults, introduced);
		QCOMPARE(binds.size(), 3); // Only defaults carrying a since marker.
		for (qsizetype i = 0; i < binds.size(); i++) {
			QCOMPARE(binds.at(i).combo, defaults.at(i).combo);
		}

		for (auto action: {Bind::Action::Global, Bind::Action::Native}) {
			auto screenshot = global(ModCtrl | ModAlt, 'S', "screenshot");
			screenshot.action = action;
			binds = {screenshot};
			mergeDefaults(binds, defaults, introduced);
			QCOMPARE(binds.size(), 3);
			QCOMPARE(binds.at(0).combo, screenshot.combo);
			QCOMPARE(binds.at(0).action, action);
			QCOMPARE(binds.at(1).name, "screenshotFreeze");
			QCOMPARE(binds.at(2).name, "record");
		}
		// A different application or a non-shortcut action does not reserve
		// Caelestia's shortcut name on another chord.
		auto other = global(ModCtrl | ModAlt, 'S', "screenshot");
		other.appid = "other";
		binds = {other};
		mergeDefaults(binds, defaults, introduced);
		QCOMPARE(binds.size(), 4);
		other.appid = "caelestia";
		other.action = Bind::Action::Exec;
		binds = {other};
		mergeDefaults(binds, defaults, introduced);
		QCOMPARE(binds.size(), 4);

		// Explicit appid and colon-prefixed names identify the same shortcut;
		// another app's since marker must not introduce Caelestia's shortcut.
		const auto explicitApp = qs::windows::hotkeys::introducedDefaults(
		    QJsonArray {QJsonObject {{"appid", "caelestia"}, {"name", "screenshot"}, {"since", "1"}}});
		binds.clear();
		mergeDefaults(binds, defaults, explicitApp);
		QCOMPARE(binds.size(), 1);
		QCOMPARE(binds.first().name, "screenshot");
		const auto otherApp = qs::windows::hotkeys::introducedDefaults(
		    QJsonArray {QJsonObject {{"name", "other:screenshot"}, {"since", "1"}}});
		binds.clear();
		mergeDefaults(binds, defaults, otherApp);
		QVERIFY(binds.isEmpty());

		// A user bind on the same chord inside a submap does not reserve the
		// global default.
		auto inSubmap = global(0, VK_SNAPSHOT, "resizeGrow");
		inSubmap.submap = "resize";
		binds = {inSubmap};
		mergeDefaults(binds, defaults, introduced);
		QCOMPARE(binds.size(), 4);
		QCOMPARE(binds.at(1).name, "screenshot");
	}

	void superChordStaysOnTheHookByDefault() {
		// A chord with the Windows key keeps its original routing: the hook
		// consumes it before the desktop supervisor, and refuses it over an
		// elevated foreground app.
		QVERIFY(!shouldRegisterHotkey(HookTrigger::Combo, ModCtrl | ModSuper, 'I', false));
		QVERIFY(!shouldRegisterHotkey(HookTrigger::Combo, ModSuper, '5', false));
		QVERIFY(!shouldRegisterHotkey(HookTrigger::Combo, ModSuper, 'B', false));

		auto mechanism = resolveTriggerMechanism(HookTrigger::Combo, ModCtrl | ModSuper, false);

		QVERIFY(!mechanism.hotkey);
		QVERIFY(mechanism.hook);
		QVERIFY(!mechanism.mask);
	}

	void registerOptInRegistersASuperChord() {
		// Regression: an opted-in Ctrl+Super+I chord arrived as a private hook message,
		// which carries no foreground permission, so the window it opened stayed
		// behind the focused application. An opted-in chord goes to
		// RegisterHotKey so Windows delivers WM_HOTKEY.
		QVERIFY(shouldRegisterHotkey(HookTrigger::Combo, ModCtrl | ModSuper, 'I', true));

		auto mechanism = resolveTriggerMechanism(HookTrigger::Combo, ModCtrl | ModSuper, true);

		QVERIFY(mechanism.hotkey);
		QVERIFY(!mechanism.hook);
		// RegisterHotKey consumes the chord, so the hook only adds the masking
		// tap that keeps the Start menu closed on the Windows key release.
		QVERIFY(mechanism.mask);
	}

	void refusedOptInFallsBackToTheHook() {
		// Windows reserves some Win+<Key> combinations; RegisterHotKey refuses
		// them and the hook must keep overriding them.
		auto mechanism = resolveTriggerMechanism(HookTrigger::Combo, ModSuper, false);

		QVERIFY(!mechanism.hotkey);
		QVERIFY(mechanism.hook);
		QVERIFY(!mechanism.mask);
	}

	void optInNeverRegistersWhatTheSupervisorConsumes() {
		// With hideTaskbar on, the supervisor's keyboard filter swallows these
		// before Windows could deliver WM_HOTKEY, so they must stay on the hook
		// even when a bind asks for registration.
		QVERIFY(supervisorOwnsChord(ModSuper, '1'));
		QVERIFY(supervisorOwnsChord(ModSuper, '9'));
		QVERIFY(supervisorOwnsChord(ModSuper | ModShift, '3'));
		QVERIFY(supervisorOwnsChord(ModSuper | ModCtrl, '3'));
		QVERIFY(supervisorOwnsChord(ModSuper | ModAlt, '3'));
		QVERIFY(supervisorOwnsChord(ModSuper, 'B'));
		QVERIFY(supervisorOwnsChord(ModSuper, 'T'));
		QVERIFY(supervisorOwnsChord(ModSuper | ModShift, 'T'));
		QVERIFY(supervisorOwnsChord(ModSuper | ModAlt, VK_RETURN));

		QVERIFY(!shouldRegisterHotkey(HookTrigger::Combo, ModSuper, '1', true));
		QVERIFY(!shouldRegisterHotkey(HookTrigger::Combo, ModSuper, 'B', true));
		QVERIFY(!shouldRegisterHotkey(HookTrigger::Combo, ModSuper | ModAlt, '2', true));
	}

	void supervisorSetMatchesItsExactModifierRules() {
		// Mirrors TaskbarShortcutPolicy.Matches: an extra modifier leaves the
		// chord alone.
		QVERIFY(!supervisorOwnsChord(ModSuper | ModCtrl | ModShift, '1'));
		QVERIFY(!supervisorOwnsChord(ModSuper | ModAlt | ModShift, '1'));
		QVERIFY(!supervisorOwnsChord(ModSuper | ModCtrl, 'B'));
		QVERIFY(!supervisorOwnsChord(ModSuper | ModCtrl, 'T'));
		QVERIFY(!supervisorOwnsChord(ModSuper, VK_RETURN));
		QVERIFY(!supervisorOwnsChord(ModCtrl | ModSuper, 'I'));
		QVERIFY(!supervisorOwnsChord(ModSuper | ModShift, 'N'));
		// Not a Windows chord at all.
		QVERIFY(!supervisorOwnsChord(ModCtrl, '1'));
		QVERIFY(!supervisorOwnsChord(0, 'T'));
	}

	void plainChordRegistersWithoutOptIn() {
		// Chords without the Windows key were always registered first.
		QVERIFY(shouldRegisterHotkey(HookTrigger::Combo, ModCtrl | ModShift, 'K', false));

		auto mechanism = resolveTriggerMechanism(HookTrigger::Combo, ModCtrl | ModShift, true);

		QVERIFY(mechanism.hotkey);
		QVERIFY(!mechanism.hook);
		QVERIFY(!mechanism.mask);
	}

	void loneModifierTapAndHoldAlwaysUseTheHook() {
		QVERIFY(!shouldRegisterHotkey(HookTrigger::Tap, ModSuper, 0, true));
		QVERIFY(!shouldRegisterHotkey(HookTrigger::Hold, ModSuper, 0, true));

		for (auto kind: {HookTrigger::Tap, HookTrigger::Hold}) {
			auto mechanism = resolveTriggerMechanism(kind, ModSuper, true);

			QVERIFY(!mechanism.hotkey);
			QVERIFY(mechanism.hook);
			QVERIFY(!mechanism.mask);
		}
	}

	void swallowedSuperChordMasksItsWinRelease() {
		// Regression: Win+V opened Explorer's clipboard flyout next to the
		// shell's picker even though the hook had already swallowed V. The
		// physical Win-up completes the key sequence for the system, so a
		// swallowed Super chord must end like a lone Super tap: mask the
		// release, replay it and keep the physical one.
		auto plan = planModifierRelease(ModSuper, true, false, false, false, false);

		QVERIFY(plan.maskAndReplay);
		QVERIFY(!plan.postTap);

		// A swallowed chord wins over a stale tap candidate: the launcher must
		// not fire after a chord.
		plan = planModifierRelease(ModSuper, true, false, true, false, false);
		QVERIFY(plan.maskAndReplay);
		QVERIFY(!plan.postTap);

		// A Super+drag already masked the sequence when it began, but the
		// swallowed chord still owns the release.
		plan = planModifierRelease(ModSuper, true, false, false, false, true);
		QVERIFY(plan.maskAndReplay);
		QVERIFY(!plan.postTap);

		// The other Windows key still being held leaves the release to it.
		plan = planModifierRelease(ModSuper, true, true, false, false, false);
		QVERIFY(!plan.maskAndReplay);

		// UIPI blocks the replay over an elevated foreground, so the physical
		// release is passed instead of being swallowed without a replay.
		plan = planModifierRelease(ModSuper, true, false, false, true, false);
		QVERIFY(!plan.maskAndReplay);
	}

	void modifierReleasePlanLeavesTheOtherReleasesAlone() {
		// Only the Win key has a release path that still runs the shell's own
		// Win+<key> handling; an Alt chord's menu bar is already masked when
		// the chord is swallowed.
		QVERIFY(!planModifierRelease(ModAlt, true, false, false, false, false).maskAndReplay);

		// A Win press without a swallowed chord keeps passing through.
		QVERIFY(!planModifierRelease(ModSuper, false, false, false, false, false).maskAndReplay);

		// An elevated foreground also passes a lone tap's release through.
		QVERIFY(!planModifierRelease(ModAlt, false, false, true, true, false).maskAndReplay);
	}

	void everyModifierTapStillFires() {
		// Regression: the release plan rejected Ctrl and Shift before the tap
		// was posted, so {"keys":"Ctrl","onRelease":true} stopped firing. A
		// Ctrl or Shift tap posts without masking; Win and Alt keep their mask
		// and replayed release.
		auto ctrl = planModifierRelease(ModCtrl, false, false, true, false, false);
		QVERIFY(!ctrl.maskAndReplay);
		QVERIFY(ctrl.postTap);

		auto shift = planModifierRelease(ModShift, false, false, true, false, false);
		QVERIFY(!shift.maskAndReplay);
		QVERIFY(shift.postTap);

		auto alt = planModifierRelease(ModAlt, false, false, true, false, false);
		QVERIFY(alt.maskAndReplay);
		QVERIFY(alt.postTap);

		auto win = planModifierRelease(ModSuper, false, false, true, false, false);
		QVERIFY(win.maskAndReplay);
		QVERIFY(win.postTap);

		// A held other side or an elevated foreground still suppresses taps.
		QVERIFY(!planModifierRelease(ModCtrl, false, true, true, false, false).postTap);
		QVERIFY(!planModifierRelease(ModShift, false, false, true, true, false).postTap);
	}

	void altTapSurvivesASuperDrag() {
		// Regression: a Super+drag set the Super-chord flag and it stayed set
		// until the next Win press. The plan applied it to Alt too, so the next
		// Alt tap silently did nothing.
		auto alt = planModifierRelease(ModAlt, false, false, true, false, true);
		QVERIFY(alt.maskAndReplay);
		QVERIFY(alt.postTap);

		// Ctrl and Shift are not part of the Windows key sequence either.
		QVERIFY(planModifierRelease(ModCtrl, false, false, true, false, true).postTap);
		QVERIFY(planModifierRelease(ModShift, false, false, true, false, true).postTap);

		// The flag still suppresses the Win tap it was set for.
		QVERIFY(!planModifierRelease(ModSuper, false, false, true, false, true).postTap);
	}

	void loneTapFallsBackWhenTheReplayFails() {
		// The lone-Super tap used to swallow the physical release whenever it
		// posted a tap, even when SendInput refused the replay, which could
		// leave the Windows key logically down. Pass the physical release when
		// the injection was incomplete, exactly as the swallowed-chord path
		// does.
		auto tap = planModifierRelease(ModSuper, false, false, true, false, false);
		QVERIFY(tap.maskAndReplay);
		QVERIFY(tap.postTap);
		QVERIFY(swallowModifierRelease(tap, true));
		QVERIFY(!swallowModifierRelease(tap, false));

		// A Ctrl tap never injects, so its release always passes through.
		auto ctrl = planModifierRelease(ModCtrl, false, false, true, false, false);
		QVERIFY(!swallowModifierRelease(ctrl, false));
	}
};

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	HotkeysRoutingTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "hotkeys_routing_test.moc"

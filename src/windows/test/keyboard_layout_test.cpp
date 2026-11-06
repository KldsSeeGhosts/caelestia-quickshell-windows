#include <thread>
#include <vector>

#include <QHash>
#include <QSet>
#include <QtTest>
#include <windows.h>

static int g_activateCalls = 0;

extern "C" HKL WINAPI TestActivateKeyboardLayout(HKL, UINT) {
	++g_activateCalls;
	return nullptr;
}

// Compile the production name resolver with activation renamed. A regression
// that names a layout by activating it now fails g_activateCalls instead of
// silently changing input state.
#define ActivateKeyboardLayout TestActivateKeyboardLayout
#include "../system/keyboard_layout_name.cpp"
#undef ActivateKeyboardLayout

#include "../system/keyboard_layout_switch.hpp"

using namespace qs::windows::sys;

namespace {
HKL hklFrom(quintptr value) { return reinterpret_cast<HKL>(value); }    // NOLINT
HWND hwndFrom(quintptr value) { return reinterpret_cast<HWND>(value); } // NOLINT

GUID guid(uint value) {
	GUID result {};
	result.Data1 = value;
	result.Data4[7] = 0x5A;
	return result;
}

InputProfile layoutProfile(HKL hkl, const QString& description, DWORD flags = TF_IPP_FLAG_ENABLED) {
	InputProfile profile;
	profile.type = TF_PROFILETYPE_KEYBOARDLAYOUT;
	profile.langid = LOWORD(reinterpret_cast<quintptr>(hkl)); // NOLINT
	profile.hkl = hkl;
	profile.flags = flags;
	profile.description = description;
	return profile;
}

InputProfile tipProfile(
    LANGID langid,
    const GUID& clsid,
    const GUID& guidProfile,
    HKL substitute,
    const QString& description,
    DWORD flags = TF_IPP_FLAG_ENABLED,
    const GUID& catid = GUID_TFCAT_TIP_KEYBOARD
) {
	InputProfile profile;
	profile.type = TF_PROFILETYPE_INPUTPROCESSOR;
	profile.langid = langid;
	profile.clsid = clsid;
	profile.guidProfile = guidProfile;
	profile.catid = catid;
	profile.hklSubstitute = substitute;
	profile.flags = flags;
	profile.description = description;
	return profile;
}

QString currentThreadLayoutName() {
	wchar_t buffer[KL_NAMELENGTH] {};
	if (GetKeyboardLayoutNameW(buffer) == FALSE) return QString();
	return QString::fromWCharArray(buffer);
}
} // namespace

class KeyboardLayoutTest: public QObject {
	Q_OBJECT;

private slots:
	void init() { g_activateCalls = 0; }

	void targetResolution() {
		auto* external = hwndFrom(0x1001); // opaque HWND stand-ins
		auto* retained = hwndFrom(0x1002);
		auto* shell = hwndFrom(0x1003);

		QCOMPARE(resolveLayoutTarget(external, true, retained, true), external);
		QCOMPARE(resolveLayoutTarget(shell, false, retained, true), retained);
		QCOMPARE(resolveLayoutTarget(nullptr, false, retained, true), retained);
		QCOMPARE(resolveLayoutTarget(external, true, shell, false), external);
		QCOMPARE(resolveLayoutTarget(shell, false, shell, false), nullptr);
		QCOMPARE(resolveLayoutTarget(nullptr, false, nullptr, false), nullptr);
	}

	void klidResolution() {
		QCOMPARE(klidFromHkl(hklFrom(0x04090409), {}), QStringLiteral("00000409"));
		QCOMPARE(klidFromHkl(hklFrom(0x04110411), {}), QStringLiteral("00000411"));

		QList<LayoutRegistryEntry> entries {
		    {QStringLiteral("00010409"), QStringLiteral("0002")},
		    {QStringLiteral("00020409"), QStringLiteral("0003")},
		};
		QCOMPARE(klidFromHkl(hklFrom(0xF0020409), entries), QStringLiteral("00010409"));
		QCOMPARE(klidFromHkl(hklFrom(0xF0030409), entries), QStringLiteral("00020409"));

		// A zero device word means the KLID is the language id.
		QCOMPARE(klidFromHkl(hklFrom(0x00000409), {}), QStringLiteral("00000409"));

		// An unknown Layout Id has no KLID.
		QVERIFY(klidFromHkl(hklFrom(0xF0090409), entries).isEmpty());

		// A normal layout with its own device word uses that word.
		QCOMPARE(klidFromHkl(hklFrom(0x040A0409), {}), QStringLiteral("0000040A"));
	}

	void klidCrossLanguage() {
		// A keyboard attached to another input language: the registry key
		// suffix (0809) does not match the HKL language, so the Layout Id is
		// matched across all keys.
		QList<LayoutRegistryEntry> entries {
		    {QStringLiteral("00010409"), QStringLiteral("0002")},
		    {QStringLiteral("00000809"), QStringLiteral("00A9")},
		};
		QCOMPARE(klidFromHkl(hklFrom(0xF0020809), entries), QStringLiteral("00010409"));
		QCOMPARE(klidFromHkl(hklFrom(0xF0A90409), entries), QStringLiteral("00000809"));

		// Layout Id compares as a number, not as text.
		QList<LayoutRegistryEntry> shortId {{QStringLiteral("00010409"), QStringLiteral("2")}};
		QCOMPARE(klidFromHkl(hklFrom(0xF0020809), shortId), QStringLiteral("00010409"));
	}

	void retainedTargetValidation() {
		LayoutTargetIdentity recorded {100, 200};
		LayoutTargetState live {true, true, false, false, 100, 200};
		QVERIFY(isLiveRetainedTarget(live, recorded));

		auto minimized = live;
		minimized.minimized = true;
		QVERIFY(!isLiveRetainedTarget(minimized, recorded));

		auto invisible = live;
		invisible.visible = false;
		QVERIFY(!isLiveRetainedTarget(invisible, recorded));

		auto cloaked = live;
		cloaked.cloaked = true;
		QVERIFY(!isLiveRetainedTarget(cloaked, recorded));

		auto gone = live;
		gone.exists = false;
		QVERIFY(!isLiveRetainedTarget(gone, recorded));

		// A recycled handle now belongs to another thread or process.
		auto otherThread = live;
		otherThread.threadId = 201;
		QVERIFY(!isLiveRetainedTarget(otherThread, recorded));

		auto otherProcess = live;
		otherProcess.processId = 101;
		QVERIFY(!isLiveRetainedTarget(otherProcess, recorded));

		// Nothing recorded means nothing to trust.
		QVERIFY(!isLiveRetainedTarget(live, {}));
	}

	void retainedTargetObservation() {
		// Real, never-shown window: it exists but is not visible.
		auto* hidden = CreateWindowExW(
		    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		    L"STATIC",
		    L"",
		    WS_POPUP,
		    0,
		    0,
		    1,
		    1,
		    nullptr,
		    nullptr,
		    GetModuleHandleW(nullptr),
		    nullptr
		);
		QVERIFY(hidden != nullptr);

		auto state = observeLayoutTarget(hidden);
		QVERIFY(state.exists);
		QVERIFY(!state.visible);
		QCOMPARE(state.processId, GetCurrentProcessId());
		QCOMPARE(state.threadId, GetCurrentThreadId());

		LayoutTargetIdentity recorded {state.processId, state.threadId};
		QVERIFY(!isLiveRetainedTarget(state, recorded));

		// The handle of a destroyed window no longer exists.
		DestroyWindow(hidden);
		auto destroyed = observeLayoutTarget(hidden);
		QVERIFY(!destroyed.exists);
		QVERIFY(!isLiveRetainedTarget(destroyed, recorded));

		QVERIFY(!observeLayoutTarget(nullptr).exists);
	}

	void inputTargetResolution() {
		struct FakeWindow {
			DWORD thread = 0;
			DWORD process = 0;
			HWND rootOwner = nullptr;
			QString windowClass;
			HWND parent = nullptr; // child windows only
			HWND owner = nullptr;  // owned popups only
		};
		struct Gui {
			HWND focus = nullptr;
			HWND active = nullptr;
		};

		QHash<HWND, FakeWindow> windows;
		QHash<DWORD, Gui> gui;
		QHash<HWND, HWND> cores;

		LayoutInputQueries queries {
		    [&](HWND hwnd, DWORD* process) {
			    auto window = windows.value(hwnd);
			    if (process != nullptr) *process = window.process;
			    return window.thread;
		    },
		    [&](DWORD thread, HWND& focus, HWND& active) {
			    if (!gui.contains(thread)) return false;
			    focus = gui.value(thread).focus;
			    active = gui.value(thread).active;
			    return true;
		    },
		    [&](HWND hwnd) { return windows.value(hwnd).rootOwner; },
		    [&](HWND hwnd) { return windows.value(hwnd).windowClass; },
		    [&](HWND frame) { return cores.value(frame, nullptr); },
		    [&](HWND parent, HWND hwnd) {
			    for (auto* up = windows.value(hwnd).parent; up != nullptr;
			         up = windows.value(up).parent) {
				    if (up == parent) return true;
			    }
			    return false;
		    },
		    [&](HWND hwnd) { return windows.value(hwnd).owner; },
		};

		auto* frame = hwndFrom(0x3001);
		auto* child = hwndFrom(0x3002);
		windows[frame] = {10, 1, frame, QStringLiteral("Notepad")};
		windows[child] = {10, 1, frame, QStringLiteral("Edit")};

		// Classic window: input is the focused child on the same thread.
		gui[10] = {child, frame};
		auto classic = resolveLayoutInput(frame, queries);
		QCOMPARE(classic.window, child);
		QCOMPARE(classic.threadId, DWORD(10));

		// No focus: the active window, then the frame.
		gui[10] = {nullptr, frame};
		QCOMPARE(resolveLayoutInput(frame, queries).window, frame);
		gui[10] = {nullptr, nullptr};
		auto nullFocus = resolveLayoutInput(frame, queries);
		QCOMPARE(nullFocus.window, frame);
		QCOMPARE(nullFocus.threadId, DWORD(10));

		// No GUI thread info at all: the frame.
		gui.clear();
		QCOMPARE(resolveLayoutInput(frame, queries).window, frame);

		// Foreign focus window (another app's root) is rejected.
		auto* foreign = hwndFrom(0x3003);
		windows[foreign] = {10, 1, foreign, QStringLiteral("Other")};
		gui[10] = {foreign, nullptr};
		QCOMPARE(resolveLayoutInput(frame, queries).window, frame);

		// An owned popup of the same app is accepted.
		auto* popup = hwndFrom(0x3004);
		windows[popup] = {10, 1, frame, QStringLiteral("#32768")};
		gui[10] = {popup, nullptr};
		QCOMPARE(resolveLayoutInput(frame, queries).window, popup);

		// Hosted UWP: the frame is ApplicationFrameHost's, the CoreWindow and its
		// focus belong to another thread and process.
		auto* host = hwndFrom(0x4001);
		auto* core = hwndFrom(0x4002);
		windows[host] = {20, 2, host, QStringLiteral("ApplicationFrameWindow")};
		windows[core] = {30, 3, host, QStringLiteral("Windows.UI.Core.CoreWindow")};
		cores[host] = core;
		gui[20] = {host, host};
		gui[30] = {core, core};
		auto hosted = resolveLayoutInput(host, queries);
		QCOMPARE(hosted.window, core);
		QCOMPARE(hosted.threadId, DWORD(30));

		// Hosted frame without a distinct CoreWindow thread uses the frame's.
		windows[core] = {20, 2, host, QStringLiteral("Windows.UI.Core.CoreWindow")};
		auto sameThread = resolveLayoutInput(host, queries);
		QCOMPARE(sameThread.window, host);
		QCOMPARE(sameThread.threadId, DWORD(20));

		// Hosted frame with a distinct CoreWindow thread but no focus or active
		// window falls back to the CoreWindow, never to the host's frame.
		windows[core] = {30, 3, host, QStringLiteral("Windows.UI.Core.CoreWindow")};
		gui[30] = {nullptr, nullptr};
		auto noFocus = resolveLayoutInput(host, queries);
		QCOMPARE(noFocus.window, core);
		QCOMPARE(noFocus.threadId, DWORD(30));

		// The frame is never input in the hosted case: no focus and the host's
		// frame as the active window still gives the CoreWindow.
		gui[30] = {nullptr, host};
		auto frameActive = resolveLayoutInput(host, queries);
		QCOMPARE(frameActive.window, core);
		QCOMPARE(frameActive.threadId, DWORD(30));

		// A focused child of the CoreWindow on its own thread is accepted, even
		// though its root owner is not the frame's.
		auto* coreChild = hwndFrom(0x4003);
		windows[coreChild] = {31, 3, coreChild, QStringLiteral("Child"), core};
		gui[30] = {coreChild, nullptr};
		auto childFocus = resolveLayoutInput(host, queries);
		QCOMPARE(childFocus.window, coreChild);
		QCOMPARE(childFocus.threadId, DWORD(31));

		// A popup owned by a window in the CoreWindow subtree is accepted.
		auto* corePopup = hwndFrom(0x4004);
		windows[corePopup] = {30, 3, corePopup, QStringLiteral("Popup"), nullptr, coreChild};
		gui[30] = {nullptr, corePopup};
		QCOMPARE(resolveLayoutInput(host, queries).window, corePopup);

		// An unrelated popup (owned by the host frame, or by nothing in the
		// subtree) is rejected in favour of the CoreWindow.
		auto* strayPopup = hwndFrom(0x4005);
		windows[strayPopup] = {30, 3, strayPopup, QStringLiteral("Popup"), nullptr, host};
		gui[30] = {strayPopup, nullptr};
		QCOMPARE(resolveLayoutInput(host, queries).window, core);
		auto* ownerless = hwndFrom(0x4006);
		windows[ownerless] = {30, 3, ownerless, QStringLiteral("Popup")};
		gui[30] = {ownerless, nullptr};
		QCOMPARE(resolveLayoutInput(host, queries).window, core);

		// A cyclic owner chain outside the subtree terminates and is rejected.
		auto* cycleA = hwndFrom(0x4007);
		auto* cycleB = hwndFrom(0x4008);
		windows[cycleA] = {30, 3, cycleA, QStringLiteral("Popup"), nullptr, cycleB};
		windows[cycleB] = {30, 3, cycleB, QStringLiteral("Popup"), nullptr, cycleA};
		gui[30] = {cycleA, nullptr};
		QCOMPARE(resolveLayoutInput(host, queries).window, core);

		// No GUI-thread info for the CoreWindow thread: the CoreWindow.
		gui.remove(30);
		auto noInfo = resolveLayoutInput(host, queries);
		QCOMPARE(noInfo.window, core);
		QCOMPARE(noInfo.threadId, DWORD(30));

		// Hosted focus that is another app's window is rejected in favour of
		// the CoreWindow.
		gui[30] = {foreign, nullptr};
		auto foreignHosted = resolveLayoutInput(host, queries);
		QCOMPARE(foreignHosted.window, core);
		QCOMPARE(foreignHosted.threadId, DWORD(30));

		QVERIFY(resolveLayoutInput(nullptr, queries).window == nullptr);
	}

	void targetLossPublishesNoIndex() {
		std::vector<HKL> layouts {hklFrom(0x04090409), hklFrom(0xF0020409)};
		auto handleOf = [](HKL hkl) { return hkl; };

		QCOMPARE(layoutIndexOf(layouts, hklFrom(0x04090409), handleOf), 0);
		QCOMPARE(layoutIndexOf(layouts, hklFrom(0xF0020409), handleOf), 1);

		// Lost target and an unlisted layout both clear the selection.
		QCOMPARE(layoutIndexOf(layouts, nullptr, handleOf), -1);
		QCOMPARE(layoutIndexOf(layouts, hklFrom(0x04070407), handleOf), -1);
		QCOMPARE(layoutIndexOf(std::vector<HKL> {}, hklFrom(0x04090409), handleOf), -1);
	}

	void comApartmentsStayBalanced() {
		// A fresh thread has no apartment; a leaked initialization would leave
		// one behind. Each case ends with exactly the caller's own balance.
		auto apartmentGone = [] {
			APTTYPE type {};
			APTTYPEQUALIFIER qualifier {};
			return CoGetApartmentType(&type, &qualifier) == CO_E_NOTINITIALIZED;
		};

		bool freshBalanced = false;
		bool alreadyInitializedBalanced = false;
		bool changedModeBalanced = false;

		std::thread([&] {
			queryInputProfiles();
			freshBalanced = apartmentGone();

			// S_FALSE: the caller already initialized this apartment.
			CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
			queryInputProfiles();
			CoUninitialize();
			alreadyInitializedBalanced = apartmentGone();

			// RPC_E_CHANGED_MODE: the caller owns an MTA.
			CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			queryInputProfiles();
			CoUninitialize();
			changedModeBalanced = apartmentGone();
		}).join();

		QVERIFY(freshBalanced);
		QVERIFY(alreadyInitializedBalanced);
		QVERIFY(changedModeBalanced);
		QCOMPARE(g_activateCalls, 0);
	}

	void layoutEntriesKeepUserOrder() {
		HKL us = hklFrom(0x04090409);
		HKL dvorak = hklFrom(0xF0020409);
		std::vector<HKL> installed {dvorak, us};
		QList<InputProfile> profiles {
		    layoutProfile(us, QStringLiteral("US")),
		    layoutProfile(dvorak, QStringLiteral("United States-Dvorak")),
		    // Disabled and not installed layouts are not selectable.
		    layoutProfile(hklFrom(0x04070407), QStringLiteral("German"), 0),
		    layoutProfile(hklFrom(0x040C040C), QStringLiteral("French")),
		};

		auto entries = buildInputEntries(profiles, installed);
		QCOMPARE(entries.size(), qsizetype(2));
		QCOMPARE(entries[0].hkl, dvorak);
		QCOMPARE(entries[0].description, QStringLiteral("United States-Dvorak"));
		QCOMPARE(entries[1].hkl, us);
		QCOMPARE(entries[1].id, QStringLiteral("4090409"));

		// TSF unavailable: every installed layout is still listed.
		auto fallback = buildInputEntries({}, installed);
		QCOMPARE(fallback.size(), qsizetype(2));
		QCOMPARE(fallback[0].hkl, dvorak);
		QCOMPARE(fallback[0].langid, LANGID(0x0409));
		QVERIFY(fallback[0].description.isEmpty());
	}

	void imeProfilesKeepIdentity() {
		HKL us = hklFrom(0x04090409);
		HKL zh = hklFrom(0x08040804);
		HKL ja = hklFrom(0x04110411);
		HKL ko = hklFrom(0x04120412);
		std::vector<HKL> installed {us, zh, ja, ko};

		QList<InputProfile> profiles {
		    layoutProfile(us, QStringLiteral("US")),
		    // Two zh-CN IMEs with no HKL and no substitute.
		    tipProfile(0x0804, guid(1), guid(11), nullptr, QStringLiteral("Microsoft Pinyin")),
		    tipProfile(0x0804, guid(2), guid(12), nullptr, QStringLiteral("Microsoft Wubi")),
		    // The Japanese IME replaces its layout through a substitute.
		    layoutProfile(
		        ja,
		        QStringLiteral("Japanese"),
		        TF_IPP_FLAG_ENABLED | TF_IPP_FLAG_SUBSTITUTEDBYINPUTPROCESSOR
		    ),
		    tipProfile(0x0411, guid(3), guid(13), ja, QStringLiteral("Microsoft IME")),
		    // Two profiles that share one substitute.
		    tipProfile(0x0412, guid(4), guid(14), ko, QStringLiteral("Korean IME")),
		    tipProfile(0x0412, guid(5), guid(15), ko, QStringLiteral("Korean Old Hangul")),
		    // Repeated enumeration of the same identity is one entry.
		    tipProfile(0x0412, guid(5), guid(15), ko, QStringLiteral("Korean Old Hangul")),
		    // Speech and disabled profiles are not keyboard input.
		    tipProfile(0x0409, guid(6), guid(16), nullptr, QStringLiteral("Speech"), TF_IPP_FLAG_ENABLED, guid(99)),
		    tipProfile(0x0804, guid(7), guid(17), nullptr, QStringLiteral("Disabled"), 0),
		};

		auto entries = buildInputEntries(profiles, installed);
		QStringList names;
		for (const auto& entry: entries) names.append(entry.description);
		QCOMPARE(
		    names,
		    QStringList({
		        QStringLiteral("US"),
		        QStringLiteral("Microsoft Pinyin"),
		        QStringLiteral("Microsoft Wubi"),
		        QStringLiteral("Microsoft IME"),
		        QStringLiteral("Korean IME"),
		        QStringLiteral("Korean Old Hangul"),
		    })
		);

		// IMEs without a handle switch through their language's layout, and
		// no bare layout entry duplicates them.
		QCOMPARE(entries[1].hkl, zh);
		QCOMPARE(entries[2].hkl, zh);
		QCOMPARE(entries[3].hkl, ja);
		QCOMPARE(entries[4].hkl, ko);
		QCOMPARE(entries[5].hkl, ko);

		// Identity is clsid, langid and guidProfile, never the shared handle.
		QSet<QString> ids;
		for (const auto& entry: entries) ids.insert(entry.id);
		QCOMPARE(ids.size(), entries.size());
		QVERIFY(entries[1].id.contains(QStringLiteral(":0804:")));
		QVERIFY(entries[1].id != entries[2].id);
		QVERIFY(entries[4].id != entries[5].id);

		// A shared handle cannot tell which profile another thread uses, so
		// no entry is marked active; a unique handle still is.
		auto handleOf = [](const InputEntry& entry) { return entry.hkl; };
		QCOMPARE(layoutIndexOf(entries, zh, handleOf), -1);
		QCOMPARE(layoutIndexOf(entries, ko, handleOf), -1);
		QCOMPARE(layoutIndexOf(entries, ja, handleOf), 3);
		QCOMPARE(layoutIndexOf(entries, us, handleOf), 0);
	}

	void replacedLayoutStaysHiddenWhenImeUsesAnotherHkl() {
		HKL ja = hklFrom(0x04110411);
		HKL replaced = hklFrom(0xF0010411);
		std::vector<HKL> installed {ja, replaced};
		QList<InputProfile> profiles {
		    tipProfile(0x0411, guid(3), guid(13), nullptr, QStringLiteral("Microsoft IME")),
		    layoutProfile(
		        replaced,
		        QStringLiteral("Japanese"),
		        TF_IPP_FLAG_ENABLED | TF_IPP_FLAG_SUBSTITUTEDBYINPUTPROCESSOR
		    ),
		};

		auto entries = buildInputEntries(profiles, installed);
		QCOMPARE(entries.size(), qsizetype(1));
		QCOMPARE(entries[0].description, QStringLiteral("Microsoft IME"));
		QCOMPARE(entries[0].hkl, ja);
		for (const auto& entry: entries) QCOMPARE(entry.hkl == replaced, false);

		// TSF still empty: both installed layouts are listed.
		auto fallback = buildInputEntries({}, installed);
		QCOMPARE(fallback.size(), qsizetype(2));
		QCOMPARE(fallback[0].hkl, ja);
		QCOMPARE(fallback[1].hkl, replaced);
	}

	void imeWithoutInstalledLanguageIsKept() {
		std::vector<HKL> installed {hklFrom(0x04090409)};
		QList<InputProfile> profiles {
		    tipProfile(0x0404, guid(8), guid(18), hklFrom(0x04040404), QStringLiteral("Bopomofo")),
		};

		auto entries = buildInputEntries(profiles, installed);
		QCOMPARE(entries.size(), qsizetype(2));
		QCOMPARE(entries.last().description, QStringLiteral("Bopomofo"));
		QVERIFY(entries.last().hkl == nullptr);
	}

	void registryResolution() {
		HKL dvorak = hklFrom(0xF0020409);
		QList<LayoutRegistryEntry> entries {
		    {QStringLiteral("00010409"), QStringLiteral("0002")},
		};

		QString requested;
		auto read = [&requested](const QString& klid) {
			requested = klid;
			return QStringLiteral("layout:") + klid;
		};

		InputEntry bare {QStringLiteral("f0020409"), 0x0409, dvorak, QString()};
		QCOMPARE(resolveLayoutName(bare, entries, read), QStringLiteral("layout:00010409"));
		QCOMPARE(requested, QStringLiteral("00010409"));

		// A TSF description wins and the registry reader is not consulted.
		requested.clear();
		InputEntry described = bare;
		described.description = QStringLiteral("United States-Dvorak");
		QCOMPARE(resolveLayoutName(described, entries, read), QStringLiteral("United States-Dvorak"));
		QVERIFY(requested.isEmpty());

		// No description and no KLID or handle: no lookup, no activation.
		QVERIFY(resolveLayoutName(bare, {}, read).isEmpty());
		InputEntry handleless {QStringLiteral("ime"), 0x0404, nullptr, QString()};
		QVERIFY(resolveLayoutName(handleless, entries, read).isEmpty());
		QVERIFY(requested.isEmpty());
	}

	void readOnlyNameResolution() {
		auto before = currentThreadLayoutName();

		// Exercise the real TSF/registry paths. Both are read-only and may
		// legitimately be empty on a machine without the corresponding data.
		auto profiles = queryInputProfiles();
		auto entries = queryLayoutRegistryEntries();
		Q_UNUSED(profiles);
		Q_UNUSED(entries);

		auto name = layoutNameFromRegistry(QStringLiteral("00000409"));
		Q_UNUSED(name);

		// Any ActivateKeyboardLayout call in the included production source
		// lands in TestActivateKeyboardLayout and increments this counter.
		QCOMPARE(g_activateCalls, 0);
		QCOMPARE(currentThreadLayoutName(), before);
	}

	void switchResults() {
		HKL layout = hklFrom(0x04090409);
		auto* target = hwndFrom(0x2001);

		int posted = 0;
		auto success = [&posted](HWND, HKL, DWORD& error) {
			++posted;
			error = 0;
			return true;
		};
		auto requested = postLayoutSwitch(target, layout, success);
		QCOMPARE(requested.result, QStringLiteral("requested"));
		QCOMPARE(requested.error, DWORD(0));
		QCOMPARE(posted, 1);

		auto denied = postLayoutSwitch(target, layout, [](HWND, HKL, DWORD& error) {
			error = ERROR_ACCESS_DENIED;
			return false;
		});
		QCOMPARE(denied.result, QStringLiteral("denied"));
		QCOMPARE(denied.error, DWORD(ERROR_ACCESS_DENIED));

		auto noTarget = postLayoutSwitch(nullptr, layout, success);
		QCOMPARE(noTarget.result, QStringLiteral("no-target"));
		QCOMPARE(noTarget.error, DWORD(0));
		QCOMPARE(posted, 1);
	}
};

QTEST_GUILESS_MAIN(KeyboardLayoutTest)
#include "keyboard_layout_test.moc"

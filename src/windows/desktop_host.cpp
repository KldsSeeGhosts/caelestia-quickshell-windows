#include "desktop_host.hpp"
#include <array>
#include <cwchar>

#include <qt_windows.h>

#include <qcoreapplication.h>
#include <qhash.h>
#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qpointer.h>
#include <qstringlist.h>
#include <qtimer.h>

#include "../core/logcat.hpp"
#include "appbar.hpp"
#include "util.hpp"

namespace qs::windows {

namespace {

QS_LOGGING_CATEGORY(logDesktop, "quickshell.windows.desktop", QtWarningMsg);

constexpr const wchar_t* LISTENER_CLASS = L"QuickshellDesktopListener";

constexpr UINT SPAWN_WORKERW = 0x052C;
constexpr UINT SPAWN_TIMEOUT_MS = 150;
constexpr int SPAWN_RETRY_MS = 500;

constexpr int SETTLE_MS = 500;

// A wallpaper owner is another process: it attaches its window to the host and
// restacks it from its own thread, and its process id is not known in advance.
// A hook scoped to a process or thread can never see that first owner, so
// desktop-wide out-of-context hooks report the only edges that are left:
// EVENT_OBJECT_REORDER on the host covers a restack of its child list,
// EVENT_OBJECT_PARENTCHANGE covers a window gaining or losing a parent without
// the host's list necessarily moving, and EVENT_OBJECT_SHOW/HIDE of a host
// child covers a window created directly inside the host. Wallpaper Engine's
// stop/play does exactly that: stop destroys its windows (so the per-process
// hooks go away with the last foreign child), and play creates new hidden
// children of the host and shows them, which raises neither a reorder nor a
// parent change. A known owner's destruction edges stay on the per-process
// hooks that only exist while it has a child in the host.
// The host belongs to Explorer, but a shell window can reorder itself after
// embedding/presentation too. Neither Explorer's scoped hook nor the foreign
// wallpaper process hooks see that edge. Include our process and let the
// idempotent order check discard feedback from our own repairs.
constexpr DWORD RECONCILE_HOOK_FLAGS = WINEVENT_OUTOFCONTEXT;

constexpr DWORD BUILD_24H2 = 26100;

// Window property marking a desktop Background panel for the group placement.
constexpr const wchar_t* BACKGROUND_PROP = L"QuickshellDesktopBackground";

// SetWindowPos flag: skip WM_WINDOWPOSCHANGING. Group placement only changes
// the Z-order (no move/size), and embedded panels clamp that message while
// not placing themselves; this keeps their position guards out of the way
// without a cross-panel flag.
constexpr UINT PLACE_ZORDER_FLAGS =
    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING;

DesktopHost* gHost = nullptr; // NOLINT

#ifdef QS_TEST
int gPlacementMoves = 0; // NOLINT
#endif

DWORD processOf(HWND hwnd) {
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	return pid;
}

bool hasClass(HWND hwnd, const wchar_t* name) {
	wchar_t cls[32] {};
	if (GetClassNameW(hwnd, cls, 32) == 0) return false;
	return wcscmp(cls, name) == 0;
}

struct Lookup {
	HWND parent = nullptr;
	HWND insertAfter = nullptr;
	const char* layout = "";
};

HWND findTopLevelIconsHost(HWND progman) {
	struct Search {
		DWORD pid = 0;
		HWND iconsHost = nullptr;
	} search {.pid = processOf(progman)};

	EnumWindows(
	    [](HWND hwnd, LPARAM param) -> BOOL {
		    auto* search = reinterpret_cast<Search*>(param); // NOLINT(performance-no-int-to-ptr)
		    if (FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr) == nullptr) return TRUE;
		    if (processOf(hwnd) != search->pid) return TRUE;
		    search->iconsHost = hwnd;
		    return FALSE;
	    },
	    reinterpret_cast<LPARAM>(&search)
	);

	return search.iconsHost;
}

HWND findIconsView(HWND progman) {
	auto* icons = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);

	if (icons == nullptr) {
		auto* host = findTopLevelIconsHost(progman);
		if (host != nullptr) icons = FindWindowExW(host, nullptr, L"SHELLDLL_DefView", nullptr);
	}

	return icons != nullptr && IsWindowVisible(icons) ? icons : nullptr;
}

Lookup findChildWorkerW(HWND progman) {
	auto* workerw = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
	if (workerw == nullptr || !IsWindowVisible(workerw)) return {};
	return {.parent = workerw, .layout = "WorkerW inside Progman"};
}

Lookup findTopLevelWorkerW(HWND progman) {
	auto* iconsHost = findTopLevelIconsHost(progman);
	if (iconsHost == nullptr) return {};

	auto pid = processOf(progman);
	auto* workerw = FindWindowExW(nullptr, iconsHost, L"WorkerW", nullptr);
	if (workerw == nullptr || processOf(workerw) != pid || !IsWindowVisible(workerw)) {
		return {};
	}

	if (FindWindowExW(workerw, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr) return {};
	return {.parent = workerw, .layout = "WorkerW behind the icons"};
}

Lookup findProgman(HWND progman) {
	auto* icons = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
	if (icons == nullptr) return {};
	return {.parent = progman, .insertAfter = icons, .layout = "Progman, below the icons"};
}

Lookup findWorkerW(HWND progman) {
	auto childLayout = windowsBuild() >= BUILD_24H2;
	auto found = childLayout ? findChildWorkerW(progman) : findTopLevelWorkerW(progman);
	if (found.parent == nullptr) {
		found = childLayout ? findTopLevelWorkerW(progman) : findChildWorkerW(progman);
	}

	return found;
}

} // namespace

bool desktopHostHasForeignChild(HWND parent, bool visibleOnly) {
	if (parent == nullptr || !IsWindow(parent)) return false;

	for (auto* child = GetWindow(parent, GW_CHILD); child != nullptr;
	     child = GetWindow(child, GW_HWNDNEXT))
	{
		if (isOwnProcessWindow(child)) continue;
		if (!visibleOnly || IsWindowVisible(child)) return true;
	}

	return false;
}

void markDesktopBackground(HWND hwnd, bool marked) {
	if (hwnd == nullptr) return;

	if (marked) {
		SetPropW(hwnd, BACKGROUND_PROP, reinterpret_cast<HANDLE>(1));
	} else {
		RemovePropW(hwnd, BACKGROUND_PROP);
	}
}

namespace {

bool isAbove(HWND parent, HWND window, HWND other) {
	for (auto* child = GetWindow(parent, GW_CHILD); child != nullptr;
	     child = GetWindow(child, GW_HWNDNEXT))
	{
		if (child == other) return false;
		if (child == window) return true;
	}

	return false;
}

} // namespace

bool desktopBackgroundsMisordered(HWND parent, bool externalWallpaperEnabled) {
	if (parent == nullptr || !IsWindow(parent)) return false;

	// Top to bottom. With integration enabled, only a visible foreign child covers the desktop, so only
	// it can make a marked background misplaced: either the background sits
	// above such a child, or no visible foreign child exists and the group is
	// not on top of the host where it belongs. A hidden foreign child never
	// makes the order wrong. Read-only, so every reconciling event can ask.
	HWND topChild = GetWindow(parent, GW_CHILD);
	HWND firstBackground = nullptr;
	HWND previousBackground = nullptr;
	bool visibleForeign = false;

	for (auto* child = topChild; child != nullptr; child = GetWindow(child, GW_HWNDNEXT)) {
		if (GetPropW(child, BACKGROUND_PROP) != nullptr) {
			if (firstBackground == nullptr) firstBackground = child;
			// The top monitor can still be correct while a foreign child
			// covers a later monitor. The own group must be contiguous.
			if (!externalWallpaperEnabled && previousBackground != nullptr
			    && GetWindow(previousBackground, GW_HWNDNEXT) != child)
			{
				return true;
			}
			previousBackground = child;
			continue;
		}

		if (!externalWallpaperEnabled || !IsWindowVisible(child) || isOwnProcessWindow(child)) continue;
		visibleForeign = true;
		if (firstBackground != nullptr) return true;
	}

	if (firstBackground == nullptr || visibleForeign) return false;
	return firstBackground != topChild;
}

void placeDesktopBackgrounds(HWND parent, bool externalWallpaperEnabled) {
	if (parent == nullptr || !IsWindow(parent)) return;

	QList<HWND> backgrounds;
	HWND bottomForeign = nullptr;

	for (auto* child = GetWindow(parent, GW_CHILD); child != nullptr;
	     child = GetWindow(child, GW_HWNDNEXT))
	{
		if (GetPropW(child, BACKGROUND_PROP) != nullptr) {
			backgrounds.append(child);
		} else if (externalWallpaperEnabled && !isOwnProcessWindow(child) && IsWindowVisible(child)) {
			// Only a visible foreign wallpaper window may pin the shell below
			// it. A hidden one contributes no pixels, so keeping the group
			// under it would leave the user with a black host. The visibility
			// edge raises the group again, and showing the window places it
			// back on top through the same path.
			bottomForeign = child;
		}
	}

	if (backgrounds.isEmpty()) return;

	if (bottomForeign != nullptr) {
		// Move every background that is still above the bottom-most visible
		// foreign sibling below it, from the bottom of the group upwards, so
		// the backgrounds keep their relative order. Moving one window does
		// not make another one unsafe, so a second call is a no-op.
		for (auto it = backgrounds.crbegin(); it != backgrounds.crend(); ++it) {
			auto* background = *it;
			if (!isAbove(parent, background, bottomForeign)) continue;
			SetWindowPos(background, bottomForeign, 0, 0, 0, 0, PLACE_ZORDER_FLAGS);
#ifdef QS_TEST
			gPlacementMoves++;
#endif
		}
		return;
	}

	// Integration disabled, or no visible foreign child left: put the group back
	// on top of the host in its existing order, then keep it contiguous. The
	// first background is the top of the group; each following one sits
	// directly below its predecessor. This too is idempotent.
	auto* previous = backgrounds.first();
	if (GetWindow(parent, GW_CHILD) != previous) {
		SetWindowPos(previous, HWND_TOP, 0, 0, 0, 0, PLACE_ZORDER_FLAGS);
#ifdef QS_TEST
		gPlacementMoves++;
#endif
	}

	for (auto* background: backgrounds.mid(1)) {
		if (GetWindow(background, GW_HWNDPREV) != previous) {
			SetWindowPos(background, previous, 0, 0, 0, 0, PLACE_ZORDER_FLAGS);
#ifdef QS_TEST
			gPlacementMoves++;
#endif
		}
		previous = background;
	}
}

DesktopHost* DesktopHost::instance() {
	static QPointer<DesktopHost> host; // NOLINT

	if (host.isNull()) {
		host = new DesktopHost(QCoreApplication::instance());
	}

	return host.data();
}

DesktopHost::DesktopHost(QObject* parent): QObject(parent) {
	gHost = this;
	this->refreshTimer.setSingleShot(true);
	QObject::connect(&this->refreshTimer, &QTimer::timeout, this, &DesktopHost::refresh);
}

DesktopHost::~DesktopHost() {
	this->removeHook();
	this->removeReconcileHook();
	if (this->listener != nullptr) DestroyWindow(this->listener);
	delete this->desktopKeyNotifier;
	if (this->desktopKeyEvent != nullptr) CloseHandle(this->desktopKeyEvent);
	if (this->desktopKey != nullptr) RegCloseKey(this->desktopKey);
	if (gHost == this) gHost = nullptr;
}

void DesktopHost::setEnabled(bool enabled) {
	if (enabled == this->mEnabled) return;
	this->mEnabled = enabled;
	emit this->enabledChanged();
	this->refresh();
}

void DesktopHost::setExternalWallpaperEnabled(bool enabled) {
	if (enabled == this->mExternalWallpaperEnabled) return;
	this->mExternalWallpaperEnabled = enabled;
	emit this->externalWallpaperEnabledChanged();
	// Reconcile immediately, including a live settings change. Do not touch
	// the foreign window or move the shell out of its desktop host.
	this->updateReportedState();
	emit this->backgroundOrderChanged();
}

HWND DesktopHost::parentWindow() {
	if (!this->mEnabled) return nullptr;

	if (!this->lookedUp || (this->mParent != nullptr && !IsWindow(this->mParent))) {
		this->refresh();
	}

	return this->mParent;
}

HWND DesktopHost::iconsHost() {
	if (!this->mEnabled) return nullptr;

	if (!this->lookedUp || (this->mIconsHost != nullptr && !IsWindow(this->mIconsHost))) {
		this->refresh();
	}

	return this->mIconsHost;
}

void DesktopHost::setAboveIcons(const QStringList& namespaces) {
	if (namespaces == this->mAboveIcons) return;
	this->mAboveIcons = namespaces;
	emit this->aboveIconsChanged();
	this->updateReportedState();
}

bool DesktopHost::allowsAboveIcons(const QString& ns) {
	return this->mAboveIcons.contains(ns)
	    && !desktopHostHasForeignChild(this->parentWindow(), true);
}

void DesktopHost::noteEmbedRefused(HWND parent) {
	if (parent == nullptr || parent != this->mParent || this->mEmbedRefused) return;
	// Publish the refusal before the acknowledgement change, so a consumer
	// reacting to underlayActiveChanged already sees the reason.
	this->setEmbedRefused(true);
	this->updateReportedState();
}

void DesktopHost::setEmbedRefused(bool refused) {
	if (refused == this->mEmbedRefused) return;
	this->mEmbedRefused = refused;
	emit this->embedRefusedChanged();
}

void DesktopHost::updateReportedState() {
	auto active = this->active();
	auto underlayActive = this->underlayActive();
	auto wasUnderlayActive = this->mLastUnderlayActive;
	// Only a visible foreign window covers the desktop. A hidden one is still
	// in the host but contributes no pixels, so it must not keep the shell
	// background pinned behind it.
	auto visibleForeign = this->mParent != nullptr
	    && desktopHostHasForeignChild(this->mParent, true);

	if (active != this->mLastActive) {
		this->mLastActive = active;
		emit this->activeChanged();
	}

	if (underlayActive != this->mLastUnderlayActive) {
		this->mLastUnderlayActive = underlayActive;
		emit this->underlayActiveChanged();
	}

	// A genuine handback is the visible foreign wallpaper window leaving a
	// host the shell was acknowledged inside. Wallpaper Engine and Explorer
	// can destroy or replace the WorkerW on that edge, and the replacement is
	// created inside Explorer's process, which the hooks scoped to the old
	// host may not watch. Do not keep embedding into the old parent: re-run
	// discovery now and let parentChanged re-embed the panels into the host
	// that is live. A refusal, a policy change and an above-icons embed are
	// not handbacks.
	if (wasUnderlayActive && !underlayActive && !this->mEmbedRefused && this->mEnabled
	    && this->mParent != nullptr && this->mAboveIcons.isEmpty() && this->mExternalWallpaperEnabled)
	{
		this->scheduleRefresh();
	}

	// A show or hide changes where the shell background belongs, not just the
	// acknowledgement: hiding the foreign window releases the group to the top
	// of the host, showing it again pins the group below it.
	if (visibleForeign != this->mLastVisibleForeign) {
		this->mLastVisibleForeign = visibleForeign;
		emit this->backgroundOrderChanged();
	}

	// The visibility edge above only covers the child appearing, leaving or
	// changing visibility. A foreign process can also restack itself beneath
	// the shell, or replace its child with one inserted beneath, while
	// visibility stays true. Reconcile the actual z-order: the check is
	// read-only and the placement is idempotent, so an already correct order
	// costs no SetWindowPos. Both the desktop-wide hook and every per-process
	// hook end here.
	auto misordered = desktopBackgroundsMisordered(this->mParent, this->mExternalWallpaperEnabled);
	if (misordered) {
		placeDesktopBackgrounds(this->mParent, this->mExternalWallpaperEnabled);
	}

	// A complete decision without any titles, classes, paths, handles or
	// process/device identifiers. Report actual order, not just the claim:
	// underlayActive is presence/policy and can stay true through a restack.
	int foreign = 0;
	int visible = 0;
	int backgrounds = 0;
	for (auto* child = this->mParent == nullptr ? nullptr : GetWindow(this->mParent, GW_CHILD);
	     child != nullptr;
	     child = GetWindow(child, GW_HWNDNEXT))
	{
		if (GetPropW(child, BACKGROUND_PROP) != nullptr) backgrounds++;
		if (isOwnProcessWindow(child)) continue;
		foreign++;
		if (IsWindowVisible(child)) visible++;
	}
	auto stillMisordered =
	    desktopBackgroundsMisordered(this->mParent, this->mExternalWallpaperEnabled);
	const auto decision = QStringLiteral(
	    "enabled=%1 policy=%2 host=%3 insertAfter=%4 aboveIcons=%5 refused=%6 "
	    "foreign=%7 visibleForeign=%8 backgrounds=%9 underlay=%10 misordered=%11"
	).arg(this->mEnabled).arg(this->mExternalWallpaperEnabled).arg(this->active())
	     .arg(this->mInsertAfter != nullptr).arg(!this->mAboveIcons.isEmpty()).arg(this->mEmbedRefused)
	     .arg(foreign).arg(visible).arg(backgrounds).arg(this->underlayActive()).arg(stillMisordered);
	if (misordered || decision != this->mLastUnderlayDecision) {
		this->mLastUnderlayDecision = decision;
		qCInfo(logDesktop).noquote() << "Underlay decision" << decision << "repair=" << misordered;
	}

	this->syncForeignWatch();
}

void DesktopHost::syncForeignWatch() {
	// The per-process hooks only make a repair immediate, so they exist only
	// while a foreign child does. A hidden child still keeps them installed:
	// its show event must not wait for the next discovery edge. The
	// desktop-wide hooks stay installed for as long as the host is enabled, so
	// the first owner is discovered even with none of these installed.
	auto watch = this->mEnabled && this->mParent != nullptr && IsWindow(this->mParent)
	    && desktopHostHasForeignChild(this->mParent, false);

#ifdef QS_TEST
	this->syncForeignHooks(watch && !this->foreignHooksDisabledForTesting);
#else
	this->syncForeignHooks(watch);
#endif
}

void DesktopHost::queueReconcile() {
	if (this->reconcilePending) return;
	this->reconcilePending = true;

	QTimer::singleShot(0, this, [this] {
		this->reconcilePending = false;
		this->updateReportedState();
	});
}

void DesktopHost::syncForeignHooks(bool watch) {
	// The hooks on Explorer's thread never see what the wallpaper process does
	// to its own window. Observe the foreign processes directly, so a known
	// owner's visibility and destruction edges reach the host immediately; an
	// arrival and a restack are the desktop-wide hooks' job. Best effort: those
	// still report both if a registration here fails.
	QList<DWORD> pids;

	if (watch) {
		for (auto* child = GetWindow(this->mParent, GW_CHILD); child != nullptr;
		     child = GetWindow(child, GW_HWNDNEXT))
		{
			if (isOwnProcessWindow(child)) continue;
			auto pid = processOf(child);
			if (pid != 0 && !pids.contains(pid)) pids.append(pid);
		}
	}

	for (auto it = this->foreignHooks.begin(); it != this->foreignHooks.end();) {
		if (pids.contains(it.key())) {
			++it;
			continue;
		}

		for (auto* hook: it.value()) {
			if (hook != nullptr) UnhookWinEvent(hook);
		}

		it = this->foreignHooks.erase(it);
	}

	for (auto pid: pids) {
		auto existing = this->foreignHooks.find(pid);
		if (existing != this->foreignHooks.end()) {
			// Retry a failed registration instead of caching it for as long
			// as the process lives.
			if (existing.value()[0] != nullptr && existing.value()[1] != nullptr) continue;

			for (auto* hook: existing.value()) {
				if (hook != nullptr) UnhookWinEvent(hook);
			}

			this->foreignHooks.erase(existing);
		}

		// CREATE..REORDER is contiguous (create, destroy, show, hide,
		// reorder); the parent change is the only other event that moves a
		// window in or out of the host.
		auto* lifecycle = SetWinEventHook(
		    EVENT_OBJECT_CREATE,
		    EVENT_OBJECT_REORDER,
		    nullptr,
		    &DesktopHost::foreignEventProc,
		    pid,
		    0,
		    WINEVENT_OUTOFCONTEXT
		);

		auto* reparent = SetWinEventHook(
		    EVENT_OBJECT_PARENTCHANGE,
		    EVENT_OBJECT_PARENTCHANGE,
		    nullptr,
		    &DesktopHost::foreignEventProc,
		    pid,
		    0,
		    WINEVENT_OUTOFCONTEXT
		);

		this->foreignHooks.insert(pid, std::array<HWINEVENTHOOK, 2> {lifecycle, reparent});
	}
}

void DesktopHost::removeForeignHooks() { this->syncForeignHooks(false); }

void DesktopHost::scheduleRefresh(int delayMs) { this->refreshTimer.start(delayMs); }

void DesktopHost::refresh() {
	this->refreshTimer.stop();

	auto* oldParent = this->mParent;
	auto* oldInsertAfter = this->mInsertAfter;
	auto* oldIconsHost = this->mIconsHost;

	if (this->mEnabled) {
		this->ensureListener();
		this->lookup();
		this->installHook();
		this->installReconcileHook();
	} else {
		this->mParent = nullptr;
		this->mInsertAfter = nullptr;
		this->mIconsView = nullptr;
		this->mIconsHost = nullptr;
		this->lookedUp = false;
		this->setEmbedRefused(false);
#ifdef QS_TEST
		this->adoptedForTesting = false;
#endif
		this->removeHook();
		this->removeReconcileHook();
	}

	if (this->mParent != oldParent || this->mInsertAfter != oldInsertAfter
	    || this->mIconsHost != oldIconsHost)
	{
		emit this->parentChanged();
	} else if (this->mParent != nullptr) {
		emit this->parentMoved();
	}

	this->updateReportedState();
}

void DesktopHost::lookup() {
#ifdef QS_TEST
	// A test host stands in for the Explorer hierarchy. A test that replaces
	// it exercises the same apply path as a real discovery.
	if (this->testDiscoveredParent != nullptr) {
		auto first = !this->lookedUp;
		this->lookedUp = true;
		this->applyLookup(
		    this->testDiscoveredParent,
		    this->testDiscoveredInsertAfter,
		    nullptr,
		    first,
		    "test host"
		);
		return;
	}

	// A test-adopted host is synthetic; never replace it with the real desktop.
	if (this->adoptedForTesting) return;
#endif

	auto first = !this->lookedUp;
	this->lookedUp = true;

	Lookup found;
	auto* progman = FindWindowW(L"Progman", nullptr);
	auto neededSpawn = false;

	if (progman != nullptr) {
		found = findWorkerW(progman);

		if (found.parent == nullptr) {
			neededSpawn = true;
			DWORD_PTR result = 0;
			auto flags = SMTO_NORMAL | SMTO_ABORTIFHUNG;
			SendMessageTimeoutW(progman, SPAWN_WORKERW, 0xD, 0x1, flags, SPAWN_TIMEOUT_MS, &result);
			found = findWorkerW(progman);

			if (found.parent == nullptr) {
				SendMessageTimeoutW(progman, SPAWN_WORKERW, 0, 0, flags, SPAWN_TIMEOUT_MS, &result);
				found = findWorkerW(progman);
			}
		}

		auto gotWorkerW = found.parent != nullptr;
		if (!gotWorkerW) found = findProgman(progman);

		if (gotWorkerW) {
			this->spawnRetriesLeft = 4;
		} else if (neededSpawn && this->spawnRetriesLeft > 0) {
			this->spawnRetriesLeft--;
			this->scheduleRefresh(SPAWN_RETRY_MS);
		}
	}

	auto* iconsView = progman == nullptr ? nullptr : findIconsView(progman);
	this->applyLookup(found.parent, found.insertAfter, iconsView, first, found.layout);
}

void DesktopHost::applyLookup(
    HWND parent,
    HWND insertAfter,
    HWND iconsView,
    bool first,
    const char* layout
) {
	if ((first || iconsView != this->mIconsView) && iconsView == nullptr && parent != nullptr) {
		qCWarning(logDesktop) << "No desktop icons view found; desktop panels that take input"
		                      << "stay top level windows";
	}

	if (first || parent != this->mParent) {
		if (parent != nullptr) {
			qCInfo(logDesktop) << "Desktop panels go into" << layout << "on build" << windowsBuild();
		} else {
			qCWarning(logDesktop) << "No desktop window found (explorer not running?);"
			                      << "desktop panels stay top level windows";
		}
	}

	if (parent != this->mParent) this->setEmbedRefused(false);
	this->mParent = parent;
	this->mInsertAfter = insertAfter;
	this->mIconsView = iconsView;
	this->mIconsHost = iconsView == nullptr ? nullptr : GetAncestor(iconsView, GA_PARENT);
}

void DesktopHost::watchWallpaperKey() {
	if (this->desktopKey == nullptr) {
		if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_NOTIFY, &this->desktopKey) != ERROR_SUCCESS) {
			this->desktopKey = nullptr;
			return;
		}
		this->desktopKeyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		this->desktopKeyNotifier = new QWinEventNotifier(this->desktopKeyEvent, this);
		QObject::connect(this->desktopKeyNotifier, &QWinEventNotifier::activated, this, [this]() {
			this->watchWallpaperKey();
			emit this->wallpaperChanged();
		});
	}

	RegNotifyChangeKeyValue(this->desktopKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET, this->desktopKeyEvent, TRUE);
}

void DesktopHost::ensureListener() {
	this->watchWallpaperKey();
	if (this->listener != nullptr) return;

	auto* module = GetModuleHandleW(nullptr);
	WNDCLASSW cls {};
	cls.lpfnWndProc = &DesktopHost::listenerProc;
	cls.hInstance = module;
	cls.lpszClassName = LISTENER_CLASS;
	RegisterClassW(&cls);

	this->listener = CreateWindowExW(
	    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
	    LISTENER_CLASS,
	    L"",
	    WS_POPUP,
	    0,
	    0,
	    0,
	    0,
	    nullptr,
	    nullptr,
	    module,
	    nullptr
	);

	if (this->listener == nullptr) {
		qCWarning(logDesktop) << "Failed to create the desktop listener window:" << GetLastError()
		                      << "- explorer restarts won't be followed";
		return;
	}

	ChangeWindowMessageFilterEx(
	    this->listener,
	    WinAppBar::taskbarCreatedMessage(),
	    MSGFLT_ALLOW,
	    nullptr
	);
}

LRESULT CALLBACK DesktopHost::listenerProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	if (gHost != nullptr) {
		if (msg == WinAppBar::taskbarCreatedMessage()) {
			gHost->scheduleRefresh();
		} else if (msg == WM_DISPLAYCHANGE) {
			gHost->scheduleRefresh(SETTLE_MS);
		} else if (msg == WM_SETTINGCHANGE && wparam == SPI_SETDESKWALLPAPER) {
			gHost->scheduleRefresh(SETTLE_MS);
		}
	}

	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void DesktopHost::installHook() {
	auto* target = this->mParent != nullptr ? this->mParent : FindWindowW(L"Progman", nullptr);
	DWORD pid = 0;
	auto thread = target == nullptr ? 0 : GetWindowThreadProcessId(target, &pid);
	if (thread == this->hookThread && (thread == 0 || this->hook != nullptr)) return;

	this->removeHook();
	if (thread == 0) return;

	this->hook = SetWinEventHook(
	    EVENT_OBJECT_CREATE,
	    EVENT_OBJECT_REORDER,
	    nullptr,
	    &DesktopHost::eventProc,
	    pid,
	    thread,
	    WINEVENT_OUTOFCONTEXT
	);

	this->moveHook = SetWinEventHook(
	    EVENT_OBJECT_LOCATIONCHANGE,
	    EVENT_OBJECT_PARENTCHANGE,
	    nullptr,
	    &DesktopHost::eventProc,
	    pid,
	    thread,
	    WINEVENT_OUTOFCONTEXT
	);

	this->hookThread = thread;

	if (this->hook == nullptr || this->moveHook == nullptr) {
		qCWarning(logDesktop) << "Could not watch the desktop window:" << GetLastError();
	}
}

void DesktopHost::removeHook() {
	if (this->hook != nullptr) UnhookWinEvent(this->hook);
	if (this->moveHook != nullptr) UnhookWinEvent(this->moveHook);
	this->hook = nullptr;
	this->moveHook = nullptr;
	this->hookThread = 0;
	this->removeForeignHooks();
}

void DesktopHost::installReconcileHook() {
#ifdef QS_TEST
	if (this->eventRouteDisabledForTesting) return;
#endif
	if (this->reorderHook != nullptr && this->reparentHook != nullptr) return;

	// Both hooks are desktop-wide: the owner is another process whose id is
	// not known until it has already attached, so no narrower scope can
	// report its arrival. They are out-of-context, which sends every event
	// here through this thread's message queue. Include this process: a panel
	// can be shown/restacked after its initial group placement, while the
	// foreign wallpaper's HWND, visibility and acknowledgement stay unchanged.
	//
	// Each hook is installed only while missing: a pair where one registration
	// failed keeps the one that succeeded, and the next refresh retries only the
	// other instead of overwriting (and leaking) a live handle.
	if (this->reorderHook == nullptr) {
		// SHOW, HIDE and REORDER are contiguous.
		this->reorderHook = SetWinEventHook(
		    EVENT_OBJECT_SHOW,
		    EVENT_OBJECT_REORDER,
		    nullptr,
		    &DesktopHost::reconcileEventProc,
		    0,
		    0,
		    RECONCILE_HOOK_FLAGS
		);

		if (this->reorderHook == nullptr) {
			qCWarning(logDesktop) << "Could not watch other processes' desktop reorders:"
			                      << GetLastError();
		}
	}

	if (this->reparentHook == nullptr) {
		this->reparentHook = SetWinEventHook(
		    EVENT_OBJECT_PARENTCHANGE,
		    EVENT_OBJECT_PARENTCHANGE,
		    nullptr,
		    &DesktopHost::reconcileEventProc,
		    0,
		    0,
		    RECONCILE_HOOK_FLAGS
		);

		if (this->reparentHook == nullptr) {
			qCWarning(logDesktop) << "Could not watch other processes' desktop repairs:"
			                      << GetLastError();
		}
	}
}

void DesktopHost::removeReconcileHook() {
	if (this->reorderHook != nullptr) UnhookWinEvent(this->reorderHook);
	if (this->reparentHook != nullptr) UnhookWinEvent(this->reparentHook);
	this->reorderHook = nullptr;
	this->reparentHook = nullptr;
}

void CALLBACK DesktopHost::reconcileEventProc(
    HWINEVENTHOOK /*hook*/,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD /*thread*/,
    DWORD /*time*/
) {
	auto* host = gHost;
	if (host == nullptr || hwnd == nullptr || host->mParent == nullptr) return;

	// A reorder names the container whose children changed, whichever process
	// changed them. Only the host's own list can move the desktop layer.
	if (event == EVENT_OBJECT_REORDER) {
		if (hwnd == host->mParent) host->queueReconcile();
		return;
	}

	// A window shown or hidden directly inside the host, from any process. The
	// parent check is cheap and discards the system-wide show/hide traffic.
	if (event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_HIDE) {
		if (idObject == OBJID_WINDOW && idChild == CHILDID_SELF
		    && GetAncestor(hwnd, GA_PARENT) == host->mParent)
		{
			host->queueReconcile();
		}
		return;
	}

	// A parent change does not name the old parent, so a child that just left
	// the host cannot be recognized by its current parent. Parent changes are
	// rare and the reconcile only reads the host's few children, so every one
	// asks for it.
	if (idObject == OBJID_WINDOW && idChild == CHILDID_SELF) host->queueReconcile();
}

void CALLBACK DesktopHost::foreignEventProc(
    HWINEVENTHOOK /*hook*/,
    DWORD event,
    HWND hwnd,
    LONG /*idObject*/,
    LONG /*idChild*/,
    DWORD /*thread*/,
    DWORD /*time*/
) {
	auto* host = gHost;
	if (host == nullptr || hwnd == nullptr || host->mParent == nullptr) return;

	// Only what can change the host's child order matters: the host's own
	// client reorder, or a child of the host. A destruction (the window is
	// already gone) and a parent change (the window may just have left the
	// host, so its new parent is not the host any more) always count.
	if (event != EVENT_OBJECT_DESTROY && event != EVENT_OBJECT_PARENTCHANGE
	    && hwnd != host->mParent && GetAncestor(hwnd, GA_PARENT) != host->mParent)
	{
		return;
	}

	host->queueReconcile();
}

void CALLBACK DesktopHost::eventProc(
    HWINEVENTHOOK /*hook*/,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD /*thread*/,
    DWORD /*time*/
) {
	auto* host = gHost;
	if (host == nullptr || hwnd == nullptr) return;

	auto scheduleMoved = [host] {
		if (host->movePending) return;
		host->movePending = true;

		QTimer::singleShot(0, host, [host] {
			host->movePending = false;
			// A child reorder is how an external wallpaper window appears in
			// or disappears from the host; keep the acknowledgement current
			// before the windows place themselves again.
			host->updateReportedState();
			emit host->parentMoved();
		});
	};

	// WorkerW child reorders arrive with OBJID_CLIENT, not OBJID_WINDOW.
	// Without them an external wallpaper restack never refreshes the host.
	if (event == EVENT_OBJECT_REORDER && idObject == OBJID_CLIENT) {
		if (hwnd == host->mParent) scheduleMoved();
		return;
	}

	if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

	if (hwnd == host->mParent) {
		switch (event) {
		case EVENT_OBJECT_LOCATIONCHANGE:
		case EVENT_OBJECT_REORDER: scheduleMoved(); break;
		case EVENT_OBJECT_DESTROY:
		case EVENT_OBJECT_HIDE: host->scheduleRefresh(SETTLE_MS); break;
		default: break;
		}

		if (hwnd != host->mIconsHost) return;
	}

	// A WorkerW appearing can be the host Explorer will show from now on. While
	// a visible foreign wallpaper child is inside the current host, the shell
	// is an acknowledged underlay and must not abandon it for a WorkerW the
	// wallpaper process itself is creating. Once the shell owns the desktop
	// again (no visible foreign child), a new WorkerW is a host replacement
	// and discovery has to run instead of keeping the stale parent.
	if (event == EVENT_OBJECT_SHOW && hasClass(hwnd, L"WorkerW")
	    && (host->mParent == nullptr || host->mInsertAfter != nullptr
	        || !desktopHostHasForeignChild(host->mParent, true)))
	{
		host->scheduleRefresh(SETTLE_MS);
		return;
	}

	if (host->mIconsView == nullptr) {
		if (event == EVENT_OBJECT_SHOW && host->mParent != nullptr
		    && hasClass(hwnd, L"SHELLDLL_DefView"))
		{
			host->scheduleRefresh(SETTLE_MS);
		}

		return;
	}

	if (hwnd == host->mIconsView) {
		if (event == EVENT_OBJECT_DESTROY || event == EVENT_OBJECT_HIDE
		    || event == EVENT_OBJECT_PARENTCHANGE)
		{
			host->scheduleRefresh(SETTLE_MS);
		}

		return;
	}

	if (hwnd == host->mIconsHost) {
		switch (event) {
		case EVENT_OBJECT_DESTROY:
		case EVENT_OBJECT_HIDE: host->scheduleRefresh(SETTLE_MS); return;
		case EVENT_OBJECT_LOCATIONCHANGE: scheduleMoved(); return;
		case EVENT_OBJECT_REORDER: break;
		default: return;
		}
	} else if ((event != EVENT_OBJECT_CREATE && event != EVENT_OBJECT_SHOW)
	           || GetAncestor(hwnd, GA_PARENT) != host->mIconsHost)
	{
		return;
	}

	if (GetAncestor(host->mIconsView, GA_PARENT) != host->mIconsHost) {
		host->scheduleRefresh(SETTLE_MS);
		return;
	}

	if (!host->restackPending) {
		host->restackPending = true;

		QTimer::singleShot(0, host, [host] {
			host->restackPending = false;
			emit host->iconsRestacked();
		});
	}
}

#ifdef QS_TEST
void DesktopHost::adoptHostForTesting(HWND parent, HWND insertAfter) {
	this->mEnabled = true;
	this->mParent = parent;
	this->mInsertAfter = insertAfter;
	this->mIconsView = nullptr;
	this->mIconsHost = nullptr;
	this->lookedUp = true;
	this->setEmbedRefused(false);
	this->adoptedForTesting = true;
	// The production hooks, scoped to the adopted host's process and thread.
	this->installHook();
	this->updateReportedState();
}

void DesktopHost::setDiscoveredHostForTesting(HWND parent, HWND insertAfter) {
	this->testDiscoveredParent = parent;
	this->testDiscoveredInsertAfter = insertAfter;
}

void DesktopHost::setReconcileSourcesForTesting(bool eventRoute, bool foreignHooks) {
	this->eventRouteDisabledForTesting = !eventRoute;
	this->foreignHooksDisabledForTesting = !foreignHooks;

	if (eventRoute) this->installReconcileHook();
	else this->removeReconcileHook();

	this->syncForeignWatch();
}

int desktopBackgroundMovesForTesting() { return gPlacementMoves; }
#endif

DesktopLayer::DesktopLayer(QObject* parent): QObject(parent) {
	auto* host = DesktopHost::instance();
	QObject::connect(host, &DesktopHost::enabledChanged, this, &DesktopLayer::enabledChanged);
	QObject::connect(
	    host,
	    &DesktopHost::externalWallpaperEnabledChanged,
	    this,
	    &DesktopLayer::externalWallpaperEnabledChanged
	);
	QObject::connect(host, &DesktopHost::activeChanged, this, &DesktopLayer::activeChanged);
	QObject::connect(host, &DesktopHost::underlayActiveChanged, this, &DesktopLayer::underlayActiveChanged);
	QObject::connect(host, &DesktopHost::embedRefusedChanged, this, &DesktopLayer::embedRefusedChanged);
	QObject::connect(host, &DesktopHost::aboveIconsChanged, this, &DesktopLayer::aboveIconsChanged);
	QObject::connect(host, &DesktopHost::wallpaperChanged, this, &DesktopLayer::wallpaperChanged);
}

bool DesktopLayer::enabled() const { return DesktopHost::instance()->enabled(); }
void DesktopLayer::setEnabled(bool enabled) { DesktopHost::instance()->setEnabled(enabled); }
bool DesktopLayer::externalWallpaperEnabled() const {
	return DesktopHost::instance()->externalWallpaperEnabled();
}
void DesktopLayer::setExternalWallpaperEnabled(bool enabled) {
	DesktopHost::instance()->setExternalWallpaperEnabled(enabled);
}
bool DesktopLayer::active() const { return DesktopHost::instance()->active(); }
bool DesktopLayer::underlayActive() const { return DesktopHost::instance()->underlayActive(); }

bool DesktopLayer::embedRefused() const { return DesktopHost::instance()->embedRefused(); }

QStringList DesktopLayer::aboveIcons() const { return DesktopHost::instance()->aboveIcons(); }

void DesktopLayer::setAboveIcons(const QStringList& namespaces) {
	DesktopHost::instance()->setAboveIcons(namespaces);
}

} // namespace qs::windows

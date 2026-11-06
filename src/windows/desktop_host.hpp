#pragma once

#include <array>

#include <qt_windows.h>

#include <qhash.h>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qstringlist.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>
#include <qwineventnotifier.h>

namespace qs::windows {

// A window inside the desktop wallpaper host that does not belong to this
// process. Live wallpapers (Wallpaper Engine and friends) add one while they
// own the desktop. `visibleOnly` selects whether a hidden child counts: the
// underlay acknowledgement and the z-order placement only treat a visible one
// as a coverer, while hook discovery still watches a hidden child so its show
// is repaired immediately.
[[nodiscard]] bool desktopHostHasForeignChild(HWND parent, bool visibleOnly = true);

// Marks a Background panel window for placeDesktopBackgrounds().
void markDesktopBackground(HWND hwnd, bool marked);

// Re-orders the marked Background windows inside `parent`. Only a visible
// foreign child covers the desktop: while one exists every background sits
// below the bottom-most visible foreign sibling. When every foreign child is
// hidden or gone, the group goes to the top of the host instead, so a hidden
// wallpaper window cannot leave the user looking at a bare host. Both paths
// keep the backgrounds' relative order and are idempotent, so a queued reorder
// cannot start a restack loop.
void placeDesktopBackgrounds(HWND parent);

// True when the marked Backgrounds are not where placeDesktopBackgrounds()
// would put them: above a visible foreign child, or not on top of the host
// while no foreign child is visible. Read-only and cheap, so every reconciling
// event can ask and only call placeDesktopBackgrounds() when it drifted.
[[nodiscard]] bool desktopBackgroundsMisordered(HWND parent);

#ifdef QS_TEST
// Number of SetWindowPos calls placeDesktopBackgrounds() has made, to prove an
// already correct order is not restacked.
[[nodiscard]] int desktopBackgroundMovesForTesting();
#endif

class DesktopHost: public QObject {
	Q_OBJECT;

public:
	static DesktopHost* instance();
	~DesktopHost() override;
	Q_DISABLE_COPY_MOVE(DesktopHost);

	[[nodiscard]] bool enabled() const { return this->mEnabled; }
	void setEnabled(bool enabled);

	[[nodiscard]] HWND parentWindow();
	[[nodiscard]] HWND insertAfter() const { return this->mInsertAfter; }
	[[nodiscard]] HWND iconsHost();

	[[nodiscard]] QStringList aboveIcons() const { return this->mAboveIcons; }
	void setAboveIcons(const QStringList& namespaces);
	// Requested icon replacement must never cover a native wallpaper owner,
	// even when optional process/config discovery failed. This is a
	// host-wide layering rule, not an acknowledgement of a rendered frame.
	[[nodiscard]] bool allowsAboveIcons(const QString& ns);

	[[nodiscard]] bool active() const { return this->mEnabled && this->mParent != nullptr; }

#ifdef QS_TEST
	// Adopt a synthetic desktop hierarchy instead of looking up Progman, so
	// tests can exercise the real host state against temporary windows.
	void adoptHostForTesting(HWND parent, HWND insertAfter = nullptr);
	// Answer discovery with `parent`, so a test can move the desktop host the
	// way Explorer does on a handback. Passing nullptr restores the adopted
	// host.
	void setDiscoveredHostForTesting(HWND parent, HWND insertAfter = nullptr);
	// Re-run discovery now against the test host, as the handback edge does,
	// so a test can replace the host without toggling the desktop layer off.
	void refreshForTesting() { this->refresh(); }
	// Chooses which reconciliation sources run: the desktop-wide event hooks
	// that replaced the 1 s poll, and the hooks scoped to the foreign
	// wallpaper processes. A test turns one off to prove the other repairs the
	// order on its own, or both off for a negative control.
	void setReconcileSourcesForTesting(bool eventRoute, bool foreignHooks);
#endif

	// The window can stay embedded beneath an external wallpaper. That needs
	// an active host with no insert-after, no icons embedding and a *visible*
	// foreign wallpaper window inside it. A hidden child releases the
	// acknowledgment and, through the same visibility rule, sends the
	// Background group to the top of the host (see placeDesktopBackgrounds).
	[[nodiscard]] bool underlayActive() const {
		return this->mEnabled && this->mParent != nullptr && !this->mEmbedRefused
		    && this->mInsertAfter == nullptr && this->mAboveIcons.isEmpty()
		    && desktopHostHasForeignChild(this->mParent, true);
	}

	// Called when a panel window could not be reparented into the desktop.
	// The window stays top level, so it cannot be reported as an underlay.
	void noteEmbedRefused(HWND parent);

	// A refused SetParent leaves a shell panel top level. The acknowledgement
	// is withdrawn, but the host stays active, so consumers need this separate
	// signal to tell a refusal from the external wallpaper window leaving.
	[[nodiscard]] bool embedRefused() const { return this->mEmbedRefused; }

signals:
	void enabledChanged();
	void activeChanged();
	void underlayActiveChanged();
	void embedRefusedChanged();
	// A foreign wallpaper child appeared in, left, or changed visibility in
	// the host. Background windows must place themselves again: hiding one
	// releases the group to the top of the host, showing it pins the group
	// below it again.
	void backgroundOrderChanged();
	void parentChanged();
	void parentMoved();
	void aboveIconsChanged();
	void iconsRestacked();
	void wallpaperChanged();

private:
	explicit DesktopHost(QObject* parent);

	void ensureListener();
	void watchWallpaperKey();
	void scheduleRefresh(int delayMs = 0);
	void refresh();
	void lookup();
	void applyLookup(HWND parent, HWND insertAfter, HWND iconsView, bool first, const char* layout);
	void setEmbedRefused(bool refused);
	void updateReportedState();
	void syncForeignWatch();
	void syncForeignHooks(bool watch);
	void removeForeignHooks();
	void installHook();
	void removeHook();
	void installReconcileHook();
	void removeReconcileHook();
	// Coalesces the events of one message-loop turn into one read-only pass.
	void queueReconcile();

	static LRESULT CALLBACK listenerProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
	static void CALLBACK eventProc(
	    HWINEVENTHOOK hook,
	    DWORD event,
	    HWND hwnd,
	    LONG idObject,
	    LONG idChild,
	    DWORD thread,
	    DWORD time
	);
	// Desktop-wide hooks, not scoped to any process or thread: the wallpaper
	// owner is another process whose id is unknown until it has attached.
	static void CALLBACK reconcileEventProc(
	    HWINEVENTHOOK hook,
	    DWORD event,
	    HWND hwnd,
	    LONG idObject,
	    LONG idChild,
	    DWORD thread,
	    DWORD time
	);
	// Hooks scoped to a foreign wallpaper process, not to Explorer.
	static void CALLBACK foreignEventProc(
	    HWINEVENTHOOK hook,
	    DWORD event,
	    HWND hwnd,
	    LONG idObject,
	    LONG idChild,
	    DWORD thread,
	    DWORD time
	);

	bool mEnabled = false;
	bool lookedUp = false;
	HWND mParent = nullptr;
	HWND mInsertAfter = nullptr;
	HWND mIconsView = nullptr;
	HWND mIconsHost = nullptr;
	QStringList mAboveIcons;
	bool mEmbedRefused = false;
	bool mLastActive = false;
	bool mLastUnderlayActive = false;
	bool mLastVisibleForeign = false;
	HWND listener = nullptr;
	HKEY desktopKey = nullptr;
	HANDLE desktopKeyEvent = nullptr;
	QWinEventNotifier* desktopKeyNotifier = nullptr;
	HWINEVENTHOOK hook = nullptr;
	HWINEVENTHOOK moveHook = nullptr;
	// EVENT_OBJECT_REORDER and EVENT_OBJECT_PARENTCHANGE across the desktop.
	HWINEVENTHOOK reorderHook = nullptr;
	HWINEVENTHOOK reparentHook = nullptr;
	DWORD hookThread = 0;
	bool movePending = false;
	bool restackPending = false;
	bool reconcilePending = false;
	int spawnRetriesLeft = 4;
	QTimer refreshTimer;
	// Per foreign process: a lifecycle hook (create..reorder) and a parent
	// change hook. They report a known owner's visibility and destruction
	// edges; the desktop-wide hooks above report everything else.
	QHash<DWORD, std::array<HWINEVENTHOOK, 2>> foreignHooks;
#ifdef QS_TEST
	bool adoptedForTesting = false;
	bool eventRouteDisabledForTesting = false;
	bool foreignHooksDisabledForTesting = false;
	HWND testDiscoveredParent = nullptr;
	HWND testDiscoveredInsertAfter = nullptr;
#endif
};

class DesktopLayer: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	// clang-format off
	Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged);
	Q_PROPERTY(bool active READ active NOTIFY activeChanged);
	Q_PROPERTY(bool underlayActive READ underlayActive NOTIFY underlayActiveChanged);
	Q_PROPERTY(bool embedRefused READ embedRefused NOTIFY embedRefusedChanged);
	Q_PROPERTY(QStringList aboveIcons READ aboveIcons WRITE setAboveIcons NOTIFY aboveIconsChanged);
	// clang-format on

public:
	explicit DesktopLayer(QObject* parent = nullptr);

	[[nodiscard]] bool enabled() const;
	void setEnabled(bool enabled);

	[[nodiscard]] bool active() const;

	[[nodiscard]] bool underlayActive() const;

	[[nodiscard]] bool embedRefused() const;

	[[nodiscard]] QStringList aboveIcons() const;
	void setAboveIcons(const QStringList& namespaces);

signals:
	void enabledChanged();
	void activeChanged();
	void underlayActiveChanged();
	void embedRefusedChanged();
	void aboveIconsChanged();
	void wallpaperChanged();
};

} // namespace qs::windows

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <unordered_map>

#include <qcolor.h>
#include <qhash.h>
#include <qlist.h>
#include <qobject.h>
#include <qpointer.h>
#include <qqmlintegration.h>
#include <qrect.h>
#include <qregularexpression.h>
#include <qscreen.h>
#include <qset.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qt_windows.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include "tiling_layout.hpp"
#include "tiling_scope.hpp"

namespace qs::windows {

class TrackedWindow;
class WindowTracker;
class VirtualDesktops;

// A facade generation's lease on the process-lifetime tiling session. The
// session outlives every QML generation but must not outlive the generation
// that asked for it: the holder claims a generation on its first write and
// every write carries it, and the manager refuses a write from a superseded
// generation. A retiring generation whose wrapper still runs its destruction
// handler therefore cannot switch off the session its replacement already
// claimed. Releasing (explicitly or on destruction) switches tiling off through
// the same check without needing JavaScript to run during engine teardown, so
// the session cannot keep tiling after its owner is gone. Releasing twice is a
// no-op.
class TilingLease {
public:
	TilingLease() = default;
	~TilingLease();
	Q_DISABLE_COPY_MOVE(TilingLease);

	quint32 claim();
	void release();
	[[nodiscard]] quint32 generation() const { return this->mGeneration; }

private:
	quint32 mGeneration = 0;
};

class TilingManager: public QObject {
	Q_OBJECT;

public:
	using Edge = tiling::Edge;

	// A title-bar drag (the native move loop) or a Super drag.
	enum class DragGesture : quint8 { Caption, Super };
	enum class DropMode : quint8 { Home, Swap, Insert, Move };

	static TilingManager* instance();
	static TilingManager* active();

	[[nodiscard]] bool enabled() const { return this->mEnabled; }
	// Hands the next generation to a facade lease.
	quint32 claimGeneration();
	// Only the holder of the latest generation may change the session.
	// Returns false when a superseded holder's late write is refused.
	bool setEnabled(bool enabled, quint32 generation);
	[[nodiscard]] qint32 gapsIn() const { return this->mGapsIn; }
	void setGapsIn(qint32 gaps);
	[[nodiscard]] qint32 gapsOut() const { return this->mGapsOut; }
	void setGapsOut(qint32 gaps);
	[[nodiscard]] qint32 singleWindowGapsOut() const { return this->mSingleWindowGapsOut; }
	void setSingleWindowGapsOut(qint32 gaps);
	[[nodiscard]] bool borderColours() const { return this->mBorderColours; }
	void setBorderColours(bool enabled);
	[[nodiscard]] QColor activeBorderColour() const { return this->mActiveBorderColour; }
	void setActiveBorderColour(const QColor& colour);
	[[nodiscard]] QColor inactiveBorderColour() const { return this->mInactiveBorderColour; }
	void setInactiveBorderColour(const QColor& colour);
	[[nodiscard]] bool preserveSplit() const { return this->mPreserveSplit; }
	void setPreserveSplit(bool preserve);
	[[nodiscard]] QStringList excluded() const { return this->mExcluded; }
	void setExcluded(const QStringList& excluded);
	[[nodiscard]] QStringList included() const { return this->mIncluded; }
	[[nodiscard]] qint32 includedRuleCount() const { return this->scopeRules.includedRuleCount(); }
	void setIncluded(const QStringList& included);
	[[nodiscard]] QString layout() const { return this->mLayout; }
	void setLayout(const QString& layout);
	[[nodiscard]] bool smartSplit() const { return this->mSmartSplit; }
	void setSmartSplit(bool smart);
	[[nodiscard]] qint32 forceSplit() const { return this->mForceSplit; }
	void setForceSplit(qint32 force);
	[[nodiscard]] double defaultSplitRatio() const { return this->mDefaultSplitRatio; }
	void setDefaultSplitRatio(double ratio);
	[[nodiscard]] QString masterNewStatus() const { return this->mMasterNewStatus; }
	void setMasterNewStatus(const QString& status);
	[[nodiscard]] bool masterNewOnTop() const { return this->mMasterNewOnTop; }
	void setMasterNewOnTop(bool onTop);
	[[nodiscard]] QString masterOrientation() const { return this->mMasterOrientation; }
	void setMasterOrientation(const QString& orientation);
	[[nodiscard]] double masterFactor() const { return this->mMasterFactor; }
	void setMasterFactor(double factor);
	[[nodiscard]] double scrollingColumnWidth() const { return this->mScrollingColumnWidth; }
	void setScrollingColumnWidth(double width);
	[[nodiscard]] QString scrollingCenter() const { return this->mScrollingCenter; }
	void setScrollingCenter(const QString& center);

	static void restoreForCrash();

	void relayout();

	[[nodiscard]] bool isTiled(TrackedWindow* window) const;
	[[nodiscard]] TrackedWindow* focusedWindow() const;

	[[nodiscard]] tiling::TilingTelemetry* telemetry() const { return this->mTelemetry; }

	// The drop the current move of a tiled window would make, for the shell's
	// preview. Rectangles are logical pixels local to the screen dragScreen()
	// names; they are null for a drop that changes nothing.
	[[nodiscard]] bool dragActive() const { return this->drag.active; }
	[[nodiscard]] QString dragMode() const;
	[[nodiscard]] QString dragScreen() const;
	[[nodiscard]] QRectF dragLanding() const;
	[[nodiscard]] QRectF dragDisplaced() const;

	bool focusDirection(Edge direction);
	bool moveDirection(Edge direction);
	bool swapDirection(Edge direction);
	bool toggleSplit(TrackedWindow* window);
	bool swapSplit(TrackedWindow* window);
	bool splitRatio(TrackedWindow* window, double value, bool exact);
	bool masterRatio(TrackedWindow* window, double value, bool exact);
	bool swapWithMaster(TrackedWindow* window);
	bool focusMaster(TrackedWindow* window);
	bool resizeTiled(TrackedWindow* window, qint32 dx, qint32 dy);
	bool setFloating(TrackedWindow* window, bool floating, bool toggle);

	bool scrollColumns(TrackedWindow* window, qint32 delta);
	bool scrollView(TrackedWindow* window, qint32 pixels);
	bool columnWidth(TrackedWindow* window, double value, bool relative);
	bool cycleColumnWidth(TrackedWindow* window, qint32 direction);
	bool allColumnsWidth(TrackedWindow* window, double value);
	bool fitColumns(TrackedWindow* window, tiling::FitMode mode);
	bool focusColumn(TrackedWindow* window, Edge direction);
	bool moveIntoColumn(TrackedWindow* window, Edge direction);
	bool swapColumn(TrackedWindow* window, Edge direction);
	bool promote(TrackedWindow* window);
	bool centerColumn(TrackedWindow* window);
	bool toggleFit(TrackedWindow* window);
	bool toggleFullWidth(TrackedWindow* window);

	bool beginEdgeDrag(TrackedWindow* window, bool left, bool top);
	void edgeDrag(const QPoint& delta);
	void endEdgeDrag();
	// Esc during a Super drag: drop the preview; the end commits nothing.
	void cancelDrag(TrackedWindow* window);

	static void resizeFloating(TrackedWindow* window, qint32 dx, qint32 dy, bool exact);
	static void center(TrackedWindow* window);
	void moveFloating(TrackedWindow* window, Edge direction) const;

signals:
	void enabledChanged();
	void gapsInChanged();
	void gapsOutChanged();
	void singleWindowGapsOutChanged();
	void borderColoursChanged();
	void activeBorderColourChanged();
	void inactiveBorderColourChanged();
	void preserveSplitChanged();
	void excludedChanged();
	void includedChanged();
	void layoutChanged();
	void smartSplitChanged();
	void forceSplitChanged();
	void defaultSplitRatioChanged();
	void masterNewStatusChanged();
	void masterNewOnTopChanged();
	void masterOrientationChanged();
	void masterFactorChanged();
	void scrollingColumnWidthChanged();
	void scrollingCenterChanged();
	void windowTilingChanged(TrackedWindow* window);
	void dragChanged();

private:
	friend class TilingManagerTest;
	friend class TilingReloadTest;
	friend class TilingDragTest;
	explicit TilingManager(QString mutexName = QStringLiteral("Local\\QuickshellTiling"));
	~TilingManager() override;
	Q_DISABLE_COPY_MOVE(TilingManager);

	struct Layout {
		GUID desktop {};
		QPointer<QScreen> screen;
		std::unique_ptr<tiling::LayoutEngine> tree;
		TrackedWindow* lastFocused = nullptr;
	};

	enum class State : quint8 {
		Out,
		Hidden,
		Suspended,
		Tiled,
	};

	enum class Override : quint8 { None, Float, Tile };

	struct EdgeDrag {
		TrackedWindow* window = nullptr;
		Layout* layout = nullptr;
		QRect box;
		std::optional<Edge> horizontal;
		std::optional<Edge> vertical;
	};

	struct Managed {
		TrackedWindow* window = nullptr;
		tiling::PlacementIdentity placement;
		tiling::PlacementDisposition disposition = tiling::PlacementDisposition::Idle;
		Layout* layout = nullptr;
		Override override = Override::None;
		bool elevated = false;
		bool autoFloat = false;
		// The layout an automatic float left. A sibling closing there frees
		// capacity and gives the window one fresh attempt.
		GUID autoFloatDesktop {};
		QPointer<QScreen> autoFloatScreen;
		bool fullscreenSuspended = false;
		int traceState = -1;
		qint64 fullscreenRecoveryUntil = 0;
		// A restored frame style can precede the app's own restore rectangle.
		// Keep the slot suspended briefly instead of resizing mid-transition.
		qint64 fullscreenExitNotBefore = 0;
		// Frame style recorded while the window is fullscreen. Regaining
		// WS_CAPTION or WS_THICKFRAME confirms an exit even when the rectangle
		// still looks fullscreen (a restored fullscreen-era normal placement).
		bool fullscreenStyleValid = false;
		LONG_PTR fullscreenStyle = 0;
		LONG_PTR fullscreenExStyle = 0;
		QRect target;
		QRect before;
		QRect accepted;
		// Last frame place() saw, when it last changed, and when waiting for it
		// to hold still began. An application resizing itself defers corrections
		// until it has been quiet; the wait is spent once per episode.
		QRect observed;
		qint64 changedAt = 0;
		qint64 settleSince = 0;
		bool settleSpent = false;
		// The window resized itself and has settled: its next correction eases
		// it back into the tile instead of jumping.
		bool selfResized = false;
		qint32 snaps = 0;
		bool snapping = false;
		QRect snapFrom;
		qint64 snapStart = 0;
		qint32 corrections = 0;
		// A larger frame was moved back into the work area for this tile.
		bool overflowClamped = false;
		qint64 lastCorrection = 0;
		bool verifying = false;
		qint64 deadline = 0;
		bool parked = false;
		QRect preTiling;
		quint64 focusStamp = 0;
		bool offscreen = false;
		QRect restore;
		std::optional<COLORREF> borderColour;
		// A window that appears while the tiler is running opens on the
		// focused monitor, as in Hyprland, instead of wherever Windows first
		// placed it. Cleared the first time the window is looked at.
		QPointer<QScreen> placeOn;
	};

	// What releasing a dragged tiled window would do. The preview and the
	// release both come from planDrop(), so the preview is the result.
	struct DropPlan {
		DropMode mode = DropMode::Home;
		Layout* layout = nullptr;
		TrackedWindow* target = nullptr;
		std::optional<Edge> side;
		// Physical frames after the drop: where the dragged window lands, and
		// where the other window goes when exactly one other tile moves and it
		// stays on this screen. Null when nothing is drawn.
		QRect landing;
		QRect displaced;
		QScreen* screen = nullptr;
		// The cursor can move anywhere inside this without changing the plan.
		QRect zone;
		// dropGeneration when the frames were computed.
		quint64 generation = 0;
	};

	// A move-size loop is classified at its first location change: a resize
	// changed the size of a window that was not maximized when it started.
	enum class DragKind : quint8 { Unknown, Move, Resize };

	struct Drag {
		DragGesture gesture = DragGesture::Caption;
		DragKind kind = DragKind::Unknown;
		bool zoomed = false;
		QRect frame;
		QPoint cursor;
		QSize threshold;
		// The cursor moved past the system drag threshold; the plan is shown.
		bool active = false;
		// The planned target stopped being a valid drop while the cursor was
		// still: the drop is home until the cursor moves past the drag
		// threshold from where it was when the plan was invalidated.
		bool invalidated = false;
		QPoint invalidatedAt;
		// Esc cancelled the drag; its end commits nothing.
		bool cancelled = false;
		DropPlan plan;
	};

	// Applies a change with no ownership check. Reached through the checked
	// overload above and directly from fixtures.
	void setEnabled(bool enabled);
	// Returns an empty string when tiling started, or a stable machine readable
	// reason when it was refused.
	[[nodiscard]] QString start();
	void stop();
	void manage(TrackedWindow* window);
	void unmanage(TrackedWindow* window);

	[[nodiscard]] State stateOf(const Managed& m) const;
	[[nodiscard]] bool floatsByRule(const Managed& m) const;
	[[nodiscard]] bool fullscreenLike(const Managed& m) const;
	[[nodiscard]] bool exitedFullscreen(const Managed& m) const;
	[[nodiscard]] tiling::WindowFacts factsOf(const Managed& m) const;
	[[nodiscard]] bool outsideScope(const Managed& m) const;
	[[nodiscard]] Layout* layoutFor(TrackedWindow* window, bool create);
	[[nodiscard]] Layout* layoutAt(const GUID& desktop, QScreen* screen, bool create);
	[[nodiscard]] bool layoutAlive(const Layout* layout) const;
	[[nodiscard]] static tiling::LayoutEngine::Id idOf(TrackedWindow* window);
	[[nodiscard]] TrackedWindow* windowOf(tiling::LayoutEngine::Id id) const;
	[[nodiscard]] tiling::Kind layoutKind() const;
	[[nodiscard]] tiling::Options options() const;
	void rebuildLayouts();

	void markWindow(TrackedWindow* window);
	void markLayout(Layout* layout);
	void schedule();
	void sync();
	void syncWindow(TrackedWindow* window);
	void insertInto(Managed& m, Layout* layout, TrackedWindow* target, const QPoint* cursor);
	void takeOut(Managed& m);
	void dropEmptyLayouts();
	void apply(Layout* layout);
	// The tree computation apply() does, for a live tree or a drop simulation.
	// Returns the work area, or a null rectangle when the monitor is gone.
	QRect computeTree(tiling::LayoutEngine& tree, QScreen* screen) const;
	[[nodiscard]] QRect tileRect(const Layout* layout, TrackedWindow* window) const;
	[[nodiscard]] QRect
	tileRect(const tiling::LayoutEngine& tree, QScreen* screen, tiling::LayoutEngine::Id id) const;
	[[nodiscard]] qint32 outerGap(const Layout* layout) const;
	[[nodiscard]] qint32 outerGap(const tiling::LayoutEngine& tree) const;
	void refreshBorderColours();
	void restoreBorderColour(Managed& m);
	void restoreBorderColours();
	void place(Managed& m, const QRect& tile);
	void send(Managed& m, const QRect& frame);
	void completePlacement(Managed& m, const QRect& frame);
	void reportPlacement(Managed& m, tiling::PlacementDisposition disposition,
	                     const QRect& frame = {}, quint32 nativeError = 0);
	void supersedePlacement(Managed& m);
	bool placementAlive(Managed& m);
	void notifyTilingChanged(TrackedWindow* window);
	void armDeadline();
	void restoreFloating(Managed& m);

	[[nodiscard]] static tiling::ScrollingLayout* scrollingOf(const Layout* layout);
	[[nodiscard]] tiling::ScrollingLayout* scrollingFor(TrackedWindow* window, Layout** layout);
	void prepareScrolling(Layout* layout) const;
	void prepareScrolling(tiling::LayoutEngine& tree, QScreen* screen) const;
	void settle(Layout* layout);
	void refocus(Layout* layout);
	bool focusIdleStrip();
	void markOffscreen(Managed& m, bool offscreen, const QRect& restore);
	void unpark(Managed& m);
	void unparkAll();
	static void rescueParked();

	void onActiveWindowChanged();
	void onRectChanged(TrackedWindow* window);
	void onMoveSizeStarted(TrackedWindow* window, bool super = false);
	void onMoveSizeEnded(TrackedWindow* window);
	void onDragMoved(TrackedWindow* window);
	void onScreensChanged();
	void verify();
	void onDeadline();
	void startSnap(Managed& m, const QRect& frame);
	void stepSnaps();
	[[nodiscard]] DropPlan
	planDrop(const Managed& m, const QPoint& cursor, DragGesture gesture, const DropPlan* previous) const;
	void simulateDrop(const Managed& m, DropPlan& plan) const;
	void commitDrop(Managed& m, const DropPlan& plan);
	void showDrag(const DropPlan& plan);
	void clearDrag();
	void replanDrag();
	// The planned target is no longer a valid drop: home until the user moves.
	void invalidateDragTarget();
	[[nodiscard]] bool dropTargetValid(const DropPlan& plan) const;
	[[nodiscard]] DropPlan homePlan(const Managed& m) const;
	void giveUp(Managed& m, const QRect& frame);
	void autoFloatWindow(Managed& m);
	void markAll();
	void refreshManagedCount();

	[[nodiscard]] Managed* tiledActive();
	[[nodiscard]] Managed* neighbour(const Managed& from, Edge direction);
	[[nodiscard]] QScreen* screenInDirection(QScreen* from, Edge direction) const;
	void swapWindows(Managed& a, Managed& b);
	void moveToLayout(Managed& m, Layout* layout, TrackedWindow* target,
	                  std::optional<Edge> side = std::nullopt);

	WindowTracker* tracker = nullptr;
	VirtualDesktops* desktops = nullptr;

	bool mEnabled = false;
	// Generation of the facade that owns the session; 0 before any facade
	// wrote and after the process-lifetime session has no writer.
	quint32 mOwner = 0;
	qint32 mGapsIn = 5;
	qint32 mGapsOut = 10;
	qint32 mSingleWindowGapsOut = 20;
	bool mBorderColours = true;
	QColor mActiveBorderColour;
	QColor mInactiveBorderColour;
	bool mPreserveSplit = true;
	QStringList mExcluded;
	QStringList mIncluded;
	QString mLayout = QStringLiteral("dwindle");
	bool mSmartSplit = false;
	qint32 mForceSplit = 0;
	double mDefaultSplitRatio = 1.0;
	QString mMasterNewStatus = QStringLiteral("slave");
	bool mMasterNewOnTop = false;
	QString mMasterOrientation = QStringLiteral("left");
	double mMasterFactor = 0.55;
	double mScrollingColumnWidth = 0.5;
	QString mScrollingCenter = QStringLiteral("overflow");
	bool fitToggled = false;
	tiling::ScopeRules scopeRules;
	tiling::TilingTelemetry* mTelemetry = nullptr;
	// Windows that already exist when tiling starts are not new arrivals:
	// they keep the monitor they are on.
	bool starting = false;

	std::unordered_map<TrackedWindow*, Managed> managed;
	std::map<QString, std::unique_ptr<Layout>> layouts;
	QSet<TrackedWindow*> dirtyWindows;
	QSet<Layout*> dirtyLayouts;
	QHash<TrackedWindow*, tiling::PlacementIdentity> unverified;
	TrackedWindow* dragging = nullptr;
	Drag drag;
	// Bumped by every tree or geometry change, so drop plans never reuse frames
	// computed before it.
	quint64 dropGeneration = 0;
	EdgeDrag edgeDragState;
	quint64 focusCounter = 0;
	quint64 lifetimeCounter = 0;
	quint64 layoutGeneration = 0;

	HANDLE ownerMutex = nullptr;
	QString mutexName;
	QTimer syncTimer;
	QTimer verifyTimer;
	QTimer deadlineTimer;
	QTimer snapTimer;
	QList<QMetaObject::Connection> connections;
};

class Tiling: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	// clang-format off
	Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged);
	// Typed exe/class/title/screen selectors in included/excluded.
	Q_PROPERTY(bool supportsScopedRules READ supportsScopedRules CONSTANT);
	Q_PROPERTY(qint32 gapsIn READ gapsIn WRITE setGapsIn NOTIFY gapsInChanged);
	Q_PROPERTY(qint32 gapsOut READ gapsOut WRITE setGapsOut NOTIFY gapsOutChanged);
	Q_PROPERTY(qint32 singleWindowGapsOut READ singleWindowGapsOut WRITE setSingleWindowGapsOut NOTIFY singleWindowGapsOutChanged);
	Q_PROPERTY(bool borderColours READ borderColours WRITE setBorderColours NOTIFY borderColoursChanged);
	Q_PROPERTY(QColor activeBorderColour READ activeBorderColour WRITE setActiveBorderColour NOTIFY activeBorderColourChanged);
	Q_PROPERTY(QColor inactiveBorderColour READ inactiveBorderColour WRITE setInactiveBorderColour NOTIFY inactiveBorderColourChanged);
	Q_PROPERTY(bool preserveSplit READ preserveSplit WRITE setPreserveSplit NOTIFY preserveSplitChanged);
	Q_PROPERTY(QStringList excluded READ excluded WRITE setExcluded NOTIFY excludedChanged);
	Q_PROPERTY(QStringList included READ included WRITE setIncluded NOTIFY includedChanged);
	// Allowlist entries that passed validation. An allowlist whose entries all
	// failed validation matches nothing, and the QML gate refuses to enable it.
	Q_PROPERTY(qint32 includedRuleCount READ includedRuleCount NOTIFY includedChanged);
	Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged);
	Q_PROPERTY(qint32 managedCount READ managedCount NOTIFY managedCountChanged);
	Q_PROPERTY(qint32 failedCount READ failedCount NOTIFY failedCountChanged);
	Q_PROPERTY(QString layout READ layout WRITE setLayout NOTIFY layoutChanged);
	Q_PROPERTY(bool smartSplit READ smartSplit WRITE setSmartSplit NOTIFY smartSplitChanged);
	Q_PROPERTY(qint32 forceSplit READ forceSplit WRITE setForceSplit NOTIFY forceSplitChanged);
	Q_PROPERTY(double defaultSplitRatio READ defaultSplitRatio WRITE setDefaultSplitRatio NOTIFY defaultSplitRatioChanged);
	Q_PROPERTY(QString masterNewStatus READ masterNewStatus WRITE setMasterNewStatus NOTIFY masterNewStatusChanged);
	Q_PROPERTY(bool masterNewOnTop READ masterNewOnTop WRITE setMasterNewOnTop NOTIFY masterNewOnTopChanged);
	Q_PROPERTY(QString masterOrientation READ masterOrientation WRITE setMasterOrientation NOTIFY masterOrientationChanged);
	Q_PROPERTY(double masterFactor READ masterFactor WRITE setMasterFactor NOTIFY masterFactorChanged);
	Q_PROPERTY(double scrollingColumnWidth READ scrollingColumnWidth WRITE setScrollingColumnWidth NOTIFY scrollingColumnWidthChanged);
	Q_PROPERTY(QString scrollingCenter READ scrollingCenter WRITE setScrollingCenter NOTIFY scrollingCenterChanged);
	// The drop a dragged tiled window would make now: "home", "swap", "insert"
	// or "move". Landing and displaced are logical, local to the screen named
	// by dragScreen, and null when there is nothing to draw.
	Q_PROPERTY(bool dragActive READ dragActive NOTIFY dragChanged);
	Q_PROPERTY(QString dragMode READ dragMode NOTIFY dragChanged);
	Q_PROPERTY(QString dragScreen READ dragScreen NOTIFY dragChanged);
	Q_PROPERTY(QRectF dragLanding READ dragLanding NOTIFY dragChanged);
	Q_PROPERTY(QRectF dragDisplaced READ dragDisplaced NOTIFY dragChanged);
	// clang-format on

public:
	explicit Tiling(QObject* parent = nullptr);
	~Tiling() override;
	[[nodiscard]] bool supportsScopedRules() const { return true; }

	Q_INVOKABLE void relayout();

	[[nodiscard]] bool enabled() const;
	void setEnabled(bool enabled);
	[[nodiscard]] qint32 gapsIn() const;
	void setGapsIn(qint32 gaps);
	[[nodiscard]] qint32 gapsOut() const;
	void setGapsOut(qint32 gaps);
	[[nodiscard]] qint32 singleWindowGapsOut() const;
	void setSingleWindowGapsOut(qint32 gaps);
	[[nodiscard]] bool borderColours() const;
	void setBorderColours(bool enabled);
	[[nodiscard]] QColor activeBorderColour() const;
	void setActiveBorderColour(const QColor& colour);
	[[nodiscard]] QColor inactiveBorderColour() const;
	void setInactiveBorderColour(const QColor& colour);
	[[nodiscard]] bool preserveSplit() const;
	void setPreserveSplit(bool preserve);
	[[nodiscard]] QStringList excluded() const;
	void setExcluded(const QStringList& excluded);
	[[nodiscard]] QStringList included() const;
	void setIncluded(const QStringList& included);
	[[nodiscard]] qint32 includedRuleCount() const;
	[[nodiscard]] QString lastError() const;
	[[nodiscard]] qint32 managedCount() const;
	[[nodiscard]] qint32 failedCount() const;
	[[nodiscard]] QString layout() const;
	void setLayout(const QString& layout);
	[[nodiscard]] bool smartSplit() const;
	void setSmartSplit(bool smart);
	[[nodiscard]] qint32 forceSplit() const;
	void setForceSplit(qint32 force);
	[[nodiscard]] double defaultSplitRatio() const;
	void setDefaultSplitRatio(double ratio);
	[[nodiscard]] QString masterNewStatus() const;
	void setMasterNewStatus(const QString& status);
	[[nodiscard]] bool masterNewOnTop() const;
	void setMasterNewOnTop(bool onTop);
	[[nodiscard]] QString masterOrientation() const;
	void setMasterOrientation(const QString& orientation);
	[[nodiscard]] double masterFactor() const;
	void setMasterFactor(double factor);
	[[nodiscard]] double scrollingColumnWidth() const;
	void setScrollingColumnWidth(double width);
	[[nodiscard]] QString scrollingCenter() const;
	void setScrollingCenter(const QString& center);
	[[nodiscard]] bool dragActive() const;
	[[nodiscard]] QString dragMode() const;
	[[nodiscard]] QString dragScreen() const;
	[[nodiscard]] QRectF dragLanding() const;
	[[nodiscard]] QRectF dragDisplaced() const;

signals:
	void enabledChanged();
	void gapsInChanged();
	void gapsOutChanged();
	void singleWindowGapsOutChanged();
	void borderColoursChanged();
	void activeBorderColourChanged();
	void inactiveBorderColourChanged();
	void preserveSplitChanged();
	void excludedChanged();
	void includedChanged();
	void layoutChanged();
	void smartSplitChanged();
	void forceSplitChanged();
	void defaultSplitRatioChanged();
	void masterNewStatusChanged();
	void masterNewOnTopChanged();
	void masterOrientationChanged();
	void masterFactorChanged();
	void scrollingColumnWidthChanged();
	void scrollingCenterChanged();
	void lastErrorChanged();
	void enableRefused(const QString& reason);
	void windowTilingFailed(const QString& address, const QString& reason);
	void managedCountChanged();
	void failedCountChanged();
	void dragChanged();

private:
	// One engine owns one Tiling singleton, so the lease marks exactly one QML
	// generation. It is claimed by the first write and released on teardown.
	TilingLease mLease;
};

} // namespace qs::windows

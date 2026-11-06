#include <algorithm>
#include <utility>

#include <qdatetime.h>
#include <qdir.h>
#include <qeventloop.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qguiapplication.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qmetaobject.h>
#include <qpa/qplatformscreen.h>
#include <qpa/qwindowsysteminterface.h>
#include <qqmlcomponent.h>
#include <qqmlengine.h>
#include <qsignalspy.h>
#include <qt_windows.h>
#include <qtemporarydir.h>
#include <qtest.h>

#include "../../core/qmlscreen.hpp"
#include "../cursor_monitor_cache.hpp"
#include "../panel_geometry.hpp"
#include "../screen_identity.hpp"
#include "../screen_surface_guard.hpp"
#include "../screen_topology_watcher.hpp"

using namespace qs::windows;

namespace {
class FakeScreen: public QPlatformScreen {
public:
	QRect rect {0, 0, 2560, 1440};
	qreal scale = 1.5;
	QRect geometry() const override { return this->rect; }
	int depth() const override { return 32; }
	QImage::Format format() const override { return QImage::Format_RGB32; }
	QString name() const override { return QStringLiteral("Contoso OLED 32"); }
	qreal devicePixelRatio() const override { return this->scale; }
	QDpi logicalDpi() const override { return {96, 96}; }
};

// Mirrors the Windows QPA shape: native platform geometry and a DPI-derived
// scale. QScreen::devicePixelRatio() is then the same factor Qt uses to scale
// the native monitor rectangle into the logical screen rectangle.
class ScaledScreen: public FakeScreen {
public:
	qreal devicePixelRatio() const override { return 1.0; }
	QDpi logicalDpi() const override {
		auto dpi = qRound(96 * this->scale);
		return {dpi, dpi};
	}
};

QJsonObject allReservations(const QString& path) {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) return {};
	auto document = QJsonDocument::fromJson(file.readAll());
	if (!document.isObject()) return {};
	return document.object().value(QStringLiteral("reservations")).toObject();
}

// The file stores a hash of the persistent id, never the id itself.
QJsonObject reservationById(const QString& path, const QString& id) {
	return allReservations(path)
	    .value(ScreenIdentityRegistry::reservationKey(id))
	    .toObject();
}

QByteArray fileBytes(const QString& path) {
	QFile file(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void writeBytes(const QString& path, const QByteArray& bytes) {
	QFile file(path);
	QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
	file.write(bytes);
}
} // namespace

class ScreenIdentityTest: public QObject {
	Q_OBJECT

private slots:
	void embeddedPanelDprMismatchDoesNotRecreateForever() {
		qreal observedScreenDpr = 1.75;
		auto needsRecreate = [&](qreal screenDpr, qreal windowDpr) {
			return embeddedPanelNeedsScaleRecreate(
			    std::exchange(observedScreenDpr, screenDpr),
			    screenDpr,
			    windowDpr
			);
		};
		// Explorer can keep a child at the parent's 100% DPI on a 175% monitor.
		// Recreating emits another DPR notification with exactly that mismatch.
		for (int i = 0; i != 1000; ++i) QVERIFY(!needsRecreate(1.75, 1.0));
		// A real monitor scale change still gets one attempt, not a retry loop.
		QVERIFY(needsRecreate(2.0, 1.0));
		for (int i = 0; i != 1000; ++i) QVERIFY(!needsRecreate(2.0, 1.0));
		QVERIFY(needsRecreate(1.75, 1.0));
		QVERIFY(!needsRecreate(1.75, 1.75));
		// Qt already applied this real change, so there is nothing to recreate.
		QVERIFY(!needsRecreate(1.5, 1.5));
	}

	void removedScreenHidesSurfaceBeforeQtFallback() {
		auto* platform = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(platform);
		auto* screen = platform->screen();
		QWindow window;
		QObject context;
		int hides = 0;
		bool hiddenBeforeFallback = false;
		ScreenSurfaceGuard guard(
		    &context,
		    [&] {
			    ++hides;
			    hiddenBeforeFallback = window.screen() == screen;
			    window.setVisible(false);
		    },
		    [](QScreen*) { return true; },
		    [&] { window.show(); }
		);
		guard.setTarget(screen);
		window.setScreen(screen);
		guard.setWindow(&window);
		window.show();
		QVERIFY(window.isVisible());
		QWindowSystemInterface::handleScreenRemoved(platform);
		QVERIFY(guard.isBlocked());
		QVERIFY(!window.isVisible());
		QCOMPARE(hides, 1);
		QVERIFY(hiddenBeforeFallback);
		QVERIFY(window.screen() != screen);

		// A dangling model binding must not silently switch to auto-screen.
		guard.setTarget(nullptr);
		QVERIFY(guard.isBlocked());
		auto* replacement = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(replacement);
		guard.setTarget(replacement->screen());
		QVERIFY(!guard.isBlocked());
		window.setScreen(replacement->screen());
		window.show();
		QVERIFY(window.isVisible());
		QWindowSystemInterface::handleScreenRemoved(replacement);
		QVERIFY(guard.isBlocked());
		QVERIFY(!window.isVisible());
	}

	void displayChangeHidesSurfaceBeforeQtRemoval() {
		auto* platform = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(platform);
		auto* screen = platform->screen();
		QWindow window;
		QObject context;
		int hides = 0;
		ScreenSurfaceGuard guard(
		    &context,
		    [&] {
			    ++hides;
			    window.setVisible(false);
		    },
		    [](QScreen*) { return true; },
		    [&] { window.show(); }
		);
		guard.setTarget(screen);
		window.setScreen(screen);
		guard.setWindow(&window);
		window.show();
		QVERIFY(window.isVisible());

		// WM_DISPLAYCHANGE arrives while the monitor is already gone from the
		// desktop but Qt still holds its QScreen. Hide before Qt's removal moves
		// the visible surface to the primary screen.
		guard.screenDetached();
		QVERIFY(guard.isBlocked());
		QVERIFY(!window.isVisible());
		QCOMPARE(hides, 1);

		// Qt's fallback now runs. It must neither re-show nor migrate the surface.
		QWindowSystemInterface::handleScreenRemoved(platform);
		QCoreApplication::processEvents();
		QVERIFY(guard.isBlocked());
		QVERIFY(!window.isVisible());
		QCOMPARE(hides, 1);
	}

	void survivingPanelFollowsNativeTopologyBeforeQScreenUpdate() {
		auto* primaryPlatform = new ScaledScreen;
		primaryPlatform->rect = {0, 0, 4480, 2520};
		primaryPlatform->scale = 1.75;
		auto* survivingPlatform = new ScaledScreen;
		survivingPlatform->rect = {-3840, 0, 3840, 2160};
		survivingPlatform->scale = 1.0;
		QWindowSystemInterface::handleScreenAdded(primaryPlatform);
		QWindowSystemInterface::handleScreenAdded(survivingPlatform);
		auto* primary = primaryPlatform->screen();
		auto* surviving = survivingPlatform->screen();
		QVERIFY(primary != nullptr);
		QVERIFY(surviving != nullptr);
		QCOMPARE(surviving->devicePixelRatio(), 1.0);
		QCOMPARE(surviving->geometry(), QRect(-3840, 0, 3840, 2160));

		// The same math WinPanelWindow::updateDimensions() applies from the
		// native monitor rectangle during WM_DISPLAYCHANGE.
		auto place = [&]() {
			return anchoredPanelGeometry(
			    panelScreenRect(survivingPlatform->rect, surviving->devicePixelRatio()),
			    {900, 40},
			    Qt::TopEdge | Qt::LeftEdge | Qt::RightEdge,
			    {}
			);
		};
		QCOMPARE(place(), QRect(-3840, 0, 3840, 40));

		// Power off the primary. Windows moves the survivor to the origin
		// immediately, while its QScreen still has the old origin.
		QWindowSystemInterface::handleScreenRemoved(primaryPlatform);
		survivingPlatform->rect = {0, 0, 3840, 2160};
		QCOMPARE(place(), QRect(0, 0, 3840, 40));
		QCOMPARE(surviving->geometry(), QRect(-3840, 0, 3840, 2160));
		QWindowSystemInterface::handleScreenGeometryChange(
		    surviving,
		    survivingPlatform->rect,
		    survivingPlatform->rect
		);
		QCoreApplication::processEvents();
		QCOMPARE(surviving->geometry(), QRect(0, 0, 3840, 2160));

		// Power the primary back on. Qt announces the returning screen before it
		// delivers the survivor's geometry change, so the native placement must
		// be final before screenAdded instantiates the new screen's windows.
		auto* returningPlatform = new ScaledScreen;
		returningPlatform->rect = {0, 0, 4480, 2520};
		returningPlatform->scale = 1.75;
		QWindowSystemInterface::handleScreenAdded(returningPlatform);
		survivingPlatform->rect = {-3840, 0, 3840, 2160};
		QCOMPARE(place(), QRect(-3840, 0, 3840, 40));
		QCOMPARE(surviving->geometry(), QRect(0, 0, 3840, 2160));

		// Qt catches up to the same rectangle, so the panel does not move again.
		QWindowSystemInterface::handleScreenGeometryChange(
		    surviving,
		    survivingPlatform->rect,
		    survivingPlatform->rect
		);
		QCoreApplication::processEvents();
		QCOMPARE(surviving->geometry(), QRect(-3840, 0, 3840, 2160));
		QCOMPARE(place(), QRect(-3840, 0, 3840, 40));

		QWindowSystemInterface::handleScreenRemoved(returningPlatform);
		QWindowSystemInterface::handleScreenRemoved(survivingPlatform);
	}

	void unexpectedReassignmentBlocksOnlyTargetedSurfaces() {
		auto* platform = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(platform);
		QWindow window;
		QObject context;
		int hides = 0;
		int restores = 0;
		bool requested = true;
		bool onTargetMonitor = true;
		ScreenSurfaceGuard guard(
		    &context,
		    [&] {
			    ++hides;
			    window.setVisible(false);
		    },
		    [&](QScreen*) { return onTargetMonitor; },
		    [&] {
			    ++restores;
			    if (requested) window.show();
		    }
		);
		guard.setTarget(platform->screen());
		window.setScreen(platform->screen());
		guard.setWindow(&window);
		window.show();
		window.setScreen(QGuiApplication::primaryScreen());
		QVERIFY(guard.isBlocked());
		QCOMPARE(hides, 1);
		QVERIFY(!window.isVisible());

		window.setScreen(platform->screen());
		QTRY_VERIFY(!guard.isBlocked());
		QTRY_VERIFY(window.isVisible());
		QCOMPARE(restores, 1);

		// A screen signal alone is insufficient until native placement follows.
		window.setScreen(QGuiApplication::primaryScreen());
		onTargetMonitor = false;
		window.setScreen(platform->screen());
		QCoreApplication::processEvents();
		QVERIFY(guard.isBlocked());
		QVERIFY(!window.isVisible());
		onTargetMonitor = true;
		guard.scheduleRestore();
		QTRY_VERIFY(!guard.isBlocked());
		QTRY_VERIFY(window.isVisible());
		QCOMPARE(restores, 2);

		// A visibility request made while blocked is still respected.
		window.setScreen(QGuiApplication::primaryScreen());
		QVERIFY(guard.isBlocked());
		requested = false;
		window.setScreen(platform->screen());
		QTRY_VERIFY(!guard.isBlocked());
		QVERIFY(!window.isVisible());
		QCOMPARE(restores, 3);
		requested = true;
		window.show();
		guard.setTarget(nullptr);
		window.setScreen(QGuiApplication::primaryScreen());
		QVERIFY(!guard.isBlocked());
		QCOMPARE(hides, 3);
		QWindowSystemInterface::handleScreenRemoved(platform);
	}

	void nativeMoveBeforeRemovalCannotReshow() {
		auto* platform = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(platform);
		auto* screen = platform->screen();
		QWindow window;
		QObject context;
		int restores = 0;
		ScreenSurfaceGuard guard(
		    &context,
		    [&] { window.hide(); },
		    [](QScreen*) { return true; },
		    [&] {
			    ++restores;
			    window.show();
		    }
		);
		guard.setTarget(screen);
		window.setScreen(screen);
		guard.setWindow(&window);
		window.show();

		// The native pre-move check runs before Qt changes the QWindow screen.
		guard.movedOffTarget();
		QVERIFY(guard.isBlocked());
		QVERIFY(!window.isVisible());
		window.setScreen(QGuiApplication::primaryScreen());
		window.setScreen(screen);
		QWindowSystemInterface::handleScreenRemoved(platform);
		QCoreApplication::processEvents();
		QVERIFY(guard.isBlocked());
		QVERIFY(!window.isVisible());
		QCOMPARE(restores, 0);
	}

	void reusedSourceRefreshesNameAndBindings() {
		auto* platform = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(platform);
		auto* screen = platform->screen();
		QVERIFY(screen);
		auto* unchangedPlatform = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(unchangedPlatform);
		auto* unchangedScreen = unchangedPlatform->screen();
		QList<DisplayIdentity> displays {
		    {QStringLiteral("DISPLAY1"), QStringLiteral("oled-path"), QStringLiteral("Contoso OLED 32")},
		    {QStringLiteral("DISPLAY2"),
		     QStringLiteral("other-oled-path"),
		     QStringLiteral("Contoso OLED 32")},
		};
		{
			ScreenIdentityRegistry registry(
			    [&] { return displays; },
			    [screen, unchangedScreen](QScreen* target) {
				    if (target == screen) return QStringLiteral("DISPLAY1");
				    if (target == unchangedScreen) return QStringLiteral("DISPLAY2");
				    return QString();
			    }
			);
			QuickshellScreenInfo info(nullptr, screen, &registry);
			QuickshellScreenInfo unchanged(nullptr, unchangedScreen, &registry);
			QSignalSpy names(&info, &QuickshellScreenInfo::nameChanged);
			QSignalSpy unchangedNames(&unchanged, &QuickshellScreenInfo::nameChanged);
			QCOMPARE(info.name(), QStringLiteral("Contoso OLED 32"));
			// Two identical labels get a deterministic, path-ordered suffix.
			QCOMPARE(unchanged.name(), QStringLiteral("Contoso OLED 32 (2)"));
			QVERIFY(
			    !info.metaObject()->property(info.metaObject()->indexOfProperty("name")).isConstant()
			);

			// Real QML binding, not a synthetic observer of the test source.
			QQmlEngine engine;
			QQmlComponent component(&engine);
			component.setData(
			    "import QtQml\nQtObject { required property QtObject screen; "
			    "property string selected: screen.name === 'Fabrikam 27Q' ? 'lcd-config' : 'oled-config' "
			    "}",
			    QUrl()
			);
			QScopedPointer<QObject> bound(component.createWithInitialProperties(
			    {{"screen", QVariant::fromValue(static_cast<QObject*>(&info))}}
			));
			QVERIFY2(bound, qPrintable(component.errorString()));
			QCOMPARE(bound->property("selected").toString(), QStringLiteral("oled-config"));

			// The GDI name and QScreen pointer stay the same. Only the target
			// path changes, exactly the Qt reuse case in issue #88.
			displays[0] =
			    {QStringLiteral("DISPLAY1"), QStringLiteral("lcd-path"), QStringLiteral("Fabrikam 27Q")};
			MSG msg {};
			msg.message = WM_DISPLAYCHANGE;
			registry.nativeEventFilter({}, &msg, nullptr);
			QTRY_COMPARE(info.name(), QStringLiteral("Fabrikam 27Q"));
			QCOMPARE(screen->name(), QStringLiteral("Contoso OLED 32")); // Qt remains stale.
			QCOMPARE(registry.identity(screen).devicePath, QStringLiteral("lcd-path"));
			QCOMPARE(bound->property("selected").toString(), QStringLiteral("lcd-config"));
			QVERIFY(names.count() >= 1);
			// The only remaining MAG label loses its suffix when the reused
			// screen becomes a different physical target.
			QCOMPARE(unchanged.name(), QStringLiteral("Contoso OLED 32"));
			QCOMPARE(unchangedNames.count(), 1);
			auto count = names.count();
			registry.refresh();
			QCOMPARE(names.count(), count); // No churn on an unchanged topology.
			displays[0].devicePath = QStringLiteral("another-lcd-path");
			registry.refresh();
			QCOMPARE(names.count(), count + 1); // Same model, different physical target.
			displays.clear();
			registry.refresh();
			QVERIFY(info.name().startsWith(QStringLiteral("Unresolved display ")));
		}
		QWindowSystemInterface::handleScreenRemoved(unchangedPlatform);
		QWindowSystemInterface::handleScreenRemoved(platform);
	}

	void identicalMonitorsGetUniqueStableNames() {
		auto* firstPlatform = new FakeScreen;
		auto* secondPlatform = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(firstPlatform);
		QWindowSystemInterface::handleScreenAdded(secondPlatform);
		auto* first = firstPlatform->screen();
		auto* second = secondPlatform->screen();
		QList<DisplayIdentity> displays {
		    {QStringLiteral("DISPLAY1"),
		     QStringLiteral("generic-path-b"),
		     QStringLiteral("Generic Monitor")},
		    {QStringLiteral("DISPLAY2"),
		     QStringLiteral("generic-path-a"),
		     QStringLiteral("Generic Monitor")},
		};
		{
			ScreenIdentityRegistry registry(
			    [&] { return displays; },
			    [first, second](QScreen* target) {
				    if (target == first) return QStringLiteral("DISPLAY1");
				    if (target == second) return QStringLiteral("DISPLAY2");
				    return QString();
			    }
			);
			QuickshellScreenInfo firstInfo(nullptr, first, &registry);
			QuickshellScreenInfo secondInfo(nullptr, second, &registry);
			// The suffix is ordered by the persistent target path, not by
			// QScreen order or enumeration order.
			QCOMPARE(secondInfo.name(), QStringLiteral("Generic Monitor"));
			QCOMPARE(firstInfo.name(), QStringLiteral("Generic Monitor (2)"));
			QCOMPARE(registry.screenByName(QStringLiteral("Generic Monitor")), second);
			QCOMPARE(registry.screenByName(QStringLiteral("Generic Monitor (2)")), first);
			QCOMPARE(registry.screenById(registry.identity(second).stableId), second);
			QVERIFY(registry.identity(first).stableId != registry.identity(second).stableId);
			QCOMPARE(registry.identity(first).friendlyName, QStringLiteral("Generic Monitor"));
			// The label is the same for both; only the QML-facing name differs.
			QVERIFY(!firstInfo.name().contains(QStringLiteral("path")));

			// A refresh or a reordered source list cannot rename either screen.
			std::reverse(displays.begin(), displays.end());
			registry.refresh();
			QCOMPARE(secondInfo.name(), QStringLiteral("Generic Monitor"));
			QCOMPARE(firstInfo.name(), QStringLiteral("Generic Monitor (2)"));

			// A direct label that already looks like a collision suffix keeps
			// its plain form; the real duplicate is the one that gets suffixed.
			displays = {
			    {QStringLiteral("DISPLAY1"),
			     QStringLiteral("generic-path-a"),
			     QStringLiteral("Other Monitor")},
			    {QStringLiteral("DISPLAY2"),
			     QStringLiteral("generic-path-b"),
			     QStringLiteral("Other Monitor (2)")},
			};
			registry.refresh();
			QCOMPARE(firstInfo.name(), QStringLiteral("Other Monitor"));
			QCOMPARE(secondInfo.name(), QStringLiteral("Other Monitor (2)"));

			// Replacing the target behind a reused QScreen changes both its id
			// and its name, never leaving stale per-target state reachable.
			displays = {
			    {QStringLiteral("DISPLAY1"),
			     QStringLiteral("generic-path-c"),
			     QStringLiteral("Replacement")},
			    {QStringLiteral("DISPLAY2"),
			     QStringLiteral("generic-path-b"),
			     QStringLiteral("Other Monitor (2)")},
			};
			registry.refresh();
			QCOMPARE(firstInfo.name(), QStringLiteral("Replacement"));
			QCOMPARE(registry.identity(first).stableId, QStringLiteral("path:generic-path-c"));
		}
		QWindowSystemInterface::handleScreenRemoved(secondPlatform);
		QWindowSystemInterface::handleScreenRemoved(firstPlatform);
	}

	void partialMetadataPreservesOnlyTheSameTarget() {
		auto* platform = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(platform);
		auto* screen = platform->screen();
		{
			QList<DisplayIdentity> displays {{"DISPLAY1", "lcd-path", "Fabrikam 27Q"}};
			ScreenIdentityRegistry registry(
			    [&] { return displays; },
			    [screen](QScreen* target) {
				    return target == screen ? QStringLiteral("DISPLAY1") : QString();
			    }
			);
			QuickshellScreenInfo info(nullptr, screen, &registry);
			QSignalSpy names(&info, &QuickshellScreenInfo::nameChanged);
			displays[0].friendlyName.clear();
			registry.refresh();
			QCOMPARE(info.name(), QStringLiteral("Fabrikam 27Q"));
			QCOMPARE(names.count(), 0);
			displays.clear();
			registry.refresh();
			QVERIFY(info.name().startsWith(QStringLiteral("Unresolved display ")));
			displays = {{"DISPLAY1", "lcd-path", {}}};
			registry.refresh();
			QCOMPARE(info.name(), QStringLiteral("Fabrikam 27Q")); // Metadata survives the gap by path.
			displays[0].devicePath = QStringLiteral("LCD-PATH");
			displays[0].friendlyName = QStringLiteral("   ");
			registry.refresh();
			QCOMPARE(info.name(), QStringLiteral("Fabrikam 27Q"));

			displays[0].devicePath = QStringLiteral("unknown-target-path");
			registry.refresh();
			QCOMPARE(info.name(), QStringLiteral("unknown-target-path"));
			QCOMPARE(registry.identity(screen).devicePath, QStringLiteral("unknown-target-path"));
			QCOMPARE(screen->name(), QStringLiteral("Contoso OLED 32")); // Never a fallback.
			displays[0].friendlyName = QStringLiteral("New monitor");
			registry.refresh();
			QCOMPARE(info.name(), QStringLiteral("New monitor"));
		}
		QWindowSystemInterface::handleScreenRemoved(platform);
	}

	void failedLookupAndPendingScreensAreUnresolved() {
		auto* platform = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(platform);
		auto* screen = platform->screen();
		{
			QList<DisplayIdentity> displays;
			ScreenIdentityRegistry registry(
			    [&] { return displays; },
			    [](QScreen*) { return QStringLiteral("DISPLAY1"); }
			);
			QuickshellScreenInfo info(nullptr, screen, &registry);
			const auto unresolved = info.name();
			QVERIFY(unresolved.startsWith(QStringLiteral("Unresolved display ")));
			// A fallback/virtual driver can give a source but no target metadata.
			displays = {{"DISPLAY1", {}, {}}};
			registry.refresh();
			QCOMPARE(info.name(), unresolved);
			// A new screen wrapper can be queried before the queued refresh.
			auto* pending = new FakeScreen;
			QWindowSystemInterface::handleScreenAdded(pending);
			{
				QuickshellScreenInfo pendingInfo(nullptr, pending->screen(), &registry);
				QVERIFY(pendingInfo.name().startsWith(QStringLiteral("Unresolved display ")));
				QVERIFY(pendingInfo.name() != unresolved);
			}
			QWindowSystemInterface::handleScreenRemoved(pending);
			ScreenIdentityRegistry unknownDevice(
			    [] { return QList<DisplayIdentity>(); },
			    [](QScreen*) { return QString(); }
			);
			QuickshellScreenInfo unknownInfo(nullptr, screen, &unknownDevice);
			QVERIFY(unknownInfo.name().startsWith(QStringLiteral("Unresolved display ")));
		}
		QWindowSystemInterface::handleScreenRemoved(platform);
	}

	void unresolvedScreensHaveDistinctLifetimeTokens() {
		// Native/GDI lookup fails for both screens, despite available display
		// metadata and Qt's identical stale friendly names.
		ScreenIdentityRegistry registry(
		    [] {
			    return QList<DisplayIdentity> {
			        {"DISPLAY1", "lcd-path", "Fabrikam 27Q"},
			        {"DISPLAY2", "oled-path", "Contoso OLED 32"},
			        {{}, "unmapped-path", "Fabrikam 27Q"}
			    };
		    },
		    [](QScreen*) { return QString(); }
		);
		auto* first = new FakeScreen;
		auto* second = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(first);
		QWindowSystemInterface::handleScreenAdded(second);
		QString firstName;
		QString secondName;
		{
			QuickshellScreenInfo firstInfo(nullptr, first->screen(), &registry);
			QuickshellScreenInfo secondInfo(nullptr, second->screen(), &registry);
			// Query before the pending refresh, just as a newly created QML
			// screen wrapper can do. refresh() must reuse these exact tokens.
			firstName = firstInfo.name();
			secondName = secondInfo.name();
			QVERIFY(firstName.startsWith(QStringLiteral("Unresolved display ")));
			QVERIFY(secondName.startsWith(QStringLiteral("Unresolved display ")));
			QVERIFY(firstName != secondName);

			QQmlEngine engine;
			QQmlComponent component(&engine);
			component.setData(
			    "import QtQml\nQtObject { required property QtObject first; required property QtObject "
			    "second; "
			    "property var overrides: ({'Fabrikam 27Q': 'lcd-config', 'Contoso OLED 32': 'oled-config', "
			    "'Unresolved display': 'shared-config'}); "
			    "property string firstConfig: overrides[first.name] ?? ''; "
			    "property string secondConfig: overrides[second.name] ?? '' }",
			    QUrl()
			);
			QScopedPointer<QObject> bound(component.createWithInitialProperties(
			    {{"first", QVariant::fromValue(static_cast<QObject*>(&firstInfo))},
			     {"second", QVariant::fromValue(static_cast<QObject*>(&secondInfo))}}
			));
			QVERIFY2(bound, qPrintable(component.errorString()));
			for (int refresh = 0; refresh != 3; ++refresh) {
				registry.refresh();
				QCOMPARE(firstInfo.name(), firstName);
				QCOMPARE(secondInfo.name(), secondName);
				QCOMPARE(registry.identity(first->screen()).friendlyName, firstName);
				QCOMPARE(registry.identity(second->screen()).friendlyName, secondName);
				QCOMPARE(bound->property("firstConfig").toString(), QString());
				QCOMPARE(bound->property("secondConfig").toString(), QString());
			}
		}
		QWindowSystemInterface::handleScreenRemoved(second);
		QWindowSystemInterface::handleScreenRemoved(first);
		// A new screen gets a fresh token, even if an allocator reuses a pointer.
		auto* replacement = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(replacement);
		const auto replacementName = registry.identity(replacement->screen()).friendlyName;
		QVERIFY(replacementName.startsWith(QStringLiteral("Unresolved display ")));
		QVERIFY(replacementName != firstName);
		QVERIFY(replacementName != secondName);
		registry.refresh();
		QCOMPARE(registry.identity(replacement->screen()).friendlyName, replacementName);
		QWindowSystemInterface::handleScreenRemoved(replacement);
	}

	void positionSwapInvalidatesCursorCacheAndFocus() {
		auto* first = new FakeScreen;
		first->rect = {-1000, 0, 1000, 800};
		first->scale = 1;
		auto* second = new FakeScreen;
		second->rect = {0, 0, 1000, 800};
		second->scale = 1;
		QWindowSystemInterface::handleScreenAdded(first);
		QWindowSystemInterface::handleScreenAdded(second);
		auto* firstScreen = first->screen();
		auto* secondScreen = second->screen();
		{
			ScreenIdentityRegistry registry(
			    [] {
				    return QList<DisplayIdentity> {
				        {"DISPLAY1", "first-path", "First"},
				        {"DISPLAY2", "second-path", "Second"}
				    };
			    },
			    [firstScreen](QScreen* screen) {
				    return screen == firstScreen ? QStringLiteral("DISPLAY1") : QStringLiteral("DISPLAY2");
			    }
			);
			ScreenTopologyWatcher topology(&registry);
			QSignalSpy added(qGuiApp, &QGuiApplication::screenAdded);
			QSignalSpy removed(qGuiApp, &QGuiApplication::screenRemoved);
			QSignalSpy changed(&registry, &ScreenIdentityRegistry::screenChanged);
			CursorMonitorCache cache;
			const RECT oldRect {-1000, 0, 0, 800};
			cache.update(oldRect);
			POINT cursor {-500, 200};
			auto* focused = firstScreen;
			auto* published = firstScreen;
			int reconciliations = 0;
			int monitorLookups = 0;
			auto screenAt = [&]() -> QScreen* {
				for (auto* screen: {firstScreen, secondScreen})
					if (screen->geometry().contains(QPoint(cursor.x, cursor.y))) return screen;
				return nullptr;
			};
			// Production HyprlandIpc wires this watcher to onScreensChanged(),
			// which invalidates the hook cache, syncs monitors and recomputes
			// focus. Substitute only the native input/monitor lookup boundary.
			connect(&topology, &QTimer::timeout, this, [&] {
				cache.reset();
				focused = screenAt();
				++reconciliations;
			});
			auto noteCursor = [&] {
				if (cache.contains(cursor)) return; // Same fast path as the hook.
				++monitorLookups;
				published = screenAt();
				QVERIFY(published);
				auto rect = published->geometry();
				cache.update({rect.x(), rect.y(), rect.x() + rect.width(), rect.y() + rect.height()});
			};

			std::swap(first->rect, second->rect);
			QWindowSystemInterface::handleScreenGeometryChange(firstScreen, first->rect, first->rect);
			QWindowSystemInterface::handleScreenGeometryChange(secondScreen, second->rect, second->rect);
			QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
			// Reproduce the old failure: movement in the old rectangle skips
			// MonitorFromPoint, though a different screen now occupies it.
			noteCursor();
			QCOMPARE(monitorLookups, 0);
			QCOMPARE(published, firstScreen);
			registry.refresh();
			QCOMPARE(changed.count(), 2);
			QTRY_COMPARE(reconciliations, 1); // Both screen changes coalesced.
			QCOMPARE(focused, secondScreen);
			QCOMPARE(added.count(), 0);
			QCOMPARE(removed.count(), 0);
			cursor.x += 10; // Still inside the previously cached rectangle.
			QVERIFY(!cache.contains(cursor));
			noteCursor();
			QCOMPARE(monitorLookups, 1);
			QCOMPARE(published, secondScreen);
			noteCursor();
			QCOMPARE(monitorLookups, 1); // The new rectangle is cached normally.
		}
		QWindowSystemInterface::handleScreenRemoved(second);
		QWindowSystemInterface::handleScreenRemoved(first);
	}

	void reusedScreenReanchorsAndRefreshesDpr() {
		auto* platform = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(platform);
		auto* screen = platform->screen();
		{
			ScreenIdentityRegistry registry(
			    [] { return QList<DisplayIdentity> {{"DISPLAY1", "same-path", "Fabrikam 27Q"}}; },
			    [](QScreen*) { return QStringLiteral("DISPLAY1"); }
			);
			QuickshellScreenInfo info(nullptr, screen, &registry);
			QSignalSpy geometry(&info, &QuickshellScreenInfo::geometryChanged);
			QSignalSpy dpr(&info, &QuickshellScreenInfo::devicePixelRatioChanged);
			QRect drawer;
			QRect utilities;
			QRegion nativeMask;
			auto reanchor = [&] {
				// The same helper is used by WinPanelWindow::updateDimensions().
				drawer = anchoredPanelGeometry(
				    screen->geometry(),
				    {900, 620},
				    Qt::TopEdge | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge,
				    {}
				);
				utilities =
				    anchoredPanelGeometry(drawer, {400, 300}, Qt::RightEdge | Qt::BottomEdge, {0, 0, 8, 8});
				nativeMask = toPhysicalRegion(QRegion(2182, 500, 12, 100), screen->devicePixelRatio());
			};
			connect(&registry, &ScreenIdentityRegistry::screenChanged, this, reanchor);
			reanchor();
			QCOMPARE(drawer.size(), QSize(2560, 1440));
			platform->scale = 1.75;
			platform->rect = {-3840, 0, 2194, 1234};
			QWindowSystemInterface::handleScreenGeometryChange(screen, platform->rect, platform->rect);
			QCoreApplication::processEvents();
			registry.refresh();
			QCOMPARE(info.devicePixelRatio(), 1.75);
			QCOMPARE(drawer, QRect(-3840, 0, 2194, 1234));
			QCOMPARE(utilities, QRect(-2054, 926, 400, 300));
			QCOMPARE(nativeMask.boundingRect(), QRect(3818, 875, 22, 175));
			QVERIFY(geometry.count() > 0);
			QVERIFY(dpr.count() > 0);
			// DPR-only changes must also notify and republish the input scale,
			// even if logical geometry and the monitor name do not change.
			auto count = dpr.count();
			platform->scale = 2.0;
			MSG msg {};
			msg.message = WM_DPICHANGED;
			registry.nativeEventFilter({}, &msg, nullptr);
			QTRY_VERIFY(dpr.count() > count);
			QCOMPARE(info.devicePixelRatio(), 2.0);
			QCOMPARE(nativeMask.boundingRect(), QRect(4364, 1000, 24, 200));
		}
		QWindowSystemInterface::handleScreenRemoved(platform);
	}

	void reservationsSurviveHotplugAndRestart() {
		QTemporaryDir temp;
		QVERIFY(temp.isValid());
		const auto reservations = temp.path() + QStringLiteral("/screen-names.json");
		const auto configDir = temp.path() + QStringLiteral("/monitors");

		auto* firstPlatform = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(firstPlatform);
		auto* first = firstPlatform->screen();
		auto deviceNames = QHash<QScreen*, QString> {{first, QStringLiteral("DISPLAY1")}};
		QList<DisplayIdentity> displays {
		    {QStringLiteral("DISPLAY1"),
		     QStringLiteral("generic-path-a"),
		     QStringLiteral("Generic Monitor")},
		};
		{
			ScreenIdentityRegistry registry(
			    [&] { return displays; },
			    [&](QScreen* target) { return deviceNames.value(target); },
			    nullptr,
			    reservations,
			    configDir
			);
			QuickshellScreenInfo firstInfo(nullptr, first, &registry);
			QCOMPARE(firstInfo.name(), QStringLiteral("Generic Monitor"));

			// B's id sorts lower, but A was discovered first and keeps its name.
			auto* secondPlatform = new FakeScreen;
			QWindowSystemInterface::handleScreenAdded(secondPlatform);
			auto* second = secondPlatform->screen();
			deviceNames.insert(second, QStringLiteral("DISPLAY2"));
			displays = {
			    {QStringLiteral("DISPLAY1"),
			     QStringLiteral("generic-path-a"),
			     QStringLiteral("Generic Monitor")},
			    {QStringLiteral("DISPLAY2"),
			     QStringLiteral("generic-path-b"),
			     QStringLiteral("Generic Monitor")},
			};
			registry.refresh();
			QuickshellScreenInfo secondInfo(nullptr, second, &registry);
			QCOMPARE(firstInfo.name(), QStringLiteral("Generic Monitor"));
			QCOMPARE(secondInfo.name(), QStringLiteral("Generic Monitor (2)"));
			QCOMPARE(
			    reservationById(reservations, QStringLiteral("path:generic-path-b"))
			        .value(QStringLiteral("name"))
			        .toString(),
			    QStringLiteral("Generic Monitor (2)")
			);

			// Removing B cannot renumber A.
			QWindowSystemInterface::handleScreenRemoved(secondPlatform);
			displays = {
			    {QStringLiteral("DISPLAY1"),
			     QStringLiteral("generic-path-a"),
			     QStringLiteral("Generic Monitor")},
			};
			registry.refresh();
			QCOMPARE(firstInfo.name(), QStringLiteral("Generic Monitor"));

			// Re-adding B restores its reserved name, not the next free suffix.
			auto* replacementPlatform = new FakeScreen;
			QWindowSystemInterface::handleScreenAdded(replacementPlatform);
			auto* replacement = replacementPlatform->screen();
			deviceNames.insert(replacement, QStringLiteral("DISPLAY2"));
			displays.append(
			    {QStringLiteral("DISPLAY2"),
			     QStringLiteral("generic-path-b"),
			     QStringLiteral("Generic Monitor")}
			);
			registry.refresh();
			QuickshellScreenInfo replacementInfo(nullptr, replacement, &registry);
			QCOMPARE(firstInfo.name(), QStringLiteral("Generic Monitor"));
			QCOMPARE(replacementInfo.name(), QStringLiteral("Generic Monitor (2)"));
			QWindowSystemInterface::handleScreenRemoved(replacementPlatform);
		}

		// A fresh registry (a shell restart) reads the same reservations.
		displays = {
		    {QStringLiteral("DISPLAY1"),
		     QStringLiteral("generic-path-a"),
		     QStringLiteral("Generic Monitor")},
		    {QStringLiteral("DISPLAY2"),
		     QStringLiteral("generic-path-b"),
		     QStringLiteral("Generic Monitor")},
		};
		auto* restartSecondPlatform = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(restartSecondPlatform);
		auto* restartSecond = restartSecondPlatform->screen();
		deviceNames.insert(restartSecond, QStringLiteral("DISPLAY2"));
		{
			ScreenIdentityRegistry registry(
			    [&] { return displays; },
			    [&](QScreen* target) { return deviceNames.value(target); },
			    nullptr,
			    reservations,
			    configDir
			);
			QuickshellScreenInfo firstInfo(nullptr, first, &registry);
			QuickshellScreenInfo secondInfo(nullptr, restartSecond, &registry);
			QCOMPARE(firstInfo.name(), QStringLiteral("Generic Monitor"));
			QCOMPARE(secondInfo.name(), QStringLiteral("Generic Monitor (2)"));
		}
		QWindowSystemInterface::handleScreenRemoved(restartSecondPlatform);
		QWindowSystemInterface::handleScreenRemoved(firstPlatform);
	}

	void pathAppearingLaterIsARemovalAndAnAddition() {
		QTemporaryDir temp;
		QVERIFY(temp.isValid());
		const auto reservations = temp.path() + QStringLiteral("/screen-names.json");
		const auto configDir = temp.path() + QStringLiteral("/monitors");

		auto* platform = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(platform);
		auto* screen = platform->screen();
		auto deviceNames = QHash<QScreen*, QString> {{screen, QStringLiteral("DISPLAY1")}};
		QList<DisplayIdentity> displays {
		    {QStringLiteral("DISPLAY1"), {}, QStringLiteral("Generic Monitor")},
		};
		QString firstSessionId;
		{
			ScreenIdentityRegistry registry(
			    [&] { return displays; },
			    [&](QScreen* target) { return deviceNames.value(target); },
			    nullptr,
			    reservations,
			    configDir
			);
			QuickshellScreenInfo info(nullptr, screen, &registry);
			QSignalSpy retired(&registry, &ScreenIdentityRegistry::identityRetired);
			QCOMPARE(info.name(), QStringLiteral("Generic Monitor"));
			QVERIFY(!registry.identity(screen).persistent);
			firstSessionId = registry.identity(screen).stableId;
			QVERIFY(firstSessionId.startsWith(QStringLiteral("session:")));
			QVERIFY(!QFileInfo::exists(reservations)); // Session ids are never persisted.

			// EnumDisplayDevices later supplies the interface path. The session id is
			// retired as if the screen had left, and the screen is added again under
			// its persistent id, which is allocated a name like any new monitor.
			displays[0].devicePath = QStringLiteral("generic-path-a");
			registry.refresh();
			QCOMPARE(retired.count(), 1);
			QCOMPARE(retired.at(0).at(0).toString(), firstSessionId);
			QCOMPARE(retired.at(0).at(1).value<QScreen*>(), screen);
			QVERIFY(registry.identity(screen).persistent);
			QCOMPARE(registry.identity(screen).stableId, QStringLiteral("path:generic-path-a"));
			QCOMPARE(info.name(), QStringLiteral("Generic Monitor"));
			QCOMPARE(
			    reservationById(reservations, QStringLiteral("path:generic-path-a"))
			        .value(QStringLiteral("name"))
			        .toString(),
			    QStringLiteral("Generic Monitor")
			);
			QVERIFY(!reservationById(reservations, firstSessionId).contains(QStringLiteral("name")));

			// A real removal retires the persistent id too, before the screen is gone.
			QWindowSystemInterface::handleScreenRemoved(platform);
			QCOMPARE(retired.count(), 2);
			QCOMPARE(retired.at(1).at(0).toString(), QStringLiteral("path:generic-path-a"));

			// The same GDI source with no path is a new session: it must not
			// inherit the old session id or any state keyed by it.
			auto* replacementPlatform = new FakeScreen;
			QWindowSystemInterface::handleScreenAdded(replacementPlatform);
			auto* replacement = replacementPlatform->screen();
			deviceNames.insert(replacement, QStringLiteral("DISPLAY1"));
			displays = {{QStringLiteral("DISPLAY1"), {}, QStringLiteral("Generic Monitor")}};
			registry.refresh();
			const auto replacementId = registry.identity(replacement).stableId;
			QVERIFY(replacementId.startsWith(QStringLiteral("session:")));
			QVERIFY(replacementId != firstSessionId);
			QVERIFY(!registry.identity(replacement).persistent);
			QVERIFY(reservationById(reservations, replacementId).isEmpty());
			QWindowSystemInterface::handleScreenRemoved(replacementPlatform);
		}
	}

	void legacyMetadataOnlyForGeneratedSuffixes() {
		QTemporaryDir temp;
		QVERIFY(temp.isValid());
		const auto reservations = temp.path() + QStringLiteral("/screen-names.json");
		const auto configDir = temp.path() + QStringLiteral("/monitors");
		QVERIFY(QDir().mkpath(configDir + QStringLiteral("/Generic Monitor")));

		auto* firstPlatform = new FakeScreen;
		auto* secondPlatform = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(firstPlatform);
		QWindowSystemInterface::handleScreenAdded(secondPlatform);
		auto* first = firstPlatform->screen();
		auto* second = secondPlatform->screen();
		QHash<QScreen*, QString> names {
		    {first, QStringLiteral("DISPLAY1")},
		    {second, QStringLiteral("DISPLAY2")},
		};
		QList<DisplayIdentity> displays {
		    {QStringLiteral("DISPLAY1"),
		     QStringLiteral("generic-path-a"),
		     QStringLiteral("Generic Monitor")},
		    {QStringLiteral("DISPLAY2"),
		     QStringLiteral("generic-path-b"),
		     QStringLiteral("Generic Monitor")},
		};
		{
			ScreenIdentityRegistry registry(
			    [&] { return displays; },
			    [&](QScreen* target) { return names.value(target); },
			    nullptr,
			    reservations,
			    configDir
			);
			QuickshellScreenInfo firstInfo(nullptr, first, &registry);
			QuickshellScreenInfo secondInfo(nullptr, second, &registry);
			QCOMPARE(firstInfo.name(), QStringLiteral("Generic Monitor"));
			QCOMPARE(secondInfo.name(), QStringLiteral("Generic Monitor (2)"));
			QCOMPARE(
			    reservationById(reservations, QStringLiteral("path:generic-path-b"))
			        .value(QStringLiteral("legacyLabel"))
			        .toString(),
			    QStringLiteral("Generic Monitor")
			);
			QVERIFY(!reservationById(reservations, QStringLiteral("path:generic-path-a"))
			             .contains(QStringLiteral("legacyLabel")));
		}
		QWindowSystemInterface::handleScreenRemoved(secondPlatform);
		QWindowSystemInterface::handleScreenRemoved(firstPlatform);

		// A genuine label that merely looks like a generated suffix carries no
		// legacy metadata.
		QTemporaryDir genuineTemp;
		QVERIFY(genuineTemp.isValid());
		const auto genuineReservations = genuineTemp.path() + QStringLiteral("/screen-names.json");
		auto* genuinePlatform = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(genuinePlatform);
		auto* genuine = genuinePlatform->screen();
		displays = {
		    {QStringLiteral("DISPLAY1"),
		     QStringLiteral("other-path"),
		     QStringLiteral("Other Monitor (2)")},
		};
		names.insert(genuine, QStringLiteral("DISPLAY1"));
		{
			ScreenIdentityRegistry registry(
			    [&] { return displays; },
			    [&](QScreen* target) { return names.value(target); },
			    nullptr,
			    genuineReservations,
			    genuineTemp.path() + QStringLiteral("/monitors")
			);
			QuickshellScreenInfo info(nullptr, genuine, &registry);
			QCOMPARE(info.name(), QStringLiteral("Other Monitor (2)"));
			const auto reservation =
			    reservationById(genuineReservations, QStringLiteral("path:other-path"));
			QCOMPARE(
			    reservation.value(QStringLiteral("name")).toString(),
			    QStringLiteral("Other Monitor (2)")
			);
			QVERIFY(!reservation.contains(QStringLiteral("legacyLabel")));
		}
		QWindowSystemInterface::handleScreenRemoved(genuinePlatform);
	}

	void sessionOnlyNamesSurviveHotplug() {
		QTemporaryDir temp;
		QVERIFY(temp.isValid());
		const auto reservations = temp.path() + QStringLiteral("/screen-names.json");

		auto* firstPlatform = new FakeScreen;
		auto* secondPlatform = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(firstPlatform);
		QWindowSystemInterface::handleScreenAdded(secondPlatform);
		auto* first = firstPlatform->screen();
		auto* second = secondPlatform->screen();
		QHash<QScreen*, QString> devices {
		    {first, QStringLiteral("DISPLAY1")},
		    {second, QStringLiteral("DISPLAY2")},
		};
		// Neither API supplied an interface path, so both ids are session-local.
		QList<DisplayIdentity> displays {
		    {QStringLiteral("DISPLAY1"), {}, QStringLiteral("Generic Monitor")},
		    {QStringLiteral("DISPLAY2"), {}, QStringLiteral("Generic Monitor")},
		};
		auto* thirdPlatform = new FakeScreen;
		auto* persistentPlatform = new FakeScreen;
		FakeScreen* survivorPlatform = nullptr;
		{
			ScreenIdentityRegistry registry(
			    [&] { return displays; },
			    [&](QScreen* target) { return devices.value(target); },
			    nullptr,
			    reservations,
			    temp.path() + QStringLiteral("/monitors")
			);
			QuickshellScreenInfo firstInfo(nullptr, first, &registry);
			QuickshellScreenInfo secondInfo(nullptr, second, &registry);
			QVERIFY(!registry.identity(first).persistent);
			QVERIFY(!registry.identity(second).persistent);
			auto names = QStringList {firstInfo.name(), secondInfo.name()};
			names.sort();
			QCOMPARE(
			    names,
			    QStringList({QStringLiteral("Generic Monitor"), QStringLiteral("Generic Monitor (2)")})
			);
			QVERIFY(!QFileInfo::exists(reservations)); // Session ids are never persisted.

			// Unplug the monitor that holds the plain name. The survivor's session
			// id is unchanged, so it keeps "Generic Monitor (2)".
			const auto firstIsPlain = firstInfo.name() == QStringLiteral("Generic Monitor");
			auto* removedPlatform = firstIsPlain ? firstPlatform : secondPlatform;
			auto& survivorInfo = firstIsPlain ? secondInfo : firstInfo;
			survivorPlatform = firstIsPlain ? secondPlatform : firstPlatform;
			displays ={firstIsPlain ? displays[1] : displays[0]};
			QWindowSystemInterface::handleScreenRemoved(removedPlatform);
			registry.refresh();
			QCOMPARE(survivorInfo.name(), QStringLiteral("Generic Monitor (2)"));
			registry.refresh();
			QCOMPARE(survivorInfo.name(), QStringLiteral("Generic Monitor (2)"));

			// A genuinely new session id is allocated a name; the plain one is free.
			QWindowSystemInterface::handleScreenAdded(thirdPlatform);
			auto* third = thirdPlatform->screen();
			devices.insert(third, QStringLiteral("DISPLAY3"));
			displays.append({QStringLiteral("DISPLAY3"), {}, QStringLiteral("Generic Monitor")});
			registry.refresh();
			QuickshellScreenInfo thirdInfo(nullptr, third, &registry);
			QCOMPARE(survivorInfo.name(), QStringLiteral("Generic Monitor (2)"));
			QCOMPARE(thirdInfo.name(), QStringLiteral("Generic Monitor"));

			// A persistent allocation reserves against the live session names too.
			QWindowSystemInterface::handleScreenAdded(persistentPlatform);
			auto* persistent = persistentPlatform->screen();
			devices.insert(persistent, QStringLiteral("DISPLAY4"));
			displays.append(
			    {QStringLiteral("DISPLAY4"),
			     QStringLiteral("generic-path-a"),
			     QStringLiteral("Generic Monitor")}
			);
			registry.refresh();
			QuickshellScreenInfo persistentInfo(nullptr, persistent, &registry);
			QCOMPARE(persistentInfo.name(), QStringLiteral("Generic Monitor (3)"));
			QCOMPARE(survivorInfo.name(), QStringLiteral("Generic Monitor (2)"));
			QCOMPARE(thirdInfo.name(), QStringLiteral("Generic Monitor"));
			QCOMPARE(
			    reservationById(reservations, QStringLiteral("path:generic-path-a"))
			        .value(QStringLiteral("name"))
			        .toString(),
			    QStringLiteral("Generic Monitor (3)")
			);
		}
		QWindowSystemInterface::handleScreenRemoved(persistentPlatform);
		QWindowSystemInterface::handleScreenRemoved(thirdPlatform);
		QWindowSystemInterface::handleScreenRemoved(survivorPlatform);
	}

	void unreadableReservationFileIsNeverOverwritten_data() {
		QTest::addColumn<QByteArray>("payload");
		QTest::newRow("malformed json") << QByteArray("{ not json");
		QTest::newRow("array root") << QByteArray("[1, 2]");
		QTest::newRow("reservations not an object")
		    << QByteArray("{\"version\":1,\"reservations\":[]}");
		QTest::newRow("empty file") << QByteArray();
	}

	void unreadableReservationFileIsNeverOverwritten() {
		QFETCH(QByteArray, payload);
		QTemporaryDir temp;
		QVERIFY(temp.isValid());
		const auto reservations = temp.path() + QStringLiteral("/screen-names.json");
		writeBytes(reservations, payload);

		auto* firstPlatform = new FakeScreen;
		auto* secondPlatform = new FakeScreen;
		auto* thirdPlatform = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(firstPlatform);
		QWindowSystemInterface::handleScreenAdded(secondPlatform);
		auto* first = firstPlatform->screen();
		auto* second = secondPlatform->screen();
		QHash<QScreen*, QString> devices {
		    {first, QStringLiteral("DISPLAY1")},
		    {second, QStringLiteral("DISPLAY2")},
		};
		QList<DisplayIdentity> displays {
		    {QStringLiteral("DISPLAY1"),
		     QStringLiteral("generic-path-a"),
		     QStringLiteral("Generic Monitor")},
		    {QStringLiteral("DISPLAY2"),
		     QStringLiteral("generic-path-b"),
		     QStringLiteral("Generic Monitor")},
		};
		{
			ScreenIdentityRegistry registry(
			    [&] { return displays; },
			    [&](QScreen* target) { return devices.value(target); },
			    nullptr,
			    reservations,
			    temp.path() + QStringLiteral("/monitors")
			);
			QuickshellScreenInfo firstInfo(nullptr, first, &registry);
			QuickshellScreenInfo secondInfo(nullptr, second, &registry);
			// Names still work, held in memory for this session.
			QCOMPARE(firstInfo.name(), QStringLiteral("Generic Monitor"));
			QCOMPARE(secondInfo.name(), QStringLiteral("Generic Monitor (2)"));

			// A later allocation and a refresh must not save either.
			QWindowSystemInterface::handleScreenAdded(thirdPlatform);
			auto* third = thirdPlatform->screen();
			devices.insert(third, QStringLiteral("DISPLAY3"));
			displays.append(
			    {QStringLiteral("DISPLAY3"),
			     QStringLiteral("generic-path-c"),
			     QStringLiteral("Generic Monitor")}
			);
			registry.refresh();
			QuickshellScreenInfo thirdInfo(nullptr, third, &registry);
			QCOMPARE(thirdInfo.name(), QStringLiteral("Generic Monitor (3)"));
			registry.refresh();
			QCOMPARE(firstInfo.name(), QStringLiteral("Generic Monitor"));
			QCOMPARE(secondInfo.name(), QStringLiteral("Generic Monitor (2)"));
		}
		QWindowSystemInterface::handleScreenRemoved(thirdPlatform);
		QWindowSystemInterface::handleScreenRemoved(secondPlatform);
		QWindowSystemInterface::handleScreenRemoved(firstPlatform);

		QCOMPARE(fileBytes(reservations), payload);
		// No stray temporary file either: nothing ever tried to save.
		QCOMPARE(
		    QDir(temp.path()).entryList(QDir::Files),
		    QStringList({QStringLiteral("screen-names.json")})
		);
	}

	void reservationKeysAreHashedAndRawKeysMigrate() {
		// Known answer: unsalted SHA-256 of the persistent id, so the key is the
		// same on every run and machine. Not the per-process salted logId.
		QCOMPARE(
		    ScreenIdentityRegistry::reservationKey(QStringLiteral("path:generic-path-b")),
		    QStringLiteral("sha256:e0002be7649c9addb036d4ecc6aeec06651aa77421a99e154724eec1f0be994c")
		);

		QTemporaryDir temp;
		QVERIFY(temp.isValid());
		const auto reservations = temp.path() + QStringLiteral("/screen-names.json");
		// What the previous revision wrote: raw interface paths as keys.
		writeBytes(
		    reservations,
		    QByteArray(
		        R"json({"version":1,"reservations":{
		            "path:generic-path-a":{"name":"Generic Monitor","label":"Generic Monitor"},
		            "path:generic-path-b":{"name":"Generic Monitor (2)","label":"Generic Monitor",
		                "legacyLabel":"Generic Monitor","legacyCopied":["state.json"]}}})json"
		    )
		);

		auto* firstPlatform = new FakeScreen;
		auto* secondPlatform = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(firstPlatform);
		QWindowSystemInterface::handleScreenAdded(secondPlatform);
		auto* first = firstPlatform->screen();
		auto* second = secondPlatform->screen();
		QHash<QScreen*, QString> devices {
		    {first, QStringLiteral("DISPLAY1")},
		    {second, QStringLiteral("DISPLAY2")},
		};
		// Enumerated in the opposite order: only the reservations decide.
		QList<DisplayIdentity> displays {
		    {QStringLiteral("DISPLAY1"),
		     QStringLiteral("generic-path-b"),
		     QStringLiteral("Generic Monitor")},
		    {QStringLiteral("DISPLAY2"),
		     QStringLiteral("generic-path-a"),
		     QStringLiteral("Generic Monitor")},
		};
		{
			ScreenIdentityRegistry registry(
			    [&] { return displays; },
			    [&](QScreen* target) { return devices.value(target); },
			    nullptr,
			    reservations,
			    temp.path() + QStringLiteral("/monitors")
			);
			QuickshellScreenInfo firstInfo(nullptr, first, &registry);
			QuickshellScreenInfo secondInfo(nullptr, second, &registry);
			QCOMPARE(firstInfo.name(), QStringLiteral("Generic Monitor (2)"));
			QCOMPARE(secondInfo.name(), QStringLiteral("Generic Monitor"));
		}
		QWindowSystemInterface::handleScreenRemoved(secondPlatform);
		QWindowSystemInterface::handleScreenRemoved(firstPlatform);

		const auto bytes = fileBytes(reservations);
		QVERIFY(!bytes.contains("generic-path"));
		QVERIFY(!bytes.contains("path:"));
		const auto stored = allReservations(reservations);
		QCOMPARE(stored.size(), 2);
		for (auto it = stored.begin(); it != stored.end(); ++it) {
			QVERIFY(it.key().startsWith(QStringLiteral("sha256:")));
			QVERIFY(it.value().toObject().contains(QStringLiteral("lastSeen")));
		}
		// The migrated entry kept everything, including a recorded legacy copy.
		const auto migrated = reservationById(reservations, QStringLiteral("path:generic-path-b"));
		QCOMPARE(migrated.value(QStringLiteral("name")).toString(), QStringLiteral("Generic Monitor (2)"));
		QCOMPARE(
		    migrated.value(QStringLiteral("legacyLabel")).toString(),
		    QStringLiteral("Generic Monitor")
		);
		QCOMPARE(migrated.value(QStringLiteral("legacyCopied")).toArray().size(), 1);
		QCOMPARE(
		    migrated.value(QStringLiteral("legacyCopied")).toArray().at(0).toString(),
		    QStringLiteral("state.json")
		);
	}

	void reservationsAreNeverEvictedAndNamesNeverReused() {
		QTemporaryDir temp;
		QVERIFY(temp.isValid());
		const auto reservations = temp.path() + QStringLiteral("/screen-names.json");
		auto entry = [](const QString& name, const QString& label) {
			QJsonObject object;
			object.insert(QStringLiteral("name"), name);
			object.insert(QStringLiteral("label"), label);
			object.insert(QStringLiteral("lastSeen"), QStringLiteral("2025-01-01"));
			return object;
		};
		auto stored = QJsonObject();
		// 150 disconnected monitors: a family of identical ones plus 148 singletons,
		// the oldest of them a singleton "Solo Monitor".
		stored.insert(
		    ScreenIdentityRegistry::reservationKey(QStringLiteral("path:solo")),
		    entry(QStringLiteral("Solo Monitor"), QStringLiteral("Solo Monitor"))
		);
		constexpr int others = 148;
		for (int index = 0; index != others; ++index) {
			stored.insert(
			    ScreenIdentityRegistry::reservationKey(QStringLiteral("path:other-%1").arg(index)),
			    entry(QStringLiteral("Other %1").arg(index), QStringLiteral("Other %1").arg(index))
			);
		}
		stored.insert(
		    ScreenIdentityRegistry::reservationKey(QStringLiteral("path:family-b")),
		    entry(QStringLiteral("Family Monitor (2)"), QStringLiteral("Family Monitor"))
		);
		auto root = QJsonObject();
		root.insert(QStringLiteral("version"), 1);
		root.insert(QStringLiteral("reservations"), stored);
		writeBytes(reservations, QJsonDocument(root).toJson());
		const auto before = allReservations(reservations).size();
		QVERIFY(before > 100);

		auto* platform = new FakeScreen;
		QWindowSystemInterface::handleScreenAdded(platform);
		auto* screen = platform->screen();
		// A different monitor that happens to have the same label as a forgotten
		// one must not be handed that monitor's name.
		QList<DisplayIdentity> displays {
		    {QStringLiteral("DISPLAY1"), QStringLiteral("new-path"), QStringLiteral("Solo Monitor")},
		};
		{
			ScreenIdentityRegistry registry(
			    [&] { return displays; },
			    [screen](QScreen* target) {
				    return target == screen ? QStringLiteral("DISPLAY1") : QString();
			    },
			    nullptr,
			    reservations,
			    temp.path() + QStringLiteral("/monitors")
			);
			QuickshellScreenInfo info(nullptr, screen, &registry);
			QCOMPARE(info.name(), QStringLiteral("Solo Monitor (2)"));
		}
		QWindowSystemInterface::handleScreenRemoved(platform);

		// Nothing was evicted: every earlier reservation is still there with its
		// name, and the new one was added.
		const auto kept = allReservations(reservations);
		QCOMPARE(kept.size(), before + 1);
		QCOMPARE(
		    reservationById(reservations, QStringLiteral("path:solo"))
		        .value(QStringLiteral("name"))
		        .toString(),
		    QStringLiteral("Solo Monitor")
		);
		QCOMPARE(
		    reservationById(reservations, QStringLiteral("path:new-path"))
		        .value(QStringLiteral("name"))
		        .toString(),
		    QStringLiteral("Solo Monitor (2)")
		);
		QSet<QString> names;
		for (auto it = kept.begin(); it != kept.end(); ++it)
			names.insert(it.value().toObject().value(QStringLiteral("name")).toString());
		QCOMPARE(names.size(), kept.size()); // No two monitors share a name.
	}
};

int main(int argc, char** argv) {
	qputenv("QT_QPA_PLATFORM", "offscreen");
	qputenv("QT_SCALE_FACTOR", "1");
	QGuiApplication app(argc, argv);
	ScreenIdentityTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "screen_identity_test.moc"

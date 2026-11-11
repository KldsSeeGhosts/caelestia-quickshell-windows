#include <qguiapplication.h>
#include <qloggingcategory.h>
#include <qqml.h>
#include <qqmlcomponent.h>
#include <qqmlengine.h>
#include <qtest.h>

#include "../tiling.hpp"
#include "tiling_fake_desktop.hpp"

namespace qs::windows {

namespace {
// The two behaviours NativeTilingBackend.qml drives through the per-engine
// `Tiling` singleton: it enables the shared session when it completes and
// disables it when it is destroyed (NativeTilingBackend.qml:107).
constexpr auto FACADE_ENABLE_QML = R"(
import QtQuick
import Quickshell.Windows

QtObject {
    Component.onCompleted: Tiling.enabled = true
    Component.onDestruction: Tiling.enabled = false
}
)";

// Observe mode: the loader builds the wrapper and applies the options, but
// nothing requests enabling (TilingService.qml:286-307).
constexpr auto FACADE_OBSERVE_QML = R"(
import QtQuick
import Quickshell.Windows

QtObject {
    Component.onCompleted: {
        Tiling.gapsIn = 8;
        Tiling.gapsOut = 12;
        Tiling.singleWindowGapsOut = 24;
        Tiling.excluded = ["class:ignored"];
    }
    Component.onDestruction: Tiling.enabled = false
}
)";

quint64 packRect(const QRect& rect) {
    auto x = static_cast<quint64>(static_cast<quint16>(rect.left()));
    auto y = static_cast<quint64>(static_cast<quint16>(rect.top()));
    auto width = static_cast<quint64>(rect.width());
    auto height = static_cast<quint64>(rect.height());
    return x | (y << 16) | (width << 32) | (height << 48);
}
} // namespace

// The reload contract. Quickshell completes the new generation's root before
// it disposes of the old one: rootwrapper.cpp:234-256 calls beginCreate,
// completeCreate and then EngineGeneration::onReload, and generation.cpp:159-169
// reloads the new root before old->destroy(), which defers the old root's
// deletion through deleteLater (generation.cpp:120). A retiring facade's
// destruction therefore always arrives after its replacement is complete.
class TilingReloadTest: public QObject {
	Q_OBJECT;

	struct Facade {
		QQmlEngine* engine = nullptr;
		QObject* object = nullptr;
	};

	TilingManager* manager = nullptr;
	QList<TrackedWindow*> windows;
	QList<QQmlEngine*> engines;

	TrackedWindow* add(quintptr id, const QRect& frame) {
		auto* window = new TrackedWindow(id);
		fakeDesktop.windows.insert(window->hwnd(), FakeWindow {});
		auto& native = fakeWindow(window->hwnd());
		native.frame = frame;
		native.normal = frame;
		windows.append(window);
		WindowTracker::instance()->fixtures.append(window);
		return window;
	}

	Facade openFacade(const char* qml) {
		auto* engine = new QQmlEngine();
		engines.append(engine);
		QQmlComponent component(engine);
		component.setData(QByteArray(qml), QUrl(QStringLiteral("qrc:/NativeTilingBackend.qml")));
		auto* object = component.create();
		if (object == nullptr) {
			qWarning() << "fixture facade failed to load:" << component.errorString();
			return {engine, nullptr};
		}
		QQmlEngine::setObjectOwnership(object, QQmlEngine::CppOwnership);
		return {engine, object};
	}

	// Models one QML reload. `interrupted` is what Quickshell does when the
	// retiring root starts tearing down (generation.cpp:89-93); without it the
	// wrapper's own destruction handler still runs and writes the disable.
	void retire(Facade facade, bool interrupted = true) {
		if (facade.engine != nullptr && interrupted) facade.engine->setInterrupted(true);
		delete facade.object;
		delete facade.engine;
		engines.removeAll(facade.engine);
	}

	void reset() {
		if (manager != nullptr && manager->enabled()) manager->setEnabled(false);
		if (manager != nullptr) {
			manager->snapshot = {};
			manager->snapshotLoaded = true;
		}
		for (auto* engine: engines) delete engine;
		engines.clear();
		qDeleteAll(windows);
		windows.clear();
		WindowTracker::instance()->fixtures.clear();
		fakeDesktop = {};
	}

private slots:
	void initTestCase() {
		qmlRegisterSingletonType<Tiling>(
		    "Quickshell.Windows",
		    1,
		    0,
		    "Tiling",
		    [](QQmlEngine*, QJSEngine*) -> QObject* { return new Tiling(); }
		);

		fakeDesktop = {};
		auto* ordinary = add(1, QRect(40, 60, 600, 400));
		// A window a previous run parked off screen still carries the park
		// property and sits outside every monitor.
		auto* parked = add(2, QRect(4000, 4000, 600, 400));
		fakeWindow(parked->hwnd()).parked = reinterpret_cast<HANDLE>(
		    static_cast<quintptr>(packRect(QRect(100, 100, 600, 400)))
		);

		manager = TilingManager::instance();

		// Prior-crash recovery stays an explicit construction-time operation:
		// it runs once, brings back only the window that carries the park
		// property, and leaves an ordinary window where it was.
		QCOMPARE(fakeDesktop.rescues, 1);
		QCOMPARE(fakeWindow(parked->hwnd()).frame, QRect(100, 100, 600, 400));
		QCOMPARE(fakeWindow(ordinary->hwnd()).frame, QRect(40, 60, 600, 400));
		QCOMPARE(fakeWindow(ordinary->hwnd()).moves, 0);
		QVERIFY(!manager->enabled());
	}

	void cleanupTestCase() { reset(); }

	void init() { reset(); }

	void cleanup() { reset(); }

	void retiringFacadeCannotDisableItsReplacement() {
		add(1, QRect(40, 60, 600, 400));

		auto retiring = openFacade(FACADE_ENABLE_QML);
		QVERIFY(retiring.object != nullptr);
		QVERIFY(manager->enabled());

		auto replacement = openFacade(FACADE_ENABLE_QML);
		QVERIFY(replacement.object != nullptr);
		QVERIFY(manager->enabled());

		// The reload disposes of the old generation after the new one completed.
		// Worst case for the invariant: the retiring wrapper's destruction
		// handler still runs while its replacement is live.
		retire(retiring, false);
		QVERIFY2(manager->enabled(), "the retiring facade disabled its replacement");

		// The replacement owns the switch, and its own teardown releases it even
		// when the engine is interrupted and no JavaScript runs.
		retire(replacement, true);
		QVERIFY(!manager->enabled());
	}

	void stoppedReloadKeepsTreeOrderAndFloats() {
		auto* a = add(1, QRect(40, 60, 600, 400));
		auto* b = add(2, QRect(700, 60, 600, 400));
		auto* c = add(3, QRect(700, 500, 600, 400));
		auto facade = openFacade(FACADE_ENABLE_QML);
		QVERIFY(facade.object != nullptr);
		manager->sync();
		auto* layout = manager->managed.at(a).layout;
		QVERIFY(layout != nullptr);
		layout->tree->swap(TilingManager::idOf(a), TilingManager::idOf(b));
		layout->tree->splitRatio(TilingManager::idOf(b), 1.4, true);
		QVERIFY(manager->setFloating(c, true, false));
		manager->sync();
		QHash<tiling::LayoutEngine::Id, tiling::LayoutEngine::Id> ids {
		    {TilingManager::idOf(a), 1}, {TilingManager::idOf(b), 2},
		};
		auto state = layout->tree->saveState(ids);
		retire(facade);
		QVERIFY(!manager->enabled());
		// Reverse discovery, and use rectangles whose order disagrees with
		// the saved tree. Neither may determine adoption order.
		WindowTracker::instance()->fixtures = {c, b, a};
		fakeWindow(a->hwnd()).frame = QRect(0, 0, 600, 400);
		fakeWindow(b->hwnd()).frame = QRect(900, 500, 600, 400);
		auto replacement = openFacade(FACADE_ENABLE_QML);
		QVERIFY(replacement.object != nullptr);
		manager->sync();
		auto* restored = manager->managed.at(a).layout;
		QVERIFY(restored != nullptr);
		QCOMPARE(restored->tree->saveState(ids), state);
		QVERIFY(manager->managed.at(c).layout == nullptr);
		QCOMPARE(manager->managed.at(c).override, TilingManager::Override::Float);
		retire(replacement);
	}

	void observeModeNeverPlacesWindows() {
		auto* window = add(1, QRect(40, 60, 600, 400));
		const auto rescues = fakeDesktop.rescues;

		auto facade = openFacade(FACADE_OBSERVE_QML);
		QVERIFY(facade.object != nullptr);

		// Observe constructs the wrapper and applies its options, and nothing
		// more: no session, no tracker hooks, no mutex, no placement.
		QVERIFY(!manager->enabled());
		QVERIFY(manager->connections.isEmpty());
		QVERIFY(!fakeDesktop.mutexOpen);
		QCOMPARE(fakeWindow(window->hwnd()).moves, 0);
		QCOMPARE(fakeWindow(window->hwnd()).placements, 0);
		QCOMPARE(fakeWindow(window->hwnd()).frame, QRect(40, 60, 600, 400));

		// Entering off destroys the wrapper; the late disable is a no-op.
		retire(facade);
		QVERIFY(!manager->enabled());
		QCOMPARE(fakeWindow(window->hwnd()).moves, 0);
		QCOMPARE(fakeWindow(window->hwnd()).placements, 0);
		QCOMPARE(fakeWindow(window->hwnd()).frame, QRect(40, 60, 600, 400));
		QCOMPARE(fakeDesktop.rescues, rescues);
	}

	void reloadCyclesDoNotDuplicateHooksOrRestores() {
		auto* window = add(1, QRect(40, 60, 600, 400));
		const auto rescues = fakeDesktop.rescues;
		qsizetype hooks = 0;
		int movesPerCycle = 0;
		int placementsPerCycle = 0;

		for (auto cycle = 0; cycle < 3; cycle++) {
			const auto movesBefore = fakeWindow(window->hwnd()).moves;
			const auto placementsBefore = fakeWindow(window->hwnd()).placements;

			auto facade = openFacade(FACADE_ENABLE_QML);
			QVERIFY(facade.object != nullptr);
			QVERIFY(manager->enabled());
			QVERIFY(!manager->connections.isEmpty());
			if (cycle == 0) hooks = manager->connections.size();
			else QCOMPARE(manager->connections.size(), hooks);

			// Drive the scheduled sync the way the event loop would.
			QVERIFY(QMetaObject::invokeMethod(&manager->syncTimer, "timeout", Qt::DirectConnection));
			QVERIFY(fakeWindow(window->hwnd()).moves > movesBefore);

			retire(facade);
			QVERIFY(!manager->enabled());
			// One disable releases every hook and restores the window once.
			QVERIFY(manager->connections.isEmpty());
			QCOMPARE(fakeWindow(window->hwnd()).frame, QRect(40, 60, 600, 400));
			QVERIFY(!fakeDesktop.mutexOpen);

			// A reload must cost the same as the first start, so no hook, timer
			// or restore survives a cycle to act a second time.
			const auto moves = fakeWindow(window->hwnd()).moves - movesBefore;
			const auto placements = fakeWindow(window->hwnd()).placements - placementsBefore;
			if (cycle == 0) {
				movesPerCycle = moves;
				placementsPerCycle = placements;
			} else {
				QCOMPARE(moves, movesPerCycle);
				QCOMPARE(placements, placementsPerCycle);
			}
			// Recovery is not repeated by an enable, a disable or a reload.
			QCOMPARE(fakeDesktop.rescues, rescues);
		}
		QVERIFY(movesPerCycle > 0);
	}
};

} // namespace qs::windows

int main(int argc, char** argv) {
	qputenv("QT_QPA_PLATFORM", "offscreen");
	QGuiApplication app(argc, argv);
	qs::windows::TilingReloadTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "tiling_reload_test.moc"

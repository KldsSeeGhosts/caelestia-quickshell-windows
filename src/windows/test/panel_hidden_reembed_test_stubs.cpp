// The panel fixture links the production WinPanelWindow graph. These inert
// definitions stand in for the QML engine, desktop-accessor and drag
// singletons that the visibility and embedding paths never use. They keep the
// fixture off the QML engine and off the live desktop.

#include <qquickitem.h>
#include <qquickwindow.h>

#include "debug/lint.hpp"
#include "desktop_drop.hpp"
#include "desktopentry_backend.hpp"
#include "generation.hpp"
#include "keyboard_hook.hpp"
#include "qmlglobal.hpp"
#include "reload.hpp"
#include "super_drag.hpp"

namespace qs::log {
void initLogCategoryLevel(const char*, QtMsgType) {}
} // namespace qs::log

// Reloadable is the base of ProxyWindowBase. The fixture drives visibility with
// setVisibleDirect(), so it only needs the bookkeeping, not a generation.
void Reloadable::reload(QObject* oldInstance) {
	this->onReload(oldInstance);
	this->reloadComplete = true;
}

void Reloadable::componentComplete() { this->reloadComplete = true; }
void Reloadable::onReloadFinished() {}
void Reloadable::onGenerationDestroyed() { this->engineGeneration = nullptr; }
void Reloadable::reloadRecursive(QObject*, QObject*) {}
void Reloadable::reloadChildrenRecursive(QObject*, QObject*) {}
QObject* Reloadable::getChildByReloadId(QObject*, const QString&) { return nullptr; }

void ReloadPropagator::onReload(QObject*) {}
QQmlListProperty<QObject> ReloadPropagator::data() {
	return QQmlListProperty<QObject>(this, &this->mChildren);
}
void ReloadPropagator::appendComponent(QQmlListProperty<QObject>*, QObject*) {}

void PostReloadHook::componentComplete() { this->isPostReload = true; }
void PostReloadHook::postReload() {
	if (this->isPostReload) return;
	this->isPostReload = true;
	this->onPostReload();
}

EngineGeneration* EngineGeneration::findObjectGeneration(const QObject*) { return nullptr; }
EngineGeneration* EngineGeneration::findEngineGeneration(const QQmlEngine*) { return nullptr; }
void EngineGeneration::trackWindowIncubationController(QQuickWindow*) {}
void EngineGeneration::quit() {}
void EngineGeneration::exit(int) {}
void EngineGeneration::onFileChanged(const QString&) {}
void EngineGeneration::onDirectoryChanged() {}
void EngineGeneration::onTrackedWindowDestroyed(QObject*) {}
void EngineGeneration::onEngineWarnings(const QList<QQmlError>&) {}

QuickshellTracked* QuickshellTracked::instance() { return nullptr; }
QuickshellScreenInfo* QuickshellTracked::screenInfo(QScreen*) const { return nullptr; }

namespace qs::debug {
void lintItemTree(QQuickItem*) {}
} // namespace qs::debug

namespace qs::windows {
bool SuperDragManager::onMouseHook(WPARAM, const MSLLHOOKSTRUCT*) { return false; }
void SuperDragManager::onHookStopped() {}
void DesktopDropManager::onMouseHook(WPARAM, const MSLLHOOKSTRUCT*) {}
void WindowsDesktopEntryBackend::noteWindowExe(const QString&, const QString&) {}
} // namespace qs::windows

namespace qs::windows::hotkeys {
bool KeyboardHook::superDown() { return false; }
void KeyboardHook::noteSuperChord() {}
bool KeyboardHook::retain() { return false; }
void KeyboardHook::release() {}
} // namespace qs::windows::hotkeys

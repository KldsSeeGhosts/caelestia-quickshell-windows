#include "../../core/reload.hpp"
#include "../../core/logcat.hpp"

// This fixture has no shell generation, file watcher or render loop. Keep
// those integrations out of the process, but compile the production loader
// and incubator unchanged. No test relies on reload traversal.
void Reloadable::componentComplete() {}
void Reloadable::reload(QObject* old) {
	this->onReload(old);
	this->reloadComplete = true;
}
void Reloadable::reloadRecursive(QObject*, QObject*) {}
void Reloadable::reloadChildrenRecursive(QObject*, QObject*) {}
void Reloadable::onReloadFinished() {}
void Reloadable::onGenerationDestroyed() {}
void ReloadPropagator::onReload(QObject*) {}
QQmlListProperty<QObject> ReloadPropagator::data() { return {}; }
void PostReloadHook::componentComplete() {}
void PostReloadHook::postReload() {}

namespace qs::log {
void initLogCategoryLevel(const char*, QtMsgType) {}
}
namespace qs::windows {
float animationIntervalMs() { return 16.0f; }
}

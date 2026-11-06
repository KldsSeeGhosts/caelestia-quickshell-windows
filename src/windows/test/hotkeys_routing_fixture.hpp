#pragma once

#include <memory>

#include <qcoreapplication.h>
#include <qtest.h>
#include <windows.h>

#define private public
#include "../hotkeys.hpp"
#undef private

namespace hotkey_fixture {
inline std::shared_ptr<const qs::windows::hotkeys::HookSnapshot> snapshot;
inline bool running = false;
inline bool canStart = true;
inline bool registerChords = false;
inline int registrations = 0;
inline int unregistrations = 0;
inline BOOL WINAPI registerHotkey(HWND, int, UINT, UINT) {
	registrations++;
	return registerChords;
}
inline BOOL WINAPI unregisterHotkey(HWND, int) {
	unregistrations++;
	return TRUE;
}
} // namespace hotkey_fixture

// Compile the production manager and its window callback. Only platform
// registration and external shell services are replaced; no global hook is
// installed and no input is injected by this fixture.
#define RegisterHotKey hotkey_fixture::registerHotkey
#define UnregisterHotKey hotkey_fixture::unregisterHotkey
#include "../hotkeys.cpp"
#undef RegisterHotKey
#undef UnregisterHotKey

EngineGeneration* EngineGeneration::currentGeneration() { return nullptr; }
EngineGeneration* EngineGeneration::findEngineGeneration(const QQmlEngine*) { return nullptr; }
void RootWrapper::reloadGraph(bool) {}
InstanceInfo InstanceInfo::CURRENT;

namespace qs::io::process {
void disownHandles(QProcess*) {}
}
namespace qs::io::ipc {
IpcHandlerRegistry* IpcHandlerRegistry::forGeneration(EngineGeneration*) { return nullptr; }
IpcHandler* IpcHandlerRegistry::findHandler(const QString&) { return nullptr; }
IpcFunction* IpcHandler::findFunction(const QString&) { return nullptr; }
IpcCallStorage::IpcCallStorage(const IpcFunction&) {}
bool IpcCallStorage::setArgumentStr(size_t, const QString&) { return false; }
void IpcFunction::invoke(QObject*, IpcCallStorage&) const {}
QString IpcFunction::toString() const { return {}; }
}
namespace qs::windows {
void runLauncher(std::function<void()>) {}
// Inert services keep their real Qt signals but have no platform observers.
InputMaskTracker::InputMaskTracker(QObject* parent): QObject(parent) {}
InputMaskTracker::~InputMaskTracker() = default;
InputMaskTracker* InputMaskTracker::instance() {
	static InputMaskTracker tracker(nullptr);
	return &tracker;
}
void InputMaskTracker::acquireButtonEvents() {}
void InputMaskTracker::releaseButtonEvents() {}
}
namespace qs::windows::sys {
KeyboardLayoutWatcher::KeyboardLayoutWatcher(QObject* parent): QObject(parent) {}
KeyboardLayoutWatcher* KeyboardLayoutWatcher::instance() {
	static KeyboardLayoutWatcher watcher(nullptr);
	return &watcher;
}
Keyboard::Keyboard(QObject* parent): QObject(parent) {}
Keyboard::~Keyboard() = default;
void Keyboard::setActive(bool) {}
QString Keyboard::activateLayoutAt(int) { return {}; }
void Keyboard::activateLayout(const QString&) {}
}
namespace qs::windows::hotkeys {
bool KeyboardHook::start(HWND, UINT) {
	return hotkey_fixture::running = hotkey_fixture::canStart;
}
void KeyboardHook::stop() { hotkey_fixture::running = false; }
bool KeyboardHook::isRunning() { return hotkey_fixture::running; }
void KeyboardHook::setSnapshot(std::shared_ptr<const HookSnapshot> value) {
	hotkey_fixture::snapshot = std::move(value);
}
bool KeyboardHook::sendKey(uint8_t) { return false; }
bool KeyboardHook::armInputObserver(HWND, UINT, UINT, std::vector<uint8_t>&) { return false; }
void KeyboardHook::clearInputObserver() {}
}

#include "hotkeys.hpp"
#include "hotkey_defaults.hpp"
#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include <qcoreapplication.h>
#include <qdir.h>
#include <qendian.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qhash.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qjsonvalue.h>
#include <qjsvalue.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qmetaobject.h>
#include <qobject.h>
#include <qpointer.h>
#include <qprocess.h>
#include <qqmlengine.h>
#include <qstandardpaths.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtimer.h>
#include <qvariant.h>

#include "../core/generation.hpp"
#include "../core/instanceinfo.hpp"
#include "../core/rootwrapper.hpp"
#include "../io/ipchandler.hpp"
#include "../io/processcore.hpp"
#include "activation_policy.hpp"
#include "input_mask.hpp"
#include "input_settle.hpp"
#include "keyboard_hook.hpp"
#include "redact.hpp"
#include "system/keyboard.hpp"
#include "util.hpp"

namespace qs::windows::hotkeys {

namespace {

Q_LOGGING_CATEGORY(logHotkeys, "quickshell.windows.hotkeys", QtInfoMsg);

constexpr auto MESSAGE_WINDOW_CLASS = L"QuickshellHotkeys";
constexpr UINT WM_QS_HOOK_BASE = WM_APP + 16;
constexpr UINT WM_QS_INPUT_KEY = WM_APP + 32;
constexpr UINT WM_QS_INPUT_RESET = WM_APP + 33;
constexpr auto DEFAULT_FILE = "defaults/windows/keybinds.json";

constexpr int RELEASE_POLL_MS = 15;

HKL activeKeyboardLayout() {
	auto* foreground = GetForegroundWindow();
	auto thread = foreground == nullptr ? 0 : GetWindowThreadProcessId(foreground, nullptr);
	return GetKeyboardLayout(thread);
}

QChar punctuationChar(const QString& name) {
	static const QHash<QString, QChar> chars = {
	    {"slash", '/'},       {"period", '.'},         {"comma", ','},
	    {"minus", '-'},       {"equal", '='},          {"plus", '+'},
	    {"bracketleft", '['}, {"bracketright", ']'},   {"semicolon", ';'},
	    {"apostrophe", '\''}, {"grave", '`'},          {"backslash", '\\'},
	};

	return chars.value(name.toLower());
}

bool dependsOnLayout(const QString& keys) {
	return std::ranges::any_of(keys.split('+'), [](const QString& part) {
		return !punctuationChar(part.trimmed()).isNull();
	});
}

uint8_t layoutVkForChar(QChar ch, HKL layout, bool& needsShift) {
	needsShift = false;

	for (UINT vk = 1; vk < 0xFF; vk++) {
		if (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) continue;
		if ((MapVirtualKeyExW(vk, MAPVK_VK_TO_CHAR, layout) & 0x7FFF) == ch.unicode()) {
			return static_cast<uint8_t>(vk);
		}
	}

	auto scan = VkKeyScanExW(ch.unicode(), layout);
	if (scan == -1 || LOBYTE(scan) == 0 || (HIBYTE(scan) & 6) != 0) return 0;

	needsShift = (HIBYTE(scan) & 1) != 0;
	return LOBYTE(scan);
}

uint8_t modifierFromName(const QString& name) {
	static const QHash<QString, uint8_t> modifiers = {
	    {"ctrl", ModCtrl},      {"control", ModCtrl},   {"ctl", ModCtrl},      {"alt", ModAlt},
	    {"shift", ModShift},    {"super", ModSuper},    {"win", ModSuper},     {"windows", ModSuper},
	    {"meta", ModSuper},     {"mod4", ModSuper},     {"logo", ModSuper},    {"super_l", ModSuper},
	    {"super_r", ModSuper},  {"lwin", ModSuper},     {"rwin", ModSuper},    {"control_l", ModCtrl},
	    {"control_r", ModCtrl}, {"alt_l", ModAlt},      {"alt_r", ModAlt},     {"shift_l", ModShift},
	    {"shift_r", ModShift},
	};

	return modifiers.value(name.toLower(), 0);
}

bool isDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

UINT hotkeyModifiers(uint8_t mods) {
	UINT result = 0;
	if (mods & ModCtrl) result |= MOD_CONTROL;
	if (mods & ModAlt) result |= MOD_ALT;
	if (mods & ModShift) result |= MOD_SHIFT;
	if (mods & ModSuper) result |= MOD_WIN;
	return result;
}

QString mechanismName(Bind::Mechanism mechanism) {
	switch (mechanism) {
	case Bind::Mechanism::Hotkey: return "hotkey";
	case Bind::Mechanism::Hook: return "hook";
	case Bind::Mechanism::Native: return "native";
	case Bind::Mechanism::Unavailable: return "unavailable";
	default: return "none";
	}
}

QString actionName(Bind::Action action) {
	switch (action) {
	case Bind::Action::Global: return "global";
	case Bind::Action::Dispatch: return "dispatch";
	case Bind::Action::Exec: return "exec";
	case Bind::Action::Ipc: return "ipc";
	case Bind::Action::Key: return "key";
	case Bind::Action::Reload: return "reload";
	default: return "native";
	}
}

bool invokeWithString(QObject* object, const char* name, const QString& argument) {
	const auto* meta = object->metaObject();

	for (auto i = 0; i < meta->methodCount(); i++) {
		auto method = meta->method(i);
		if (method.name() != name || method.parameterCount() != 1) continue;

		switch (method.parameterMetaType(0).id()) {
		case QMetaType::QString:
			return method.invoke(object, Qt::DirectConnection, Q_ARG(QString, argument));
		case QMetaType::QVariant:
			return method.invoke(object, Qt::DirectConnection, Q_ARG(QVariant, QVariant(argument)));
		default:
			if (method.parameterMetaType(0) == QMetaType::fromType<QJSValue>()) {
				return method.invoke(object, Qt::DirectConnection, Q_ARG(QJSValue, QJSValue(argument)));
			}
		}
	}

	return false;
}

bool isConsoleProgram(const QString& program) {
	auto path = QFileInfo(program).isFile() ? program : QStandardPaths::findExecutable(program);
	if (path.isEmpty()) return false;

	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) return false;

	auto dos = file.read(0x40);
	if (dos.length() < 0x40 || !dos.startsWith("MZ")) return false;
	auto peOffset = qFromLittleEndian<quint32>(dos.constData() + 0x3c);

	if (!file.seek(peOffset)) return false;
	auto header = file.read(4 + 20 + 70);
	if (header.length() < 4 + 20 + 70 || !header.startsWith(QByteArray("PE\0\0", 4))) return false;

	auto subsystem = qFromLittleEndian<quint16>(header.constData() + 4 + 20 + 68);
	return subsystem == IMAGE_SUBSYSTEM_WINDOWS_CUI;
}

bool tryStartDetached(const QString& command) {
	auto parts = QProcess::splitCommand(command);
	if (parts.isEmpty()) return false;

	auto program = parts.takeFirst();
	if (!program.contains('/') && !program.contains('\\')) {
		auto local = QDir(QCoreApplication::applicationDirPath()).filePath(program);
		if (QFileInfo(local).isFile()) program = local;
	}

	auto process = QProcess();
	process.setProgram(program);
	process.setArguments(parts);
	process.setWorkingDirectory(QDir::homePath());

	if (isConsoleProgram(program)) {
		process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
			args->flags &= ~static_cast<DWORD>(CREATE_NO_WINDOW);
			args->flags |= CREATE_NEW_CONSOLE;
			args->inheritHandles = false;
			args->startupInfo->dwFlags &= ~static_cast<DWORD>(STARTF_USESTDHANDLES);
			args->startupInfo->hStdInput = nullptr;
			args->startupInfo->hStdOutput = nullptr;
			args->startupInfo->hStdError = nullptr;
		});
	} else {
		qs::io::process::disownHandles(&process);
	}

	if (process.startDetached()) return true;

	// File name only: the arguments and directories can hold user data.
	qCDebug(logHotkeys) << "Could not start" << QFileInfo(program).fileName() << ":"
	                    << process.errorString();
	return false;
}

HookTrigger::Kind triggerKind(const Bind& bind) {
	if (bind.combo.lone == 0) return HookTrigger::Combo;
	return bind.onRelease ? HookTrigger::Tap : HookTrigger::Hold;
}

QString bindBaseId(const QJsonObject& object) {
	auto action = object.value("action").toString();

	if (action == "global") {
		auto name = object.value("name").toString();
		auto appid = object.value("appid").toString("quickshell");
		if (auto colon = name.indexOf(':'); colon != -1) {
			appid = name.left(colon);
			name = name.mid(colon + 1);
		}
		return "global:" + appid + ":" + name;
	}

	if (action == "dispatch") return "dispatch:" + object.value("dispatch").toString();
	if (action == "exec") return "exec:" + object.value("command").toString();
	if (action == "key") return "key:" + object.value("key").toString();
	if (action == "native") return "native:" + object.value("keys").toString();
	if (action == "ipc") {
		return "ipc:" + object.value("target").toString() + ":" + object.value("function").toString();
	}

	return action;
}

QStringList defaultBindIds(const QJsonArray& binds) {
	auto counts = QHash<QString, int>();
	auto ids = QStringList();

	for (const auto& entry: binds) {
		auto id = bindBaseId(entry.toObject());
		counts[id]++;
		ids.append(id);
	}

	for (auto i = 0; i < ids.size(); i++) {
		if (counts.value(ids.at(i)) > 1) ids[i] += "@" + binds.at(i).toObject().value("keys").toString();
	}

	return ids;
}

QByteArray extendDefaults(const QJsonObject& user, const QByteArray& defaults) {
	auto defaultBinds = QJsonDocument::fromJson(defaults).object().value("binds").toArray();
	auto ids = defaultBindIds(defaultBinds);

	auto overrides = QHash<QString, QJsonObject>();
	auto added = QJsonArray();

	for (const auto& entry: user.value("binds").toArray()) {
		auto object = entry.toObject();
		auto base = object.value("base").toString();
		if (base.isEmpty()) added.append(object);
		else overrides.insert(base, object);
	}

	auto merged = QJsonArray();

	for (auto i = 0; i < defaultBinds.size(); i++) {
		auto object = defaultBinds.at(i).toObject();
		auto override = overrides.take(ids.at(i));

		auto keys = override.value("keys").toString();
		if (!keys.isEmpty()) object.insert("keys", keys);

		if (override.value("disabled").toBool()) {
			object = QJsonObject {
			    {"keys", object.value("keys")},
			    {"action", "native"},
			    {"disabled", object},
			};
		}

		merged.append(object);
	}

	for (const auto& base: overrides.keys()) {
		qCDebug(logHotkeys) << "Ignoring a change to" << base << "which is no longer a default bind";
	}

	for (const auto& entry: added) merged.append(entry);

	return QJsonDocument(QJsonObject {{"binds", merged}}).toJson(QJsonDocument::Compact);
}

bool parseMouseKeys(const QString& text, KeyCombo& combo) {
	combo = KeyCombo();
	auto buttons = 0;

	for (const auto& part: text.split('+')) {
		auto token = part.trimmed();

		if (auto bit = modifierFromName(token); bit != 0) {
			combo.mods |= bit;
		} else if (token.startsWith("mouse", Qt::CaseInsensitive)) {
			buttons++;
		} else {
			return false;
		}
	}

	return buttons == 1;
}

} // namespace

uint8_t keyNameToVk(const QString& name) {
	auto key = name.toLower();
	if (key.startsWith("xf86")) key = key.mid(4);

	if (key.length() == 1) {
		auto c = key.at(0).unicode();
		if (c >= 'a' && c <= 'z') return static_cast<uint8_t>(c - 'a' + 'A');
		if (c >= '0' && c <= '9') return static_cast<uint8_t>(c);
	}

	if (key.length() >= 2 && key.at(0) == 'f') {
		auto ok = false;
		auto n = key.mid(1).toInt(&ok);
		if (ok && n >= 1 && n <= 24) return static_cast<uint8_t>(VK_F1 + n - 1);
	}

	for (const auto* prefix: {"kp_", "numpad"}) {
		if (key.startsWith(prefix)) {
			auto ok = false;
			auto n = key.mid(static_cast<qsizetype>(qstrlen(prefix))).toInt(&ok);
			if (ok && n >= 0 && n <= 9) return static_cast<uint8_t>(VK_NUMPAD0 + n);
		}
	}

	if (key.startsWith("vk:")) key = key.mid(3);
	if (key.startsWith("0x")) {
		auto ok = false;
		auto vk = key.mid(2).toUInt(&ok, 16);
		if (ok && vk > 0 && vk < 0xFF) return static_cast<uint8_t>(vk);
	}

	static const QHash<QString, uint8_t> keys = {
	    {"return", VK_RETURN},
	    {"enter", VK_RETURN},
	    {"space", VK_SPACE},
	    {"tab", VK_TAB},
	    {"escape", VK_ESCAPE},
	    {"esc", VK_ESCAPE},
	    {"backspace", VK_BACK},
	    {"delete", VK_DELETE},
	    {"del", VK_DELETE},
	    {"insert", VK_INSERT},
	    {"ins", VK_INSERT},
	    {"home", VK_HOME},
	    {"end", VK_END},
	    {"pageup", VK_PRIOR},
	    {"page_up", VK_PRIOR},
	    {"prior", VK_PRIOR},
	    {"pagedown", VK_NEXT},
	    {"page_down", VK_NEXT},
	    {"next", VK_NEXT},
	    {"left", VK_LEFT},
	    {"right", VK_RIGHT},
	    {"up", VK_UP},
	    {"down", VK_DOWN},
	    {"print", VK_SNAPSHOT},
	    {"printscreen", VK_SNAPSHOT},
	    {"sysrq", VK_SNAPSHOT},
	    {"pause", VK_PAUSE},
	    {"scrolllock", VK_SCROLL},
	    {"scroll_lock", VK_SCROLL},
	    {"capslock", VK_CAPITAL},
	    {"caps_lock", VK_CAPITAL},
	    {"menu", VK_APPS},
	    {"apps", VK_APPS},
	    {"slash", VK_OEM_2},
	    {"period", VK_OEM_PERIOD},
	    {"comma", VK_OEM_COMMA},
	    {"minus", VK_OEM_MINUS},
	    {"equal", VK_OEM_PLUS},
	    {"plus", VK_OEM_PLUS},
	    {"bracketleft", VK_OEM_4},
	    {"bracketright", VK_OEM_6},
	    {"semicolon", VK_OEM_1},
	    {"apostrophe", VK_OEM_7},
	    {"grave", VK_OEM_3},
	    {"backslash", VK_OEM_5},
	    {"kp_add", VK_ADD},
	    {"kp_subtract", VK_SUBTRACT},
	    {"kp_multiply", VK_MULTIPLY},
	    {"kp_divide", VK_DIVIDE},
	    {"kp_decimal", VK_DECIMAL},
	    {"volumeup", VK_VOLUME_UP},
	    {"audioraisevolume", VK_VOLUME_UP},
	    {"volumedown", VK_VOLUME_DOWN},
	    {"audiolowervolume", VK_VOLUME_DOWN},
	    {"volumemute", VK_VOLUME_MUTE},
	    {"audiomute", VK_VOLUME_MUTE},
	    {"medianext", VK_MEDIA_NEXT_TRACK},
	    {"audionext", VK_MEDIA_NEXT_TRACK},
	    {"mediaprev", VK_MEDIA_PREV_TRACK},
	    {"audioprev", VK_MEDIA_PREV_TRACK},
	    {"mediaplaypause", VK_MEDIA_PLAY_PAUSE},
	    {"audioplay", VK_MEDIA_PLAY_PAUSE},
	    {"audiopause", VK_MEDIA_PLAY_PAUSE},
	    {"mediastop", VK_MEDIA_STOP},
	    {"audiostop", VK_MEDIA_STOP},
	    {"mail", VK_LAUNCH_MAIL},
	    {"calculator", VK_LAUNCH_APP2},
	};

	return keys.value(key, 0);
}

bool parseKeys(const QString& text, KeyCombo& combo, QString& error) {
	combo = KeyCombo();
	uint8_t mods = 0;
	uint8_t modCount = 0;

	for (const auto& part: text.split('+')) {
		auto token = part.trimmed();

		if (token.isEmpty()) {
			error = "empty key name (write \"Plus\" or \"Equal\" for the + key)";
			return false;
		}

		if (auto bit = modifierFromName(token); bit != 0) {
			if ((mods & bit) == 0) modCount++;
			mods |= bit;
			continue;
		}

		auto vk = keyNameToVk(token);

		if (auto ch = punctuationChar(token); !ch.isNull()) {
			auto needsShift = false;

			if (auto layoutVk = layoutVkForChar(ch, activeKeyboardLayout(), needsShift); layoutVk != 0) {
				vk = layoutVk;

				if (needsShift && (mods & ModShift) == 0) {
					mods |= ModShift;
					modCount++;
				}
			}
		}

		if (vk == 0) {
			error = QString("unknown key \"%1\"").arg(token);
			return false;
		}

		if (combo.vk != 0) {
			error = "more than one non-modifier key";
			return false;
		}

		combo.vk = vk;
	}

	if (combo.vk == 0) {
		if (modCount != 1) {
			error = "a combo of modifiers alone can't be bound, only a single modifier";
			return false;
		}

		combo.lone = mods;
		return true;
	}

	combo.mods = mods;

	if (combo.vk == VK_DELETE && (mods & ModCtrl) && (mods & ModAlt)) {
		error = "Ctrl+Alt+Delete belongs to Windows";
		return false;
	}

	return true;
}

HotkeyManager* HotkeyManager::instance() {
	static QPointer<HotkeyManager> manager; // NOLINT

	if (manager.isNull()) {
		manager = new HotkeyManager(QCoreApplication::instance());
	}

	return manager.data();
}

HotkeyManager::HotkeyManager(QObject* parent): QObject(parent) {
	WNDCLASSW wndClass {};
	wndClass.lpfnWndProc = &HotkeyManager::messageWindowProc;
	wndClass.hInstance = GetModuleHandleW(nullptr);
	wndClass.lpszClassName = MESSAGE_WINDOW_CLASS;
	RegisterClassW(&wndClass);

	this->messageWindow = CreateWindowExW(
	    0,
	    MESSAGE_WINDOW_CLASS,
	    L"",
	    0,
	    0,
	    0,
	    0,
	    0,
	    HWND_MESSAGE,
	    nullptr,
	    wndClass.hInstance,
	    nullptr
	);

	if (this->messageWindow == nullptr) {
		qCWarning(logHotkeys) << "Failed to create the hotkey message window, global shortcuts are "
		                         "unavailable.";
	}

	this->reloadTimer.setSingleShot(true);
	this->reloadTimer.setInterval(200);
	QObject::connect(&this->reloadTimer, &QTimer::timeout, this, &HotkeyManager::loadFile);

	auto* layouts = sys::KeyboardLayoutWatcher::instance();
	this->lastLayout = activeKeyboardLayout();
	QObject::connect(layouts, &sys::KeyboardLayoutWatcher::currentChanged, this, [this, layouts] {
		auto* layout = layouts->current();
		if (layout == this->lastLayout) return;
		this->lastLayout = layout;
		if (this->loaded) this->relayout();
	});

	this->releaseTimer.setInterval(RELEASE_POLL_MS);
	QObject::connect(&this->releaseTimer, &QTimer::timeout, this, &HotkeyManager::pollReleases);

	QObject::connect(&this->watcher, &QFileSystemWatcher::fileChanged, this, &HotkeyManager::scheduleReload);
	QObject::connect(&this->watcher, &QFileSystemWatcher::directoryChanged, this, &HotkeyManager::scheduleReload);

	QObject::connect(InputMaskTracker::instance(), &InputMaskTracker::buttonPressed, this, [this] {
		if (this->inputSettle.pending()) this->finishInputSettle(false);
	});
	QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] {
		this->unregisterAll();
		KeyboardHook::stop();
		this->hookRunning = false;
	});
}

HotkeyManager::~HotkeyManager() {
	this->unregisterAll();
	KeyboardHook::stop();

	if (this->messageWindow != nullptr) {
		DestroyWindow(this->messageWindow);
		this->messageWindow = nullptr;
	}
}

void HotkeyManager::setShellDir(const QString& shellDir) {
	if (shellDir.isEmpty() || QDir::isRelativePath(shellDir)) return;
	// A new QML generation must not inherit the previous generation's mode.
	(void)this->setSubmap(QStringLiteral("reset"));
	if (this->loaded && shellDir == this->shellDir) return;

	this->shellDir = shellDir;
	this->loaded = true;
	this->parsed = false;
	this->loadFile();
}

void HotkeyManager::reload() {
	this->parsed = false;
	this->loadFile();
}

bool HotkeyManager::setSubmap(const QString& name, QString* refusal) {
	// Hyprland v0.54.0 KeybindManager::setSubmap: empty/reset is global,
	// otherwise the name must occur on a configured bind. No implicit Escape.
	const auto target = name == QStringLiteral("reset") ? QString() : name;
	if (!target.isEmpty() && !std::ranges::any_of(this->binds, [&](const Bind& bind) {
		    return bind.submap == target;
	    }))
	{
		if (refusal != nullptr) *refusal = QStringLiteral("invalid-target");
		return false;
	}
	if (target == this->mActiveSubmap) return true;

	this->unregisterAll(false);
	this->mActiveSubmap = target;
	this->registerAll();
	if (!target.isEmpty() && !this->hookRunning) {
		// Without the hook's desktop-switch event, even registered-only maps
		// could survive a session lock. Refuse entry and restore global routing.
		this->unregisterAll();
		this->registerAll();
		if (refusal != nullptr) *refusal = QStringLiteral("operation-unsupported");
		emit this->bindsChanged();
		return false;
	}
	emit this->activeSubmapChanged();
	emit this->bindsChanged();
	return true;
}

QString HotkeyManager::userFilePath() const {
	// A shell can override the keybinds file without editing its installed
	// defaults. The explicit environment path wins so an isolated prototype can
	// point at its own profile-local file.
	auto override = qEnvironmentVariable("CAELESTIA_WINDOWS_KEYBINDS");
	if (!override.isEmpty()) return override;

	if (this->userFile.isEmpty()) return QString();
	if (!QDir::isRelativePath(this->userFile)) return this->userFile;

	// Caelestia and the isolated Windows prototypes expose CAELESTIA_CONFIG_DIR
	// as the shell configuration directory. Prefer a keybinds override living
	// there so the same directory that holds shell.json holds the user map,
	// then fall back to the existing per-user data location.
	auto configDir = qEnvironmentVariable("CAELESTIA_CONFIG_DIR");
	if (!configDir.isEmpty() && !this->configFile.isEmpty()
	    && QDir::isRelativePath(this->configFile))
	{
		auto candidate = QDir(configDir).filePath(this->configFile);
		if (QFileInfo::exists(candidate)) return candidate;
	}

	auto base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
	if (base.isEmpty()) return QString();
	return base + '/' + this->userFile;
}

void HotkeyManager::readDefaultFile() {
	this->userFile.clear();
	this->configFile.clear();

	auto file = QFile(this->defaultFilePath());
	if (this->defaultFilePath().isEmpty() || !file.open(QFile::ReadOnly)) return;

	auto defaults = QJsonDocument::fromJson(file.readAll()).object();
	this->userFile = defaults.value("userFile").toString().trimmed();
	this->configFile = defaults.value("configFile").toString().trimmed();
}

QString HotkeyManager::defaultFilePath() const {
	if (this->shellDir.isEmpty()) return QString();
	return this->shellDir + '/' + DEFAULT_FILE;
}

void HotkeyManager::scheduleReload() { this->reloadTimer.start(); }

void HotkeyManager::loadFile() {
	this->readDefaultFile();
	auto path = this->userFilePath();
	if (path.isEmpty() || !QFileInfo::exists(path)) path = this->defaultFilePath();

	QByteArray data;
	auto file = QFile(path);
	if (path.isEmpty() || !file.open(QFile::ReadOnly)) path.clear();
	else data = file.readAll();

	auto extends = false;
	if (!path.isEmpty() && path == this->userFilePath()) {
		auto user = QJsonDocument::fromJson(data).object();
		if (user.value("extends").toString() == "defaults") {
			auto defaults = QFile(this->defaultFilePath());
			if (defaults.open(QFile::ReadOnly)) {
				data = extendDefaults(user, defaults.readAll());
				extends = true;
			}
		}
	}

	if (!this->parsed || path != this->mConfigPath || data != this->loadedData) {
		if (path.isEmpty()) {
			auto looked = QStringList {this->userFilePath(), this->defaultFilePath()};
			looked.removeAll(QString());
			qCWarning(logHotkeys).noquote() << "No keybinds file found (looked for"
			                                << qs::windows::logPath(looked.join(" and "))
			                                << "), global shortcuts are disabled.";
		}

		this->parsed = true;
		this->mConfigPath = path;
		this->loadedData = data;

		this->unregisterAll();
		this->parse(data, path);
		if (!extends && !path.isEmpty() && path == this->userFilePath()) this->mergeDefaults();
		this->registerAll();
		emit this->bindsChanged();
	}

	this->updateWatches();
}

void HotkeyManager::mergeDefaults() {
	auto defaultPath = this->defaultFilePath();
	auto file = QFile(defaultPath);
	if (defaultPath.isEmpty() || !file.open(QFile::ReadOnly)) return;

	auto data = file.readAll();
	auto introduced = introducedDefaults(QJsonDocument::fromJson(data).object().value("binds").toArray());
	if (introduced.isEmpty()) return;

	auto userBinds = this->binds;
	this->parse(data, defaultPath);
	auto defaults = this->binds;
	this->binds = userBinds;

	hotkeys::mergeDefaults(this->binds, defaults, introduced);
	for (auto i = userBinds.size(); i < this->binds.size(); i++) {
		const auto& bind = this->binds.at(i);
		qCInfo(logHotkeys) << "Adding default bind" << bind.keys << "for" << bind.name
		                   << "missing from the user keybinds file";
	}
}

void HotkeyManager::updateWatches() {
	auto userPath = this->userFilePath();

	QStringList wanted;
	if (!this->mConfigPath.isEmpty()) wanted.append(this->mConfigPath);
	auto defaultPath = this->defaultFilePath();
	if (!defaultPath.isEmpty() && !wanted.contains(defaultPath)) wanted.append(defaultPath);
	if (!userPath.isEmpty()) {
		auto userDir = QFileInfo(userPath).absolutePath();
		if (QFileInfo::exists(userDir)) wanted.append(userDir);
		else wanted.append(QFileInfo(userDir).absolutePath());
	}

	// Watch the shell configuration directory even when the override does not
	// exist yet, so creating it applies the user map without a restart.
	auto configDir = qEnvironmentVariable("CAELESTIA_CONFIG_DIR");
	if (!configDir.isEmpty() && !this->configFile.isEmpty()
	    && QDir::isRelativePath(this->configFile) && QFileInfo::exists(configDir))
	{
		wanted.append(QDir(configDir).absolutePath());
	}

	auto current = this->watcher.files() + this->watcher.directories();
	for (const auto& path: current) {
		if (!wanted.contains(path)) this->watcher.removePath(path);
	}

	for (const auto& path: wanted) {
		if (!current.contains(path) && QFileInfo::exists(path)) this->watcher.addPath(path);
	}
}

void HotkeyManager::parse(const QByteArray& data, const QString& path) {
	this->binds.clear();
	if (data.isEmpty()) return;

	QJsonParseError parseError {};
	auto document = QJsonDocument::fromJson(data, &parseError);
	if (document.isNull()) {
		qCWarning(logHotkeys) << "Failed to parse" << qs::windows::logPath(path) << ":"
		                      << parseError.errorString() << "at offset"
		                      << parseError.offset;
		return;
	}

	auto list = document.object().value("binds").toArray();

	for (auto i = 0; i < list.size(); i++) {
		auto object = list.at(i).toObject();
		auto bind = Bind();
		auto where = QString("%1 bind %2").arg(path).arg(i);

		bind.keys = object.value("keys").toString();
		bind.submap = object.value("submap").toString();
		if (bind.submap == QStringLiteral("reset")) bind.submap.clear();
		bind.description = object.value("description").toString();
		bind.onRelease = object.value("onRelease").toBool();
		bind.repeat = object.value("repeat").toBool();
		bind.preferRegister = object.value("register").toBool();

		auto action = object.value("action").toString();

		QString error;
		if (!parseKeys(bind.keys, bind.combo, error)
		    && (action != "native" || !parseMouseKeys(bind.keys, bind.combo)))
		{
			qCWarning(logHotkeys).noquote() << where << "(" << bind.keys << "):" << error;
			continue;
		}

		if (action == "global") {
			bind.action = Bind::Action::Global;
			parseGlobalName(object, bind);

			if (bind.name.isEmpty()) {
				qCWarning(logHotkeys).noquote() << where << ": global bind without a name";
				continue;
			}
		} else if (action == "dispatch") {
			bind.action = Bind::Action::Dispatch;
			bind.argument = object.value("dispatch").toString();
		} else if (action == "exec") {
			bind.action = Bind::Action::Exec;
			bind.argument = object.value("command").toString();
		} else if (action == "ipc") {
			bind.action = Bind::Action::Ipc;
			bind.name = object.value("target").toString();
			bind.function = object.value("function").toString();

			for (const auto& arg: object.value("args").toArray()) {
				bind.args.append(arg.isString() ? arg.toString() : arg.toVariant().toString());
			}

			if (bind.name.isEmpty() || bind.function.isEmpty()) {
				qCWarning(logHotkeys).noquote() << where << ": ipc bind needs a target and a function";
				continue;
			}
		} else if (action == "key") {
			bind.action = Bind::Action::Key;
			bind.sendVk = keyNameToVk(object.value("key").toString());

			if (bind.sendVk == 0) {
				qCWarning(logHotkeys).noquote() << where << ": unknown key" << object.value("key").toString();
				continue;
			}
		} else if (action == "reload") {
			bind.action = Bind::Action::Reload;
		} else if (action == "native") {
			bind.action = Bind::Action::Native;
			bind.mechanism = Bind::Mechanism::Native;

			auto disabled = object.value("disabled").toObject();
			if (disabled.value("action").toString() == "global") parseGlobalName(disabled, bind);
		} else {
			qCWarning(logHotkeys).noquote() << where << ": unknown action" << action;
			continue;
		}

		if ((bind.action == Bind::Action::Dispatch || bind.action == Bind::Action::Exec)
		    && bind.argument.isEmpty())
		{
			qCWarning(logHotkeys).noquote() << where << ": empty" << action;
			continue;
		}

		this->binds.append(bind);
	}

	qCInfo(logHotkeys) << "Loaded" << this->binds.size() << "binds from"
	                   << qs::windows::logPath(path);
}

void HotkeyManager::unregisterAll(bool resetSubmap) {
	if (this->gateActive) this->finishInputSettle(false);

	this->releaseAllGlobals();
	this->releaseTimer.stop();

	for (auto i = 0; i < this->triggers.size(); i++) {
		if (this->triggers.at(i).mechanism == Bind::Mechanism::Hotkey) {
			UnregisterHotKey(this->messageWindow, static_cast<int>(i + 1));
		}
	}

	this->triggers.clear();

	this->serial++;
	KeyboardHook::setSnapshot(nullptr);
	if (resetSubmap && !this->mActiveSubmap.isEmpty()) {
		this->mActiveSubmap.clear();
		emit this->activeSubmapChanged();
	}
}

qsizetype HotkeyManager::addToTrigger(qsizetype index) {
	const auto& bind = this->binds.at(index);
	auto kind = triggerKind(bind);

	for (auto i = 0; i < this->triggers.size(); i++) {
		auto& trigger = this->triggers[i];
		if (trigger.kind != kind || trigger.combo != bind.combo) continue;

		trigger.binds.append(index);
		trigger.repeat |= bind.repeat;
		trigger.preferRegister |= bind.preferRegister;
		return i;
	}

	this->triggers.append(Trigger {
	    .kind = kind,
	    .combo = bind.combo,
	    .binds = {index},
	    .repeat = bind.repeat,
	    .preferRegister = bind.preferRegister,
	});

	return this->triggers.size() - 1;
}

void HotkeyManager::registerAll() {
	if (this->messageWindow == nullptr) return;

	for (auto i = 0; i < this->binds.size(); i++) {
		auto& bind = this->binds[i];
		if (bind.action == Bind::Action::Native) continue;
		bind.mechanism = Bind::Mechanism::None;
		if (bind.submap == this->mActiveSubmap) this->addToTrigger(i);
	}

	this->registerTriggers();
}

void HotkeyManager::resetTrigger(qsizetype index) {
	auto& trigger = this->triggers[index];

	if (trigger.mechanism == Bind::Mechanism::Hotkey) {
		UnregisterHotKey(this->messageWindow, static_cast<int>(index + 1));
	}

	trigger.mechanism = Bind::Mechanism::None;
	if (trigger.binds.isEmpty()) trigger.down = false;
	trigger.repeat = std::ranges::any_of(trigger.binds, [this](qsizetype bind) {
		return this->binds.at(bind).repeat;
	});
	trigger.preferRegister = std::ranges::any_of(trigger.binds, [this](qsizetype bind) {
		return this->binds.at(bind).preferRegister;
	});
}

void HotkeyManager::relayout() {
	auto moved = QList<qsizetype>();

	for (auto i = 0; i < this->binds.size(); i++) {
		auto& bind = this->binds[i];
		if (!dependsOnLayout(bind.keys)) continue;

		auto combo = KeyCombo();
		auto error = QString();
		if (!parseKeys(bind.keys, combo, error) || combo == bind.combo) continue;

		bind.combo = combo;
		if (bind.action != Bind::Action::Native && bind.submap == this->mActiveSubmap) moved.append(i);
	}

	if (moved.isEmpty() || this->messageWindow == nullptr) return;

	auto released = QList<QPair<QString, QString>>();

	for (auto index: moved) {
		const auto& bind = this->binds.at(index);
		auto key = qMakePair(bind.appid, bind.name);
		if (bind.action == Bind::Action::Global && this->pressedGlobals.contains(key)) {
			released.append(key);
		}

		for (auto i = 0; i < this->triggers.size(); i++) {
			if (this->triggers[i].binds.removeAll(index) != 0) this->resetTrigger(i);
		}
	}

	for (auto index: moved) this->resetTrigger(this->addToTrigger(index));

	this->registerTriggers();

	for (const auto& [appid, name]: released) this->emitGlobal(appid, name, false);
	emit this->bindsChanged();
}

void HotkeyManager::registerTriggers() {
	auto snapshot = std::make_shared<HookSnapshot>();
	snapshot->serial = this->serial;

	for (auto i = 0; i < this->triggers.size(); i++) {
		auto& trigger = this->triggers[i];
		if (trigger.binds.isEmpty()) continue;

		if (trigger.mechanism == Bind::Mechanism::Hotkey) {
			if ((trigger.combo.mods & ModSuper) != 0) snapshot->masks.push_back(trigger.combo);
			continue;
		}

		auto registered = false;

		if (trigger.preferRegister && trigger.kind == HookTrigger::Combo
		    && supervisorOwnsChord(trigger.combo.mods, trigger.combo.vk))
		{
			qCInfo(logHotkeys) << "Ignoring register for" << this->binds.at(trigger.binds.first()).keys
			                   << "because the desktop supervisor consumes this chord, using the keyboard hook";
		}

		if (shouldRegisterHotkey(
		        trigger.kind,
		        trigger.combo.mods,
		        trigger.combo.vk,
		        trigger.preferRegister
		    ))
		{
			auto modifiers = hotkeyModifiers(trigger.combo.mods) | (trigger.repeat ? 0 : MOD_NOREPEAT);
			auto id = static_cast<int>(i + 1);

			registered = RegisterHotKey(this->messageWindow, id, modifiers, trigger.combo.vk) != FALSE;

			if (!registered) {
				qCInfo(logHotkeys) << "RegisterHotKey failed for" << this->binds.at(trigger.binds.first()).keys
				                   << "(error" << GetLastError() << "), using the keyboard hook";
			}
		}

		auto mechanism = resolveTriggerMechanism(trigger.kind, trigger.combo.mods, registered);

		if (mechanism.hotkey) trigger.mechanism = Bind::Mechanism::Hotkey;

		if (mechanism.hook) {
			trigger.mechanism = Bind::Mechanism::Hook;
			snapshot->triggers.push_back(HookTrigger {
			    .kind = trigger.kind,
			    .combo = trigger.combo,
			    .id = static_cast<uint32_t>(i),
			});
		}

		if (mechanism.mask) snapshot->masks.push_back(trigger.combo);
	}

	// An active map also needs the existing hook's desktop-switch notification,
	// even when every key in that map was accepted by RegisterHotKey.
	if (snapshot->triggers.empty() && snapshot->masks.empty() && !this->gateActive
	    && this->mActiveSubmap.isEmpty()) {
		KeyboardHook::stop();
		this->hookRunning = false;
	} else {
		KeyboardHook::setSnapshot(snapshot);

		if (!this->hookRunning) {
			this->hookRunning = KeyboardHook::start(this->messageWindow, WM_QS_HOOK_BASE);
			if (this->hookRunning) qCInfo(logHotkeys) << "Keyboard hook installed.";
		}

		if (!this->hookRunning) {
			qCWarning(logHotkeys) << "Failed to install the keyboard hook (error" << GetLastError()
			                      << "), hook-backed binds are unavailable.";

			for (auto& trigger: this->triggers) {
				if (trigger.mechanism == Bind::Mechanism::Hook) {
					trigger.mechanism = Bind::Mechanism::Unavailable;
				}
			}
		}
	}

	for (const auto& trigger: this->triggers) {
		for (auto index: trigger.binds) this->binds[index].mechanism = trigger.mechanism;
	}
}

bool HotkeyManager::hookActive() const { return this->hookRunning; }

QVariantList HotkeyManager::bindsInfo() const {
	QVariantList list;

	for (const auto& bind: this->binds) {
		QVariantMap map;
		map["keys"] = bind.keys;
		map["submap"] = bind.submap;
		map["action"] = actionName(bind.action);
		map["mechanism"] = mechanismName(bind.mechanism);
		map["description"] = bind.description;
		map["onRelease"] = bind.onRelease;
		map["repeat"] = bind.repeat;
		map["register"] = bind.preferRegister;

		switch (bind.action) {
		case Bind::Action::Global: map["name"] = bind.appid + ':' + bind.name; break;
		case Bind::Action::Dispatch: map["dispatch"] = bind.argument; break;
		case Bind::Action::Exec: map["command"] = bind.argument; break;
		case Bind::Action::Ipc:
			map["target"] = bind.name;
			map["function"] = bind.function;
			map["args"] = bind.args;
			break;
		default: break;
		}

		list.append(map);
	}

	return list;
}

void HotkeyManager::registerShortcut(QObject* shortcut) {
	if (shortcut == nullptr) return;
	this->shortcuts.removeIf([](const QPointer<QObject>& entry) { return entry.isNull(); });
	if (!this->shortcuts.contains(shortcut)) this->shortcuts.append(shortcut);
}

void HotkeyManager::unregisterShortcut(QObject* shortcut) {
	this->shortcuts.removeIf([shortcut](const QPointer<QObject>& entry) {
		return entry.isNull() || entry == shortcut;
	});
}

bool HotkeyManager::beginChordReleaseWait() {
	if (this->messageWindow == nullptr) return false;

	switch (classifyChordRequest(this->dispatching, this->gateActive)) {
	case ChordRequest::SendNow:
		// A direct IPC request is not tied to the held chord. It supersedes a
		// pending keybind wait instead of being rejected by it.
		if (this->gateActive) this->finishInputSettle(false);
		return false;
	case ChordRequest::AlreadyWaiting: return true;
	case ChordRequest::Wait: break;
	}

	// Every bind may be a RegisterHotKey chord, in which case the hook was
	// stopped. Start it for this wait only, with no stale trigger snapshot; it
	// is stopped again when the wait finishes unless a bind needs it.
	auto startedHook = false;
	if (!KeyboardHook::isRunning()) {
		KeyboardHook::setSnapshot(nullptr);
		if (!KeyboardHook::start(this->messageWindow, WM_QS_HOOK_BASE)) {
			qCWarning(logHotkeys) << "Keyboard hook unavailable, display off can't wait for the chord "
			                         "release.";
			return false;
		}
		startedHook = true;
	}

	// The hook thread takes the snapshot and subscribes in one step, so a
	// release cannot land between the two and leave the wait armed forever.
	std::vector<uint8_t> keys;
	if (!KeyboardHook::armInputObserver(
	        this->messageWindow,
	        WM_QS_INPUT_KEY,
	        WM_QS_INPUT_RESET,
	        keys
	    ))
	{
		if (startedHook) {
			KeyboardHook::stop();
			KeyboardHook::setSnapshot(nullptr);
		}
		qCWarning(logHotkeys) << "Keyboard hook did not answer, display off can't wait for the chord "
		                         "release.";
		return false;
	}

	auto result = this->inputSettle.arm(keys);
	if (result != InputSettle::Result::Waiting) {
		KeyboardHook::clearInputObserver();
		if (startedHook) {
			KeyboardHook::stop();
			KeyboardHook::setSnapshot(nullptr);
		}
		return false;
	}

	if (!this->gateButtonEvents) {
		InputMaskTracker::instance()->acquireButtonEvents();
		this->gateButtonEvents = true;
	}

	this->gateActive = true;
	return true;
}

void HotkeyManager::onPhysicalKey(quint8 vk, bool up) {
	if (!this->gateActive) return;

	auto result = up ? this->inputSettle.release(vk) : this->inputSettle.press(vk);
	if (result == InputSettle::Result::Settled) this->finishInputSettle(true);
	else if (result == InputSettle::Result::Cancelled) this->finishInputSettle(false);
}

void HotkeyManager::onInputLifecycleLost() {
	// A lock screen, UAC or switch user takes every hold away. The releases
	// will never arrive, so the wait must not stay armed.
	if (this->gateActive) this->finishInputSettle(false);
	this->releaseAllGlobals();
	(void)this->setSubmap(QStringLiteral("reset"));
}

void HotkeyManager::finishInputSettle(bool settled) {
	if (!this->gateActive) return;

	this->gateActive = false;
	this->inputSettle.cancel();
	KeyboardHook::clearInputObserver();

	if (this->gateButtonEvents) {
		InputMaskTracker::instance()->releaseButtonEvents();
		this->gateButtonEvents = false;
	}

	if (!this->hookRunning && KeyboardHook::isRunning()) {
		KeyboardHook::stop();
		KeyboardHook::setSnapshot(nullptr);
	}

	if (settled) emit this->chordReleaseSettled();
	else emit this->chordReleaseCancelled();
}

void HotkeyManager::triggerGlobal(const QString& name) {
	auto appid = QString("quickshell");
	auto shortcut = name.trimmed();

	if (auto colon = shortcut.indexOf(':'); colon != -1) {
		appid = shortcut.left(colon);
		shortcut = shortcut.mid(colon + 1);
	}

	this->emitGlobal(appid, shortcut, true);
	this->emitGlobal(appid, shortcut, false);
}

LRESULT CALLBACK HotkeyManager::messageWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_HOTKEY) {
		HotkeyManager::instance()->onHotkey(static_cast<int>(wParam));
		return 0;
	}

	if (msg == WM_QS_INPUT_KEY) {
		HotkeyManager::instance()->onPhysicalKey(
		    static_cast<quint8>(wParam),
		    static_cast<LPARAM>(lParam) != 0
		);
		return 0;
	}

	if (msg == WM_QS_INPUT_RESET) {
		HotkeyManager::instance()->onInputLifecycleLost();
		return 0;
	}

	if (msg >= WM_QS_HOOK_BASE && msg < WM_QS_HOOK_BASE + HookEventCount) {
		HotkeyManager::instance()->onHookEvent(
		    static_cast<HookEvent>(msg - WM_QS_HOOK_BASE),
		    static_cast<quint32>(wParam),
		    static_cast<quint32>(lParam)
		);

		return 0;
	}

	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void HotkeyManager::onHotkey(int id) {
	auto index = static_cast<qsizetype>(id) - 1;
	if (index < 0 || index >= this->triggers.size()) return;

	auto& trigger = this->triggers[index];
	if (trigger.mechanism != Bind::Mechanism::Hotkey) return;

	// WM_HOTKEY is a key Windows delivered to this process, so the activation
	// it causes may ask for the foreground once under the documented rule.
	auto scope = ActivationDispatchScope(ActivationSource::Hotkey);

	if (trigger.down) {
		this->fire(index, Phase::Repeat);
		return;
	}

	trigger.down = true;
	this->fire(index, Phase::Press);
	if (!this->releaseTimer.isActive()) this->releaseTimer.start();
}

void HotkeyManager::pollReleases() {
	auto anyDown = false;

	for (auto i = 0; i < this->triggers.size(); i++) {
		auto& trigger = this->triggers[i];
		if (!trigger.down || trigger.mechanism != Bind::Mechanism::Hotkey) continue;

		if (isDown(trigger.combo.vk)) {
			anyDown = true;
			continue;
		}

		trigger.down = false;
		// A registered hotkey's release is still part of the same input event.
		auto scope = ActivationDispatchScope(ActivationSource::Hotkey);
		this->fire(i, Phase::Release);
	}

	if (!anyDown) this->releaseTimer.stop();
}

void HotkeyManager::onHookEvent(HookEvent event, quint32 triggerId, quint32 serial) {
	if (event == HookReset) {
		if (serial == this->serial) this->onInputLifecycleLost();
		return;
	}
	if (serial != this->serial || triggerId >= static_cast<quint32>(this->triggers.size())) return;

	auto index = static_cast<qsizetype>(triggerId);

	// The WH_KEYBOARD_LL hook swallowed this key, so Windows did not deliver it
	// to the shell. Activation from here has no documented foreground right and
	// is the only source allowed the accepted escalation.
	auto scope = ActivationDispatchScope(ActivationSource::Hook);

	switch (event) {
	case HookPressed: this->fire(index, Phase::Press); break;
	case HookRepeated: this->fire(index, Phase::Repeat); break;
	case HookReleased: this->fire(index, Phase::Release); break;
	case HookTapped: this->fire(index, Phase::Tap); break;
	default: break;
	}
}

void HotkeyManager::fire(qsizetype trigger, Phase phase) {
	auto indexes = this->triggers.at(trigger).binds;
	auto binds = QList<Bind>();
	for (auto index: indexes) binds.append(this->binds.at(index));

	for (const auto& bind: binds) this->fireBind(bind, phase);
}

void HotkeyManager::fireBind(const Bind& bind, Phase phase) {
	qCDebug(logHotkeys) << "Bind" << bind.keys << actionName(bind.action) << "phase"
	                    << static_cast<int>(phase);

	if (bind.action == Bind::Action::Global) {
		if (phase == Phase::Tap || (bind.onRelease && phase == Phase::Release)) {
			this->emitGlobal(bind.appid, bind.name, true);
			this->emitGlobal(bind.appid, bind.name, false);
		} else if (!bind.onRelease) {
			if (phase == Phase::Press || (phase == Phase::Repeat && bind.repeat)) {
				this->emitGlobal(bind.appid, bind.name, true);
			} else if (phase == Phase::Release) {
				this->emitGlobal(bind.appid, bind.name, false);
			}
		}

		return;
	}

	auto run = false;
	if (phase == Phase::Tap) run = true;
	else if (bind.onRelease) run = phase == Phase::Release;
	else run = phase == Phase::Press || (phase == Phase::Repeat && bind.repeat);

	if (!run) return;

	// An action's IPC function runs synchronously here. The flag lets it ask
	// for the triggering chord's release instead of guessing from key state; an
	// external IPC call leaves it false and runs at once.
	auto wasDispatching = this->dispatching;
	this->dispatching = true;

	switch (bind.action) {
	case Bind::Action::Dispatch: this->runDispatch(bind); break;
	case Bind::Action::Exec: this->runExec(bind); break;
	case Bind::Action::Ipc: this->runIpc(bind); break;
	case Bind::Action::Key: KeyboardHook::sendKey(bind.sendVk); break;
	case Bind::Action::Reload: this->runReload(); break;
	default: break;
	}

	this->dispatching = wasDispatching;
}

void HotkeyManager::emitGlobal(const QString& appid, const QString& name, bool pressed) {
	auto key = qMakePair(appid, name);

	if (pressed) {
		if (!this->pressedGlobals.contains(key)) this->pressedGlobals.append(key);
	} else {
		this->pressedGlobals.removeAll(key);
	}

	auto delivered = false;

	auto shortcuts = this->shortcuts;
	for (const auto& shortcut: shortcuts) {
		if (shortcut.isNull()) continue;
		if (shortcut->property("appid").toString() != appid) continue;
		if (shortcut->property("name").toString() != name) continue;

		QMetaObject::invokeMethod(shortcut.data(), pressed ? "pressed" : "released", Qt::DirectConnection);
		delivered = true;
	}

	if (!delivered && pressed) {
		qCInfo(logHotkeys).noquote() << "No GlobalShortcut is listening for"
		                             << qs::windows::logId(appid + ':' + name);
	}
}

void HotkeyManager::releaseAllGlobals() {
	auto pressed = this->pressedGlobals;
	for (const auto& [appid, name]: pressed) this->emitGlobal(appid, name, false);

	for (auto& trigger: this->triggers) trigger.down = false;
}

void HotkeyManager::runDispatch(const Bind& bind) {
	auto* generation = EngineGeneration::currentGeneration();
	if (generation == nullptr || generation->engine == nullptr) {
		qCWarning(logHotkeys) << "No shell generation to dispatch"
		                      << qs::windows::logId(bind.argument) << "to.";
		return;
	}

	auto* hyprland = generation->engine->singletonInstance<QObject*>("Quickshell.Hyprland", "Hyprland");

	if (hyprland == nullptr) {
		qCWarning(logHotkeys) << "Quickshell.Hyprland is unavailable, can't dispatch"
		                      << qs::windows::logId(bind.argument);
		return;
	}

	if (!invokeWithString(hyprland, "dispatch", bind.argument)) {
		qCWarning(logHotkeys) << "Hyprland.dispatch(" << qs::windows::logId(bind.argument)
		                      << ") could not be called.";
	}
}

void HotkeyManager::runExec(const Bind& bind) const {
	auto commands = QStringList();

	for (const auto& alternative: bind.argument.split(QStringLiteral(" || "))) {
		auto command = alternative;
		command.replace("%SHELL%", this->shellDir, Qt::CaseInsensitive);

		auto source = command.toStdWString();
		auto size = ExpandEnvironmentStringsW(source.c_str(), nullptr, 0);
		if (size > 0) {
			auto buffer = std::wstring(size, L'\0');
			ExpandEnvironmentStringsW(source.c_str(), buffer.data(), size);
			command = QString::fromWCharArray(buffer.c_str());
		}

		commands.append(command);
	}

	runLauncher([commands, argument = bind.argument]() {
		for (const auto& command: commands) {
			if (tryStartDetached(command)) return;
		}

		qCWarning(logHotkeys) << "Failed to start any alternative of"
		                      << qs::windows::logId(argument);
	});
}

void HotkeyManager::runIpc(const Bind& bind) {
	using io::ipc::IpcCallStorage;
	using io::ipc::IpcHandlerRegistry;

	auto* generation = EngineGeneration::currentGeneration();
	if (generation == nullptr) {
		qCWarning(logHotkeys) << "No shell generation for ipc call" << bind.name << bind.function;
		return;
	}

	auto* handler = IpcHandlerRegistry::forGeneration(generation)->findHandler(bind.name);
	if (handler == nullptr) {
		qCWarning(logHotkeys) << "No IpcHandler with target" << bind.name;
		return;
	}

	auto* function = handler->findFunction(bind.function);
	if (function == nullptr) {
		qCWarning(logHotkeys) << "IpcHandler" << bind.name << "has no function" << bind.function
		                      << "(functions need typed arguments and return type)";
		return;
	}

	if (function->argumentTypes.length() != bind.args.length()) {
		qCWarning(logHotkeys) << "Ipc function" << function->toString() << "takes"
		                      << function->argumentTypes.length() << "arguments, got"
		                      << bind.args.size();
		return;
	}

	auto storage = IpcCallStorage(*function);
	for (auto i = 0; i < bind.args.length(); i++) {
		if (!storage.setArgumentStr(i, bind.args.at(i))) {
			qCWarning(logHotkeys) << "Invalid argument" << i << "for" << function->toString();
			return;
		}
	}

	function->invoke(handler, storage);
}

void HotkeyManager::runReload() {
	(void)this->setSubmap(QStringLiteral("reset"));
	auto* generation = EngineGeneration::currentGeneration();
	if (generation == nullptr || generation->wrapper == nullptr) return;

	auto* wrapper = generation->wrapper;
	QTimer::singleShot(0, wrapper, [wrapper]() { wrapper->reloadGraph(true); });
}

Hotkeys::Hotkeys(QObject* parent): QObject(parent) {
	QObject::connect(HotkeyManager::instance(), &HotkeyManager::bindsChanged, this, &Hotkeys::bindsChanged);
	QObject::connect(
	    HotkeyManager::instance(), &HotkeyManager::activeSubmapChanged,
	    this, &Hotkeys::activeSubmapChanged
	);
	QObject::connect(
	    HotkeyManager::instance(),
	    &HotkeyManager::chordReleaseSettled,
	    this,
	    &Hotkeys::chordReleaseSettled
	);
	QObject::connect(
	    HotkeyManager::instance(),
	    &HotkeyManager::chordReleaseCancelled,
	    this,
	    &Hotkeys::chordReleaseCancelled
	);
}

Hotkeys* Hotkeys::create(QQmlEngine* engine, QJSEngine* /*jsEngine*/) {
	auto* hotkeys = new Hotkeys(nullptr);

	auto entry = QFileInfo(InstanceInfo::CURRENT.configPath).fileName();
	if (!entry.isEmpty() && entry.compare("shell.qml", Qt::CaseInsensitive) != 0) {
		qCInfo(logHotkeys) << "Not loading binds in" << qs::windows::logId(entry)
		                   << "(only a config's shell.qml does)";
		return hotkeys;
	}

	if (auto* generation = EngineGeneration::findEngineGeneration(engine)) {
		auto* manager = HotkeyManager::instance();
		manager->setShellDir(generation->rootPath.path());
		QObject::connect(engine, &QObject::destroyed, manager, [manager] {
			(void)manager->setSubmap(QStringLiteral("reset"));
		});
	}

	return hotkeys;
}

QString Hotkeys::configPath() const { return HotkeyManager::instance()->configPath(); }
QVariantList Hotkeys::binds() const { return HotkeyManager::instance()->bindsInfo(); }
bool Hotkeys::hookActive() const { return HotkeyManager::instance()->hookActive(); }
QString Hotkeys::activeSubmap() const { return HotkeyManager::instance()->activeSubmap(); }

void Hotkeys::registerShortcut(QObject* shortcut) {
	HotkeyManager::instance()->registerShortcut(shortcut);
}

void Hotkeys::unregisterShortcut(QObject* shortcut) {
	HotkeyManager::instance()->unregisterShortcut(shortcut);
}

void Hotkeys::reload() { HotkeyManager::instance()->reload(); }

void Hotkeys::trigger(const QString& name) { HotkeyManager::instance()->triggerGlobal(name); }

bool Hotkeys::beginChordReleaseWait() {
	return HotkeyManager::instance()->beginChordReleaseWait();
}

} // namespace qs::windows::hotkeys

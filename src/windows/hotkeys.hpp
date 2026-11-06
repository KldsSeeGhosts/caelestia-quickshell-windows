#pragma once

#include <qt_windows.h>

#include <qcontainerfwd.h>
#include <qfilesystemwatcher.h>
#include <qlist.h>
#include <qobject.h>
#include <qpointer.h>
#include <qqmlengine.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>
#include <qvariant.h>

#include "input_settle.hpp"
#include "keyboard_hook.hpp"

namespace qs::windows::hotkeys {

[[nodiscard]] bool parseKeys(const QString& text, KeyCombo& combo, QString& error);
[[nodiscard]] uint8_t keyNameToVk(const QString& name);

struct Bind {
	enum class Action : quint8 {
		Global,
		Dispatch,
		Exec,
		Ipc,
		Key,
		Reload,
		Native,
	};

	enum class Mechanism : quint8 {
		None,
		Hotkey,
		Hook,
		Native,
		Unavailable,
	};

	QString keys;
	QString submap;
	KeyCombo combo;
	Action action = Action::Native;
	QString appid;
	QString name;
	QString argument;
	QString function;
	QStringList args;
	uint8_t sendVk = 0;
	bool onRelease = false;
	bool repeat = false;
	bool preferRegister = false;
	QString description;
	Mechanism mechanism = Mechanism::None;
};

class HotkeyManager: public QObject {
	Q_OBJECT;

public:
	~HotkeyManager() override;
	Q_DISABLE_COPY_MOVE(HotkeyManager);

	static HotkeyManager* instance();

	void setShellDir(const QString& shellDir);
	void reload();
	// Empty/reset selects the global map; unknown names leave routing unchanged.
	[[nodiscard]] bool setSubmap(const QString& name, QString* refusal = nullptr);
	[[nodiscard]] QString activeSubmap() const { return this->mActiveSubmap; }

	void registerShortcut(QObject* shortcut);
	void unregisterShortcut(QObject* shortcut);

	void triggerGlobal(const QString& name);

	// Arms a one-shot wait for the keys that triggered the current action to
	// come up. Returns false when no action is being dispatched (an external
	// IPC request) or nothing is held, so the caller runs immediately.
	bool beginChordReleaseWait();

	[[nodiscard]] QString configPath() const { return this->mConfigPath; }
	[[nodiscard]] QVariantList bindsInfo() const;
	[[nodiscard]] bool hookActive() const;

signals:
	void bindsChanged();
	void activeSubmapChanged();
	void chordReleaseSettled();
	void chordReleaseCancelled();

private:
	explicit HotkeyManager(QObject* parent);

	enum class Phase : quint8 {
		Press,
		Repeat,
		Release,
		Tap,
	};

	struct Trigger {
		HookTrigger::Kind kind = HookTrigger::Combo;
		KeyCombo combo;
		Bind::Mechanism mechanism = Bind::Mechanism::None;
		QList<qsizetype> binds;
		bool repeat = false;
		bool preferRegister = false;
		bool down = false;
	};

	void loadFile();
	void parse(const QByteArray& data, const QString& path);
	void unregisterAll(bool resetSubmap = true);
	void registerAll();
	qsizetype addToTrigger(qsizetype index);
	void resetTrigger(qsizetype index);
	void registerTriggers();
	void relayout();
	void mergeDefaults();
	void updateWatches();
	void scheduleReload();

	void onHotkey(int id);
	void onHookEvent(HookEvent event, quint32 triggerId, quint32 serial);
	void pollReleases();
	void onPhysicalKey(quint8 vk, bool up);
	void onInputLifecycleLost();
	void finishInputSettle(bool settled);
	void fire(qsizetype trigger, Phase phase);
	void fireBind(const Bind& bind, Phase phase);
	void emitGlobal(const QString& appid, const QString& name, bool pressed);
	void releaseAllGlobals();

	void runDispatch(const Bind& bind);
	void runExec(const Bind& bind) const;
	void runIpc(const Bind& bind);
	void runReload();

	[[nodiscard]] QString userFilePath() const;
	[[nodiscard]] QString defaultFilePath() const;
	void readDefaultFile();

	static LRESULT CALLBACK messageWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

	HWND messageWindow = nullptr;
	QString shellDir;
	QString userFile;
	QString configFile;
	QString mConfigPath;
	QByteArray loadedData;
	bool loaded = false;
	bool parsed = false;

	QList<Bind> binds;
	QString mActiveSubmap;
	QList<Trigger> triggers;
	quint32 serial = 0;
	bool hookRunning = false;
	bool dispatching = false;
	bool gateActive = false;
	bool gateButtonEvents = false;
	InputSettle inputSettle;

	QList<QPointer<QObject>> shortcuts;
	QList<QPair<QString, QString>> pressedGlobals;

	QFileSystemWatcher watcher;
	QTimer reloadTimer;
	QTimer releaseTimer;
	HKL lastLayout = nullptr;
};

class Hotkeys: public QObject {
	Q_OBJECT;
	Q_PROPERTY(QString configPath READ configPath NOTIFY bindsChanged);
	Q_PROPERTY(QVariantList binds READ binds NOTIFY bindsChanged);
	Q_PROPERTY(bool hookActive READ hookActive NOTIFY bindsChanged);
	Q_PROPERTY(QString activeSubmap READ activeSubmap NOTIFY activeSubmapChanged);
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit Hotkeys(QObject* parent);

	static Hotkeys* create(QQmlEngine* engine, QJSEngine* jsEngine);

	[[nodiscard]] QString configPath() const;
	[[nodiscard]] QVariantList binds() const;
	[[nodiscard]] bool hookActive() const;
	[[nodiscard]] QString activeSubmap() const;

	Q_INVOKABLE void registerShortcut(QObject* shortcut);
	Q_INVOKABLE void unregisterShortcut(QObject* shortcut);
	Q_INVOKABLE void reload();
	Q_INVOKABLE void trigger(const QString& name);

	// True when the current keybind action must wait for its triggering keys to
	// be released; false means run the action now.
	Q_INVOKABLE bool beginChordReleaseWait();

signals:
	void bindsChanged();
	void activeSubmapChanged();
	void chordReleaseSettled();
	void chordReleaseCancelled();
};

} // namespace qs::windows::hotkeys

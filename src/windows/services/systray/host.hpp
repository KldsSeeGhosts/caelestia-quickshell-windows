#pragma once

#include <mutex>
#include <vector>

#include <qt_windows.h>

#include <qhash.h>
#include <qlist.h>
#include <qobject.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>
#include <quuid.h>

#include "hook.hpp"
#include "item.hpp"

namespace qs::windows::services::systray {

class TrayHost: public QObject {
	Q_OBJECT;

public:
	static TrayHost* instance();
	~TrayHost() override;

	[[nodiscard]] const QList<SystemTrayItem*>& items() const { return this->mItems; }
	[[nodiscard]] QVariantMap hookCapability() const { return trayHookCapability(this->hookState); }

#ifdef QS_TEST
	static TrayHost* createForTest();
	void applyForTest(const TrayIconMessage& message) { this->apply(message); }
	[[nodiscard]] int ownerWaitCountForTest() const { return this->ownerWatches.size(); }
#endif

signals:
	void itemAdded(SystemTrayItem* item);
	void itemRemoved(SystemTrayItem* item);
	void hookCapabilityChanged();

private:
	explicit TrayHost();

	void apply(const TrayIconMessage& message);
	void add(const TrayIconMessage& message);
	void remove(SystemTrayItem* item);
	void watchOwner(SystemTrayItem* item);
	void unwatchOwner(SystemTrayItem* item);
	void onOwnerExited(quint64 token);
	static void CALLBACK ownerExited(PVOID context, BOOLEAN timedOut);
	void rememberDeletion(HWND hwnd, UINT uid, const QUuid& guid);
	void startSnapshot();
	void scheduleSnapshot(int delayMs);
	void onSnapshotDone(bool ok);
	void recoverCallbacks();
	void onToolbarData(const std::vector<ExplorerIconData>& icons);
	void requestTitle(const QString& exePath);
	void readTitles();
	void onTitles(const QHash<QString, QString>& found);
	[[nodiscard]] SystemTrayItem* find(HWND hwnd, UINT uid, const QUuid& guid) const;
	[[nodiscard]] QString uniqueId(const QString& base, UINT uid) const;
	[[nodiscard]] bool deletedRecently(const TrayIconMessage& message);

	struct Deletion {
		HWND hwnd;
		UINT uid;
		QUuid guid;
		ULONGLONG at;
	};

	// A wait callback runs on a thread-pool thread. It only reads host under
	// this mutex and posts to the owning thread; the destructor clears host
	// under the same mutex and then unregisters every wait, so a callback can
	// never touch a destroyed host.
	struct WaitState {
		std::mutex mutex;
		TrayHost* host = nullptr;
	};

	struct OwnerWaitContext {
		WaitState* state;
		quint64 token;
	};

	struct OwnerWatch {
		HANDLE process = nullptr;
		HANDLE wait = nullptr;
		DWORD pid = 0;
		quint64 token = 0;
		OwnerWaitContext* context = nullptr;
	};

	QList<SystemTrayItem*> mItems;
	TrayHookState hookState = TrayHookState::Disabled;
	QList<Deletion> deletions;
	QHash<SystemTrayItem*, OwnerWatch> ownerWatches;
	quint64 nextOwnerToken = 0;
	WaitState* waitState = nullptr;
	bool watchesLost = false;

	TrayIconSink sink;
	QTimer snapshotTimer;
	bool snapshotRunning = false;
	bool snapshotAgain = false;
	ULONGLONG snapshotStartedAt = 0;
	ULONGLONG snapshotDoneAt = 0;
	int snapshotReported = 0;

	QTimer pollTimer;
	QTimer recoverTimer;
	bool toolbarReading = false;
	bool recoverAgain = false;

	QHash<QString, QString> titles;
	QList<QString> titleQueue;
	bool titlesReading = false;
};

} // namespace qs::windows::services::systray

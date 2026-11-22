#pragma once

#include <memory>

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstringview.h>
#include <qt_windows.h>

class QJSEngine;
class QQmlEngine;

namespace qs::windows {

class SupervisorBridge: public QObject {
	Q_OBJECT;
	QML_NAMED_ELEMENT(Supervisor);
	QML_SINGLETON;
	Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged);
	Q_PROPERTY(bool hideTaskbar READ hideTaskbar NOTIFY takeoverChanged);
	Q_PROPERTY(bool taskbarTakenOver READ taskbarTakenOver NOTIFY takeoverChanged);
	Q_PROPERTY(int explorerEpoch READ explorerEpoch NOTIFY takeoverChanged);

public:
	static SupervisorBridge* instance();
	static SupervisorBridge* create(QQmlEngine*, QJSEngine*);

	[[nodiscard]] bool connected() const { return this->mConnected; }
	[[nodiscard]] bool hideTaskbar() const { return this->mHideTaskbar; }
	[[nodiscard]] bool taskbarTakenOver() const { return this->mTaskbarTakenOver; }
	[[nodiscard]] int explorerEpoch() const { return this->mExplorerEpoch; }
	[[nodiscard]] bool isCoordinatingExplorer() const { return this->connected(); }

	void start();
	void stop();
	void reportAppBar(QStringView op, HWND hwnd, int edge, const RECT& rect);
	Q_INVOKABLE void reportReady();
	Q_INVOKABLE void reportPaused(bool paused);

signals:
	void connectedChanged();
	void takeoverChanged();
	void explorerSettled(quint64 explorerPid, qint64 explorerHwnd, int explorerEpoch, bool taskbarSuppressed);

private:
	SupervisorBridge();
	~SupervisorBridge() override;

	void setConnected(bool connected);
	void setTakeover(bool hide, bool takenOver, int epoch);

	struct Worker;
	const std::unique_ptr<Worker> worker;
	bool mConnected = false;
	bool mHideTaskbar = false;
	bool mTaskbarTakenOver = false;
	int mExplorerEpoch = 0;
};

} // namespace qs::windows

#pragma once

#include <functional>
#include <memory>

#include <qobject.h>
#include <qstring.h>
#include <qvariant.h>

struct IAudioSessionControl;
struct IAudioSessionManager2;

namespace qs::windows::services::pipewire {

struct SessionSnapshot {
	QString key;
	QString endpointId;
	QString name;
	QString description;
	QString nickname;
	QVariantMap properties;
	int flow = 0;
	int state = 0;
	bool controlAvailable = false;
	bool stateAvailable = false;
	bool muted = false;
	float volume = 0;
	QString error;
};

// All native session interfaces, including volume controls, belong to this MTA.
// Only values and command keys cross the GUI boundary.
class PwSessionWorker: public QObject {
	Q_OBJECT;

public:
	using ManagerFactory = std::function<qint32(const QString&, IAudioSessionManager2**)>;
	explicit PwSessionWorker(ManagerFactory factory = {});
	~PwSessionWorker() override;
	void start();
	void stop();
	void attachEndpoint(const QString& id, int flow);
	void removeEndpoint(const QString& id);
	void setVolume(const QString& key, float volume);
	void setMuted(const QString& key, bool muted);
	void refreshSession(const QString& key);
	void sessionStateChanged(const QString& key, int state);
	void sessionDisconnected(const QString& key, bool serviceStopped);
	void scanEndpoint(const QString& id);
	void adoptSession(const QString& id, IAudioSessionControl* control);

signals:
	void sessionChanged(const SessionSnapshot& snapshot);
	void sessionRemoved(const QString& key);
	void error(const QString& message);

private:
	struct Private;
	std::unique_ptr<Private> d;
};

} // namespace qs::windows::services::pipewire
Q_DECLARE_METATYPE(qs::windows::services::pipewire::SessionSnapshot)

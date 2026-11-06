#pragma once

#include <memory>

#include <qobject.h>
#include <qqmlintegration.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include "../../../core/model.hpp"
#include "backend_status.hpp"
#include "player.hpp"

namespace qs::windows::services::mpris {

class GsmtcBackend;

class Mpris: public QObject {
	Q_OBJECT;
	Q_PROPERTY(UntypedObjectModel* players READ players CONSTANT);
	Q_PROPERTY(bool ready READ ready NOTIFY statusChanged);
	Q_PROPERTY(QString lastError READ lastError NOTIFY statusChanged);
	QML_NAMED_ELEMENT(Mpris);
	QML_SINGLETON;

public:
	explicit Mpris(QObject* parent = nullptr);
	~Mpris() override;
	Q_DISABLE_COPY_MOVE(Mpris);

	[[nodiscard]] UntypedObjectModel* players() { return &this->mPlayers; }
	[[nodiscard]] bool ready() const { return this->mStatus.ready(); }
	[[nodiscard]] QString lastError() const { return this->mStatus.lastError(); }
	void backendSetStatus(const QString& error) { this->mStatus.setResult(error); }

	void backendAddPlayer(
	    quint64 sessionId,
	    const QString& identity,
	    const QString& desktopEntry,
	    const QString& dbusName,
	    const MprisPlayer::PlaybackSnapshot& playback,
	    const MprisPlayer::TimelineSnapshot& timeline,
	    const MprisPlayer::MediaSnapshot& media
	);
	void backendRemovePlayer(quint64 sessionId);
	void backendUpdatePlaybackInfo(quint64 sessionId, const MprisPlayer::PlaybackSnapshot& snapshot);
	void backendUpdateTimeline(quint64 sessionId, const MprisPlayer::TimelineSnapshot& snapshot);
	void backendUpdateMediaProperties(quint64 sessionId, const MprisPlayer::MediaSnapshot& snapshot);

signals:
	void statusChanged();

private:
	[[nodiscard]] MprisPlayer* findPlayer(quint64 sessionId) const;

	ObjectModel<MprisPlayer> mPlayers {this};
	MprisBackendStatus mStatus {this};
	std::unique_ptr<GsmtcBackend> mBackend;
};

} // namespace qs::windows::services::mpris

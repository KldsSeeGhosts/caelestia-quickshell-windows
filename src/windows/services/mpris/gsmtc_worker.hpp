#pragma once

#include <memory>
#include <optional>
#include <unordered_map>

#include <qbytearray.h>
#include <qhash.h>
#include <qobject.h>
#include <qpointer.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>
#include <qtypes.h>
#include <qvector.h>

#include <winrt/Windows.Media.Control.h>

#include "app_identity.hpp"
#include "player.hpp"
#include "../../native_callback_gate.hpp"

namespace qs::windows::services::mpris {

class Mpris;

class GsmtcWorker: public QObject {
	Q_OBJECT;

public:
	explicit GsmtcWorker(Mpris* frontend, QString artCacheDirectory);
	~GsmtcWorker() override;
	Q_DISABLE_COPY_MOVE(GsmtcWorker);

	void cmdPlay(quint64 sessionId);
	void cmdPause(quint64 sessionId);
	void cmdTogglePlayPause(quint64 sessionId);
	void cmdNext(quint64 sessionId);
	void cmdPrevious(quint64 sessionId);
	void cmdSetPosition(quint64 sessionId, qint64 ticks);
	void cmdSetShuffle(quint64 sessionId, bool shuffle);
	void cmdSetLoopState(quint64 sessionId, MprisLoopState::Enum state);

public slots:
	void start();
	void shutdown();

private:
	struct SessionEntry {
		winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession session {nullptr};
		winrt::event_token mediaToken {};
		winrt::event_token playbackToken {};
		winrt::event_token timelineToken {};
		QString trackKey;
		quint32 uniqueId = 0;
		QString cachedArtUrl;
		QByteArray artHash;
		QByteArray staleArtHash;
		int emptyReads = 0;
		int artRetries = 0;
	};

	void resyncSessions();
	void publishStatus(const QString& error = {});
	void addSession(const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession& session);
	void removeSession(quint64 sessionId);
	[[nodiscard]] bool sessionStillTracked(
	    const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession& session
	) const;

	void refreshPlaybackInfo(quint64 sessionId);
	void refreshTimeline(quint64 sessionId);
	void refreshMediaProperties(quint64 sessionId);
	void scheduleReread(quint64 sessionId);
	void updateArt(
	    SessionEntry& entry,
	    quint64 sessionId,
	    const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionMediaProperties&
	        props
	);

	[[nodiscard]] std::optional<MprisPlayer::PlaybackSnapshot>
	buildPlaybackSnapshot(const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession& session
	) const;
	[[nodiscard]] std::optional<MprisPlayer::TimelineSnapshot> buildTimelineSnapshot(
	    const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession& session
	) const;

	std::shared_ptr<NativeCallbackGate<GsmtcWorker>> mGate;
	QPointer<Mpris> mFrontend;
	const QString mArtCacheDirectory;

	winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager mManager {
	    nullptr
	};
	winrt::event_token mSessionsChangedToken {};
	winrt::event_token mCurrentSessionChangedToken {};

	std::unordered_map<quint64, std::unique_ptr<SessionEntry>> mEntries;
	QHash<QString, AppIdentity> mAppIdentities;
	quint64 mNextId = 1;
	bool mApartmentInitialized = false;
};

} // namespace qs::windows::services::mpris

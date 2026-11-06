#pragma once

#include <utility>

#include <qobject.h>
#include <qstring.h>

namespace qs::windows::services::mpris {

// Lifecycle status is independent of the session count. An empty successful
// scan is ready, while an initialization or scan failure is unavailable.
class MprisBackendStatus: public QObject {
	Q_OBJECT;

public:
	explicit MprisBackendStatus(QObject* parent = nullptr): QObject(parent) {}

	[[nodiscard]] bool ready() const { return this->mReady; }
	[[nodiscard]] QString lastError() const { return this->mLastError; }

	void setResult(QString error) {
		auto ready = error.isEmpty();
		if (this->mReady == ready && this->mLastError == error) return;
		this->mReady = ready;
		this->mLastError = std::move(error);
		// Both observations are current before any consumer is notified.
		emit this->changed();
	}

signals:
	void changed();

private:
	bool mReady = false;
	QString mLastError;
};

} // namespace qs::windows::services::mpris

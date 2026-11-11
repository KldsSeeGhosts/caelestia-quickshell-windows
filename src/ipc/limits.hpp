#pragma once

#include <qlocalsocket.h>
#include <qobject.h>
#include <qtimer.h>

namespace qs::ipc {

// Same-user robustness limits, not an authentication boundary. Signal listeners
// count toward the connection cap, but have no deadline after a complete request.
struct IpcLimits {
	static constexpr int MaxConnections = 128;
	static constexpr qint64 MaxIncompleteBytes = 4 * 1024 * 1024;
	static constexpr int IncompleteDeadlineMs = 30'000;
};

class IpcAdmission {
public:
	bool acquire() {
		if (connections >= IpcLimits::MaxConnections) return false;
		++connections;
		return true;
	}
	void release() { --connections; }

private:
	int connections = 0;
};

class IpcInputGuard: public QObject {
public:
	explicit IpcInputGuard(QLocalSocket* socket, int deadlineMs = IpcLimits::IncompleteDeadlineMs)
	    : QObject(socket)
	    , socket(socket) {
		socket->setReadBufferSize(IpcLimits::MaxIncompleteBytes + 1);
		deadline.setSingleShot(true);
		deadline.setInterval(deadlineMs);
		connect(&deadline, &QTimer::timeout, this, [this] { this->socket->abort(); });
		connect(socket, &QLocalSocket::readyRead, this, [this] {
			if (this->socket->bytesAvailable() > IpcLimits::MaxIncompleteBytes)
				this->socket->abort();
			else if (!deadline.isActive())
				deadline.start();
		});
		deadline.start();
	}

	void complete() { deadline.stop(); }

private:
	QLocalSocket* socket;
	QTimer deadline;
};

} // namespace qs::ipc

#pragma once

#include <array>
#include <limits>
#include <optional>
#include <variant>

#include <qbytearray.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qstringlist.h>

namespace qs::windows::supervisor {

using namespace Qt::StringLiterals;

constexpr qsizetype MaxFrame = 16 * 1024; // Payload bytes, excluding LF.
constexpr qsizetype MaxQueue = 64;

class FrameBuffer {
public:
	// A false return is terminal. Do not feed more bytes after a bad frame.
	template <typename Consumer>
	bool append(QByteArrayView bytes, Consumer consume) {
		while (!bytes.empty()) {
			const auto lf = bytes.indexOf('\n');
			const auto length = lf < 0 ? bytes.size() : lf;
			if (length > MaxFrame - this->pending.size()) return false;
			this->pending.append(bytes.first(length));
			if (lf < 0) return true;
			if (!consume(QByteArrayView(this->pending))) return false;
			this->pending.clear();
			bytes = bytes.sliced(lf + 1);
		}
		return true;
	}

private:
	QByteArray pending;
};

struct Explorer {
	quint32 pid = 0;
	qint64 hwnd = 0;
	int epoch = 0;
	bool operator==(const Explorer&) const = default;
};

class SettledEpoch {
public:
	bool accept(int epoch) {
		if (epoch < 0 || epoch <= this->last) return false;
		this->last = epoch;
		return true;
	}

private:
	int last = -1;
};

struct HelloMessage {
	QString token;
	int pid;
	QStringList capabilities;
	bool operator==(const HelloMessage&) const = default;
};
struct WelcomeMessage {
	bool hideTaskbar, taskbarTakenOver;
	Explorer explorer;
	bool operator==(const WelcomeMessage&) const = default;
};
struct PingMessage {
	QString nonce;
	bool operator==(const PingMessage&) const = default;
};
struct PongMessage {
	QString nonce;
	bool operator==(const PongMessage&) const = default;
};
struct LifecycleMessage {
	QString state;
	bool operator==(const LifecycleMessage&) const = default;
};
struct AppBarLeaseMessage {
	QString op;
	qint64 hwnd;
	int edge;
	std::array<int, 4> rect;
	bool operator==(const AppBarLeaseMessage&) const = default;
};
struct TakeoverMessage {
	bool takenOver, hideTaskbar;
	Explorer explorer;
	bool operator==(const TakeoverMessage&) const = default;
};
struct ExplorerSettledMessage {
	Explorer explorer;
	bool taskbarSuppressed;
	bool operator==(const ExplorerSettledMessage&) const = default;
};

using Message = std::variant<HelloMessage, WelcomeMessage, PingMessage, PongMessage, LifecycleMessage,
                             AppBarLeaseMessage, TakeoverMessage, ExplorerSettledMessage>;

inline QJsonObject json(const HelloMessage& m) {
	return {{"type"_L1, "hello"_L1}, {"token"_L1, m.token}, {"pid"_L1, m.pid},
	        {"capabilities"_L1, QJsonArray::fromStringList(m.capabilities)}};
}
inline void setExplorer(QJsonObject& object, const Explorer& e) {
	object.insert("explorerPid"_L1, qint64(e.pid));
	object.insert("explorerHwnd"_L1, e.hwnd);
	object.insert("explorerEpoch"_L1, e.epoch);
}
inline QJsonObject json(const WelcomeMessage& m) {
	auto object = QJsonObject {{"type"_L1, "welcome"_L1}, {"hideTaskbar"_L1, m.hideTaskbar},
	                           {"taskbarTakenOver"_L1, m.taskbarTakenOver}};
	setExplorer(object, m.explorer);
	return object;
}
inline QJsonObject json(const PingMessage& m) { return {{"type"_L1, "ping"_L1}, {"nonce"_L1, m.nonce}}; }
inline QJsonObject json(const PongMessage& m) { return {{"type"_L1, "pong"_L1}, {"nonce"_L1, m.nonce}}; }
inline QJsonObject json(const LifecycleMessage& m) {
	return {{"type"_L1, "lifecycle"_L1}, {"state"_L1, m.state}};
}
inline QJsonObject json(const AppBarLeaseMessage& m) {
	return {{"type"_L1, "appbar"_L1}, {"op"_L1, m.op}, {"hwnd"_L1, m.hwnd}, {"edge"_L1, m.edge},
	        {"rect"_L1, QJsonArray {m.rect[0], m.rect[1], m.rect[2], m.rect[3]}}};
}
inline QJsonObject json(const TakeoverMessage& m) {
	auto object = QJsonObject {{"type"_L1, "takeover"_L1}, {"takenOver"_L1, m.takenOver},
	                           {"hideTaskbar"_L1, m.hideTaskbar}};
	setExplorer(object, m.explorer);
	return object;
}
inline QJsonObject json(const ExplorerSettledMessage& m) {
	auto object = QJsonObject {{"type"_L1, "explorer_settled"_L1}, {"taskbarSuppressed"_L1, m.taskbarSuppressed}};
	setExplorer(object, m.explorer);
	return object;
}

inline QByteArray encode(const Message& message) {
	auto object = std::visit([](const auto& m) { return json(m); }, message);
	object.insert("v"_L1, 1);
	auto frame = QJsonDocument(object).toJson(QJsonDocument::Compact);
	if (frame.size() > MaxFrame) return {};
	frame.append('\n');
	return frame;
}

template <typename T>
std::optional<T> integer(const QJsonValue& value) {
	constexpr auto sentinel = std::numeric_limits<qint64>::max();
	const auto n = value.toInteger(sentinel);
	if ((n == sentinel && value.toInteger(0) != sentinel) || n < std::numeric_limits<T>::min()
	    || n > std::numeric_limits<T>::max())
		return {};
	return static_cast<T>(n);
}

inline std::optional<Explorer> explorer(const QJsonObject& o) {
	const auto pid = integer<quint32>(o["explorerPid"_L1]);
	const auto hwnd = integer<qint64>(o["explorerHwnd"_L1]);
	const auto epoch = integer<int>(o["explorerEpoch"_L1]);
	if (!pid || !hwnd || !epoch || *epoch < 0) return {};
	return Explorer {*pid, *hwnd, *epoch};
}

inline bool validToken(QStringView token) {
	if (token.size() != 32) return false;
	for (auto c: token) {
		if ((c < u'0' || c > u'9') && (c < u'a' || c > u'f') && (c < u'A' || c > u'F')) return false;
	}
	return true;
}

inline std::optional<Message> decode(QByteArrayView bytes) {
	if (bytes.size() > MaxFrame) return {};
	const auto d = QJsonDocument::fromJson(bytes.toByteArray());
	if (!d.isObject()) return {};
	const auto o = d.object();
	if (integer<int>(o["v"_L1]) != 1) return {};
	const auto type = o["type"_L1].toString();
	if (type == "hello"_L1) {
		const auto token = o["token"_L1].toString();
		const auto pid = integer<int>(o["pid"_L1]);
		const auto caps = o["capabilities"_L1];
		if (!validToken(token) || !pid || *pid <= 0 || !caps.isArray()) return {};
		auto names = QStringList();
		for (const auto& item: caps.toArray()) {
			if (!item.isString()) return {};
			names.append(item.toString());
		}
		return HelloMessage {token, *pid, names};
	}
	if (type == "ping"_L1 || type == "pong"_L1) {
		const auto nonce = o["nonce"_L1].toString();
		if (nonce.isEmpty()) return {};
		if (type == "ping"_L1) return PingMessage {nonce};
		return PongMessage {nonce};
	}
	if (type == "lifecycle"_L1) {
		const auto state = o["state"_L1].toString();
		if (state != "ready"_L1 && state != "paused"_L1 && state != "active"_L1) return {};
		return LifecycleMessage {state};
	}
	if (type == "appbar"_L1) {
		const auto op = o["op"_L1].toString();
		const auto hwnd = integer<qint64>(o["hwnd"_L1]);
		const auto edge = integer<int>(o["edge"_L1]);
		const auto rect = o["rect"_L1].toArray();
		if ((op != "register"_L1 && op != "update"_L1 && op != "remove"_L1) || !hwnd || !*hwnd
		    || !edge || *edge < 0 || *edge > 3 || rect.size() != 4)
			return {};
		auto result = AppBarLeaseMessage {op, *hwnd, *edge, {}};
		for (qsizetype i = 0; i < rect.size(); ++i) {
			const auto coordinate = integer<int>(rect.at(i));
			if (!coordinate) return {};
			result.rect[static_cast<size_t>(i)] = *coordinate;
		}
		return result;
	}
	const auto e = explorer(o);
	const auto hide = o["hideTaskbar"_L1];
	if (!e) return {};
	if (type == "welcome"_L1) {
		const auto takenOver = o["taskbarTakenOver"_L1];
		if (hide.isBool() && takenOver.isBool()) return WelcomeMessage {hide.toBool(), takenOver.toBool(), *e};
	} else if (type == "takeover"_L1) {
		const auto takenOver = o["takenOver"_L1];
		if (hide.isBool() && takenOver.isBool()) return TakeoverMessage {takenOver.toBool(), hide.toBool(), *e};
	} else if (type == "explorer_settled"_L1) {
		const auto suppressed = o["taskbarSuppressed"_L1];
		if (suppressed.isBool()) return ExplorerSettledMessage {*e, suppressed.toBool()};
	}
	return {};
}

} // namespace qs::windows::supervisor

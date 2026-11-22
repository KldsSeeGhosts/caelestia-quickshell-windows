#include <qtest.h>

#include "../supervisor_protocol.hpp"

using namespace qs::windows::supervisor;

class SupervisorProtocolTest: public QObject {
	Q_OBJECT;

private slots:
	void messagesRoundTrip() {
		const auto explorer = Explorer {4294967295U, 9223372036854775807LL, 7};
		const Message messages[] = {
		    HelloMessage {u"0123456789abcdef0123456789abcdef"_s, 42,
		                  {u"liveness"_s, u"explorer-epoch"_s, u"appbar-lease"_s, u"lifecycle"_s}},
		    WelcomeMessage {true, false, explorer},
		    PingMessage {u"abc123"_s},
		    PongMessage {u"abc123"_s},
		    LifecycleMessage {u"ready"_s},
		    LifecycleMessage {u"paused"_s},
		    LifecycleMessage {u"active"_s},
		    AppBarLeaseMessage {u"register"_s, 9007199254740993LL, 0, {-512, 0, 0, 32}},
		    AppBarLeaseMessage {u"update"_s, 9007199254740993LL, 1, {-512, 0, 0, 32}},
		    AppBarLeaseMessage {u"remove"_s, 9007199254740993LL, 3, {}},
		    TakeoverMessage {true, true, explorer},
		    ExplorerSettledMessage {explorer, true},
		};
		for (const auto& message: messages) {
			const auto bytes = encode(message);
			QVERIFY(bytes.endsWith('\n'));
			QVERIFY(decode(QByteArrayView(bytes).chopped(1)) == message);
		}
	}

	void framing() {
		auto frames = FrameBuffer();
		auto messages = QList<Message>();
		auto consume = [&](QByteArrayView frame) {
			const auto message = decode(frame);
			if (!message) return false;
			messages.append(*message);
			return true;
		};
		const auto first = encode(PingMessage {u"ä\n"_s});
		const auto second = encode(LifecycleMessage {u"paused"_s});
		const auto mid = first.indexOf(QByteArray::fromHex("c3a4")) + 1;
		QVERIFY(mid > 0);
		QVERIFY(frames.append(QByteArrayView(first).first(mid), consume));
		QVERIFY(messages.isEmpty());
		QVERIFY(frames.append(first.sliced(mid) + second + first.chopped(1), consume));
		QCOMPARE(messages.size(), 2);
		QVERIFY(frames.append("\n", consume));
		QCOMPARE(messages.size(), 3);
		QVERIFY(messages.first() == messages.last());
	}

	void frameLimits() {
		auto frames = FrameBuffer();
		auto count = 0;
		auto consume = [&](QByteArrayView frame) { ++count; return frame.size() == MaxFrame; };
		QVERIFY(frames.append(QByteArray(MaxFrame, ' '), consume));
		QCOMPARE(count, 0);
		QVERIFY(frames.append("\n", consume));
		QCOMPARE(count, 1);
		QVERIFY(frames.append(QByteArray(MaxFrame, ' '), consume));
		QVERIFY(!frames.append(" \n", consume));
		QCOMPARE(count, 1);
		QVERIFY(encode(PingMessage {QString(MaxFrame, u'x')}).isEmpty());
	}

	void rejectsInvalidFrames() {
		const QByteArrayView invalid[] = {
		    "", "[]", "{", R"({"v":2,"type":"ping","nonce":"valid"})",
		    R"({"v":"1","type":"ping","nonce":"valid"})", R"({"v":1,"type":"ping","nonce":42})",
		    R"({"v":1,"type":"lifecycle","state":"other"})", R"({"v":1,"type":"debug"})",
		    R"({"v":1,"type":"appbar","op":"register","hwnd":1,"edge":0,"rect":[0,1,2]})",
		    R"({"v":1,"type":"takeover","takenOver":true,"explorerPid":1,"explorerHwnd":2,"explorerEpoch":3})",
		};
		for (auto frame: invalid) QVERIFY(!decode(frame));
	}

	void settledEpochIsMonotonic() {
		auto epoch = SettledEpoch();
		QVERIFY(!epoch.accept(-1));
		QVERIFY(epoch.accept(0));
		QVERIFY(!epoch.accept(0));
		QVERIFY(epoch.accept(42));
		QVERIFY(!epoch.accept(1));
		QVERIFY(!epoch.accept(42));
		QVERIFY(epoch.accept(43));
	}
};

QTEST_GUILESS_MAIN(SupervisorProtocolTest)
#include "supervisor_protocol_test.moc"

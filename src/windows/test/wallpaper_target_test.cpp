#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <qelapsedtimer.h>
#include <qtest.h>

#include "../system/wallpaper_target.hpp"
#include "../system/wallpaper_worker.hpp"

using namespace qs::windows;
using namespace qs::windows::sys::wallpaper_target;
namespace worker = qs::windows::sys::wallpaper_worker;

namespace {
struct FakeState {
	std::mutex mutex;
	std::condition_variable wake;
	bool blocked = false;
	bool released = false;
	bool refuse = false;
	bool disconnect = false;
	bool blockDestruction = false;
	int delay = 0;
	QString path = QStringLiteral("original");
	std::atomic<int> calls = 0;
	std::atomic<int> factories = 0;
	std::atomic<int> destroyed = 0;
	std::atomic<DWORD> owner = 0;
	std::atomic<bool> wrongThread = false;
	~FakeState() { this->release(); }
	void release() {
		{
			std::lock_guard lock(this->mutex);
			this->released = true;
		}
		this->wake.notify_all();
	}
};

struct FakeProvider: worker::Provider {
	std::shared_ptr<FakeState> state;
	explicit FakeProvider(std::shared_ptr<FakeState> state): state(std::move(state)) {
		++this->state->factories;
		this->state->owner = GetCurrentThreadId();
	}
	~FakeProvider() override {
		if (this->state->owner != GetCurrentThreadId()) this->state->wrongThread = true;
		{
			std::unique_lock lock(this->state->mutex);
			this->state->wake.wait(lock, [&] {
				return !this->state->blockDestruction || this->state->released;
			});
		}
		++this->state->destroyed;
	}
	worker::Result execute(const worker::Request& request) override {
		auto& s = *this->state;
		if (s.owner != GetCurrentThreadId()) s.wrongThread = true;
		++s.calls;
		{
			std::unique_lock lock(s.mutex);
			s.wake.wait(lock, [&] { return !s.blocked || s.released; });
		}
		if (s.delay) std::this_thread::sleep_for(std::chrono::milliseconds(s.delay));
		if (s.refuse) return {false, {}, s.disconnect};
		if (request.kind != worker::Kind::Read && request.kind != worker::Kind::ReadScreen)
			s.path = request.path;
		return {true, s.path, s.disconnect};
	}
};

// Always unblock on assertion failure too. No test leaves a hung thread.
struct ReleaseOnExit {
	std::shared_ptr<FakeState> state;
	~ReleaseOnExit() { this->state->release(); }
};

worker::Factory factory(const std::shared_ptr<FakeState>& state) {
	return [state] { return std::make_unique<FakeProvider>(state); };
}

worker::Request setRequest(worker::Kind kind = worker::Kind::Set) {
	worker::Request request;
	request.kind = kind;
	request.path = QStringLiteral("new");
	return request;
}
} // namespace

class TestWallpaperTarget: public QObject {
	Q_OBJECT

private slots:
	void cleanup() { QTRY_COMPARE_WITH_TIMEOUT(worker::Channel::liveCount(), 0, 2000); }

	void workerHealthyContractsAndOwnership() {
		auto state = std::make_shared<FakeState>();
		const auto gui = GetCurrentThreadId();
		worker::Channel channel(factory(state), this);
		QCOMPARE(channel.read({}), QStringLiteral("original"));
		int results = 0;
		channel.finished = [&](const worker::Request&, bool ok, const QString& reason) {
			QCOMPARE(GetCurrentThreadId(), gui);
			QVERIFY(ok);
			QVERIFY(reason.isEmpty());
			++results;
		};
		for (const auto kind: {worker::Kind::Set, worker::Kind::SetScreen, worker::Kind::SetDesktops}) {
			auto request = setRequest(kind);
			request.target = kind == worker::Kind::SetScreen ? QStringLiteral("monitor") : QString();
			QVERIFY(channel.command(request));
		}
		QCOMPARE(results, 3);
		QCOMPARE(channel.read({}), QStringLiteral("new"));
		// A healthy empty wallpaper, such as a solid color, clears the cache.
		state->path.clear();
		QCOMPARE(channel.read({}), QString());
		QVERIFY(state->owner != gui);
		channel.shutdown();
		QTRY_COMPARE(state->destroyed.load(), 1);
		QVERIFY(!state->wrongThread);
	}

	void blockedReadReturnsCacheAndBlockedSetterReturnsWithinBound() {
		auto state = std::make_shared<FakeState>();
		ReleaseOnExit release {state};
		worker::Channel channel(factory(state), this);
		QCOMPARE(channel.read({}), QStringLiteral("original"));
		{
			std::lock_guard lock(state->mutex);
			state->blocked = true;
		}
		QElapsedTimer elapsed;
		elapsed.start();
		QCOMPARE(channel.read({}), QStringLiteral("original"));
		QVERIFY(elapsed.elapsed() < 250);
		elapsed.restart();
		QVERIFY(!channel.command(setRequest()));
		QVERIFY(elapsed.elapsed() < 250);
		int completions = 0;
		channel.finished = [&](const worker::Request&, bool ok, const QString&) {
			QVERIFY(ok);
			++completions;
		};
		state->release();
		QTRY_COMPARE(completions, 1);
		QCOMPARE(channel.read({}), QStringLiteral("new"));
		QCOMPARE(state->factories.load(), 1);
	}

	void slow300msCallsCompleteWithoutReplacement_data() {
		QTest::addColumn<int>("kind");
		for (const auto kind:
		     {worker::Kind::Read,
		      worker::Kind::ReadScreen,
		      worker::Kind::Set,
		      worker::Kind::SetScreen,
		      worker::Kind::SetDesktops,
		      worker::Kind::Background})
			QTest::newRow(qPrintable(QString::number(static_cast<int>(kind)))) << static_cast<int>(kind);
	}

	void slow300msCallsCompleteWithoutReplacement() {
		QFETCH(int, kind);
		auto state = std::make_shared<FakeState>();
		state->delay = 300;
		worker::Channel channel(factory(state), this);
		channel.setStuckBound(std::chrono::milliseconds(1000));
		auto request = setRequest(static_cast<worker::Kind>(kind));
		int results = 0;
		channel.finished = [&](const worker::Request&, bool ok, const QString&) {
			QVERIFY(ok);
			++results;
		};
		QElapsedTimer elapsed;
		elapsed.start();
		const auto read =
		    request.kind == worker::Kind::Read || request.kind == worker::Kind::ReadScreen;
		if (read) QCOMPARE(channel.read(request), QString());
		else if (request.kind == worker::Kind::Background) channel.background(request);
		else QVERIFY(!channel.command(request));
		QVERIFY(elapsed.elapsed() < 250);
		QTest::qWait(350);
		QCOMPARE(state->factories.load(), 1);
		if (read) {
			// Another slow refresh returns the completed first read's cache.
			QCOMPARE(channel.read(request), QStringLiteral("original"));
		} else {
			QCOMPARE(results, 1);
			// Per-desktop changes cannot assume which desktop is current.
			// Its cache updates only once a real read has completed.
			QTRY_COMPARE_WITH_TIMEOUT(channel.read({}), QStringLiteral("new"), 1500);
		}
	}

	void lateRefusalAndBusyCommandKeepOriginalRequest() {
		auto state = std::make_shared<FakeState>();
		state->delay = 300;
		state->refuse = true;
		worker::Channel channel(factory(state), this);
		int results = 0;
		channel.finished = [&](const worker::Request& request, bool ok, const QString& reason) {
			QVERIFY(!ok);
			QCOMPARE(request.path, QStringLiteral("new"));
			QCOMPARE(reason, QStringLiteral("provider-refused"));
			++results;
		};
		QVERIFY(!channel.command(setRequest()));
		auto newer = setRequest();
		newer.path = QStringLiteral("not-accepted");
		QElapsedTimer elapsed;
		elapsed.start();
		QVERIFY(!channel.command(newer));
		QVERIFY(elapsed.elapsed() < 50);
		QTRY_COMPARE(results, 1);
		QCOMPARE(state->calls.load(), 1);
		QCOMPARE(state->factories.load(), 1);
	}

	void retiredWorkerCapRecoversWhenCapacityReturns() {
		QList<std::shared_ptr<FakeState>> states;
		for (int i = 0; i < 5; ++i) {
			auto state = std::make_shared<FakeState>();
			state->blocked = i < 4;
			states.append(state);
		}
		struct ReleaseAll {
			QList<std::shared_ptr<FakeState>> states;
			~ReleaseAll() {
				for (const auto& state: this->states) state->release();
			}
		} release {states};
		std::atomic<int> generation = 0;
		worker::Channel channel(
		    [&] {
			    const auto index = generation++;
			    return std::make_unique<FakeProvider>(states.at(std::min(index, 4)));
		    },
		    this
		);
		channel.setStuckBound(std::chrono::milliseconds(50));
		QCOMPARE(channel.read({}), QString());
		QTRY_COMPARE_WITH_TIMEOUT(generation.load(), 4, 6000);
		QTRY_COMPARE_WITH_TIMEOUT(states[3]->calls.load(), 1, 1000);
		QTest::qWait(250);
		QCOMPARE(worker::Channel::liveCount(), 4);
		QCOMPARE(generation.load(), 4);
		for (const auto& state: states) state->release();
		QTRY_COMPARE_WITH_TIMEOUT(generation.load(), 5, 6000);
		QTRY_COMPARE(channel.read({}), QStringLiteral("original"));
	}

	void slowActivationCompletesAndReleaseDoesNotBlockGui() {
		auto state = std::make_shared<FakeState>();
		state->blockDestruction = true;
		ReleaseOnExit release {state};
		std::atomic<int> activations = 0;
		worker::Channel channel(
		    [&] {
			    ++activations;
			    std::this_thread::sleep_for(std::chrono::milliseconds(300));
			    return std::make_unique<FakeProvider>(state);
		    },
		    this
		);
		QElapsedTimer elapsed;
		elapsed.start();
		QCOMPARE(channel.read({}), QString());
		QVERIFY(elapsed.elapsed() < 250);
		QTRY_COMPARE(channel.read({}), QStringLiteral("original"));
		QCOMPARE(activations.load(), 1);
		elapsed.restart();
		channel.shutdown();
		QVERIFY(elapsed.elapsed() < 100);
		QCOMPARE(state->destroyed.load(), 0);
		QCOMPARE(worker::Channel::liveCount(), 1);
		state->release();
		QTRY_COMPARE(worker::Channel::liveCount(), 0);
		QCOMPARE(state->destroyed.load(), 1);
		QVERIFY(!state->wrongThread);
	}

	void disconnectedProviderRecoversWithoutReplaying() {
		auto old = std::make_shared<FakeState>();
		old->disconnect = true;
		old->refuse = true;
		auto replacement = std::make_shared<FakeState>();
		std::atomic<int> generation = 0;
		worker::Channel channel(
		    [&] { return std::make_unique<FakeProvider>(generation++ == 0 ? old : replacement); },
		    this
		);
		int results = 0;
		channel.finished = [&](const worker::Request&, bool ok, const QString& reason) {
			QVERIFY(!ok);
			QCOMPARE(reason, QStringLiteral("provider-disconnected"));
			++results;
		};
		QVERIFY(!channel.command(setRequest()));
		QCOMPARE(results, 1);
		QTRY_COMPARE(replacement->calls.load(), 1);
		QCOMPARE(old->calls.load(), 1);
		QCOMPARE(channel.read({}), QStringLiteral("original"));
	}

	void resultHandlerCannotQueueReentrantMutation() {
		auto state = std::make_shared<FakeState>();
		worker::Channel channel(factory(state), this);
		int results = 0;
		channel.finished = [&](const worker::Request&, bool ok, const QString&) {
			QVERIFY(ok);
			++results;
			QVERIFY(!channel.command(setRequest()));
			QCOMPARE(channel.read({}), QStringLiteral("new"));
		};
		QVERIFY(channel.command(setRequest()));
		QCOMPARE(results, 1);
		QCOMPARE(state->calls.load(), 1);
	}

	void stuckWorkerIsReplacedAndLateResultIsDropped() {
		auto old = std::make_shared<FakeState>();
		old->blocked = true;
		ReleaseOnExit release {old};
		auto replacement = std::make_shared<FakeState>();
		replacement->path = QStringLiteral("replacement");
		int generation = 0; // Factory executes on worker, generations never overlap acquisition.
		worker::Channel channel(
		    [&] { return std::make_unique<FakeProvider>(generation++ == 0 ? old : replacement); },
		    this
		);
		channel.setStuckBound(std::chrono::milliseconds(180));
		int results = 0;
		QString verdict;
		channel.finished = [&](const worker::Request&, bool ok, const QString& reason) {
			QVERIFY(!ok);
			verdict = reason;
			++results;
		};
		QElapsedTimer elapsed;
		elapsed.start();
		QVERIFY(!channel.command(setRequest()));
		QVERIFY(elapsed.elapsed() < 250);
		QCOMPARE(results, 0); // 100 ms GUI expiry is not a stuck verdict.
		QCOMPARE(old->factories.load(), 1);
		QTRY_COMPARE(verdict, QStringLiteral("provider-timeout"));
		QTRY_COMPARE(replacement->calls.load(), 1);
		QCOMPARE(channel.read({}), QStringLiteral("replacement"));
		old->release();
		QTRY_COMPARE(old->destroyed.load(), 1);
		QTest::qWait(30);
		QCOMPARE(results, 1);
		QCOMPARE(channel.read({}), QStringLiteral("replacement"));
	}

	void explorerRestartDiscardsOldReadAndNeverReplaysMutation() {
		auto old = std::make_shared<FakeState>();
		old->blocked = true;
		old->path = QStringLiteral("stale");
		ReleaseOnExit release {old};
		auto replacement = std::make_shared<FakeState>();
		replacement->path = QStringLiteral("fresh");
		std::atomic<int> generation = 0;
		worker::Channel channel(
		    [&] { return std::make_unique<FakeProvider>(generation++ == 0 ? old : replacement); },
		    this
		);
		QCOMPARE(channel.read({}), QString());
		QVERIFY(!channel.command(setRequest()));
		int results = 0;
		channel.finished = [&](const worker::Request&, bool ok, const QString& reason) {
			QVERIFY(!ok);
			QCOMPARE(reason, QStringLiteral("explorer-restarted"));
			++results;
		};
		channel.invalidate();
		QCOMPARE(results, 1);
		QTRY_COMPARE(replacement->calls.load(), 1);
		old->release();
		QTRY_COMPARE(old->destroyed.load(), 1);
		QCOMPARE(old->calls.load(), 1); // Queued old mutation was discarded.
		QCOMPARE(channel.read({}), QStringLiteral("fresh"));
		QCOMPARE(results, 1);
	}

	void backgroundCoalescesAndShutdownNeverJoins() {
		auto state = std::make_shared<FakeState>();
		state->blocked = true;
		ReleaseOnExit release {state};
		QElapsedTimer elapsed;
		elapsed.start();
		{
			worker::Channel channel(factory(state), this);
			channel.background(setRequest(worker::Kind::Background));
			QTRY_COMPARE(state->calls.load(), 1);
			auto request = setRequest(worker::Kind::Background);
			request.path = QStringLiteral("obsolete");
			channel.background(request);
			request.path = QStringLiteral("latest");
			channel.background(request);
			state->release();
			QTRY_COMPARE(state->calls.load(), 2);
			QTRY_COMPARE(channel.read({}), QStringLiteral("latest"));
			{
				std::lock_guard lock(state->mutex);
				state->blocked = true;
				state->released = false;
			}
			channel.background(setRequest(worker::Kind::Background));
			QTRY_VERIFY(state->calls.load() >= 4);
			elapsed.restart();
		}
		QVERIFY(elapsed.elapsed() < 100);
		state->release();
		QTRY_COMPARE(state->destroyed.load(), 1);
	}

	void normalization() {
		QCOMPARE(
		    normalizedPath(QStringLiteral("\\\\?\\DISPLAY#AbC")),
		    QStringLiteral("//?/display#abc")
		);
		QCOMPARE(normalizedPath(QString::fromUtf8("\\Ä\\Ω")), QString::fromUtf8("/ä/ω"));
		QCOMPARE(targetId({}), QString());
		QCOMPARE(
		    targetId(QStringLiteral("ABC")),
		    QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
		);
		QCOMPARE(
		    targetId(QStringLiteral("\\\\?\\DISPLAY#AbC")),
		    targetId(QStringLiteral("//?/display#abc"))
		);
		QCOMPARE(targetId(QString::fromUtf8("\\Ä\\Ω")), targetId(QString::fromUtf8("/ä/ω")));
		QCOMPARE(targetId(QStringLiteral("path-a")).size(), 64);
		QVERIFY(targetId(QStringLiteral("path-a")) != targetId(QStringLiteral("path-b")));
	}

	void exactRoutingAndRemoval() {
		QList<DisplayIdentity> live {
		    {QStringLiteral("DISPLAY1"),
		     QStringLiteral("\\\\?\\Primary"),
		     QStringLiteral("Same label"),
		     {},
		     QStringLiteral("Same label"),
		     true},
		    {QStringLiteral("DISPLAY2"),
		     QStringLiteral("\\\\?\\Secondary"),
		     QStringLiteral("Same label"),
		     {},
		     QStringLiteral("Same label (2)"),
		     true}
		};
		QCOMPARE(
		    devicePath(live, QStringLiteral("same LABEL (2)")),
		    QStringLiteral("\\\\?\\Secondary")
		);
		QVERIFY(devicePath(live, {}).isEmpty());
		QVERIFY(devicePath(live, QStringLiteral("DISPLAY2")).isEmpty());
		QVERIFY(devicePath(live, QStringLiteral("missing")).isEmpty());
		live.removeLast();
		QVERIFY(devicePath(live, QStringLiteral("Same label (2)")).isEmpty());
	}

	void rejectsAmbiguityAndMissingPath() {
		QList<DisplayIdentity> live {
		    {{}, QStringLiteral("\\\\?\\Target"), {}, {}, QStringLiteral("screen"), true},
		    {{}, QStringLiteral("other"), {}, {}, QStringLiteral("SCREEN"), true}
		};
		QVERIFY(devicePath(live, QStringLiteral("screen")).isEmpty());
		live[1].uniqueName = QStringLiteral("other");
		live[1].devicePath = QStringLiteral("//?/target");
		QVERIFY(devicePath(live, QStringLiteral("screen")).isEmpty());
		live.removeLast();
		live[0].devicePath.clear();
		QVERIFY(devicePath(live, QStringLiteral("screen")).isEmpty());
		QVERIFY(devicePath({}, QStringLiteral("screen")).isEmpty());
	}

	void comSpellingMatchesOnlyExactTarget() {
		QStringList paths {QStringLiteral("\\\\?\\Primary"), QStringLiteral("\\\\?\\SeCoNdArY")};
		QCOMPARE(
		    monitorPath(paths, QStringLiteral("//?/secondary")),
		    QStringLiteral("\\\\?\\SeCoNdArY")
		);
		QVERIFY(monitorPath(paths, QStringLiteral("//?/removed")).isEmpty());
		QVERIFY(monitorPath(paths, {}).isEmpty());
		QVERIFY(monitorPath({}, QStringLiteral("//?/secondary")).isEmpty());
		paths.append(QStringLiteral("//?/SECONDARY"));
		QVERIFY(monitorPath(paths, QStringLiteral("//?/secondary")).isEmpty());
	}
};

QTEST_GUILESS_MAIN(TestWallpaperTarget)
#include "wallpaper_target_test.moc"

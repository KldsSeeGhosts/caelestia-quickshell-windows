#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <qcoreapplication.h>
#include <qelapsedtimer.h>
#include <qfile.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qprocess.h>
#include <qstring.h>
#include <qt_windows.h>
#include <qtemporarydir.h>
#include <qtest.h>

#include "../transition_trace.hpp"

// The queue's worker mutex is private. One row forces the valid startup
// interleaving where push("first") loses try_lock to the worker and its drop
// report becomes the blocking sink call; holding that mutex from the test is
// the only deterministic way to do it. Every header the queue includes is
// above, so the macro only relaxes access in that header.
#define private public
#include "../transition_trace_queue.hpp"
#undef private

// The contract under test is not machine speed: a producer must return without
// the sink advancing, and shutdown must return while the sink is still blocked.
// Both tests prove that with latches the sink fixture controls. The timeout is
// only a backstop for a hang, so a loaded machine cannot turn a pass into a
// failure.
static constexpr auto HangGuard = std::chrono::seconds(10);

// The trace sink drains on its own thread, so a process can exit before the
// last records reach the file. Wait until every pushed event is accounted for
// as a record or a drop report, so the fixture asserts "no silent loss" rather
// than racing process exit against the drain.
static bool
traceComplete(const QString& path, quint64 expected, std::chrono::milliseconds timeout) {
	QElapsedTimer elapsed;
	elapsed.start();
	for (;;) {
		quint64 total = 0;
		QFile file(path);
		if (file.open(QIODevice::ReadOnly)) {
			while (!file.atEnd()) {
				QJsonParseError error {};
				auto doc = QJsonDocument::fromJson(file.readLine(), &error);
				if (error.error != QJsonParseError::NoError) break; // partial tail line: retry
				auto record = doc.object();
				if (record.value("event").toString() == QStringLiteral("trace_dropped")) {
					total += record.value("count").toString().toULongLong();
				} else {
					++total;
				}
			}
		}
		if (total >= expected) return true;
		if (elapsed.elapsed() >= timeout.count()) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

// A one-shot latch for handing a result back from a test thread. The sink
// fixture owns its own condition variable; these waits must not depend on it.
struct Latch {
	std::mutex mutex;
	std::condition_variable wake;
	bool set = false;

	void signal() {
		{
			std::lock_guard lock(this->mutex);
			this->set = true;
		}
		this->wake.notify_all();
	}

	bool waitFor(std::chrono::milliseconds timeout) {
		std::unique_lock lock(this->mutex);
		return this->wake.wait_for(lock, timeout, [&] { return this->set; });
	}
};

struct BlockedSinkResult {
	const char* error = nullptr; // null when the burst completed as expected
	bool sinkStillBlocked = false;
	quint64 burstDropped = 0;
	int sinkCalls = 0;
};

// Runs a ten-event burst against a sink that blocks inside its first call. The
// setup callback performs the initial push; when that push loses try_lock to
// the worker, the first call is the drop report for the setup record. Drops are
// counted only after the first call, so setup accounting never leaks into the
// burst.
static BlockedSinkResult
runBlockedSinkBurst(const std::function<void(qs::windows::transition_trace::Queue&)>& setup) {
	using namespace qs::windows::transition_trace;
	struct Fixture {
		std::mutex mutex;
		std::condition_variable wake;
		bool entered = false;
		bool released = false;
		bool exited = false;
		int calls = 0;
		quint64 dropped = 0;
	};
	auto fixture = std::make_shared<Fixture>();
	Queue queue(2, [fixture](Record record) {
		std::unique_lock lock(fixture->mutex);
		++fixture->calls;
		fixture->entered = true;
		fixture->wake.notify_all();
		fixture->wake.wait(lock, [&] { return fixture->released; });
		if (fixture->calls > 1 && QString::fromLatin1(record.event) == QStringLiteral("trace_dropped"))
		{
			fixture->dropped += record.fields.value("count").toString().toULongLong();
		}
		fixture->exited = true;
	});
	BlockedSinkResult result;
	setup(queue);
	{
		std::unique_lock lock(fixture->mutex);
		if (!fixture->wake.wait_for(lock, HangGuard, [&] { return fixture->entered; })) {
			result.error = "The sink never received the setup record";
			fixture->released = true;
			lock.unlock();
			fixture->wake.notify_all();
			return result;
		}
	}
	Latch burst;
	std::thread producer([&] {
		for (int i = 0; i < 10; ++i) queue.push("test", {});
		burst.signal();
	});
	if (!burst.waitFor(HangGuard)) {
		// Let both sides go before failing so the cleanup cannot hang.
		{
			std::lock_guard lock(fixture->mutex);
			fixture->released = true;
		}
		fixture->wake.notify_all();
		producer.join();
		result.error = "A producer waited for the blocked sink";
		return result;
	}
	{
		std::lock_guard lock(fixture->mutex);
		result.sinkStillBlocked = !fixture->exited && fixture->calls == 1;
		fixture->released = true;
	}
	fixture->wake.notify_all();
	producer.join();
	queue.stop(HangGuard);
	{
		std::lock_guard lock(fixture->mutex);
		result.burstDropped = fixture->dropped;
		result.sinkCalls = fixture->calls;
	}
	return result;
}

class TraceTest: public QObject {
	Q_OBJECT
private slots:
	void disabledAndOrdered() {
		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		auto path = dir.filePath("trace.jsonl");
		auto run = [&](bool enabled) {
			QProcess process;
			auto env = QProcessEnvironment::systemEnvironment();
			if (enabled) env.insert("QS_TRANSITION_TRACE", path);
			else env.remove("QS_TRANSITION_TRACE");
			process.setProcessEnvironment(env);
			process.start(QCoreApplication::applicationFilePath(), {"--writer"});
			return process.waitForFinished(30000) && process.exitCode() == 0;
		};
		QVERIFY(run(false));
		QVERIFY(!QFile::exists(path));
		QVERIFY2(run(true), "The trace writer exited before every record reached the file");
		QFile file(path);
		QVERIFY(file.open(QIODevice::ReadOnly));
		qint64 previous = 0;
		int count = 0;
		quint64 dropped = 0;
		while (!file.atEnd()) {
			QJsonParseError error {};
			auto doc = QJsonDocument::fromJson(file.readLine(), &error);
			QCOMPARE(error.error, QJsonParseError::NoError);
			auto record = doc.object();
			bool ok = false;
			auto ticks = record.value("qpc").toString().toLongLong(&ok);
			QVERIFY(ok);
			QVERIFY(ticks >= previous);
			QVERIFY(record.value("qpcFrequency").toString().toLongLong() > 0);
			QVERIFY(!record.value("utc").toString().isEmpty());
			if (record.value("event").toString() == QStringLiteral("trace_dropped")) {
				dropped += record.value("count").toString().toULongLong();
			} else {
				QCOMPARE(record.value("event").toString(), QStringLiteral("test"));
				++count;
			}
			previous = ticks;
		}
		QCOMPARE(static_cast<quint64>(count) + dropped, 400);
	}

	void slowSinkDoesNotBlockAndReportsDrops() {
		using namespace qs::windows::transition_trace;
		auto result = runBlockedSinkBurst([](Queue& queue) { queue.push("first", {}); });
		QVERIFY2(!result.error, result.error ? result.error : "");
		QVERIFY2(result.sinkStillBlocked, "The sink advanced while the producers ran");
		QCOMPARE(result.burstDropped, 8);
		// first, the two queued records and one aggregated drop report
		QCOMPARE(result.sinkCalls, 4);
	}

	void startupDropIsNotCountedAsBurstDrop() {
		using namespace qs::windows::transition_trace;
		// Hold the worker mutex so the setup push loses try_lock: its drop for
		// "first" is then the call that blocks the sink. The burst's eight
		// drops must still be reported as eight, not nine.
		// The push runs on another thread: try_lock on a std::mutex the calling
		// thread already owns is undefined behaviour.
		auto result = runBlockedSinkBurst([](Queue& queue) {
			std::lock_guard lock(queue.state->mutex);
			std::thread([&queue] { queue.push("first", {}); }).join();
		});
		QVERIFY2(!result.error, result.error ? result.error : "");
		QVERIFY2(result.sinkStillBlocked, "The sink advanced while the producers ran");
		QCOMPARE(result.burstDropped, 8);
		QCOMPARE(result.sinkCalls, 4);
	}

	void shutdownIsBoundedForBlockedSink() {
		using namespace qs::windows::transition_trace;
		struct Fixture {
			std::mutex mutex;
			std::condition_variable wake;
			bool entered = false;
			bool released = false;
			bool exited = false;
		};
		auto fixture = std::make_shared<Fixture>();
		auto queue = std::make_unique<Queue>(2, [fixture](Record) {
			std::unique_lock lock(fixture->mutex);
			fixture->entered = true;
			fixture->wake.notify_all();
			fixture->wake.wait(lock, [&] { return fixture->released; });
			fixture->exited = true;
			fixture->wake.notify_all();
		});
		queue->push("first", {});
		{
			std::unique_lock lock(fixture->mutex);
			QVERIFY2(
			    fixture->wake.wait_for(lock, HangGuard, [&] { return fixture->entered; }),
			    "The sink never received the first record"
			);
		}
		// Destroy the queue on another thread and require it to return while
		// the sink is provably still blocked: shutdown must not wait for I/O.
		Latch destroyed;
		std::thread shutdown([&] {
			queue.reset();
			destroyed.signal();
		});
		if (!destroyed.waitFor(HangGuard)) {
			// Let both sides go before failing so the cleanup cannot hang.
			{
				std::lock_guard lock(fixture->mutex);
				fixture->released = true;
			}
			fixture->wake.notify_all();
			shutdown.join();
			QFAIL("Shutdown waited for the blocked sink");
		}
		bool stillBlocked = false;
		{
			std::lock_guard lock(fixture->mutex);
			stillBlocked = !fixture->exited;
			fixture->released = true;
		}
		fixture->wake.notify_all();
		shutdown.join();
		QVERIFY2(stillBlocked, "The sink advanced before shutdown returned");
		QVERIFY2(!queue, "Shutdown returned without destroying the queue");
		// The shared state must outlive the timed-out shutdown: the detached
		// worker still finishes its sink call and exits normally.
		{
			std::unique_lock lock(fixture->mutex);
			QVERIFY2(
			    fixture->wake.wait_for(lock, HangGuard, [&] { return fixture->exited; }),
			    "The detached worker did not finish after the sink was released"
			);
		}
	}
};

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	if (app.arguments().contains("--writer")) {
		std::vector<std::thread> workers;
		for (int i = 0; i < 4; ++i)
			workers.emplace_back([i] {
				for (int j = 0; j < 100; ++j) {
					QS_TRANSITION_TRACE("test", {{"worker", i}, {"sequence", j}});
				}
			});
		for (auto& worker: workers) worker.join();
		const auto path = qEnvironmentVariable("QS_TRANSITION_TRACE");
		if (!path.isEmpty() && !traceComplete(path, 400, std::chrono::seconds(10))) return 3;
		return 0;
	}
	TraceTest test;
	return QTest::qExec(&test, argc, argv);
}
#include "transition_trace_test.moc"

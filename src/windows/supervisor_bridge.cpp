#include "supervisor_bridge.hpp"

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

#include <qhash.h>
#include <qjsengine.h>
#include <qmetaobject.h>

#include "supervisor_protocol.hpp"

namespace qs::windows {

using namespace supervisor;

namespace {

class Handle {
public:
	explicit Handle(HANDLE handle): value(handle) {}
	~Handle() { if (*this) CloseHandle(this->value); }
	Handle(const Handle&) = delete;
	Handle& operator=(const Handle&) = delete;
	explicit operator bool() const { return this->value && this->value != INVALID_HANDLE_VALUE; }
	const HANDLE value;
};

struct Operation {
	Handle event {CreateEventW(nullptr, TRUE, FALSE, nullptr)};
	OVERLAPPED native {};
	bool pending = false;

	void prepare() {
		ResetEvent(this->event.value);
		this->native = {};
		this->native.hEvent = this->event.value;
	}

	bool finish(HANDLE pipe, bool wait, DWORD& count) {
		this->pending = false;
		return GetOverlappedResult(pipe, &this->native, &count, wait ? TRUE : FALSE) != FALSE;
	}
};

} // namespace

struct SupervisorBridge::Worker {
	explicit Worker(SupervisorBridge* bridge): bridge(bridge) {}

	// GUI-side state is replayed after welcome if startup beats the handshake.
	bool activate() {
		std::lock_guard lock(this->mutex);
		if (!this->running) return false;
		this->authenticated = true;
		if (this->ready) this->enqueueLocked(LifecycleMessage {u"ready"_s});
		if (this->paused) this->enqueueLocked(LifecycleMessage {*this->paused ? u"paused"_s : u"active"_s});
		for (const auto& lease: this->leases) this->enqueueLocked(lease);
		this->leases.clear();
		return true;
	}

	void enqueueLocked(const Message& message) {
		auto bytes = encode(message);
		if (bytes.isEmpty()) return;
		if (this->outbound.size() == MaxQueue) this->outbound.pop_front();
		this->outbound.push_back(std::move(bytes));
		SetEvent(this->wake);
	}

	bool post(Message message, quint64 run) {
		if (++this->posted > MaxQueue) {
			--this->posted;
			return false;
		}
		QMetaObject::invokeMethod(this->bridge, [this, message = std::move(message), run] {
			--this->posted;
			if (run != this->generation) return;
			if (const auto* m = std::get_if<WelcomeMessage>(&message)) {
				if (!this->activate()) return;
				this->bridge->setTakeover(m->hideTaskbar, m->taskbarTakenOver, m->explorer.epoch);
				this->bridge->setConnected(true);
				return;
			}
			if (!this->bridge->connected()) return;
			if (const auto* m = std::get_if<PingMessage>(&message)) {
				// The worker can read a ping even if the GUI is frozen. Only
				// this queued GUI callback can prove the GUI answered it.
				std::lock_guard lock(this->mutex);
				if (this->authenticated) this->enqueueLocked(PongMessage {m->nonce});
			} else if (const auto* m = std::get_if<TakeoverMessage>(&message)) {
				this->bridge->setTakeover(m->hideTaskbar, m->takenOver, m->explorer.epoch);
			} else if (const auto* m = std::get_if<ExplorerSettledMessage>(&message)) {
				if (!this->settled.accept(m->explorer.epoch)) return;
				this->bridge->setTakeover(
				    this->bridge->hideTaskbar(), m->taskbarSuppressed, m->explorer.epoch
				);
				emit this->bridge->explorerSettled(
				    m->explorer.pid, m->explorer.hwnd, m->explorer.epoch, m->taskbarSuppressed
				);
			}
		}, Qt::QueuedConnection);
		return true;
	}

	void run(QString name, QString token, DWORD watchdogPid, quint64 run) {
		this->exchange(name, std::move(token), watchdogPid, run);
		{
			std::lock_guard lock(this->mutex);
			this->running = this->authenticated = false;
			this->outbound.clear();
			this->leases.clear();
		}
		QMetaObject::invokeMethod(this->bridge, [this, run] {
			if (run != this->generation) return;
			this->bridge->setConnected(false);
			this->bridge->setTakeover(false, false, 0);
		}, Qt::QueuedConnection);
	}

	void exchange(const QString& name, QString token, DWORD watchdogPid, quint64 run) {
		auto pipe = Handle(CreateFileW(
		    reinterpret_cast<LPCWSTR>(name.utf16()), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
		    OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr
		));
		if (!pipe) return;
		ULONG serverPid = 0;
		if (!GetNamedPipeServerProcessId(pipe.value, &serverPid) || serverPid != watchdogPid) return;
		auto read = Operation();
		auto write = Operation();
		if (!read.event || !write.event) return;
		auto input = std::array<char, 4096>();
		auto output = encode(HelloMessage {
		    std::move(token), static_cast<int>(GetCurrentProcessId()),
		    {u"liveness"_s, u"explorer-epoch"_s, u"appbar-lease"_s, u"lifecycle"_s},
		});
		auto offset = qsizetype(0);
		auto frames = FrameBuffer();
		auto welcomed = false;
		const auto deadline = GetTickCount64() + 5000;
		auto receive = [&](DWORD count) {
			return count != 0 && frames.append(QByteArrayView(input.data(), count), [&](QByteArrayView frame) {
				auto message = decode(frame);
				if (!message) return false;
				if (!welcomed) {
					if (!std::holds_alternative<WelcomeMessage>(*message)) return false;
					welcomed = true;
				} else if (!std::holds_alternative<PingMessage>(*message)
				           && !std::holds_alternative<TakeoverMessage>(*message)
				           && !std::holds_alternative<ExplorerSettledMessage>(*message)) {
					return false;
				}
				return this->post(std::move(*message), run);
			});
		};
		auto written = [&](DWORD count) {
			offset += count;
			if (offset == output.size()) {
				output.clear();
				offset = 0;
			}
			return count != 0;
		};

		while (WaitForSingleObject(this->stopEvent, 0) == WAIT_TIMEOUT) {
			const auto now = GetTickCount64();
			if (!welcomed && now >= deadline) break;
			auto immediate = false;
			DWORD count = 0;
			if (!read.pending) {
				read.prepare();
				if (ReadFile(pipe.value, input.data(), DWORD(input.size()), &count, &read.native)) {
					if (!receive(count)) break;
					immediate = true;
				} else if (GetLastError() == ERROR_IO_PENDING) read.pending = true;
				else break;
			}
			if (!write.pending && output.isEmpty()) {
				std::lock_guard lock(this->mutex);
				if (!this->outbound.empty()) {
					output = std::move(this->outbound.front());
					this->outbound.pop_front();
				}
			}
			if (!write.pending && !output.isEmpty()) {
				write.prepare();
				if (WriteFile(pipe.value, output.constData() + offset, DWORD(output.size() - offset), &count, &write.native)) {
					if (!written(count)) break;
					immediate = true;
				} else if (GetLastError() == ERROR_IO_PENDING) write.pending = true;
				else break;
			}
			const HANDLE events[] = {this->stopEvent, read.event.value, write.pending ? write.event.value : this->wake};
			// A read that has not been posted may have left a signaled manual event.
			if (!read.pending) ResetEvent(read.event.value);
			const auto timeout = immediate ? 0U : welcomed ? INFINITE : DWORD(deadline - now);
			const auto signaled = WaitForMultipleObjects(3, events, FALSE, timeout);
			if (signaled == WAIT_OBJECT_0 || signaled == WAIT_FAILED) break;
			if (signaled == WAIT_OBJECT_0 + 1 && (!read.finish(pipe.value, false, count) || !receive(count))) break;
			if (signaled == WAIT_OBJECT_0 + 2 && write.pending
			    && (!write.finish(pipe.value, false, count) || !written(count)))
				break;
		}

		// OVERLAPPED and the corresponding buffers must outlive cancellation.
		CancelIoEx(pipe.value, nullptr);
		DWORD ignored = 0;
		if (read.pending) read.finish(pipe.value, true, ignored);
		if (write.pending) write.finish(pipe.value, true, ignored);
	}

	SupervisorBridge* const bridge;
	std::mutex mutex;
	std::thread thread;
	HANDLE stopEvent = nullptr;
	HANDLE wake = nullptr;
	std::deque<QByteArray> outbound;
	QHash<qint64, AppBarLeaseMessage> leases;
	bool running = false;
	bool authenticated = false;
	bool ready = false;
	std::optional<bool> paused;
	std::atomic<qsizetype> posted = 0;
	quint64 generation = 0; // GUI thread only.
	SettledEpoch settled;
};

SupervisorBridge::SupervisorBridge(): worker(std::make_unique<Worker>(this)) {}
SupervisorBridge::~SupervisorBridge() { this->stop(); }

SupervisorBridge* SupervisorBridge::instance() {
	static auto bridge = SupervisorBridge();
	return &bridge;
}

SupervisorBridge* SupervisorBridge::create(QQmlEngine*, QJSEngine*) {
	auto* bridge = instance();
	QJSEngine::setObjectOwnership(bridge, QJSEngine::CppOwnership);
	return bridge;
}

void SupervisorBridge::start() {
	if (this->worker->thread.joinable()) return;
	auto name = qEnvironmentVariable("CAELESTIA_SUPERVISOR_PIPE");
	auto token = qEnvironmentVariable("CAELESTIA_SUPERVISOR_TOKEN");
	qunsetenv("CAELESTIA_SUPERVISOR_PIPE");
	qunsetenv("CAELESTIA_SUPERVISOR_TOKEN");
	constexpr auto prefix = QStringView(uR"(\\.\pipe\CaelestiaSupervisor-)");
	if (!name.startsWith(prefix) || !validToken(token)) return;
	const auto tail = QStringView(name).sliced(prefix.size());
	const auto sep = tail.indexOf(u'-');
	if (sep <= 0 || tail.size() - sep != 17) return;
	bool pidOk = false;
	const auto watchdogPid = tail.first(sep).toUInt(&pidOk);
	if (!pidOk || !watchdogPid) return;
	for (auto c: tail.sliced(sep + 1)) {
		if ((c < u'0' || c > u'9') && (c < u'a' || c > u'f')) return;
	}
	auto& w = *this->worker;
	w.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	w.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!w.stopEvent || !w.wake) { this->stop(); return; }
	const auto run = ++w.generation;
	{
		std::lock_guard lock(w.mutex);
		w.running = true;
	}
	try {
		w.thread = std::thread([&w, name = std::move(name), token = std::move(token), watchdogPid, run]() mutable {
			w.run(std::move(name), std::move(token), watchdogPid, run);
		});
	} catch (const std::system_error&) {
		this->stop();
	}
}

void SupervisorBridge::stop() {
	auto& w = *this->worker;
	++w.generation;
	if (w.thread.joinable()) {
		SetEvent(w.stopEvent);
		w.thread.join();
	}
	{
		std::lock_guard lock(w.mutex);
		w.running = w.authenticated = false;
		if (w.stopEvent) CloseHandle(w.stopEvent);
		if (w.wake) CloseHandle(w.wake);
		w.stopEvent = w.wake = nullptr;
	}
	w.settled = {};
	this->setConnected(false);
	this->setTakeover(false, false, 0);
}

void SupervisorBridge::setConnected(bool connected) {
	if (this->mConnected == connected) return;
	this->mConnected = connected;
	emit this->connectedChanged();
}

void SupervisorBridge::setTakeover(bool hide, bool takenOver, int epoch) {
	if (this->mHideTaskbar == hide && this->mTaskbarTakenOver == takenOver && this->mExplorerEpoch == epoch) return;
	this->mHideTaskbar = hide;
	this->mTaskbarTakenOver = takenOver;
	this->mExplorerEpoch = epoch;
	emit this->takeoverChanged();
}

void SupervisorBridge::reportReady() {
	std::lock_guard lock(this->worker->mutex);
	if (!this->worker->running) return;
	this->worker->ready = true;
	if (this->worker->authenticated) this->worker->enqueueLocked(LifecycleMessage {u"ready"_s});
}

void SupervisorBridge::reportPaused(bool paused) {
	std::lock_guard lock(this->worker->mutex);
	if (!this->worker->running) return;
	this->worker->paused = paused;
	if (this->worker->authenticated)
		this->worker->enqueueLocked(LifecycleMessage {paused ? u"paused"_s : u"active"_s});
}

void SupervisorBridge::reportAppBar(QStringView op, HWND hwnd, int edge, const RECT& rect) {
	std::lock_guard lock(this->worker->mutex);
	if (!this->worker->running) return;
	auto lease = AppBarLeaseMessage {
	    op.toString(), reinterpret_cast<qint64>(hwnd), edge,
	    {int(rect.left), int(rect.top), int(rect.right), int(rect.bottom)},
	};
	if (this->worker->authenticated) this->worker->enqueueLocked(lease);
	else if (op == "remove"_L1) this->worker->leases.remove(lease.hwnd);
	else if (this->worker->leases.size() < MaxQueue - 2 || this->worker->leases.contains(lease.hwnd))
		this->worker->leases.insert(lease.hwnd, lease);
}

} // namespace qs::windows

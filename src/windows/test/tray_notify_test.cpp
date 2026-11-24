#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

#include <qscopeguard.h>
#include <qt_windows.h>
#include <qtest.h>
#include <shellapi.h>

#include "../services/systray/seed.hpp"

namespace qs::windows::services::systray {
QImage imageFromIcon(HICON) { return {}; }
} // namespace qs::windows::services::systray

using namespace qs::windows::services::systray;

namespace {

constexpr auto CALLBACK_MESSAGE = WM_APP + 1;
constexpr GUID ICON_GUID =
    {0x9d3a704c, 0xea80, 0x4bd3, {0x8b, 0x09, 0x41, 0x2a, 0x26, 0xc9, 0xb4, 0x88}};

class TestBackend: public TrayNotifyBackend {
public:
	explicit TestBackend(Registration result, bool seed = false): result(result), seed(seed) {}

	Registration registerCallback(Callback notify) override {
		this->workerId.store(GetCurrentThreadId());
		this->registrations++;
		this->notify = std::move(notify);
		if (FAILED(this->result.result)) return this->result;

		// A message-only window dispatches live test callbacks on the STA.
		// It never registers a taskbar class or talks to Explorer.
		WNDCLASSW cls {};
		cls.lpfnWndProc = &TestBackend::windowProc;
		cls.hInstance = GetModuleHandleW(nullptr);
		cls.lpszClassName = L"QsTrayNotifyTestBackend";
		RegisterClassW(&cls);
		auto* hwnd = CreateWindowExW(
		    0,
		    cls.lpszClassName,
		    L"",
		    0,
		    0,
		    0,
		    0,
		    0,
		    HWND_MESSAGE,
		    nullptr,
		    cls.hInstance,
		    this
		);
		if (hwnd == nullptr) return {HRESULT_FROM_WIN32(GetLastError())};

		this->window.store(hwnd);
		if (this->seed) this->deliver(hwnd, 0);
		return this->result;
	}

	void unregisterCallback() override {
		this->unregisterThread.store(GetCurrentThreadId());
		this->notify = {};
		if (auto* hwnd = this->window.exchange(nullptr)) DestroyWindow(hwnd);
		this->unregistrations++;
	}

	bool post(ULONG event) const {
		auto* hwnd = this->window.load();
		return hwnd != nullptr && PostMessageW(hwnd, CALLBACK_MESSAGE, event, 0);
	}

	std::atomic<DWORD> workerId = 0;
	std::atomic<DWORD> unregisterThread = 0;
	std::atomic<int> registrations = 0;
	std::atomic<int> unregistrations = 0;

private:
	static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM event, LPARAM data) {
		if (message == WM_NCCREATE) {
			const auto* create = reinterpret_cast<const CREATESTRUCTW*>(data);
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
		} else if (message == CALLBACK_MESSAGE) {
			auto* self = reinterpret_cast<TestBackend*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
			self->deliver(hwnd, static_cast<ULONG>(event));
			return 0;
		}
		return DefWindowProcW(hwnd, message, event, data);
	}

	void deliver(HWND hwnd, ULONG event) {
		wchar_t exe[] = L"C:\\tray-test\\owner.exe";
		wchar_t tip[] = L"fixture icon";
		const TrayNotifyItem item {
		    .exeName = exe,
		    .tip = tip,
		    .hwnd = hwnd,
		    .id = 19,
		    .guid = ICON_GUID,
		};
		this->notify(event, &item);
	}

	Registration result;
	bool seed;
	Callback notify;
	std::atomic<HWND> window = nullptr;
};

} // namespace

class TrayNotifyTest: public QObject {
	Q_OBJECT

private slots:
	void liveAddModifyDelete() {
		std::mutex mutex;
		QList<TrayIconMessage> icons;
		auto backend = std::make_shared<TestBackend>(TrayNotifyBackend::Registration {S_OK, 7}, true);
		auto* fake = backend.get();
		TrayNotifyObserver observer([backend] { return backend; });
		QCOMPARE(observer.state(), ComObserverState::Inactive);
		observer.start(
		    [&](TrayIconMessage message) {
			    const auto lock = std::lock_guard(mutex);
			    icons.append(std::move(message));
		    },
		    {}
		);
		QTRY_COMPARE(observer.state(), ComObserverState::Live);
		QVERIFY(fake->post(0));
		QVERIFY(fake->post(1));
		QVERIFY(fake->post(2));
		QTRY_COMPARE(
		    ([&] {
			    const auto lock = std::lock_guard(mutex);
			    return icons.size();
		    }()),
		    4
		);

		observer.stop();
		QCOMPARE(icons.at(0).message, DWORD(NIM_ADD));
		QVERIFY(icons.at(0).seeded);
		QCOMPARE(icons.at(1).message, DWORD(NIM_ADD));
		QCOMPARE(icons.at(2).message, DWORD(NIM_MODIFY));
		QCOMPARE(icons.at(3).message, DWORD(NIM_DELETE));
		for (auto i = 1; i < icons.size(); i++) {
			QVERIFY(!icons.at(i).seeded);
			QVERIFY(icons.at(i).fromObserver);
			QCOMPARE(icons.at(i).guid, QUuid(ICON_GUID));
			QCOMPARE(icons.at(i).uid, UINT(19));
			QCOMPARE(icons.at(i).callbackMessage, UINT(0));
			QCOMPARE(icons.at(i).version, UINT(0));
		}
		QCOMPARE(icons.at(2).tip, QStringLiteral("fixture icon"));
		QCOMPARE(icons.at(2).exePath, QStringLiteral("C:\\tray-test\\owner.exe"));
	}

	void registrationOutcomes_data() {
		QTest::addColumn<long>("result");
		QTest::addColumn<unsigned long>("handle");
		QTest::addColumn<bool>("hasHandle");
		QTest::addColumn<bool>("seed");
		QTest::addColumn<int>("state");
		QTest::newRow("silent-stub") << S_OK << 0UL << true << false << int(ComObserverState::StubNoOp);
		QTest::newRow("access-denied")
		    << E_ACCESSDENIED << 0UL << true << false << int(ComObserverState::Unavailable);
		QTest::newRow("server-retry") << RPC_E_SERVERCALL_RETRYLATER << 0UL << true << false
		                              << int(ComObserverState::Unavailable);
		QTest::newRow("handle-only") << S_OK << 33UL << true << false << int(ComObserverState::Live);
		QTest::newRow("callback-only") << S_OK << 0UL << true << true << int(ComObserverState::Live);
		QTest::newRow("win7") << S_OK << 0UL << false << true << int(ComObserverState::Live);
		QTest::newRow("win7-empty") << S_OK << 0UL << false << false
		                            << int(ComObserverState::Unavailable);
	}

	void registrationOutcomes() {
		QFETCH(long, result);
		QFETCH(unsigned long, handle);
		QFETCH(bool, hasHandle);
		QFETCH(bool, seed);
		QFETCH(int, state);
		auto expected = static_cast<ComObserverState>(state);
		auto backend = std::make_shared<TestBackend>(
		    TrayNotifyBackend::Registration {result, handle, hasHandle},
		    seed
		);
		auto* fake = backend.get();
		std::atomic<ComObserverState> published = ComObserverState::Inactive;
		TrayNotifyObserver observer([backend] { return backend; });
		observer.start({}, [&](ComObserverState next) { published.store(next); });
		QTRY_COMPARE(published.load(), expected);
		QCOMPARE(observer.state(), expected);
		QCOMPARE(observer.canRetry(), result == RPC_E_SERVERCALL_RETRYLATER);
		QCOMPARE(fake->unregistrations.load(), expected == ComObserverState::Live ? 0 : 1);

		observer.stop();
		QCOMPARE(observer.state(), ComObserverState::Inactive);
		QCOMPARE(fake->unregistrations.load(), 1);
	}

	void resyncAndStopJoinSta() {
		auto backend = std::make_shared<TestBackend>(TrayNotifyBackend::Registration {S_OK, 7}, true);
		auto* fake = backend.get();
		std::atomic<int> seeds = 0;
		TrayNotifyObserver observer([backend] { return backend; });
		observer.start(
		    [&](const TrayIconMessage& icon) {
			    if (icon.seeded) seeds++;
		    },
		    {}
		);
		QTRY_COMPARE(observer.state(), ComObserverState::Live);
		auto id = fake->workerId.load();
		QVERIFY(id != GetCurrentThreadId());
		auto* thread = OpenThread(SYNCHRONIZE, FALSE, id);
		QVERIFY(thread != nullptr);
		const auto close = qScopeGuard([&] { CloseHandle(thread); });

		observer.resync();
		QTRY_COMPARE(seeds.load(), 2);
		QTRY_COMPARE(observer.state(), ComObserverState::Live);
		QCOMPARE(fake->workerId.load(), id);
		QCOMPARE(fake->registrations.load(), 2);
		QCOMPARE(fake->unregistrations.load(), 1);

		observer.stop();
		QCOMPARE(observer.state(), ComObserverState::Inactive);
		QCOMPARE(fake->unregistrations.load(), 2);
		QCOMPARE(fake->unregisterThread.load(), id);
		QCOMPARE(WaitForSingleObject(thread, 0), DWORD(WAIT_OBJECT_0));
	}

	void restartCreatesFreshBackend() {
		QList<std::shared_ptr<TestBackend>> created;
		TrayNotifyObserver observer([&] {
			auto backend =
			    std::make_shared<TestBackend>(TrayNotifyBackend::Registration {S_OK, 7}, false);
			created.append(backend);
			return backend;
		});

		observer.start({}, {});
		QTRY_COMPARE(observer.state(), ComObserverState::Live);
		observer.stop();
		observer.start({}, {});
		QTRY_COMPARE(observer.state(), ComObserverState::Live);
		observer.stop();

		QCOMPARE(created.size(), 2);
		QVERIFY(created.at(0) != created.at(1));
		QCOMPARE(created.at(0)->registrations.load(), 1);
		QCOMPARE(created.at(1)->registrations.load(), 1);
	}
};

QTEST_GUILESS_MAIN(TrayNotifyTest)
#include "tray_notify_test.moc"

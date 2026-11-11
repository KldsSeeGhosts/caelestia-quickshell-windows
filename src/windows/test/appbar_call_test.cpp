#include <cstdio>
#include <memory>
#include <vector>

#include <qcoreapplication.h>
#include <qelapsedtimer.h>
#include <qprocess.h>
#include <qtest.h>
#include <quuid.h>

#include "../appbar_call.hpp"
#include "../window_responsiveness.hpp"

using qs::windows::AppBarChannel;

namespace {
constexpr UINT REQUEST = WM_APP + 41;
constexpr auto OWNER_CLASS = L"QuickshellUnresponsiveOwnerFixture";

LRESULT CALLBACK ownerProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
	if (message == REQUEST) return 42;
	return DefWindowProcW(hwnd, message, wp, lp);
}

// The child owns only a hidden fixture window. Kernel events, not window
// messages or injected input, let the parent resume/stop its message pump.
int ownerMain(const QString& name, bool pumping) {
	auto resumeName = (name + "-resume").toStdWString();
	auto stopName = (name + "-stop").toStdWString();
	auto resume = OpenEventW(SYNCHRONIZE, FALSE, resumeName.c_str());
	auto stop = OpenEventW(SYNCHRONIZE, FALSE, stopName.c_str());
	if (!resume || !stop) return 2;
	WNDCLASSW cls {};
	cls.lpfnWndProc = ownerProc;
	cls.hInstance = GetModuleHandleW(nullptr);
	cls.lpszClassName = OWNER_CLASS;
	if (!RegisterClassW(&cls)) return 3;
	auto hwnd = CreateWindowExW(
	    WS_EX_NOACTIVATE, OWNER_CLASS, L"", WS_POPUP, 0, 0, 1, 1,
	    nullptr, nullptr, cls.hInstance, nullptr
	);
	if (!hwnd) return 4;
	std::printf("%llu\n", static_cast<unsigned long long>(reinterpret_cast<quintptr>(hwnd)));
	std::fflush(stdout);
	HANDLE events[] {stop, resume};
	if (!pumping && WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0) {
		DestroyWindow(hwnd);
		return 0;
	}
	while (MsgWaitForMultipleObjects(1, &stop, FALSE, INFINITE, QS_ALLINPUT) != WAIT_OBJECT_0) {
		MSG msg {};
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
	}
	DestroyWindow(hwnd);
	CloseHandle(resume);
	CloseHandle(stop);
	return 0;
}

class Owner {
public:
	~Owner() {
		if (this->stop) SetEvent(this->stop);
		if (this->resume) SetEvent(this->resume);
		if (this->process.state() != QProcess::NotRunning && !this->process.waitForFinished(3000)) {
			this->process.kill();
			this->process.waitForFinished(3000);
		}
		if (this->resume) CloseHandle(this->resume);
		if (this->stop) CloseHandle(this->stop);
	}

	bool start(bool pumping) {
		auto name = QStringLiteral("Local\\qs-hung-owner-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
		this->resume = CreateEventW(nullptr, TRUE, FALSE, (name + "-resume").toStdWString().c_str());
		this->stop = CreateEventW(nullptr, TRUE, FALSE, (name + "-stop").toStdWString().c_str());
		if (!this->resume || !this->stop) return false;
		this->process.start(QCoreApplication::applicationFilePath(),
		                    {"--window-owner", name, pumping ? "pump" : "hang"});
		if (!this->process.waitForStarted(3000) || !this->process.waitForReadyRead(3000)) return false;
		this->hwnd = reinterpret_cast<HWND>(this->process.readLine().trimmed().toULongLong());
		return this->hwnd && IsWindow(this->hwnd);
	}

	void recover() { SetEvent(this->resume); }
	HWND hwnd = nullptr;
	QProcess process;
	HANDLE resume = nullptr;
	HANDLE stop = nullptr;
};

AppBarChannel::Operation request(HWND target) {
	return [target](DWORD, APPBARDATA* data) -> UINT_PTR {
		auto result = SendMessageW(target, REQUEST, 0, 0);
		data->rc.right = static_cast<LONG>(result);
		return static_cast<UINT_PTR>(result);
	};
}
} // namespace

class AppBarCallTest: public QObject {
	Q_OBJECT
private slots:
	void responsivenessDetectsNewHangAndRecovery() {
		Owner owner;
		QVERIFY(owner.start(false));
		QElapsedTimer clock;
		clock.start();
		QVERIFY(!qs::windows::windowResponding(owner.hwnd));
		QVERIFY(clock.elapsed() < 1000);
		owner.recover();
		QTRY_VERIFY(qs::windows::windowResponding(owner.hwnd));
		QVERIFY(!qs::windows::windowResponding(nullptr));
	}

	void healthyOwnerPreservesResultAndOutput() {
		Owner owner;
		QVERIFY(owner.start(true));
		AppBarChannel channel;
		APPBARDATA data {};
		auto answer = channel.call(owner.hwnd, ABM_QUERYPOS, data, request(owner.hwnd));
		QVERIFY(answer);
		QCOMPARE(*answer, UINT_PTR(42));
		QCOMPARE(data.rc.right, 42);
	}

	void synchronousGuiCallbackIsNotStarved() {
		Owner owner;
		QVERIFY(owner.start(true));
		WNDCLASSW cls {};
		cls.lpfnWndProc = ownerProc;
		cls.hInstance = GetModuleHandleW(nullptr);
		cls.lpszClassName = OWNER_CLASS;
		RegisterClassW(&cls);
		auto hwnd = CreateWindowExW(
		    0, OWNER_CLASS, L"", WS_POPUP, 0, 0, 1, 1,
		    nullptr, nullptr, cls.hInstance, nullptr
		);
		QVERIFY(hwnd);
		AppBarChannel channel;
		APPBARDATA data {};
		auto result = channel.call(owner.hwnd, ABM_NEW, data, request(hwnd));
		DestroyWindow(hwnd);
		QVERIFY(result);
		QCOMPARE(*result, UINT_PTR(42));
	}

	void hungOwnerIsBoundedAndRecovers() {
		Owner owner;
		QVERIFY(owner.start(false));
		AppBarChannel channel;
		APPBARDATA data {};
		data.rc.right = 7;
		QElapsedTimer clock;
		clock.start();
		QVERIFY(!channel.call(owner.hwnd, ABM_QUERYPOS, data, request(owner.hwnd)));
		QVERIFY(clock.elapsed() < 1500);
		QCOMPARE(data.rc.right, 7);
		// A busy owner is rejected without spawning another stuck thread.
		clock.restart();
		QVERIFY(!channel.call(owner.hwnd, ABM_SETPOS, data, request(owner.hwnd)));
		QVERIFY(clock.elapsed() < 100);
		owner.recover();
		// A late completion must never write through the caller's old pointer.
		data.rc.right = 9;
		DWORD_PTR result = 0;
		QTRY_VERIFY(SendMessageTimeoutW(owner.hwnd, REQUEST, 0, 0,
		                               SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &result));
		QCOMPARE(data.rc.right, 9);
		QTRY_VERIFY(channel.call(owner.hwnd, ABM_QUERYPOS, data, request(owner.hwnd)).has_value());
		QCOMPARE(data.rc.right, 42);
	}

	void replacementDoesNotWaitForOldOwner() {
		Owner oldOwner, replacement;
		QVERIFY(oldOwner.start(false));
		QVERIFY(replacement.start(true));
		AppBarChannel channel;
		APPBARDATA data {};
		QVERIFY(!channel.call(oldOwner.hwnd, ABM_NEW, data, request(oldOwner.hwnd)));
		auto result = channel.call(replacement.hwnd, ABM_NEW, data, request(replacement.hwnd));
		QVERIFY(result);
		QCOMPARE(*result, UINT_PTR(42));
	}

	void outstandingWorkersAreCapped() {
		AppBarChannel channel;
		std::vector<std::unique_ptr<Owner>> owners;
		for (size_t i = 0; i < AppBarChannel::MAX_PENDING; ++i) {
			auto owner = std::make_unique<Owner>();
			QVERIFY(owner->start(false));
			APPBARDATA data {};
			QVERIFY(!channel.call(owner->hwnd, ABM_NEW, data, request(owner->hwnd)));
			owners.push_back(std::move(owner));
		}
		Owner replacement;
		QVERIFY(replacement.start(true));
		APPBARDATA data {};
		QElapsedTimer clock;
		clock.start();
		QVERIFY(!channel.call(replacement.hwnd, ABM_NEW, data, request(replacement.hwnd)));
		QVERIFY(clock.elapsed() < 100);
		owners.front()->recover();
		QTRY_VERIFY(channel.call(replacement.hwnd, ABM_NEW, data, request(replacement.hwnd)).has_value());
	}

	void workerExceptionDoesNotEscape() {
		Owner owner;
		QVERIFY(owner.start(true));
		AppBarChannel channel;
		APPBARDATA data {};
		data.rc.right = 7;
		QVERIFY(!channel.call(owner.hwnd, ABM_NEW, data, [](DWORD, APPBARDATA* data) -> UINT_PTR {
			data->rc.right = 99;
			throw 1;
		}));
		QCOMPARE(data.rc.right, 7);
		QVERIFY(channel.call(owner.hwnd, ABM_NEW, data, request(owner.hwnd)));
	}
};

int main(int argc, char** argv) {
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
	if (argc == 4 && qstrcmp(argv[1], "--window-owner") == 0)
		return ownerMain(QString::fromLocal8Bit(argv[2]), qstrcmp(argv[3], "pump") == 0);
	QCoreApplication app(argc, argv);
	AppBarCallTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "appbar_call_test.moc"

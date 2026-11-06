#include <memory>
#include <thread>

#include <QtTest/QtTest>

#include "../native_callback_gate.hpp"
#include "../services/message_window.hpp"

using qs::windows::NativeCallbackGate;
using qs::windows::services::ServiceMessageWindow;

class NativeLifetimeTest: public QObject {
	Q_OBJECT;

private slots:
	void messageHandlersLeaveWithTheirOwner() {
		auto* window = ServiceMessageWindow::instance();
		QVERIFY(window->hwnd());
		constexpr UINT message = WM_APP + 47;
		int calls = 0;
		for (int i = 0; i < 100; ++i) {
			auto owner = std::make_unique<QObject>();
			auto payload = std::make_shared<int>(0);
			std::weak_ptr<int> weak = payload;
			window->addHandler(message, owner.get(), [payload, &calls](WPARAM, LPARAM) { ++calls; });
			payload.reset();
			SendMessageW(window->hwnd(), message, 0, 0);
			QCOMPARE(calls, i + 1);
			owner.reset();
			QVERIFY(weak.expired());
			SendMessageW(window->hwnd(), message, 0, 0);
			QCOMPARE(calls, i + 1);
		}
	}

	void messageHandlerMayDestroyItsOwner() {
		auto* window = ServiceMessageWindow::instance();
		constexpr UINT message = WM_APP + 48;
		auto owner = std::make_unique<QObject>();
		int calls = 0;
		window->addHandler(message, owner.get(), [&](WPARAM, LPARAM) { ++calls; owner.reset(); });
		window->addHandler(message, owner.get(), [&](WPARAM, LPARAM) { ++calls; });
		SendMessageW(window->hwnd(), message, 0, 0);
		QCOMPARE(calls, 1);
	}

	void nativeCallbackCanOutliveReceiver() {
		auto receiver = std::make_unique<QObject>();
		auto gate = std::make_shared<NativeCallbackGate<QObject>>(receiver.get());
		int calls = 0;
		std::thread callback([gate, &calls] { gate->post([&calls](QObject*) { ++calls; }); });
		callback.join();
		QCoreApplication::processEvents();
		QCOMPARE(calls, 1);
		gate->post([&calls](QObject*) { ++calls; });
		gate->close();
		receiver.reset();
		std::thread lateCallback([gate, &calls] { gate->post([&calls](QObject*) { ++calls; }); });
		lateCallback.join();
		QCoreApplication::processEvents();
		QCOMPARE(calls, 1);
	}

};

QTEST_GUILESS_MAIN(NativeLifetimeTest)
#include "native_lifetime_test.moc"

#include <chrono>
#include <functional>
#include <future>
#include <latch>
#include <thread>

#include <qcoreapplication.h>
#include <qdebug.h>
#include <qevent.h>
#include <qsignalspy.h>
#include <qstring.h>
#include <qtest.h>
#include <winrt/Windows.Foundation.Collections.h>

#include "adapter.hpp"
#include "backend_types.hpp"
#include "bluetooth.hpp"
#include "bt_worker.hpp"
#include "device.hpp"
#include "pairing_policy.hpp"
#include "pairing_request.hpp"
#include "radio_result.hpp"

using namespace qs::bluetooth;

namespace {
// Every flag WinRT's DevicePairingKinds defines, as (mask, name) pairs.
struct KindCase {
	quint32 mask;
	const char* name;
};

constexpr KindCase kAllKinds[] = {
    {pairingkinds::None, "None"},
    {pairingkinds::ConfirmOnly, "ConfirmOnly"},
    {pairingkinds::DisplayPin, "DisplayPin"},
    {pairingkinds::ProvidePin, "ProvidePin"},
    {pairingkinds::ConfirmPinMatch, "ConfirmPinMatch"},
    {pairingkinds::ProvidePasswordCredential, "ProvidePasswordCredential"},
    {pairingkinds::ProvideAddress, "ProvideAddress"},
};

// A COM fake is assigned to the real Entry::pairOp, so the worker calls this
// Cancel(). Completion is deliberately independent of cancellation delivery.
struct FakePairingOperation
    : winrt::implements<
          FakePairingOperation,
          winrt::Windows::Foundation::IAsyncOperation<
              winrt::Windows::Devices::Enumeration::DevicePairingResult>,
          winrt::Windows::Foundation::IAsyncInfo> {
	using Result = winrt::Windows::Devices::Enumeration::DevicePairingResult;
	using Handler = winrt::Windows::Foundation::AsyncOperationCompletedHandler<Result>;
	using AsyncStatus = winrt::Windows::Foundation::AsyncStatus;

	std::shared_ptr<PairingAttemptState> state;
	std::function<void(qint32)> completed;
	Handler handler {nullptr};
	int cancelCalls = 0;
	bool throwCancel = false;
	bool canceledWhileActive = false;
	bool finished = false;

	uint32_t Id() const { return 1; }
	AsyncStatus Status() const { return finished ? AsyncStatus::Completed : AsyncStatus::Started; }
	winrt::hresult ErrorCode() const { return {}; }
	void Cancel() {
		++cancelCalls;
		// Also verifies that native cancellation runs outside the attempt lock.
		std::lock_guard lock(state->mutex);
		canceledWhileActive = state->active;
		if (throwCancel) throw winrt::hresult_error(winrt::hresult(0x80004005u));
	}
	void Close() const {}
	void Completed(const Handler& value) { handler = value; }
	Handler Completed() const { return handler; }
	Result GetResults() const { throw winrt::hresult_not_implemented(); }

	void finish(qint32 result) {
		finished = true;
		completed(result);
	}
};

struct FakeConfirmOnlyArgs {
	std::shared_ptr<PairingAttemptState> state;
	mutable int accepts = 0;
	mutable bool acceptedAfterInvalidation = false;

	quint32 PairingKind() const { return pairingkinds::ConfirmOnly; }
	void Accept() const {
		++accepts;
		// The production seam holds state->mutex throughout Accept().
		acceptedAfterInvalidation = !state->active;
	}
};

// Watcher event payloads only. No WinRT activation or device access is needed.
using namespace winrt::Windows::Devices::Enumeration;
using FakeProperties = winrt::Windows::Foundation::Collections::IMapView<
    winrt::hstring,
    winrt::Windows::Foundation::IInspectable>;
constexpr auto kWatcherId = L"fixture-aabbccddeeff";

struct FakeDeviceInformation: winrt::implements<FakeDeviceInformation, IDeviceInformation> {
	winrt::hstring Id() const { return kWatcherId; }
	winrt::hstring Name() const { return L"Fixture device"; }
	bool IsEnabled() const { return true; }
	bool IsDefault() const { return false; }
	winrt::Windows::Devices::Enumeration::EnclosureLocation EnclosureLocation() const {
		return nullptr;
	}
	FakeProperties Properties() const { return nullptr; }
	void Update(const DeviceInformationUpdate&) const { throw winrt::hresult_not_implemented(); }
	winrt::Windows::Foundation::IAsyncOperation<DeviceThumbnail> GetThumbnailAsync() const {
		throw winrt::hresult_not_implemented();
	}
	winrt::Windows::Foundation::IAsyncOperation<DeviceThumbnail> GetGlyphThumbnailAsync() const {
		throw winrt::hresult_not_implemented();
	}
};

struct FakeDeviceInformationUpdate
    : winrt::implements<FakeDeviceInformationUpdate, IDeviceInformationUpdate> {
	winrt::hstring Id() const { return kWatcherId; }
	FakeProperties Properties() const { return nullptr; }
};
} // namespace

// This fixture never constructs BtBackend, starts a worker, or creates WinRT
// devices. Fake completion delivery uses the real worker gate and terminal path.
namespace qs::bluetooth {
struct BluetoothTestAccess {
	WinBluetooth frontend {false};
	BtWorker worker {&frontend};
	QString key = QStringLiteral("aa:bb:cc:dd:ee:ff");
	BluetoothDevice* device;
	QHash<quint64, winrt::com_ptr<FakePairingOperation>> operations;

	BluetoothTestAccess() {
		DeviceSnapshot snapshot;
		snapshot.key = key;
		frontend.backendDeviceChanged(snapshot);
		device = frontend.mDeviceMap.value(key);
		auto entry = std::make_unique<BtWorker::Entry>();
		entry->key = key;
		worker.mEntries.emplace(key, std::move(entry));
	}

	quint64 begin(bool activeWorker = true) {
		auto attempt = device->beginPairing();
		if (activeWorker) {
			auto* entry = worker.findEntry(key);
			entry->pairingAttempt = attempt;
			entry->pairingState = std::make_shared<PairingAttemptState>();
			entry->pairingKind.clear();
			auto op = winrt::make_self<FakePairingOperation>();
			op->state = entry->pairingState;
			op->completed = [gate = worker.mGate, key = key, attempt](qint32 result) {
				gate->post([key, attempt, result](BtWorker* self) {
					self->onPairCompleted(
					    key,
					    attempt,
					    winrt::Windows::Foundation::AsyncStatus::Completed,
					    static_cast<winrt::Windows::Devices::Enumeration::DevicePairingResultStatus>(result)
					);
				});
			};
			entry->pairOp = op.as<winrt::Windows::Foundation::IAsyncOperation<
			    winrt::Windows::Devices::Enumeration::DevicePairingResult>>();
			operations.insert(attempt, op);
		}
		return attempt;
	}

	void complete(quint64 attempt, qint32 result) { operations.value(attempt)->finish(result); }
	void cancel(quint64 attempt) {
		worker.mGate->post([key = key, attempt](BtWorker* self) { self->cmdCancelPair(key, attempt); });
	}
	void kind(quint64 attempt, const QString& kind) {
		worker.mGate->post([key = key, attempt, kind](BtWorker* self) {
			self->notePairingKind(key, attempt, kind);
		});
	}
	void remove() {
		worker.findEntry(key)->published = true;
		worker.removeEntry(key);
	}
	void watcherAdded() {
		worker.mRemovalGraceMs = 1;
		worker.onAdded(
		    BtWorker::UnpairedClassic,
		    winrt::make<FakeDeviceInformation>().as<DeviceInformation>()
		);
	}
	void watcherRemoved() {
		worker.onRemoved(
		    BtWorker::UnpairedClassic,
		    winrt::make<FakeDeviceInformationUpdate>().as<DeviceInformationUpdate>()
		);
	}
	bool containsEntry() { return worker.findEntry(key) != nullptr; }
	bool containsDevice() const { return frontend.mDeviceMap.contains(key); }
	void staleNotifications(quint64 attempt) {
		QMetaObject::invokeMethod(
		    &frontend,
		    [this, attempt] {
			    frontend.backendPairingKind(key, attempt, QStringLiteral("DisplayPin"));
			    frontend
			        .backendPairFinished(key, attempt, false, pairingresults::PairingCanceled, QString());
		    },
		    Qt::QueuedConnection
		);
	}
	void radioResult(bool requestAccess, const RadioResult& result) {
		if (requestAccess) worker.applyRadioAccess(result);
		else worker.onPowerCompleted(false, 1, result);
	}
	const AdapterSnapshot& radioSnapshot() const { return worker.mAdapterSnapshot; }
	static void flush() {
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
	}
};
} // namespace qs::bluetooth

class BtPairingPolicyTest: public QObject {
	Q_OBJECT;

private slots:
	void confirmOnlyIsTheOnlyAcceptedKind() {
		for (const auto& kind: kAllKinds) {
			auto decision = decidePairingPolicy(kind.mask);
			QCOMPARE(decision.kind, QString::fromLatin1(kind.name));

			if (kind.mask == pairingkinds::ConfirmOnly) {
				QVERIFY(decision.accept);
				QVERIFY(!decision.delegateToSystemSettings);
			} else {
				QVERIFY(!decision.accept);
				QVERIFY(decision.delegateToSystemSettings);
			}
		}
	}

	void kindCombinationsAndUnknownBitsAreDeclined() {
		auto combined = decidePairingPolicy(pairingkinds::ConfirmOnly | pairingkinds::ProvidePin);
		QVERIFY(!combined.accept);
		QVERIFY(combined.delegateToSystemSettings);
		QCOMPARE(combined.kind, QStringLiteral("ConfirmOnly|ProvidePin"));

		auto unknown = decidePairingPolicy(0x40);
		QVERIFY(!unknown.accept);
		QCOMPARE(unknown.kind, QStringLiteral("Unknown"));

		auto mixed = pairingkinds::DisplayPin | pairingkinds::ConfirmPinMatch | 0x80;
		QCOMPARE(decidePairingPolicy(mixed).kind, QStringLiteral("DisplayPin|ConfirmPinMatch|Unknown"));
	}

	void nativeRequestSeamAdvertisesOnlyConfirmOnly() {
		struct FakeCustomPairing {
			mutable winrt::Windows::Devices::Enumeration::DevicePairingKinds advertised {};
			int PairAsync(
			    winrt::Windows::Devices::Enumeration::DevicePairingKinds kinds,
			    winrt::Windows::Devices::Enumeration::DevicePairingProtectionLevel protection
			) const {
				advertised = kinds;
				return protection
				    == winrt::Windows::Devices::Enumeration::DevicePairingProtectionLevel::Default;
			}
		} custom;
		QCOMPARE(startPairingOperation(custom), 1);
		QCOMPARE(
		    custom.advertised,
		    winrt::Windows::Devices::Enumeration::DevicePairingKinds::ConfirmOnly
		);
	}

	void nativeRequestSeamNeverReadsSecrets() {
		struct FakeRequestArgs {
			quint32 kind;
			mutable int accepts = 0;
			mutable int pinReads = 0;
			mutable int kindReads = 0;
			quint32 PairingKind() const {
				++kindReads;
				return kind;
			}
			void Accept() const { ++accepts; }
			QString Pin() const {
				++pinReads;
				return QStringLiteral("SECRET-CANARY-987654");
			}
		};
		for (auto kind:
		     {pairingkinds::None,
		      pairingkinds::ConfirmOnly,
		      pairingkinds::DisplayPin,
		      pairingkinds::ProvidePin,
		      pairingkinds::ConfirmPinMatch,
		      pairingkinds::ProvidePasswordCredential,
		      pairingkinds::ProvideAddress,
		      pairingkinds::ConfirmOnly | pairingkinds::DisplayPin,
		      quint32(0x80)})
		{
			FakeRequestArgs args {kind};
			QString log;
			PairingAttemptState state;
			auto decision = answerPairingCeremony(
			    args,
			    state,
			    [&](quint32 observed, const PairingPolicyDecision& decision) {
				    log = pairingCeremonyLogText(observed, decision.accept);
			    }
			);
			QVERIFY(decision.has_value());
			QCOMPARE(args.accepts, kind == pairingkinds::ConfirmOnly ? 1 : 0);
			QCOMPARE(args.pinReads, 0);
			QVERIFY(!log.contains(QStringLiteral("SECRET-CANARY-987654")));
			if (!decision->accept) QVERIFY(log.contains(QStringLiteral("Windows Settings")));
			state.invalidate();
			int reports = 0;
			auto reads = args.kindReads;
			auto accepts = args.accepts;
			QVERIFY(!answerPairingCeremony(args, state, [&](quint32, const PairingPolicyDecision&) {
				++reports;
			}));
			QCOMPARE(args.kindReads, reads);
			QCOMPARE(args.accepts, accepts);
			QCOMPARE(args.pinReads, 0);
			QCOMPARE(reports, 0);
		}
	}

	void everyResultStatusMapsToAName() {
		struct StatusCase {
			qint32 status;
			const char* name;
			bool succeeded;
		};

		constexpr StatusCase cases[] = {
		    {0, "Paired", true},
		    {1, "NotReadyToPair", false},
		    {2, "NotPaired", false},
		    {3, "AlreadyPaired", true},
		    {4, "ConnectionRejected", false},
		    {5, "TooManyConnections", false},
		    {6, "HardwareFailure", false},
		    {7, "AuthenticationTimeout", false},
		    {8, "AuthenticationNotAllowed", false},
		    {9, "AuthenticationFailure", false},
		    {10, "NoSupportedProfiles", false},
		    {11, "ProtectionLevelCouldNotBeMet", false},
		    {12, "AccessDenied", false},
		    {13, "InvalidCeremonyData", false},
		    {14, "PairingCanceled", false},
		    {15, "OperationAlreadyInProgress", false},
		    {16, "RequiredHandlerNotRegistered", false},
		    {17, "RejectedByHandler", false},
		    {18, "RemoteDeviceHasAssociation", false},
		    {19, "Failed", false},
		};

		for (const auto& item: cases) {
			QCOMPARE(pairingResultStatusName(item.status), QString::fromLatin1(item.name));
			QCOMPARE(pairingResultSucceeded(item.status), item.succeeded);
		}

		QCOMPARE(pairingResultStatusName(-1), QStringLiteral("Failed"));
		QCOMPARE(pairingResultStatusName(20), QStringLiteral("Failed"));
		QCOMPARE(pairingResultStatusName(9999), QStringLiteral("Failed"));
		QVERIFY(!pairingResultSucceeded(20));
	}

	void workerCompleteThenCancelIsExactlyOnce() {
		BluetoothTestAccess fixture;
		QSignalSpy finished(fixture.device, &BluetoothDevice::pairingFinished);
		auto attempt = fixture.begin();
		fixture.kind(attempt, QStringLiteral("ConfirmOnly"));
		fixture.complete(attempt, pairingresults::Paired);
		fixture.cancel(attempt);
		fixture.flush();
		QCOMPARE(finished.count(), 1);
		QCOMPARE(finished.at(0).at(1).toString(), QStringLiteral("Paired"));
		QCOMPARE(finished.at(0).at(2).toString(), QStringLiteral("ConfirmOnly"));
		QVERIFY(!fixture.device->pairing());
		fixture.cancel(attempt);
		fixture.flush();
		QCOMPARE(finished.count(), 1);
	}

	void workerCancelThenCompleteIsExactlyOnce() {
		BluetoothTestAccess fixture;
		QSignalSpy finished(fixture.device, &BluetoothDevice::pairingFinished);
		auto attempt = fixture.begin();
		fixture.kind(attempt, QStringLiteral("ProvidePin"));
		fixture.cancel(attempt);
		fixture.complete(attempt, pairingresults::Paired);
		fixture.flush();
		QCOMPARE(finished.count(), 1);
		QCOMPARE(finished.at(0).at(0).toBool(), false);
		QCOMPARE(finished.at(0).at(1).toString(), QStringLiteral("PairingCanceled"));
		QCOMPARE(finished.at(0).at(2).toString(), QStringLiteral("ProvidePin"));
		QVERIFY(!fixture.device->pairing());
		QCOMPARE(fixture.operations.value(attempt)->cancelCalls, 1);
		QVERIFY(!fixture.operations.value(attempt)->canceledWhileActive);
	}

	void workerCancellationWaitsForCheckedCeremony() {
		BluetoothTestAccess fixture;
		QSignalSpy finished(fixture.device, &BluetoothDevice::pairingFinished);
		auto attempt = fixture.begin();
		auto op = fixture.operations.value(attempt);
		FakeConfirmOnlyArgs args {op->state};
		std::latch checked {1};
		std::latch resume {1};
		std::promise<void> cancelReturned;
		auto canceled = cancelReturned.get_future();
		bool cancellationWaited = false;
		std::jthread callback([&] {
			answerPairingCeremony(args, *op->state, [&](quint32, const PairingPolicyDecision&) {
				// This report seam runs after the active check, before Accept().
				checked.count_down();
				resume.wait();
			});
		});
		checked.wait();
		std::jthread releaseCallback([&] {
			// Cancellation must wait for the callback's critical section. A
			// check-then-act implementation would return while it is paused.
			cancellationWaited =
			    canceled.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
			resume.count_down();
		});
		fixture.worker.cmdCancelPair(fixture.key, attempt);
		cancelReturned.set_value();
		callback.join();
		releaseCallback.join();
		fixture.flush();
		QVERIFY(cancellationWaited);
		QCOMPARE(args.accepts, 1); // Serialized before invalidation.
		QVERIFY(!args.acceptedAfterInvalidation);
		QVERIFY(!op->state->active);
		QCOMPARE(op->cancelCalls, 1);
		QVERIFY(!op->canceledWhileActive);
		fixture.complete(attempt, pairingresults::Paired);
		fixture.flush();
		QCOMPARE(finished.count(), 1);
		QCOMPARE(finished.at(0).at(1).toString(), QStringLiteral("PairingCanceled"));
	}

	void workerInvalidatedAttemptRejectsDelayedCeremony() {
		BluetoothTestAccess fixture;
		QSignalSpy finished(fixture.device, &BluetoothDevice::pairingFinished);
		auto attempt = fixture.begin();
		auto op = fixture.operations.value(attempt);
		FakeConfirmOnlyArgs args {op->state};
		std::latch ready {1};
		std::latch resume {1};
		int reports = 0;
		std::jthread callback([&] {
			ready.count_down();
			resume.wait();
			answerPairingCeremony(args, *op->state, [&](quint32, const PairingPolicyDecision&) {
				++reports;
			});
		});
		ready.wait();
		fixture.worker.cmdCancelPair(fixture.key, attempt);
		fixture.flush();
		resume.count_down();
		callback.join();
		QCOMPARE(args.accepts, 0);
		QCOMPARE(reports, 0);
		QCOMPARE(op->cancelCalls, 1);
		QCOMPARE(finished.count(), 1);
	}

	void workerDelayedNativeCancellationIsExactlyOnce() {
		BluetoothTestAccess fixture;
		QSignalSpy finished(fixture.device, &BluetoothDevice::pairingFinished);
		auto attempt = fixture.begin();
		auto op = fixture.operations.value(attempt);
		fixture.cancel(attempt);
		fixture.flush();
		QCOMPARE(op->cancelCalls, 1);
		QVERIFY(!op->finished);
		QVERIFY(!fixture.device->pairing());
		QCOMPARE(finished.count(), 1);
		QCOMPARE(finished.at(0).at(1).toString(), QStringLiteral("PairingCanceled"));
		fixture.cancel(attempt);
		fixture.complete(attempt, pairingresults::PairingCanceled);
		fixture.flush();
		QCOMPARE(op->cancelCalls, 1);
		QCOMPARE(finished.count(), 1);
	}

	void workerThrowingNativeCancelIsExactlyOnce() {
		BluetoothTestAccess fixture;
		QSignalSpy finished(fixture.device, &BluetoothDevice::pairingFinished);
		auto attempt = fixture.begin();
		auto op = fixture.operations.value(attempt);
		op->throwCancel = true;
		fixture.cancel(attempt);
		fixture.flush();
		QCOMPARE(op->cancelCalls, 1);
		QVERIFY(!op->canceledWhileActive);
		QVERIFY(!fixture.device->pairing());
		QCOMPARE(finished.count(), 1);
		QCOMPARE(finished.at(0).at(1).toString(), QStringLiteral("PairingCanceled"));
		fixture.complete(attempt, pairingresults::Paired);
		fixture.cancel(attempt);
		fixture.flush();
		QCOMPARE(op->cancelCalls, 1);
		QCOMPARE(finished.count(), 1);
	}

	void workerRetryBeforeNativeCompletionIsIsolated() {
		BluetoothTestAccess fixture;
		QSignalSpy finished(fixture.device, &BluetoothDevice::pairingFinished);
		auto first = fixture.begin();
		fixture.cancel(first);
		fixture.flush();
		QCOMPARE(finished.count(), 1);
		auto oldOp = fixture.operations.value(first);
		QVERIFY(!oldOp->finished);
		QCOMPARE(oldOp->cancelCalls, 1);
		auto retry = fixture.begin();
		QVERIFY(retry != first);
		FakeConfirmOnlyArgs oldArgs {oldOp->state};
		answerPairingCeremony(oldArgs, *oldOp->state, [](quint32, const PairingPolicyDecision&) {});
		QCOMPARE(oldArgs.accepts, 0);
		fixture.complete(first, pairingresults::Paired);
		fixture.cancel(first);
		fixture.kind(first, QStringLiteral("DisplayPin"));
		fixture.staleNotifications(first);
		fixture.flush();
		QCOMPARE(finished.count(), 1);
		QVERIFY(fixture.device->pairing());
		QCOMPARE(fixture.operations.value(retry)->cancelCalls, 0);
		QCOMPARE(fixture.device->bindableLastPairingKind().value(), QString());
		fixture.kind(retry, QStringLiteral("ConfirmOnly"));
		fixture.complete(retry, pairingresults::AlreadyPaired);
		fixture.cancel(retry);
		fixture.flush();
		QCOMPARE(finished.count(), 2);
		QCOMPARE(finished.at(1).at(1).toString(), QStringLiteral("AlreadyPaired"));
		QCOMPARE(finished.at(1).at(2).toString(), QStringLiteral("ConfirmOnly"));
		QVERIFY(!fixture.device->pairing());
	}

	void workerDeviceRemovalCancelsAttempt() {
		BluetoothTestAccess fixture;
		QSignalSpy finished(fixture.device, &BluetoothDevice::pairingFinished);
		auto attempt = fixture.begin();
		auto op = fixture.operations.value(attempt);
		fixture.remove();
		fixture.flush();
		QVERIFY(!fixture.containsEntry());
		QVERIFY(!fixture.containsDevice());
		QCOMPARE(op->cancelCalls, 1);
		QVERIFY(!op->canceledWhileActive);
		QCOMPARE(finished.count(), 1);
		QCOMPARE(finished.at(0).at(1).toString(), QStringLiteral("PairingCanceled"));
		FakeConfirmOnlyArgs args {op->state};
		answerPairingCeremony(args, *op->state, [](quint32, const PairingPolicyDecision&) {});
		QCOMPARE(args.accepts, 0);
		fixture.complete(attempt, pairingresults::Paired);
		fixture.cancel(attempt);
		fixture.flush();
		QCOMPARE(finished.count(), 1);
		QCOMPARE(op->cancelCalls, 1);
	}

	void workerWatcherRemovalCancelsAttemptAfterGrace() {
		BluetoothTestAccess fixture;
		fixture.watcherAdded();
		fixture.flush();
		QSignalSpy finished(fixture.device, &BluetoothDevice::pairingFinished);
		QSignalSpy removed(fixture.frontend.devices(), &UntypedObjectModel::objectRemovedPost);
		auto attempt = fixture.begin();
		auto op = fixture.operations.value(attempt);

		fixture.watcherRemoved();
		QVERIFY(fixture.containsEntry());
		QVERIFY(op->state->active);
		QCOMPARE(op->cancelCalls, 0);
		QTRY_VERIFY_WITH_TIMEOUT(!fixture.containsEntry(), 1000);
		fixture.flush();
		QVERIFY(!fixture.containsDevice());
		QCOMPARE(removed.count(), 1);
		QVERIFY(!op->state->active);
		QCOMPARE(op->cancelCalls, 1);
		QVERIFY(!op->canceledWhileActive);
		QVERIFY(!op->finished);
		QCOMPARE(finished.count(), 1);
		QCOMPARE(finished.at(0).at(0).toBool(), false);
		QCOMPARE(finished.at(0).at(1).toString(), QStringLiteral("PairingCanceled"));

		FakeConfirmOnlyArgs args {op->state};
		int reports = 0;
		QVERIFY(!answerPairingCeremony(
		    args, *op->state, [&](quint32, const PairingPolicyDecision&) { ++reports; }
		));
		QCOMPARE(args.accepts, 0);
		QCOMPARE(reports, 0);
		fixture.complete(attempt, pairingresults::Paired);
		fixture.cancel(attempt);
		fixture.flush();
		QVERIFY(!fixture.containsEntry());
		QCOMPARE(finished.count(), 1);
		QCOMPARE(removed.count(), 1);
		QCOMPARE(op->cancelCalls, 1);
	}

	void workerWatcherReappearanceWithinGraceKeepsAttemptActive() {
		BluetoothTestAccess fixture;
		fixture.watcherAdded();
		fixture.flush();
		QSignalSpy finished(fixture.device, &BluetoothDevice::pairingFinished);
		QSignalSpy removed(fixture.frontend.devices(), &UntypedObjectModel::objectRemovedPost);
		auto attempt = fixture.begin();
		auto op = fixture.operations.value(attempt);

		fixture.watcherRemoved();
		fixture.watcherAdded();
		QTest::qWait(10); // Deliver the shortened removal timer after the Added event.
		fixture.flush();
		QVERIFY(fixture.containsEntry());
		QVERIFY(fixture.containsDevice());
		QVERIFY(op->state->active);
		QVERIFY(fixture.device->pairing());
		QCOMPARE(op->cancelCalls, 0);
		QCOMPARE(finished.count(), 0);
		QCOMPARE(removed.count(), 0);

		FakeConfirmOnlyArgs args {op->state};
		QVERIFY(answerPairingCeremony(
		    args, *op->state, [](quint32, const PairingPolicyDecision&) {}
		));
		QCOMPARE(args.accepts, 1);
		QVERIFY(!args.acceptedAfterInvalidation);
		fixture.complete(attempt, pairingresults::Paired);
		fixture.flush();
		QCOMPARE(finished.count(), 1);
		QCOMPARE(finished.at(0).at(1).toString(), QStringLiteral("Paired"));
		QCOMPARE(removed.count(), 0);
	}

	void workerPreCeremonyFailureCompletesOnce() {
		BluetoothTestAccess fixture;
		QSignalSpy finished(fixture.device, &BluetoothDevice::pairingFinished);
		auto attempt = fixture.begin(false);
		fixture.worker.cmdPair(fixture.key, attempt); // Empty fake transport, no native call.
		fixture.cancel(attempt);
		fixture.flush();
		QCOMPARE(finished.count(), 1);
		QCOMPARE(finished.at(0).at(1).toString(), QStringLiteral("NotReadyToPair"));
		QCOMPARE(finished.at(0).at(2).toString(), QString());
	}

	void radioHresultsSurviveAsyncAndThrownFailures() {
		using winrt::Windows::Devices::Radios::RadioAccessStatus;
		using winrt::Windows::Foundation::AsyncStatus;
		struct FakeOperation {
			winrt::hresult error;
			bool throwResults = false;
			RadioAccessStatus GetResults() const {
				if (throwResults) throw winrt::hresult_error(error);
				return RadioAccessStatus::Allowed;
			}
			winrt::hresult ErrorCode() const { return error; }
		};
		BluetoothTestAccess fixture;
		// Both native radio operations use readRadioResult and the same worker projection.
		for (bool requestAccess: {true, false}) {
			for (auto error:
			     {winrt::hresult(static_cast<qint32>(0x80070005u)),
			      winrt::hresult(static_cast<qint32>(0x80004005u))})
			{
				for (bool throws: {false, true}) {
					auto result = readRadioResult(
					    FakeOperation {error, throws},
					    throws ? AsyncStatus::Completed : AsyncStatus::Error
					);
					fixture.radioResult(requestAccess, result);
					QCOMPARE(
					    fixture.radioSnapshot().radioAccessDenied,
					    error == winrt::hresult(static_cast<qint32>(0x80070005u))
					);
					QCOMPARE(
					    fixture.radioSnapshot().lastError,
					    error == winrt::hresult(static_cast<qint32>(0x80070005u))
					        ? QStringLiteral("0x80070005")
					        : QStringLiteral("0x80004005")
					);
				}
				// Synchronous RequestAccessAsync/SetStateAsync throws enter with this same result.
				fixture.radioResult(
				    requestAccess,
				    {AsyncStatus::Error, RadioAccessStatus::Unspecified, error}
				);
				QVERIFY(!fixture.radioSnapshot().lastError.isEmpty());
			}
			fixture.radioResult(requestAccess, {AsyncStatus::Completed, RadioAccessStatus::DeniedByUser});
			QVERIFY(fixture.radioSnapshot().radioAccessDenied);
			fixture.radioResult(requestAccess, readRadioResult(FakeOperation {}, AsyncStatus::Completed));
			QVERIFY(!fixture.radioSnapshot().radioAccessDenied);
			QVERIFY(fixture.radioSnapshot().lastError.isEmpty());
		}
	}

	void debugOutputNeverLeaksBluetoothIdentifiers() {
		DeviceSnapshot deviceSnapshot;
		deviceSnapshot.key = QStringLiteral("aa:bb:cc:dd:ee:ff");
		deviceSnapshot.address = QStringLiteral("AA:BB:CC:DD:EE:FF");
		deviceSnapshot.name = QStringLiteral("Avery's Headphones");
		deviceSnapshot.deviceName = QStringLiteral("Headphones");

		BluetoothDevice device(deviceSnapshot);
		auto deviceText = QString();
		QDebug(&deviceText) << &device;

		QVERIFY(!deviceText.contains(QStringLiteral("aa:bb")));
		QVERIFY(!deviceText.contains(QStringLiteral("Headphones")));
		QVERIFY(deviceText.contains(QStringLiteral("redacted:")));

		AdapterSnapshot adapterSnapshot;
		adapterSnapshot.id = QStringLiteral("\\\\?\\USB#VID_1234&PID_5678#INSTANCE");
		adapterSnapshot.name = QStringLiteral("Intel Bluetooth");
		adapterSnapshot.power = RadioPower::On;

		BluetoothAdapter adapter(adapterSnapshot);
		auto adapterText = QString();
		QDebug(&adapterText) << &adapter;

		QVERIFY(!adapterText.contains(QStringLiteral("VID_1234")));
		QVERIFY(!adapterText.contains(QStringLiteral("Intel")));
		QVERIFY(adapterText.contains(QStringLiteral("redacted:")));
	}

	void adapterSurfacesRadioAccessDenialToQml() {
		AdapterSnapshot snapshot;
		snapshot.id = QStringLiteral("fixture-adapter");
		snapshot.name = QStringLiteral("Fixture Adapter");
		snapshot.power = RadioPower::On;
		snapshot.radioAccessDenied = true;
		snapshot.lastError = QStringLiteral("DeniedByUser");

		BluetoothAdapter adapter(snapshot);
		QVERIFY(adapter.bindableRadioAccessDenied().value());
		QCOMPARE(adapter.bindableLastError().value(), QStringLiteral("DeniedByUser"));

		// A later allowed access clears the denial.
		AdapterSnapshot allowed;
		allowed.id = snapshot.id;
		allowed.name = snapshot.name;
		allowed.power = RadioPower::On;
		adapter.applySnapshot(allowed);
		QVERIFY(!adapter.bindableRadioAccessDenied().value());
		QCOMPARE(adapter.bindableLastError().value(), QString());
	}
};

QTEST_GUILESS_MAIN(BtPairingPolicyTest);
#include "bt_pairing_policy_test.moc"

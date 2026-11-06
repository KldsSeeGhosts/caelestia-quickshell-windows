#pragma once

#include <mutex>
#include <optional>

#include <winrt/Windows.Devices.Enumeration.h>

#include "pairing_policy.hpp"

namespace qs::bluetooth {

// The handler retains this state even after its entry has finished or retried.
// Acceptance and invalidation share one critical section, so acceptance cannot
// start after the worker has invalidated the attempt.
struct PairingAttemptState {
	std::mutex mutex;
	bool active = true;

	void invalidate() {
		std::lock_guard lock(mutex);
		active = false;
	}
};

// DisplayPin can pair even when canceled. Never advertise a ceremony that
// requires secret material or confirmation UI hosted by Windows Settings.
template <typename CustomPairing>
auto startPairingOperation(const CustomPairing& custom) {
	using namespace winrt::Windows::Devices::Enumeration;
	return custom.PairAsync(DevicePairingKinds::ConfirmOnly, DevicePairingProtectionLevel::Default);
}

// Shared by the native handler and fake request args. No Pin() or secret-bearing
// Accept overload is used. Report the decision before accepting the ceremony.
template <typename RequestArgs, typename Report>
std::optional<PairingPolicyDecision>
answerPairingCeremony(const RequestArgs& args, PairingAttemptState& state, Report&& report) {
	std::lock_guard lock(state.mutex);
	if (!state.active) return std::nullopt;
	auto kind = static_cast<quint32>(args.PairingKind());
	auto decision = decidePairingPolicy(kind);
	report(kind, decision);
	if (decision.accept) args.Accept();
	return decision;
}

} // namespace qs::bluetooth

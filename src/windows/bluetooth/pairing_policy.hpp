#pragma once

#include <qstring.h>
#include <qtypes.h>

namespace qs::bluetooth {

// Pairing ceremony kinds as a plain flag mask mirroring WinRT's
// DevicePairingKinds values, so the pairing policy can be decided and tested
// without WinRT devices. bt_worker.cpp static_asserts the correspondence.
namespace pairingkinds {
// clang-format off
constexpr quint32 None                      = 0x0;
constexpr quint32 ConfirmOnly               = 0x1;
constexpr quint32 DisplayPin                = 0x2;
constexpr quint32 ProvidePin                = 0x4;
constexpr quint32 ConfirmPinMatch           = 0x8;
constexpr quint32 ProvidePasswordCredential = 0x10;
constexpr quint32 ProvideAddress            = 0x20;
// clang-format on
} // namespace pairingkinds

// DevicePairingResultStatus values as plain integers, mirroring WinRT's enum.
namespace pairingresults {
constexpr qint32 Paired = 0;
constexpr qint32 AlreadyPaired = 3;
constexpr qint32 PairingCanceled = 14;
constexpr qint32 Failed = 19;

// Used by the worker for attempts that fail before Windows sees a ceremony.
constexpr qint32 NotReadyToPair = 1;
} // namespace pairingresults

struct PairingPolicyDecision {
	/// Whether the backend may accept the ceremony by itself.
	bool accept = false;
	/// Whether the user should be sent to Windows Settings to finish pairing.
	bool delegateToSystemSettings = false;
	/// Short name of the observed kind, never empty.
	QString kind;
};

/// Decides how the backend answers a pairing ceremony. Only ConfirmOnly is
/// accepted automatically; every kind that involves a PIN, passkey, password
/// or address must be delegated to Windows Settings.
[[nodiscard]] PairingPolicyDecision decidePairingPolicy(quint32 pairingKind);

/// Short stable name for a kind mask: a single known flag maps to its name,
/// a combination maps to its names joined by '|', unknown bits map to
/// "Unknown" and an empty mask maps to "None".
[[nodiscard]] QString pairingKindName(quint32 pairingKind);

/// True for the result statuses that leave the device paired.
[[nodiscard]] bool pairingResultSucceeded(qint32 status);

/// Maps a DevicePairingResultStatus value to a short status string;
/// out-of-range values map to "Failed".
[[nodiscard]] QString pairingResultStatusName(qint32 status);

/// Log text for a ceremony decision. Deliberately takes no PIN/passkey
/// argument, so no secret material can ever reach the log.
[[nodiscard]] QString pairingCeremonyLogText(quint32 pairingKind, bool accepted);

} // namespace qs::bluetooth

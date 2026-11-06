#include "pairing_policy.hpp"

#include <qstring.h>
#include <qstringlist.h>
#include <qtypes.h>

namespace qs::bluetooth {

namespace {

struct NamedKind {
	quint32 flag;
	const char* name;
};

// Ordered by flag value; keep in sync with pairingkinds.
constexpr NamedKind KNOWN_KINDS[] = {
    {pairingkinds::ConfirmOnly, "ConfirmOnly"},
    {pairingkinds::DisplayPin, "DisplayPin"},
    {pairingkinds::ProvidePin, "ProvidePin"},
    {pairingkinds::ConfirmPinMatch, "ConfirmPinMatch"},
    {pairingkinds::ProvidePasswordCredential, "ProvidePasswordCredential"},
    {pairingkinds::ProvideAddress, "ProvideAddress"},
};

} // namespace

QString pairingKindName(quint32 pairingKind) {
	if (pairingKind == pairingkinds::None) return QStringLiteral("None");

	QStringList names;
	auto known = quint32(0);

	for (const auto& kind: KNOWN_KINDS) {
		if ((pairingKind & kind.flag) == kind.flag) {
			known |= kind.flag;
			names.append(QString::fromLatin1(kind.name));
		}
	}

	if (pairingKind & ~known) names.append(QStringLiteral("Unknown"));
	return names.join(QLatin1Char('|'));
}

PairingPolicyDecision decidePairingPolicy(quint32 pairingKind) {
	PairingPolicyDecision decision;
	decision.kind = pairingKindName(pairingKind);
	decision.accept = pairingKind == pairingkinds::ConfirmOnly;
	decision.delegateToSystemSettings = !decision.accept;
	return decision;
}

bool pairingResultSucceeded(qint32 status) {
	return status == pairingresults::Paired || status == pairingresults::AlreadyPaired;
}

QString pairingResultStatusName(qint32 status) {
	switch (status) {
	case 0: return QStringLiteral("Paired");
	case 1: return QStringLiteral("NotReadyToPair");
	case 2: return QStringLiteral("NotPaired");
	case 3: return QStringLiteral("AlreadyPaired");
	case 4: return QStringLiteral("ConnectionRejected");
	case 5: return QStringLiteral("TooManyConnections");
	case 6: return QStringLiteral("HardwareFailure");
	case 7: return QStringLiteral("AuthenticationTimeout");
	case 8: return QStringLiteral("AuthenticationNotAllowed");
	case 9: return QStringLiteral("AuthenticationFailure");
	case 10: return QStringLiteral("NoSupportedProfiles");
	case 11: return QStringLiteral("ProtectionLevelCouldNotBeMet");
	case 12: return QStringLiteral("AccessDenied");
	case 13: return QStringLiteral("InvalidCeremonyData");
	case 14: return QStringLiteral("PairingCanceled");
	case 15: return QStringLiteral("OperationAlreadyInProgress");
	case 16: return QStringLiteral("RequiredHandlerNotRegistered");
	case 17: return QStringLiteral("RejectedByHandler");
	case 18: return QStringLiteral("RemoteDeviceHasAssociation");
	case 19: return QStringLiteral("Failed");
	default: return QStringLiteral("Failed");
	}
}

QString pairingCeremonyLogText(quint32 pairingKind, bool accepted) {
	auto kind = pairingKindName(pairingKind);
	if (accepted) {
		return QStringLiteral("Pairing ceremony %1 accepted automatically").arg(kind);
	}

	return QStringLiteral(
	           "Pairing ceremony %1 declined: it needs a secret or a match the shell cannot host. Pair "
	           "the device from Windows Settings > Bluetooth & devices."
	)
	    .arg(kind);
}

} // namespace qs::bluetooth

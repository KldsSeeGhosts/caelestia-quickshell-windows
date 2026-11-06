#pragma once

#include <variant>

#include <qstring.h>
#include <qvariantmap.h>

namespace qs::windows {

enum class DesktopAvailability {
	Available,
	Disabled,
	UnsupportedBuild,
	Disconnected,
	PermissionDenied,
	MissingProvider,
};

enum class DesktopOperation { Switch, Create, Move, Remove, Pin };

inline QString desktopAvailabilityName(DesktopAvailability state) {
	switch (state) {
	case DesktopAvailability::Available: return QStringLiteral("available");
	case DesktopAvailability::Disabled: return QStringLiteral("disabled");
	case DesktopAvailability::PermissionDenied: return QStringLiteral("denied");
	default: return QStringLiteral("unavailable");
	}
}

inline QString desktopAvailabilityReason(DesktopAvailability state) {
	switch (state) {
	case DesktopAvailability::Available: return {};
	case DesktopAvailability::Disabled: return QStringLiteral("dispatch-disabled");
	case DesktopAvailability::UnsupportedBuild: return QStringLiteral("unsupported-build");
	case DesktopAvailability::Disconnected: return QStringLiteral("provider-disconnected");
	case DesktopAvailability::PermissionDenied: return QStringLiteral("permission-denied");
	case DesktopAvailability::MissingProvider: return QStringLiteral("provider-unavailable");
	}
	return {};
}

enum class DesktopCommandOutcome { Accepted, Observed, Denied, Superseded, TimedOut };

inline QString desktopCommandOutcomeName(DesktopCommandOutcome outcome) {
	switch (outcome) {
	case DesktopCommandOutcome::Accepted: return QStringLiteral("accepted");
	case DesktopCommandOutcome::Observed: return QStringLiteral("observed");
	case DesktopCommandOutcome::Denied: return QStringLiteral("denied");
	case DesktopCommandOutcome::Superseded: return QStringLiteral("superseded");
	case DesktopCommandOutcome::TimedOut: return QStringLiteral("timed-out");
	}
	return {};
}

struct DesktopRefusal {
	QString operation;
	QString reason;
};

struct DesktopUnobserved {
	QString operation;
};

struct DesktopRequestResult {
	quint64 id;
	QString operation;
	DesktopCommandOutcome outcome;
	QString reason;
};

// Synchronous compatibility commands have no pending request identity. Native
// acceptance still must not be represented as observed completion.
struct DesktopImmediateResult {
	QString operation;
	DesktopCommandOutcome outcome;
	QString reason;
};

using DesktopCommandResult =
    std::variant<std::monostate, DesktopRefusal, DesktopUnobserved, DesktopRequestResult,
                 DesktopImmediateResult>;

inline QVariantMap desktopCommandMap(const DesktopCommandResult& result) {
	if (const auto* refusal = std::get_if<DesktopRefusal>(&result)) {
		return {
		    {QStringLiteral("operation"), refusal->operation},
		    {QStringLiteral("outcome"), QStringLiteral("denied")},
		    {QStringLiteral("reason"), refusal->reason}
		};
	}
	if (const auto* request = std::get_if<DesktopRequestResult>(&result)) {
		return {
		    {QStringLiteral("operation"), request->operation},
		    {QStringLiteral("outcome"), desktopCommandOutcomeName(request->outcome)},
		    {QStringLiteral("reason"), request->reason},
		    {QStringLiteral("requestId"), QString::number(request->id)}
		};
	}
	if (const auto* unobserved = std::get_if<DesktopUnobserved>(&result)) {
		return {
		    {QStringLiteral("operation"), unobserved->operation},
		    {QStringLiteral("outcome"), QStringLiteral("unknown")},
		    {QStringLiteral("reason"), QStringLiteral("completion-unobserved")}
		};
	}
	if (const auto* immediate = std::get_if<DesktopImmediateResult>(&result)) {
		return {
		    {QStringLiteral("operation"), immediate->operation},
		    {QStringLiteral("outcome"), desktopCommandOutcomeName(immediate->outcome)},
		    {QStringLiteral("reason"), immediate->reason}
		};
	}
	return {{QStringLiteral("outcome"), QStringLiteral("none")}};
}

} // namespace qs::windows

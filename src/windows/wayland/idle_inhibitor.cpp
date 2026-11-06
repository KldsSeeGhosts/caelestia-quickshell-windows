#include "idle_inhibitor.hpp"

#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>

namespace qs::windows::wayland {

namespace {
Q_LOGGING_CATEGORY(logIdleInhibitor, "quickshell.windows.idleinhibitor", QtWarningMsg);

const IdleRequestFns kPowerFns {PowerSetRequest, PowerClearRequest};
}

IdleInhibitor::IdleInhibitor(QObject* parent): QObject(parent) {
	QObject::connect(this, &IdleInhibitor::enabledChanged, this, &IdleInhibitor::update);
}

IdleInhibitor::~IdleInhibitor() {
	if (this->request == nullptr) return;
	applyIdleRequest(this->request, false, this->state, kPowerFns);
	CloseHandle(this->request);
}

void IdleInhibitor::setWindow(QObject* window) {
	if (window == this->mWindow) return;
	this->mWindow = window;
	emit this->windowChanged();
}

void IdleInhibitor::setError(const QString& message) {
	if (message == this->lastError) return;
	this->lastError = message;
	emit this->errorChanged();
}

void IdleInhibitor::publish() {
	const bool now = this->state.held();
	if (now == this->heldNotified) return;
	this->heldNotified = now;
	emit this->heldChanged();
}

void IdleInhibitor::update() {
	const bool enable = this->bEnabled.value();
	if (enable && this->state.fullyHeld()) {
		this->setError({});
		return;
	}
	if (!enable && !this->state.held()) {
		this->setError({});
		return;
	}

	if (enable && this->request == nullptr) {
		auto reason = REASON_CONTEXT {};
		reason.Version = POWER_REQUEST_CONTEXT_VERSION;
		reason.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
		reason.Reason.SimpleReasonString = const_cast<LPWSTR>(L"Quickshell idle inhibitor"); // NOLINT
		this->request = PowerCreateRequest(&reason);
		if (this->request == INVALID_HANDLE_VALUE) {
			this->request = nullptr;
			qCWarning(logIdleInhibitor) << "PowerCreateRequest failed:" << GetLastError();
			this->setError(QStringLiteral("PowerCreateRequest failed"));
			this->publish();
			return;
		}
	}

	const bool ok = applyIdleRequest(this->request, enable, this->state, kPowerFns);
	this->publish();
	if (ok) {
		this->setError({});
		return;
	}
	const DWORD error = GetLastError();
	qCWarning(logIdleInhibitor) << (enable ? "PowerSetRequest failed:" : "PowerClearRequest failed:") << error;
	this->setError(enable ? QStringLiteral("PowerSetRequest failed") : QStringLiteral("PowerClearRequest failed"));
}

} // namespace qs::windows::wayland

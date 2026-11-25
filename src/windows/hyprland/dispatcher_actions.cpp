#include "dispatcher.hpp"

#include <optional>
#include <utility>

#include <qlogging.h>
#include <qloggingcategory.h>

#ifndef QS_TEST
#include "../tiling.hpp"
#include "../virtual_desktops.hpp"
#include "connection.hpp"
#include "hyprland_toplevel.hpp"
#endif
#include "dispatch_refusals.hpp"
#include "pin_support.hpp"

using namespace qs::windows;

namespace qs::hyprland::ipc {

namespace {
Q_LOGGING_CATEGORY(logDispatchAction, "quickshell.hyprland.dispatch", QtWarningMsg);
}

Dispatcher::Dispatcher(HyprlandIpc* ipc): ipc(ipc) {}

#ifdef QS_TEST
Dispatcher::Dispatcher(TestHooks hooks): ipc(nullptr), testHooks(std::move(hooks)) {}
#endif

WindowTracker* Dispatcher::actionTracker() const {
#ifdef QS_TEST
	if (this->testHooks.tracker != nullptr) return this->testHooks.tracker;
	return nullptr;
#else
	return this->ipc == nullptr ? nullptr : this->ipc->tracker();
#endif
}

bool Dispatcher::pinActionSupported() const {
#ifdef QS_TEST
	return this->testHooks.pinSupported ? this->testHooks.pinSupported() : false;
#else
	auto* desktops = this->ipc == nullptr ? nullptr : this->ipc->desktops();
	return desktops != nullptr && desktops->pinSupported();
#endif
}

std::optional<bool> Dispatcher::pinState(HWND hwnd) const {
#ifdef QS_TEST
	if (this->testHooks.queryPin) return this->testHooks.queryPin(hwnd);
	return std::nullopt;
#else
	auto* desktops = this->ipc == nullptr ? nullptr : this->ipc->desktops();
	if (desktops == nullptr) return std::nullopt;
	return desktops->isWindowPinned(hwnd);
#endif
}

bool Dispatcher::setPinState(HWND hwnd, bool pinned) {
#ifdef QS_TEST
	return this->testHooks.setPin ? this->testHooks.setPin(hwnd, pinned) : false;
#else
	auto* desktops = this->ipc == nullptr ? nullptr : this->ipc->desktops();
	return desktops != nullptr && desktops->pinWindow(hwnd, pinned);
#endif
}

std::optional<FloatingChangeResult>
Dispatcher::changeFloating(TrackedWindow* window, bool floating, bool toggle) const {
#ifdef QS_TEST
	if (this->testHooks.setFloating) return this->testHooks.setFloating(window, floating, toggle);
	return std::nullopt;
#else
	auto* tiling = TilingManager::active();
	if (tiling == nullptr) return std::nullopt;
	return tiling->setFloatingDetailed(window, floating, toggle);
#endif
}

void Dispatcher::pin(TrackedWindow* window, PinRequest request) {
	if (window == nullptr) return;

	const auto result = dispatchPinAccess(
	    [this] { return this->pinActionSupported(); },
	    [this](HWND hwnd) { return this->pinState(hwnd); },
	    [this](HWND hwnd, bool pinned) { return this->setPinState(hwnd, pinned); },
	    window->hwnd(),
	    request
	);

	if (result != PinDispatchResult::Succeeded) {
		if (result == PinDispatchResult::Unsupported) {
			this->unsupported("pin (needs VirtualDesktopAccessor.dll with pin exports)");
		} else if (result == PinDispatchResult::QueryFailed) {
			this->unsupported("pin (VirtualDesktopAccessor pin state query failed)");
		} else {
			this->unsupported("pin (VirtualDesktopAccessor call failed)");
		}
		reportPinRefusal(this->actionTracker(), result, window->elevated());
		return;
	}

#ifndef QS_TEST
	// Pinned is now read from the accessor, so refresh the window's ipc object.
	if (this->ipc != nullptr) {
		if (auto* toplevel = this->ipc->toplevelForWindow(window)) toplevel->markIpcObjectDirty();
	}
#endif
}

void Dispatcher::toggleFloating(TrackedWindow* window, const QString& action) {
	const auto result = this->changeFloating(window, action == "float", action == "toggle");
	if (!result.has_value()) {
		this->unsupported("togglefloating (without tiling)");
		reportToggleFloatingDisabled(
		    this->actionTracker(), window != nullptr && window->elevated()
		);
		return;
	}

	if (window == nullptr) return;
	reportToggleFloatingResult(this->actionTracker(), *result, window->elevated());
}

void Dispatcher::refuseImmediateMove(
    TrackedWindow* target,
    const QString& code,
    const std::function<void()>& publishCommandResult,
    quint64 existingMoveSequence,
    const std::shared_ptr<WindowActionOrderState>& existingActionOrder
) {
	auto* tracker = this->actionTracker();
	QPointer<WindowTracker> guardedTracker(tracker);
	const auto actionOrder = existingActionOrder != nullptr
	    ? existingActionOrder
	    : (target != nullptr ? target->actionOrderState() : nullptr);
	const auto moveSequence = existingMoveSequence != 0
	    ? existingMoveSequence
	    : (target != nullptr ? target->beginMoveRequest()
	                         : (tracker != nullptr ? tracker->nextActionSequence() : 0));
	auto orderSuperseded = [&] {
		return actionOrder != nullptr
		    && (actionOrder->latestMoveSequence != moveSequence
		        || actionOrder->lastActionErrorSequence >= moveSequence);
	};
	if (orderSuperseded()) return;
	if (publishCommandResult) publishCommandResult();
	if (orderSuperseded()) return;
	if (!guardedTracker || guardedTracker->lastActionErrorSequence() >= moveSequence) return;
	guardedTracker->reportActionRefusalWithSequence(
	    moveSequence, QStringLiteral("move"), moveRefusalReason(code, false), false, actionOrder
	);
}

void Dispatcher::unsupported(const QString& what) {
	if (this->warned.contains(what)) return;
	this->warned.insert(what);
	qCInfo(logDispatchAction) << "Dispatcher" << what
	                          << "has no Windows equivalent; ignored (further uses are not logged)";
}

} // namespace qs::hyprland::ipc

#pragma once

#include <qpointer.h>
#include <qtypes.h>

#include "../accessor_state.hpp"
#include "dispatcher_args.hpp"

namespace qs::hyprland::ipc {

// Pinned state on Windows comes from VirtualDesktopAccessor.dll. Without the
// complete pin API the shell must report `pinSupported == false` and `pinned == false`
// rather than claiming a pin state it cannot read or change.
struct PinCapability {
	bool supported = false;
	bool pinned = false;
};

[[nodiscard]] constexpr PinCapability pinCapability(bool pinSupported, bool accessorPinned) {
	return PinCapability {pinSupported, pinSupported && accessorPinned};
}

enum class PinDispatchResult { Unsupported, Failed, Succeeded };

// Shared with fake-accessor tests; no native tracker or desktop is constructed.
template <typename Desktops, typename Window>
PinDispatchResult
dispatchPin(Desktops& desktops, Window window, PinRequest request = PinRequest::Toggle) {
	if (!desktops.pinSupported()) return PinDispatchResult::Unsupported;

	auto pinned = desktops.isWindowPinned(window);
	auto desired = request == PinRequest::Pin ? true : request == PinRequest::Unpin ? false : !pinned;
	if (desired == pinned) return PinDispatchResult::Succeeded;
	if (!desktops.pinWindow(window, desired)) return PinDispatchResult::Failed;
	return PinDispatchResult::Succeeded;
}

// The QML facade uses this observation core directly. It can be exercised
// without constructing HyprlandIpc, a native tracker, or a native tiler.
class PinSupport: public QObject {
	Q_OBJECT;

public:
	explicit PinSupport(qs::windows::AccessorState* state): state(state) {
		QObject::connect(
		    state,
		    &qs::windows::AccessorState::accessorStateChanged,
		    this,
		    &PinSupport::pinSupportedChanged
		);
		QObject::connect(
		    state,
		    &qs::windows::AccessorState::accessorStateChanged,
		    this,
		    &PinSupport::desktopControlSupportedChanged
		);
	}

	[[nodiscard]] bool pinSupported() const {
		return this->state != nullptr && this->state->pinSupported();
	}
	[[nodiscard]] bool desktopControlSupported() const {
		return this->state != nullptr && this->state->desktopControlSupported();
	}

signals:
	void pinSupportedChanged();
	void desktopControlSupportedChanged();

private:
	QPointer<qs::windows::AccessorState> state;
};

} // namespace qs::hyprland::ipc

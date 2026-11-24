#pragma once

namespace qs::windows {

// Reverify the native attachment even when discovery found the same host.
// Matching geometry and child order do not mean the last frame survived.
template <typename Parent, typename Reattach, typename Place, typename Visible, typename Present>
void reconcileEmbeddedPanel(
    Parent parentBefore,
    Reattach reattach,
    Place place,
    Visible visible,
    Present present
) {
	auto parent = reattach();
	if (parent == nullptr) return;

	// A different parent has already been placed by the re-embed path.
	if (parent == parentBefore) place();
	if (visible()) present();
}

} // namespace qs::windows

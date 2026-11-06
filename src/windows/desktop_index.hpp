#pragma once

#include <qt_windows.h>

#include <qlist.h>
#include <qtypes.h>

namespace qs::windows {

// Resolves a desktop GUID to its position in an ordered desktop list.
//
// A null GUID never names a real desktop. The single-desktop fallback is the
// exception: when neither the registry nor the accessor reports any desktop
// ids, VirtualDesktops keeps one null placeholder entry for desktop 0. That
// exact list must resolve, or tiling treats every layout on the placeholder as
// dead and re-creates it on every sync.
template <typename Desktops>
qsizetype desktopIndexOf(const Desktops& desktops, const GUID& id) {
	static const GUID nullId {};

	if (IsEqualGUID(id, nullId)) {
		if (desktops.length() != 1) return -1;
		return IsEqualGUID(desktops.at(0).id, nullId) ? 0 : -1;
	}

	for (qsizetype i = 0; i < desktops.length(); i++) {
		if (IsEqualGUID(desktops.at(i).id, id)) return i;
	}

	return -1;
}

} // namespace qs::windows

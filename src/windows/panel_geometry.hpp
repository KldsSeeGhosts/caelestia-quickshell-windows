#pragma once

#include <cmath>

#include <qmargins.h>
#include <qnamespace.h>
#include <qrect.h>
#include <qregion.h>
#include <qsize.h>

namespace qs::windows {

// Explorer-embedded children can retain their parent's DPI. Recreating on
// that persistent window/screen mismatch feeds back through DPR notifications.
inline bool embeddedPanelNeedsScaleRecreate(qreal previousScreenDpr, qreal screenDpr, qreal windowDpr) {
	return !qFuzzyCompare(previousScreenDpr, screenDpr) && !qFuzzyCompare(windowDpr, screenDpr);
}

inline QRegion toPhysicalRegion(const QRegion& region, qreal dpr) {
	QRegion physical;
	for (const auto& rect: region) {
		auto left = static_cast<int>(std::floor(rect.x() * dpr));
		auto top = static_cast<int>(std::floor(rect.y() * dpr));
		auto right = static_cast<int>(std::ceil((rect.x() + rect.width()) * dpr));
		auto bottom = static_cast<int>(std::ceil((rect.y() + rect.height()) * dpr));
		physical += QRect(left, top, right - left, bottom - top);
	}
	return physical;
}

// Qt keeps a screen's native origin and scales only its extent. Derive the
// logical rectangle from the native monitor rectangle for the moment between
// WM_DISPLAYCHANGE, which already reports the new topology, and Qt's
// QScreen::geometryChanged, which still describes the old one.
inline QRect panelScreenRect(const QRect& nativeRect, qreal scale) {
	if (!nativeRect.isValid() || scale <= 0) return {};
	return QRect(
	    nativeRect.topLeft(),
	    QSize(qRound(nativeRect.width() / scale), qRound(nativeRect.height() / scale))
	);
}

// Re-evaluate against the current screen, even when QScreen* is unchanged.
inline QRect anchoredPanelGeometry(
    const QRect& base, const QSize& size, Qt::Edges anchors, const QMargins& margins
) {
	auto axis = [](int start, int length, int requested, bool first, bool last, int before, int after) {
		if (first && last) return QPair(start + before, length - before - after);
		if (first) return QPair(start + before, requested);
		if (last) return QPair(start + length - requested - after, requested);
		return QPair(start + length / 2 - requested / 2, requested);
	};
	auto x = axis(base.x(), base.width(), size.width(), anchors.testFlag(Qt::LeftEdge),
	              anchors.testFlag(Qt::RightEdge), margins.left(), margins.right());
	auto y = axis(base.y(), base.height(), size.height(), anchors.testFlag(Qt::TopEdge),
	              anchors.testFlag(Qt::BottomEdge), margins.top(), margins.bottom());
	return {x.first, y.first, x.second, y.second};
}

} // namespace qs::windows

#pragma once

#include <optional>

#include <qlist.h>
#include <qrect.h>
#include <qregularexpression.h>
#include <qstring.h>

namespace qs::hyprland::ipc {

// A command-local view of the existing monitor model, not another identity
// registry. Geometry is in one native desktop coordinate space, including on
// mixed-DPI desktops; never multiply Qt's per-screen logical origins by DPR.
struct MonitorFocusTarget {
	qint32 id;
	QString name;
	QRect geometry;
};

// Hyprland v0.54.0, Compositor.cpp getMonitorFromString/getMonitorInDirection:
// https://github.com/hyprwm/Hyprland/blob/0002f148c9a4fe421a9d33c0faa5528cdc411e62/src/Compositor.cpp
// Relative offsets wrap in live model order. Directions select an adjoining
// edge with the greatest overlap (first in model order breaks ties).
inline qsizetype resolveMonitorTarget(
    const QString& argument,
    const QList<MonitorFocusTarget>& monitors,
    qsizetype focused
) {
	const auto target = argument.trimmed();
	if (target.isEmpty() || monitors.isEmpty()) return -1;
	const auto hasFocus = focused >= 0 && focused < monitors.size();

	if (target == "l" || target == "r" || target == "u" || target == "d") {
		if (!hasFocus || !monitors[focused].geometry.isValid()) return -1;
		const auto source = monitors[focused].geometry;
		qsizetype best = -1;
		auto bestOverlap = -1;
		for (qsizetype i = 0; i < monitors.size(); ++i) {
			const auto rect = monitors[i].geometry;
			if (i == focused || !rect.isValid()) continue;
			const auto horizontal = target == "l" || target == "r";
			const auto adjacent =
			    target == "l" ? source.x() == rect.x() + rect.width()
			    : target == "r" ? source.x() + source.width() == rect.x()
			    : target == "u" ? source.y() == rect.y() + rect.height()
			                    : source.y() + source.height() == rect.y();
			if (!adjacent) continue;
			const auto overlap = horizontal
			    ? qMin(source.y() + source.height(), rect.y() + rect.height())
			        - qMax(source.y(), rect.y())
			    : qMin(source.x() + source.width(), rect.x() + rect.width())
			        - qMax(source.x(), rect.x());
			if (overlap > 0 && overlap > bestOverlap) {
				best = i;
				bestOverlap = overlap;
			}
		}
		return best;
	}

	static const QRegularExpression relative(QStringLiteral("^[+-][0-9]+$"));
	static const QRegularExpression number(QStringLiteral("^[0-9]+$"));
	if (target.startsWith('+') || target.startsWith('-')) {
		auto ok = false;
		const auto delta = target.toLongLong(&ok);
		if (!hasFocus || !ok || !relative.match(target).hasMatch()) return -1;
		return (focused + delta % monitors.size() + monitors.size()) % monitors.size();
	}
	if (number.match(target).hasMatch()) {
		auto ok = false;
		const auto id = target.toInt(&ok);
		if (!ok) return -1;
		for (qsizetype i = 0; i < monitors.size(); ++i) {
			if (monitors[i].id == id) return i;
		}
		return -1;
	}
	for (qsizetype i = 0; i < monitors.size(); ++i) {
		if (monitors[i].name == target) return i;
	}
	return -1;
}

template <typename Window, typename OnCurrent>
bool isFocusCandidate(Window* window, OnCurrent onCurrent) {
	return !window->minimized() && !window->cloaked() && onCurrent(window);
}

// Top-to-bottom tracked windows supplied by native EnumWindows, plus the window
// last focused on the target monitor. Hyprland focuses the window last focused
// there, not the topmost one: an always-on-top player would otherwise win over
// the window the user was working in. Stacking order is the fallback for a
// monitor without history, and for that window once it is gone.
// Activation stays owned by TrackedWindow, including modal redirection and
// source policy. There is no deferred activation for a later focus action to
// race with.
template <typename Windows, typename Window, typename Screen, typename OnCurrent, typename Focus>
std::optional<bool> focusMonitorWindows(
    Windows windows,
    Window* lastFocused,
    Screen screen,
    OnCurrent onCurrent,
    Focus focus
) {
	// Only a window present in the live enumeration is promoted.
	if (lastFocused != nullptr && windows.removeOne(lastFocused)) windows.prepend(lastFocused);

	for (auto* window: windows) {
		if (window->screen() != screen || !isFocusCandidate(window, onCurrent)) continue;
		if (!window->activate()) return false;
		focus();
		return true;
	}
	// An empty monitor changes the shared focused screen, not the foreground.
	focus();
	return std::nullopt;
}

} // namespace qs::hyprland::ipc

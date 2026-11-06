#pragma once

// Parsing and arithmetic shared by the Hyprland dispatchers. No HWND, tracker,
// tiler or desktop is touched here, so the cases can be tested directly.

#include <cmath>
#include <optional>

#include <qhash.h>
#include <qmath.h>
#include <qminmax.h>
#include <qregularexpression.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtypes.h>

namespace qs::hyprland::ipc {

// Only the default/prev cycle is supported. Do not silently ignore Hyprland's
// tiled/floating/history/visible filters or a Lua window selector.
inline std::optional<bool> parseClassicCycle(const QString& argument) {
	const auto value = argument.trimmed();
	if (value.isEmpty()) return false;
	if (value == "prev" || value == "p" || value == "last" || value == "l") return true;
	return std::nullopt;
}

// Lua's next=false is the backwards spelling:
// https://github.com/hyprwm/Hyprland/blob/a8a2c24c5bdcec8fc0f65eb24b2933488d90851e/src/config/lua/bindings/LuaBindingsDispatchers.cpp
inline std::optional<bool> parseLuaCycle(
    const QString& scalar,
    const QHash<QString, QString>& table,
    const QStringList& positional
) {
	if (!scalar.isEmpty() || !positional.isEmpty()) return std::nullopt;
	for (auto it = table.cbegin(); it != table.cend(); ++it) {
		if (it.key() != "next" || (it.value() != "true" && it.value() != "false")) {
			return std::nullopt;
		}
	}
	return table.value("next") == "false";
}

// The value of a `class:` / `title:` window selector. It matches the literal
// text, or the text read as a regular expression. A pattern that does not
// compile is never evaluated as one (it can still match literally), and its
// text is never reported: callers log a fixed category only.
class WindowPattern {
public:
	explicit WindowPattern(const QString& text)
	    : mText(text)
	    , mRegex(text) {}

	[[nodiscard]] bool validRegex() const { return this->mRegex.isValid(); }

	[[nodiscard]] bool matches(const QString& value) const {
		if (value == this->mText) return true;
		return this->mRegex.isValid() && this->mRegex.match(value).hasMatch();
	}

private:
	QString mText;
	QRegularExpression mRegex;
};

// A `workspace` / `movetoworkspace` target. `index` is zero-based; `delta` is
// the signed step of a relative target such as `r+1`.
struct WorkspaceTarget {
	enum class Kind { Invalid, Previous, Special, Name, Empty, Number, Relative };

	Kind kind = Kind::Invalid;
	qsizetype index = -1;
	qsizetype delta = 0;
	QString name;
};

struct WorkspaceResolution {
	qsizetype index = -1;
	bool create = false;
};

inline WorkspaceTarget parseWorkspaceTarget(const QString& arg) {
	auto text = arg.trimmed();
	WorkspaceTarget target;

	if (text.isEmpty()) return target;

	if (text == "previous" || text == "previous_per_monitor") {
		target.kind = WorkspaceTarget::Kind::Previous;
		return target;
	}

	if (text.startsWith("special")) {
		target.kind = WorkspaceTarget::Kind::Special;
		return target;
	}

	if (text.startsWith("name:")) {
		target.kind = WorkspaceTarget::Kind::Name;
		target.name = text.mid(5);
		return target;
	}

	if (text == "empty") {
		target.kind = WorkspaceTarget::Kind::Empty;
		return target;
	}

	auto relative = text;
	if (relative.startsWith('e') || relative.startsWith('r') || relative.startsWith('m')) {
		relative = relative.mid(1);
	}

	if (relative.startsWith('+') || relative.startsWith('-')) {
		auto ok = false;
		target.delta = relative.toInt(&ok);
		if (ok) target.kind = WorkspaceTarget::Kind::Relative;
		return target;
	}

	auto ok = false;
	auto number = text.toInt(&ok);
	if (ok && number >= 1) {
		target.kind = WorkspaceTarget::Kind::Number;
		target.index = number - 1;
	}

	return target;
}

inline WorkspaceResolution resolveWorkspaceTarget(
    const WorkspaceTarget& target,
    qsizetype current,
    qsizetype count,
    qsizetype limit,
    qsizetype previous
) {
	WorkspaceResolution result;

	if (target.kind == WorkspaceTarget::Kind::Previous) {
		result.index = previous;
		return result;
	}

	if (target.kind == WorkspaceTarget::Kind::Number) {
		result.index = target.index;
		result.create = true;
		return result;
	}

	if (target.kind != WorkspaceTarget::Kind::Relative) return result;

	auto cap = qMax(count, limit);
	if (cap < 1) cap = 1;

	result.index = qBound(qsizetype(0), current + target.delta, cap - 1);
	result.create = result.index >= count;
	return result;
}

struct MoveToWorkspaceArgs {
	QString workspace;
	QString window;
};

// `movetoworkspace <target>[,<window selector>]`
inline MoveToWorkspaceArgs parseMoveToWorkspaceArgs(const QString& args) {
	auto comma = args.indexOf(',');
	MoveToWorkspaceArgs parsed;
	parsed.workspace = comma == -1 ? args : args.left(comma);
	parsed.window = comma == -1 ? QString() : args.mid(comma + 1);
	return parsed;
}

// A `pin` request. Empty/unknown actions toggle, matching Hyprland's `pin`.
enum class PinRequest { Toggle, Pin, Unpin };

inline PinRequest parsePinRequest(const QString& action) {
	auto text = action.trimmed().toLower();
	if (text == "on" || text == "true" || text == "pin") return PinRequest::Pin;
	if (text == "off" || text == "false" || text == "unpin") return PinRequest::Unpin;
	return PinRequest::Toggle;
}

struct ResizeValue {
	bool valid = false;
	bool percent = false;
	double value = 0.0;
};

struct ResizeArguments {
	bool valid = false;
	bool exact = false;
	ResizeValue width;
	ResizeValue height;
};

inline ResizeValue parseResizeValue(QString text) {
	text = text.trimmed();
	ResizeValue value;
	if (text.isEmpty()) return value;

	auto percent = text.endsWith('%');
	if (percent) text.chop(1);

	auto ok = false;
	auto number = text.toDouble(&ok);
	// Reject nan/inf and sizes no monitor can have, so rounding can't overflow.
	value.valid = ok && std::isfinite(number) && std::abs(number) <= (percent ? 1000.0 : 100000.0);
	value.value = value.valid ? number : 0.0;
	value.percent = percent;
	return value;
}

// `resizeactive [exact] <width> <height>`, where each axis is a pixel delta or
// a percentage of the window (relative) or screen (exact).
inline ResizeArguments parseResizeArguments(const QString& args) {
	auto parts = args.split(' ', Qt::SkipEmptyParts);
	ResizeArguments parsed;

	if (!parts.isEmpty() && parts.first() == "exact") {
		parsed.exact = true;
		parts.removeFirst();
	}

	if (parts.length() < 2) return parsed;

	parsed.width = parseResizeValue(parts[0]);
	parsed.height = parseResizeValue(parts[1]);
	parsed.valid = parsed.width.valid && parsed.height.valid;
	return parsed;
}

// Exact mode treats a zero axis as "keep the current size" so a height-only
// exact resize cannot collapse the window to zero width.
inline qint32 resizeAxis(const ResizeValue& value, qint32 whole, qint32 current, bool exact) {
	if (!value.valid) return 0;
	auto amount = qRound(value.percent ? value.value * whole / 100.0 : value.value);
	return exact && amount == 0 ? current : amount;
}

} // namespace qs::hyprland::ipc

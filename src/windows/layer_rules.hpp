#pragma once

#include <optional>

#include <qbytearray.h>
#include <qlist.h>
#include <qregularexpression.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtypes.h>
#include <qvariant.h>

#include "blur_policy.hpp"
#include "panel_animation.hpp"

namespace qs::windows {

struct LayerRule {
	bool blur = false;
	std::optional<qreal> ignoreAlpha;
	PanelAnimationStyle animation;
	PanelEffect effect = PanelEffect::Auto;
	BackdropMaterial backdrop = BackdropMaterial::Auto;
};

class LayerRules {
public:
	[[nodiscard]] static LayerRules
	parse(const QByteArray& data, const QString& path, QStringList* warnings = nullptr);

	[[nodiscard]] LayerRule ruleFor(const QString& ns) const;
	[[nodiscard]] QVariantList info() const;

	[[nodiscard]] bool valid() const { return this->mValid; }
	[[nodiscard]] qsizetype size() const { return this->rules.size(); }
	[[nodiscard]] bool enabled() const { return this->mEnabled; }
	[[nodiscard]] bool layered() const { return this->mLayered; }

private:
	struct Rule {
		QString pattern;
		QRegularExpression regex;
		std::optional<bool> blur;
		std::optional<std::optional<qreal>> ignoreAlpha;
		std::optional<PanelAnimationStyle> animation;
		std::optional<PanelEffect> effect;
		std::optional<std::optional<BackdropMaterial>> backdrop;
	};

	QList<Rule> rules;
	bool mValid = false;
	bool mEnabled = true;
	bool mLayered = false;
};

} // namespace qs::windows

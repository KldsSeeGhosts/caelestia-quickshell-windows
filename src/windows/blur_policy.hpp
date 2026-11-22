#pragma once

#include <cstdint>

#include <qstringview.h>

namespace qs::windows {

enum class BackdropMaterial { Auto, Host, Acrylic, Mica, MicaAlt };
enum class BlurMode { Disabled, SystemAcrylic, SystemMica, SystemMicaAlt, HostBackdrop };

constexpr std::uint32_t HOST_BACKDROP_MIN_BUILD = 22000;
constexpr std::uint32_t SYSTEM_BACKDROP_MIN_BUILD = 22621;

struct BlurPolicyInput {
	std::uint32_t windowsBuild = 0;
	bool enabled = false;
	bool forceDisabled = false;
	BackdropMaterial requested = BackdropMaterial::Auto;
	bool hasIgnoreAlpha = false;
	bool hasInputMask = false;
	// Opacity affects whether the backdrop is shown, not which material is chosen.
	bool transparentWindow = true;
};

[[nodiscard]] constexpr BlurMode chooseBlurMode(const BlurPolicyInput& in) {
	if (!in.enabled || in.forceDisabled || in.windowsBuild < HOST_BACKDROP_MIN_BUILD) {
		return BlurMode::Disabled;
	}

	if (in.windowsBuild < SYSTEM_BACKDROP_MIN_BUILD) return BlurMode::HostBackdrop;

	switch (in.requested) {
	case BackdropMaterial::Host: return BlurMode::HostBackdrop;
	case BackdropMaterial::Acrylic: return BlurMode::SystemAcrylic;
	case BackdropMaterial::Mica: return BlurMode::SystemMica;
	case BackdropMaterial::MicaAlt: return BlurMode::SystemMicaAlt;
	case BackdropMaterial::Auto:
		return in.hasIgnoreAlpha || in.hasInputMask ? BlurMode::HostBackdrop : BlurMode::SystemAcrylic;
	}

	return BlurMode::Disabled;
}

[[nodiscard]] constexpr QStringView backdropMaterialName(BackdropMaterial material) {
	switch (material) {
	case BackdropMaterial::Auto: return u"auto";
	case BackdropMaterial::Host: return u"host";
	case BackdropMaterial::Acrylic: return u"acrylic";
	case BackdropMaterial::Mica: return u"mica";
	case BackdropMaterial::MicaAlt: return u"mica-alt";
	}

	return {};
}

[[nodiscard]] inline bool parseBackdropMaterial(QStringView text, BackdropMaterial* out) {
	constexpr BackdropMaterial materials[] = {
	    BackdropMaterial::Auto,
	    BackdropMaterial::Host,
	    BackdropMaterial::Acrylic,
	    BackdropMaterial::Mica,
	    BackdropMaterial::MicaAlt,
	};

	text = text.trimmed();
	for (auto material: materials) {
		if (text.compare(backdropMaterialName(material), Qt::CaseInsensitive) == 0) {
			if (out != nullptr) *out = material;
			return true;
		}
	}

	return false;
}

[[nodiscard]] constexpr QStringView blurModeName(BlurMode mode) {
	switch (mode) {
	case BlurMode::Disabled: return u"disabled";
	case BlurMode::SystemAcrylic: return u"system-acrylic";
	case BlurMode::SystemMica: return u"system-mica";
	case BlurMode::SystemMicaAlt: return u"system-mica-alt";
	case BlurMode::HostBackdrop: return u"host-backdrop";
	}

	return {};
}

} // namespace qs::windows

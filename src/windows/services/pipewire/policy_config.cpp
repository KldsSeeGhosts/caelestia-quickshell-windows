#include "policy_config.hpp"

#include <qstringlist.h>

#include "com_util.hpp"

namespace qs::windows::services::pipewire {

const CLSID CLSID_PolicyConfigClient = {
    0x870af99c,
    0x171d,
    0x4f9e,
    {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}
};

const IID IID_IPolicyConfig = {
    0xf8679f50,
    0x850a,
    0x41cf,
    {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}
};

QString setDefaultEndpointRoles(IPolicyConfig* config, const QString& endpointId) {
	const auto* wideId = reinterpret_cast<LPCWSTR>(endpointId.utf16());
	QStringList errors;
	for (auto role: {eConsole, eMultimedia, eCommunications}) {
		auto hr = config->SetDefaultEndpoint(wideId, role);
		if (SUCCEEDED(hr)) continue;
		const auto* name = role == eConsole    ? "eConsole"
		                 : role == eMultimedia ? "eMultimedia"
		                                       : "eCommunications";
		errors.append(audioControlError(QStringLiteral("SetDefaultEndpoint(%1)").arg(name), hr));
	}
	return errors.join(QStringLiteral("; "));
}

} // namespace qs::windows::services::pipewire

#pragma once

#include <algorithm>
#include <qjsonarray.h>
#include <qjsonobject.h>

#include "hotkeys.hpp"

namespace qs::windows::hotkeys {

inline void parseGlobalName(const QJsonObject& object, Bind& bind) {
	bind.name = object.value("name").toString();
	bind.appid = object.value("appid").toString("quickshell");

	if (auto colon = bind.name.indexOf(':'); colon != -1) {
		bind.appid = bind.name.left(colon);
		bind.name = bind.name.mid(colon + 1);
	}
}

inline QList<Bind> introducedDefaults(const QJsonArray& entries) {
	QList<Bind> introduced;
	for (const auto& entry: entries) {
		auto object = entry.toObject();
		if (!object.contains("since")) continue;
		Bind bind;
		parseGlobalName(object, bind);
		introduced.append(bind);
	}
	return introduced;
}

// Shared by the file loader and the offscreen hotkeys fixture. This is the
// shipped merge rule, not a test-side implementation of it.
inline void mergeDefaults(
    QList<Bind>& binds,
    const QList<Bind>& defaults,
    const QList<Bind>& introduced
) {
	const auto userBinds = binds;
	for (const auto& bind: defaults) {
		const auto isIntroduced = std::ranges::any_of(introduced, [&](const Bind& entry) {
			return entry.appid == bind.appid && entry.name == bind.name;
		});
		if (bind.action != Bind::Action::Global || !isIntroduced) continue;

		auto taken = std::ranges::any_of(userBinds, [&](const Bind& existing) {
			// A bind in another submap takes neither the chord nor the name.
			return existing.submap == bind.submap
			    && (existing.combo == bind.combo
			        || ((existing.action == Bind::Action::Global || existing.action == Bind::Action::Native)
			            && existing.name == bind.name && existing.appid == bind.appid));
		});

		if (!taken) binds.append(bind);
	}
}

} // namespace qs::windows::hotkeys

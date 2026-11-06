#pragma once

#include <qguiapplication.h>
#include <qtimer.h>

#include "screen_identity.hpp"

namespace qs::windows {

// Reused QScreens can change topology without add/remove. Reconcile once per
// burst, after all registry snapshots have been published.
class ScreenTopologyWatcher: public QTimer {
public:
	explicit ScreenTopologyWatcher(ScreenIdentityRegistry* registry, QObject* parent = nullptr)
	    : QTimer(parent) {
		this->setSingleShot(true);
		this->setInterval(0);
		auto restart = qOverload<>(&QTimer::start);
		QObject::connect(registry, &ScreenIdentityRegistry::screenChanged, this, restart);
		if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
			QObject::connect(app, &QGuiApplication::screenAdded, this, restart);
			QObject::connect(app, &QGuiApplication::screenRemoved, this, restart);
		}
	}
};

} // namespace qs::windows

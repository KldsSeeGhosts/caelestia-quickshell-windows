#include "fullscreen_occluders.hpp"

#include <qcoreapplication.h>

namespace qs::windows {

FullscreenOccluders* FullscreenOccluders::instance() {
	static auto* instance = new FullscreenOccluders(); // NOLINT
	if (instance->parent() == nullptr && QCoreApplication::instance() != nullptr) {
		instance->setParent(QCoreApplication::instance());
	}

	return instance;
}

bool FullscreenOccluders::active(QScreen* screen) const {
	return screen != nullptr && this->mActive.contains(screen);
}

void FullscreenOccluders::set(QScreen* screen, bool active) {
	if (screen == nullptr || this->mActive.contains(screen) == active) return;

	if (!active) {
		this->mActive.remove(screen);
	} else {
		this->mActive.insert(screen);

		// A screen address can be reused after a hot-unplug; never keep its state.
		if (!this->mWatched.contains(screen)) {
			this->mWatched.insert(screen);
			QObject::connect(screen, &QObject::destroyed, this, [this, screen]() {
				this->mActive.remove(screen);
				this->mWatched.remove(screen);
			});
		}
	}

	emit this->changed(screen);
}

} // namespace qs::windows

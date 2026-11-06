#include "window_surfaces.hpp"

#include <qloggingcategory.h>

#include "system_surface.hpp"
#include "window_tracker.hpp"

namespace qs::windows {

namespace {
Q_LOGGING_CATEGORY(logSurfaces, "quickshell.windows.surfaces", QtWarningMsg);
}

ShellSurfaces::ShellSurfaces(QObject* parent): QObject(parent) {
	this->mExtra = system_surface::extraShellSurfaceRules();
}

QVariant ShellSurfaces::extra() const { return this->mExtra; }

void ShellSurfaces::setExtra(const QVariant& extra) {
	auto config = system_surface::shellSurfaceRulesFromConfig(extra);
	if (config.ignored > 0) {
		qCWarning(logSurfaces) << "Ignored" << config.ignored
		                       << "non-string shell-surface selectors";
	}
	if (config.rules == this->mExtra) return;

	this->mExtra = config.rules;
	system_surface::setExtraShellSurfaceRules(this->mExtra);

	// Classified windows that are already tracked must follow a live change.
	// Do not construct the tracker just to store a list; a later first rescan
	// reads the policy anyway.
	if (auto* tracker = WindowTracker::instanceIfCreated()) tracker->rescan();

	// Rule text may carry user-chosen titles or executable names, so only the
	// count is logged.
	qCInfo(logSurfaces) << "Extra shell-surface rules:" << this->mExtra.size();
	emit this->extraChanged();
}

} // namespace qs::windows

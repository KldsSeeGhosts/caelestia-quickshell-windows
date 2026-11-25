#include <functional>

#include <qhash.h>
#include <qstring.h>
#include <qstringlist.h>

#include "../core/desktopentry.hpp"
#include "../desktopentry_backend.hpp"
#include "../process_launch.hpp"
#include "../util.hpp"

namespace {

QHash<QString, QString> gParsingNames;
QHash<QString, QString> gLinkTargets;
QHash<QString, QString> gShortcutPaths;

} // namespace

namespace qs::windows::sys::test {

void setParsingNameForId(const QString& id, const QString& token) {
	gParsingNames.insert(id, token);
}

void setLinkTargetForId(const QString& id, const QString& target) {
	gLinkTargets.insert(id, target);
}

void setShortcutPathForId(const QString& id, const QString& path) {
	gShortcutPaths.insert(id, path);
}

void clearParsingNames() {
	gParsingNames.clear();
	gLinkTargets.clear();
	gShortcutPaths.clear();
}

} // namespace qs::windows::sys::test

DesktopEntryManager::DesktopEntryManager() {}

DesktopEntryManager* DesktopEntryManager::instance() {
	static auto* instance = new DesktopEntryManager(); // NOLINT
	return instance;
}

DesktopEntry* DesktopEntryManager::byId(const QString&) { return nullptr; }
DesktopEntry* DesktopEntryManager::heuristicLookup(const QString&) { return nullptr; }
ObjectModel<DesktopEntry>* DesktopEntryManager::applications() { return &this->mApplications; }
void DesktopEntryManager::handleFileChanges() {}
void DesktopEntryManager::onScanCompleted(const QList<ParsedDesktopEntryData>&) {}

void DesktopEntry::execute() const {}
QVector<DesktopAction*> DesktopEntry::actions() const { return this->mActions; }
void DesktopAction::execute() const {}

DesktopEntries::DesktopEntries() = default;
DesktopEntry* DesktopEntries::byId(const QString& id) { return DesktopEntryManager::instance()->byId(id); }
DesktopEntry* DesktopEntries::heuristicLookup(const QString& name) {
	return DesktopEntryManager::instance()->heuristicLookup(name);
}
ObjectModel<DesktopEntry>* DesktopEntries::applications() {
	return DesktopEntryManager::instance()->applications();
}

namespace qs::windows {

QString WindowsDesktopEntryBackend::parsingNameForId(const QString& id) {
	return gParsingNames.value(id);
}

QString WindowsDesktopEntryBackend::linkTargetForId(const QString& id) { return gLinkTargets.value(id); }

QString WindowsDesktopEntryBackend::shortcutPathForId(const QString& id) {
	return gShortcutPaths.value(id);
}

unsigned long WindowsDesktopEntryBackend::launchSync(
    const QString&,
    const QString&,
    const QString&
) {
	return ERROR_NOT_SUPPORTED;
}

HWND hwndOf(const QWindow*) { return nullptr; }

void runLauncher(std::function<void()>) {}

namespace process {

LaunchResult shellExecuteTarget(
    const QString&,
    const QString&,
    const QString&,
    const QString&,
    HWND,
    bool,
    bool
) {
	return detail::launchResultForError(ERROR_NOT_SUPPORTED);
}

QString joinCommandArguments(const QStringList& arguments) { return arguments.join(u' '); }

} // namespace process

} // namespace qs::windows

#include <qdir.h>
#include <qfileinfo.h>
#include <qhash.h>
#include <qtest.h>
#include <quuid.h>

#include <objbase.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/base.h>

#include "../desktopentry_backend.hpp"
#include "app_identity.hpp"

using qs::windows::DesktopEntryAliasCandidate;
using qs::windows::WindowsDesktopEntryBackend;
using qs::windows::services::mpris::AppIdentityHint;
using qs::windows::services::mpris::resolveAppIdentity;

// The shell's desktop entry manager is the only backend consumer this fixture
// does not exercise; the scan and alias lookup below are the real ones.
void DesktopEntryManager::installBackend(DesktopEntryBackend*) {}

namespace {

QString normalizedPath(const QString& path) { return QDir::toNativeSeparators(path).toLower(); }

} // namespace

class MprisAppIdentityTest: public QObject {
	Q_OBJECT;

	bool comInitialized = false;
	bool aliasIndexScanned = false;
	QHash<QString, QString> aliasExecutables; // executable stem -> desktop entry
	QHash<QString, QString> aliasTargets;     // normalized executable path -> desktop entry

	void scanAliasIndex() {
		if (this->aliasIndexScanned) return;
		this->aliasIndexScanned = true;

		// Build the same pure index the scan publishes, not a copy of its
		// rules, so the rows below exercise the production ambiguity handling.
		QList<DesktopEntryAliasCandidate> candidates;
		for (const auto& entry: WindowsDesktopEntryBackend().scan()) {
			candidates.append({
			    entry.id,
			    WindowsDesktopEntryBackend::linkTargetForId(entry.id),
			    entry.execString,
			});
		}

		auto maps = WindowsDesktopEntryBackend::buildAliasMaps(candidates);
		this->aliasExecutables = maps.exeNames;
		this->aliasTargets = maps.targets;
	}

private slots:
	void initTestCase() {
		auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		QVERIFY2(SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE, "CoInitializeEx failed");
		this->comInitialized = SUCCEEDED(hr);
	}

	void cleanupTestCase() {
		if (this->comInitialized) CoUninitialize();
	}

	// The alias index is pure, so these rows need no COM item and no Start menu.
	void uniqueExecutableBasenameAliases() {
		const auto maps = WindowsDesktopEntryBackend::buildAliasMaps(
		    QList<DesktopEntryAliasCandidate> {
		        {QStringLiteral("alpha"), QStringLiteral("C:/Apps/Alpha/alpha.exe"), QString()},
		    }
		);

		QCOMPARE(maps.exeNames.value(QStringLiteral("alpha")), QStringLiteral("alpha"));
		QCOMPARE(
		    maps.targets.value(normalizedPath(QStringLiteral("C:/Apps/Alpha/alpha.exe"))),
		    QStringLiteral("alpha")
		);
	}

	void sharedExecutableBasenameIsAmbiguous() {
		// Squirrel's Update.exe and generic launcher/electron basenames are
		// shared by unrelated shortcuts. A basename alias would launch whichever
		// app registered first, so a shared basename must name nothing even
		// though each exact executable still does.
		const auto maps = WindowsDesktopEntryBackend::buildAliasMaps(
		    QList<DesktopEntryAliasCandidate> {
		        {QStringLiteral("alpha-shortcut"), QStringLiteral("C:/Apps/Alpha/Update.exe"),
		         QString()},
		        {QStringLiteral("beta-shortcut"), QStringLiteral("C:/Apps/Beta/Update.exe"),
		         QString()},
		    }
		);

		QVERIFY2(
		    !maps.exeNames.contains(QStringLiteral("update")),
		    "a basename shared by two applications must not alias"
		);
		QCOMPARE(
		    maps.targets.value(normalizedPath(QStringLiteral("C:/Apps/Alpha/Update.exe"))),
		    QStringLiteral("alpha-shortcut")
		);
		QCOMPARE(
		    maps.targets.value(normalizedPath(QStringLiteral("C:/Apps/Beta/Update.exe"))),
		    QStringLiteral("beta-shortcut")
		);
	}

	void sharedExecutablePathIsAmbiguous() {
		// Two shortcuts to one executable with different arguments (a browser
		// profile, an Electron host) are different launch targets: the path
		// must name nothing, not the first shortcut.
		const auto maps = WindowsDesktopEntryBackend::buildAliasMaps(
		    QList<DesktopEntryAliasCandidate> {
		        {QStringLiteral("alpha-shortcut"), QStringLiteral("C:/Apps/Host/host.exe"), QString()},
		        {QStringLiteral("beta-shortcut"), QStringLiteral("C:/Apps/Host/host.exe"), QString()},
		    }
		);

		QVERIFY2(
		    !maps.targets.contains(normalizedPath(QStringLiteral("C:/Apps/Host/host.exe"))),
		    "an executable path shared by two shortcuts must not alias"
		);
		QVERIFY(!maps.exeNames.contains(QStringLiteral("host")));
	}

	void sameAppThroughTargetAndTokenIsNotAmbiguous() {
		const auto maps = WindowsDesktopEntryBackend::buildAliasMaps(
		    QList<DesktopEntryAliasCandidate> {
		        {QStringLiteral("gamma"), QStringLiteral("C:/Apps/Gamma/gamma.exe"),
		         QStringLiteral("C:/Apps/Gamma/gamma.exe")},
		    }
		);

		QCOMPARE(maps.exeNames.value(QStringLiteral("gamma")), QStringLiteral("gamma"));
	}

	void unresolvedAumidKeepsRawId() {
		const auto aumid = QStringLiteral("Caelestia.Test.Unresolvable.")
		                 + QUuid::createUuid().toString(QUuid::WithoutBraces);

		auto identity = resolveAppIdentity(aumid);
		QCOMPARE(identity.name, aumid);
		QCOMPARE(identity.desktopEntry, aumid);
	}

	void packagedAppUsesAppInfo() {
		const auto aumid = QStringLiteral("Microsoft.WindowsCalculator_8wekyb3d8bbwe!App");

		QString expectedName;
		try {
			auto info = winrt::Windows::ApplicationModel::AppInfo::GetFromAppUserModelId(
			    winrt::hstring(aumid.toStdWString())
			);
			auto display = info.DisplayInfo();
			QVERIFY(display);
			expectedName = QString::fromWCharArray(display.DisplayName().c_str());
		} catch (const winrt::hresult_error&) {
			QSKIP("The inbox Calculator package is not installed");
		}

		QVERIFY(!expectedName.isEmpty());

		auto identity = resolveAppIdentity(aumid);
		QCOMPARE(identity.name, expectedName);
		// The desktop entry scan keys a packaged app by its lowercased AUMID.
		QCOMPARE(identity.desktopEntry, aumid.toLower());
	}

	void desktopExecutableUsesVersionResources() {
		auto path = QDir(QString::fromLocal8Bit(qgetenv("WINDIR")))
		                .filePath(QStringLiteral("System32/notepad.exe"));
		if (!QFileInfo::exists(path)) QSKIP("The inbox Notepad executable is not present");

		auto nativePath = QDir::toNativeSeparators(path);
		auto identity = resolveAppIdentity(nativePath);

		// An executable outside the AppsFolder is its own icon key, so the
		// icon provider can render it without a copied image.
		QCOMPARE(identity.desktopEntry, nativePath);
		QVERIFY(!identity.name.isEmpty());
		QVERIFY2(identity.name != nativePath, "the friendly name was not resolved");
		QVERIFY2(
		    identity.name != QFileInfo(path).completeBaseName(),
		    "the version resource was not used"
		);
	}

	void desktopAppUsesAppsFolderItem() {
		const auto aumid = QStringLiteral(
		    "{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\WindowsPowerShell\\v1.0\\powershell.exe"
		);

		auto identity = resolveAppIdentity(aumid);
		if (identity.name == aumid) QSKIP("Windows PowerShell is not exposed in the AppsFolder");

		// The desktop entry scan indexes an AppsFolder item by its lowercased
		// token, which is also the id the icon provider receives.
		QCOMPARE(identity.desktopEntry, aumid.toLower());
	}

	void runtimeAppIdUsesDesktopEntryHint() {
		// An explicit runtime AUMID has no AppsFolder token of its own, but the
		// window's app id maps to a desktop entry the caller already knows.
		const auto aumid = QStringLiteral("RuntimeAumidWithoutAnAppsFolderToken");
		auto hint = AppIdentityHint {
		    .desktopEntry = QStringLiteral("powershell"),
		    .parsingName =
		        QStringLiteral(
		            "{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\WindowsPowerShell\\v1.0\\powershell.exe"
		        ),
		    .executablePath = QDir(QString::fromLocal8Bit(qgetenv("WINDIR")))
		                          .filePath(QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe")),
		};
		if (!QFileInfo::exists(hint.executablePath)) {
			QSKIP("Windows PowerShell is not present");
		}

		auto identity = resolveAppIdentity(aumid, hint);
		QCOMPARE(identity.desktopEntry, hint.desktopEntry);
		QVERIFY(!identity.name.isEmpty());
		QVERIFY2(identity.name != aumid, "the hint was not used");
	}

	void executableAumidUsesAliasIndex() {
		this->scanAliasIndex();
		if (this->aliasExecutables.isEmpty()) {
			QSKIP("The Windows Apps folder exposes no executable entries");
		}

		for (auto it = this->aliasExecutables.constBegin(); it != this->aliasExecutables.constEnd();
		     ++it)
		{
			// A GSMTC session reports an executable AUMID with its extension,
			// such as "msedge.exe", in whatever case the app chose. The alias
			// lookup must reach the same stem-keyed desktop entry.
			auto aumid = it.key().toUpper() + QStringLiteral(".EXE");
			QCOMPARE(WindowsDesktopEntryBackend::aliasForAppId(aumid), it.value());
		}
	}

	void windowFallbackIdUsesAliasIndex() {
		this->scanAliasIndex();
		if (this->aliasTargets.isEmpty()) {
			QSKIP("No Windows Apps entry exposes an executable link target");
		}

		// The window tracker records its fallback app id without ".exe", so an
		// executable media AUMID for the same window has to derive the stem to
		// find the executable and its desktop entry.
		const auto runtimeAumid = QStringLiteral("caelestia-test-runtime-aumid");
		auto target = this->aliasTargets.constBegin();
		WindowsDesktopEntryBackend::noteWindowExe(runtimeAumid, target.key());
		QCOMPARE(
		    WindowsDesktopEntryBackend::aliasForAppId(
		        runtimeAumid.toUpper() + QStringLiteral(".EXE")
		    ),
		    target.value()
		);
	}
};

QTEST_GUILESS_MAIN(MprisAppIdentityTest)

#include "mpris_app_identity_test.moc"

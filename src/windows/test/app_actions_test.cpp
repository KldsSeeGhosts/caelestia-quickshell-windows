#include <functional>

#include <qcoreapplication.h>
#include <qdir.h>
#include <qfile.h>
#include <qsignalspy.h>
#include <qtemporarydir.h>
#include <qtest.h>
#include <qvector.h>

#include <qt_windows.h>

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winioctl.h>
#include <wrl/client.h>

#include "../process_launch.hpp"
#include "../system/app_actions.hpp"

using Microsoft::WRL::ComPtr;

using ShellExecuteInvoker = std::function<qs::windows::process::LaunchResult(
    const QString&,
    const QString&,
    const QString&,
    const QString&,
    HWND,
    bool,
    bool
)>;
using KnownFolderPathInvoker = std::function<HRESULT(const GUID&, DWORD, QString*)>;
using ReparsePointInvoker = std::function<bool(const QString&, DWORD*, QString*, bool*)>;

namespace qs::windows::sys::test {
void setParsingNameForId(const QString& id, const QString& token);
void setLinkTargetForId(const QString& id, const QString& target);
void setShortcutPathForId(const QString& id, const QString& path);
void clearParsingNames();
void setPropertiesInvokerForTest(const std::function<DWORD(const QString&, HWND, DWORD)>& invoker);
void setShortcutRootsForTest(const QStringList& roots);
void setDriveTypeInvokerForTest(const std::function<UINT(const QString&)>& invoker);
void setFileAttributesInvokerForTest(const std::function<DWORD(const QString&)>& invoker);
void setKnownFolderPathInvokerForTest(const KnownFolderPathInvoker& invoker);
void setReparsePointInvokerForTest(const ReparsePointInvoker& invoker);
void setShellExecuteTargetInvokerForTest(const ShellExecuteInvoker& invoker);
void resetPropertiesTestHooks();
} // namespace qs::windows::sys::test

using qs::windows::sys::AppActions;

namespace {

struct InvokeLog {
	int calls = 0;
	QString path;
	DWORD flags = 0;
	DWORD error = ERROR_SUCCESS;
};

struct ShellExecuteLog {
	int calls = 0;
	QString target;
	QString verb;
	bool invokeIdList = false;
};

QString writeFile(QTemporaryDir& directory, const QString& name) {
	const auto path = directory.filePath(name);
	QFile file(path);
	if (!file.open(QFile::WriteOnly)) return {};
	file.write("fixture");
	return QDir::toNativeSeparators(path);
}

bool createShortcut(const QString& shortcutPath, const QString& target) {
	const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	if (FAILED(com)) return false;

	auto ok = false;
	ComPtr<IShellLinkW> link;
	if (SUCCEEDED(CoCreateInstance(
	        CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)
	    )))
	{
		if (target.startsWith(QStringLiteral("shell::"), Qt::CaseInsensitive)) {
			PIDLIST_ABSOLUTE pidl = nullptr;
			if (SUCCEEDED(SHParseDisplayName(target.toStdWString().c_str(), nullptr, &pidl, 0, nullptr))) {
				ok = SUCCEEDED(link->SetIDList(pidl));
				ILFree(pidl);
			}
		} else {
			ok = SUCCEEDED(link->SetPath(target.toStdWString().c_str()));
		}

		if (ok) {
			ComPtr<IPersistFile> persist;
			const auto query = link->QueryInterface(IID_PPV_ARGS(&persist));
			auto save = HRESULT(E_FAIL);
			if (SUCCEEDED(query)) save = persist->Save(shortcutPath.toStdWString().c_str(), TRUE);
			ok = SUCCEEDED(query) && SUCCEEDED(save);
		}
	}

	link.Reset();
	CoUninitialize();
	return ok;
}

class AppActionsTest: public QObject {
	Q_OBJECT;

	void installInvoker(InvokeLog& log) {
		qs::windows::sys::test::setPropertiesInvokerForTest([&log](const QString& path, HWND, DWORD flags) {
			log.calls++;
			log.path = path;
			log.flags = flags;
			return log.error;
		});
	}

	void verifySynchronousFlags(DWORD flags) {
		QVERIFY((flags & SEE_MASK_ASYNCOK) == 0);
		QVERIFY((flags & SEE_MASK_INVOKEIDLIST) != 0);
		QVERIFY((flags & SEE_MASK_FLAG_NO_UI) != 0);
	}

private slots:
	void init() {
		qs::windows::sys::test::resetPropertiesTestHooks();
		qs::windows::sys::test::clearParsingNames();
		qs::windows::sys::test::setShellExecuteTargetInvokerForTest({});
		qs::windows::sys::test::setShortcutRootsForTest({});
	}

	void cleanup() {
		qs::windows::sys::test::resetPropertiesTestHooks();
		qs::windows::sys::test::clearParsingNames();
		qs::windows::sys::test::setShellExecuteTargetInvokerForTest({});
	}

	void unknownEntryRejectsProperties() {
		AppActions actions;
		QSignalSpy errors(&actions, &AppActions::lastErrorChanged);

		QVERIFY(!actions.openProperties(QStringLiteral("missing-fixture-entry")));
		QCOMPARE(actions.lastError(), QStringLiteral("The application entry is not available"));
		QCOMPARE(errors.size(), 1);
		QCOMPARE(
		    actions.info(QStringLiteral("missing-fixture-entry"))
		        .value(QStringLiteral("canOpenProperties"))
		        .toBool(),
		    false
		);
	}

	void propertiesStartMenuRootsDoNotVerifyKnownFolders() {
		qs::windows::sys::test::resetPropertiesTestHooks();
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto target = writeFile(directory, QStringLiteral("fixture.exe"));
		QVERIFY(!target.isEmpty());
		qs::windows::sys::test::setParsingNameForId(QStringLiteral("fixture-known-folder"), target);

		QVector<DWORD> folderFlags;
		qs::windows::sys::test::setKnownFolderPathInvokerForTest(
		    [&](const GUID&, DWORD flags, QString*) {
			    folderFlags.append(flags);
			    return E_FAIL;
		    }
		);

		AppActions actions;
		(void)actions.info(QStringLiteral("fixture-known-folder"));
		QCOMPARE(folderFlags.size(), 2);
		for (const auto flags: folderFlags) QCOMPARE(flags, DWORD(KF_FLAG_DONT_VERIFY));
	}

	void existingFilePublishesAndInvokesPropertiesSynchronously() {
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto target = writeFile(directory, QStringLiteral("fixture.exe"));
		QVERIFY(!target.isEmpty());
		qs::windows::sys::test::setParsingNameForId(QStringLiteral("fixture-win32"), target);

		InvokeLog invoke;
		installInvoker(invoke);
		AppActions actions;
		const auto info = actions.info(QStringLiteral("fixture-win32"));
		QCOMPARE(info.value(QStringLiteral("packaged")).toBool(), false);
		QCOMPARE(info.value(QStringLiteral("canOpenLocation")).toBool(), true);
		QCOMPARE(info.value(QStringLiteral("canOpenProperties")).toBool(), true);

		QVERIFY(actions.openProperties(QStringLiteral("fixture-win32")));
		QCOMPARE(invoke.calls, 1);
		QCOMPARE(invoke.path, target);
		verifySynchronousFlags(invoke.flags);
		QCOMPARE(actions.lastError(), QString());
	}

	void deletionAndRecreationAreResolvedLiveOnTheSameInstance() {
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto target = writeFile(directory, QStringLiteral("fixture.exe"));
		QVERIFY(!target.isEmpty());
		qs::windows::sys::test::setParsingNameForId(QStringLiteral("fixture-live"), target);

		InvokeLog invoke;
		installInvoker(invoke);
		AppActions actions;
		const auto id = QStringLiteral("fixture-live");
		QCOMPARE(actions.info(id).value(QStringLiteral("canOpenProperties")).toBool(), true);

		QVERIFY(QFile::remove(target));
		QCOMPARE(actions.info(id).value(QStringLiteral("canOpenProperties")).toBool(), false);
		QVERIFY(!actions.openProperties(id));
		QCOMPARE(invoke.calls, 0);

		QCOMPARE(writeFile(directory, QStringLiteral("fixture.exe")), target);
		QCOMPARE(actions.info(id).value(QStringLiteral("canOpenProperties")).toBool(), true);
		QVERIFY(actions.openProperties(id));
		QCOMPARE(invoke.calls, 1);
		QCOMPARE(invoke.path, target);
		verifySynchronousFlags(invoke.flags);
	}

	void registeredShortcutIsPreferredOverItsDestination() {
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto target = writeFile(directory, QStringLiteral("fixture.exe"));
		const auto shortcut = QDir::toNativeSeparators(directory.filePath(QStringLiteral("Fixture.lnk")));
		QVERIFY(!target.isEmpty());
		QVERIFY(createShortcut(shortcut, target));
		qs::windows::sys::test::setShortcutRootsForTest({QDir::toNativeSeparators(directory.path())});
		qs::windows::sys::test::setParsingNameForId(QStringLiteral("fixture-shortcut"), target);
		qs::windows::sys::test::setLinkTargetForId(QStringLiteral("fixture-shortcut"), target);

		InvokeLog invoke;
		installInvoker(invoke);
		AppActions actions;
		QCOMPARE(
		    actions.info(QStringLiteral("fixture-shortcut"))
		        .value(QStringLiteral("canOpenProperties"))
		        .toBool(),
		    true
		);
		QVERIFY(actions.openProperties(QStringLiteral("fixture-shortcut")));
		QCOMPARE(invoke.calls, 1);
		QCOMPARE(invoke.path, shortcut);
		verifySynchronousFlags(invoke.flags);
	}

	void registeredShortcutOutsideStartMenuRootsIsUsedExactly() {
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("outside")));
		const auto target = writeFile(directory, QStringLiteral("fixture.exe"));
		const auto shortcut = QDir::toNativeSeparators(
		    directory.filePath(QStringLiteral("outside/Fixture Registered.lnk"))
		);
		QVERIFY(!target.isEmpty());
		QVERIFY(createShortcut(shortcut, target));
		qs::windows::sys::test::setShortcutRootsForTest({});
		qs::windows::sys::test::setParsingNameForId(QStringLiteral("fixture-registered"), target);
		qs::windows::sys::test::setLinkTargetForId(QStringLiteral("fixture-registered"), target);
		qs::windows::sys::test::setShortcutPathForId(QStringLiteral("fixture-registered"), shortcut);

		InvokeLog invoke;
		installInvoker(invoke);
		AppActions actions;
		const auto id = QStringLiteral("fixture-registered");
		QCOMPARE(actions.info(id).value(QStringLiteral("canOpenProperties")).toBool(), true);
		QVERIFY(actions.openProperties(id));
		QCOMPARE(invoke.calls, 1);
		QCOMPARE(invoke.path, shortcut);
	}

	void registeredShortcutWinsOverAnotherShortcutToTheSameDestination() {
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("scan")));
		QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("registered")));
		const auto target = writeFile(directory, QStringLiteral("fixture.exe"));
		const auto decoy = QDir::toNativeSeparators(
		    directory.filePath(QStringLiteral("scan/Fixture Decoy.lnk"))
		);
		const auto registered = QDir::toNativeSeparators(
		    directory.filePath(QStringLiteral("registered/Fixture Registered.lnk"))
		);
		QVERIFY(!target.isEmpty());
		QVERIFY(createShortcut(decoy, target));
		QVERIFY(createShortcut(registered, target));
		qs::windows::sys::test::setShortcutRootsForTest(
		    {QDir::toNativeSeparators(directory.filePath(QStringLiteral("scan")))}
		);
		qs::windows::sys::test::setParsingNameForId(QStringLiteral("fixture-duplicate"), target);
		qs::windows::sys::test::setLinkTargetForId(QStringLiteral("fixture-duplicate"), target);
		qs::windows::sys::test::setShortcutPathForId(QStringLiteral("fixture-duplicate"), registered);

		InvokeLog invoke;
		installInvoker(invoke);
		AppActions actions;
		const auto id = QStringLiteral("fixture-duplicate");
		QCOMPARE(actions.info(id).value(QStringLiteral("canOpenProperties")).toBool(), true);
		QVERIFY(actions.openProperties(id));
		QCOMPARE(invoke.calls, 1);
		QCOMPARE(invoke.path, registered);
	}

	void shortcutToShellNamespaceTargetKeepsShortcutProperties() {
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto shellTarget = QStringLiteral("shell:::{26EE0668-A00A-44D7-9371-BEB064C98683}");
		const auto shortcut = QDir::toNativeSeparators(directory.filePath(QStringLiteral("Namespace Fixture.lnk")));
		QVERIFY(createShortcut(shortcut, shellTarget));
		qs::windows::sys::test::setShortcutRootsForTest({QDir::toNativeSeparators(directory.path())});
		qs::windows::sys::test::setParsingNameForId(QStringLiteral("fixture-namespace"), shellTarget);
		qs::windows::sys::test::setLinkTargetForId(QStringLiteral("fixture-namespace"), shellTarget);

		InvokeLog invoke;
		installInvoker(invoke);
		AppActions actions;
		QCOMPARE(
		    actions.info(QStringLiteral("fixture-namespace"))
		        .value(QStringLiteral("canOpenProperties"))
		        .toBool(),
		    true
		);
		QVERIFY(actions.openProperties(QStringLiteral("fixture-namespace")));
		QCOMPARE(invoke.calls, 1);
		QCOMPARE(invoke.path, shortcut);
	}

	void nonexistentExecutableDoesNotPublishProperties() {
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto missing = QDir::toNativeSeparators(directory.filePath(QStringLiteral("missing.exe")));
		qs::windows::sys::test::setParsingNameForId(QStringLiteral("fixture-missing"), missing);

		AppActions actions;
		QCOMPARE(
		    actions.info(QStringLiteral("fixture-missing"))
		        .value(QStringLiteral("canOpenProperties"))
		        .toBool(),
		    false
		);
		QVERIFY(!actions.openProperties(QStringLiteral("fixture-missing")));
	}

	void malformedAndNonFilesystemTargetsDoNotPublishProperties_data() {
		QTest::addColumn<QString>("target");
		QTest::newRow("uri") << QStringLiteral("https://example.invalid/app.exe");
		QTest::newRow("shell namespace") << QStringLiteral("shell:AppsFolder\\Fixture.App_123456789abcd!App");
		QTest::newRow("control character") << QStringLiteral("C:\\Fixture\nApp.exe");
		QTest::newRow("embedded nul") << QString::fromUtf16(u"C:\\Fixture\0App.exe");
	}

	void malformedAndNonFilesystemTargetsDoNotPublishProperties() {
		QFETCH(QString, target);
		qs::windows::sys::test::setParsingNameForId(QStringLiteral("fixture-invalid"), target);

		AppActions actions;
		QCOMPARE(
		    actions.info(QStringLiteral("fixture-invalid"))
		        .value(QStringLiteral("canOpenProperties"))
		        .toBool(),
		    false
		);
	}

	void uncTargetsAreRejectedBeforeAnyFilesystemProbe_data() {
		QTest::addColumn<QString>("target");
		QTest::newRow("unc") << QStringLiteral("\\\\nonexistent-network-host.invalid\\share\\app.exe");
		QTest::newRow("extended unc")
		    << QStringLiteral("\\\\?\\UNC\\nonexistent-network-host.invalid\\share\\app.exe");
		QTest::newRow("device unc")
		    << QStringLiteral("\\\\.\\UNC\\nonexistent-network-host.invalid\\share\\app.exe");
		QTest::newRow("slash unc") << QStringLiteral("//nonexistent-network-host.invalid/share/app.exe");
	}

	void uncTargetsAreRejectedBeforeAnyFilesystemProbe() {
		QFETCH(QString, target);
		auto driveCalls = 0;
		auto filesystemProbes = 0;
		qs::windows::sys::test::setDriveTypeInvokerForTest([&](const QString&) {
			driveCalls++;
			return DRIVE_FIXED;
		});
		qs::windows::sys::test::setFileAttributesInvokerForTest([&](const QString&) {
			filesystemProbes++;
			return DWORD(FILE_ATTRIBUTE_NORMAL);
		});
		qs::windows::sys::test::setParsingNameForId(QStringLiteral("fixture-unc"), target);

		AppActions actions;
		const auto id = QStringLiteral("fixture-unc");
		QCOMPARE(actions.info(id).value(QStringLiteral("canOpenProperties")).toBool(), false);
		QVERIFY(!actions.openProperties(id));
		QCOMPARE(driveCalls, 0);
		QCOMPARE(filesystemProbes, 0);
	}

	void mappedNetworkDrivesAreRejectedBeforeTheTargetIsProbed_data() {
		QTest::addColumn<QString>("target");
		QTest::newRow("drive") << QStringLiteral("Z:\\fixture\\app.exe");
		QTest::newRow("extended drive") << QStringLiteral("\\\\?\\Z:\\fixture\\app.exe");
	}

	void mappedNetworkDrivesAreRejectedBeforeTheTargetIsProbed() {
		QFETCH(QString, target);
		auto driveCalls = 0;
		auto filesystemProbes = 0;
		qs::windows::sys::test::setDriveTypeInvokerForTest([&](const QString& root) {
			driveCalls++;
			if (root.compare(QStringLiteral("Z:\\"), Qt::CaseInsensitive) == 0) return UINT(DRIVE_REMOTE);
			const auto wide = root.toStdWString();
			return GetDriveTypeW(wide.c_str());
		});
		qs::windows::sys::test::setFileAttributesInvokerForTest([&](const QString&) {
			filesystemProbes++;
			return DWORD(FILE_ATTRIBUTE_NORMAL);
		});
		qs::windows::sys::test::setParsingNameForId(QStringLiteral("fixture-mapped-network"), target);

		AppActions actions;
		const auto id = QStringLiteral("fixture-mapped-network");
		QCOMPARE(actions.info(id).value(QStringLiteral("canOpenProperties")).toBool(), false);
		QVERIFY(!actions.openProperties(id));
		QCOMPARE(filesystemProbes, 0);
		QVERIFY(driveCalls > 0);
	}

	void redirectedShortcutRootsAreSkippedBeforeEnumeration() {
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto target = writeFile(directory, QStringLiteral("fixture.exe"));
		QVERIFY(!target.isEmpty());
		qs::windows::sys::test::setShortcutRootsForTest(
		    {QStringLiteral("\\\\nonexistent-network-host.invalid\\startmenu")}
		);
		qs::windows::sys::test::setParsingNameForId(QStringLiteral("fixture-redirected-root"), target);

		AppActions actions;
		const auto id = QStringLiteral("fixture-redirected-root");
		QCOMPARE(actions.info(id).value(QStringLiteral("canOpenProperties")).toBool(), true);
	}

	void networkShortcutRootIsRejectedBeforeEnumeration() {
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto target = writeFile(directory, QStringLiteral("fixture.exe"));
		const auto shortcut = QDir::toNativeSeparators(directory.filePath(QStringLiteral("Fixture.lnk")));
		QVERIFY(!target.isEmpty());
		QVERIFY(createShortcut(shortcut, target));
		QVERIFY(QFile::remove(target));

		const auto nativeRoot = QDir::toNativeSeparators(directory.path());
		const auto driveRoot = nativeRoot.left(2) + QStringLiteral("\\");
		auto rootValidationCalls = 0;
		qs::windows::sys::test::setDriveTypeInvokerForTest([&](const QString& root) {
			if (root.compare(driveRoot, Qt::CaseInsensitive) == 0 && rootValidationCalls++ == 0) {
				return UINT(DRIVE_REMOTE);
			}
			const auto wide = root.toStdWString();
			return GetDriveTypeW(wide.c_str());
		});
		qs::windows::sys::test::setShortcutRootsForTest({nativeRoot});
		qs::windows::sys::test::setParsingNameForId(QStringLiteral("fixture-network-root"), target);
		qs::windows::sys::test::setLinkTargetForId(QStringLiteral("fixture-network-root"), target);

		AppActions actions;
		const auto id = QStringLiteral("fixture-network-root");
		QCOMPARE(actions.info(id).value(QStringLiteral("canOpenProperties")).toBool(), false);
		QVERIFY(rootValidationCalls > 0);
	}

	void directorySymlinkToUncIsRejectedBeforeProbingBelowIt() {
		const auto redirectedAncestor = QStringLiteral("C:\\Redirected");
		const auto redirectedRoot = QStringLiteral("C:\\Redirected\\Start Menu");
		const auto redirectedTarget = QStringLiteral("C:\\Redirected\\Start Menu\\app.exe");
		auto ancestorChecked = 0;
		auto probedBelowSymlink = 0;

		qs::windows::sys::test::setDriveTypeInvokerForTest([](const QString& root) {
			if (root.compare(QStringLiteral("C:\\"), Qt::CaseInsensitive) == 0) return UINT(DRIVE_FIXED);
			return UINT(DRIVE_NO_ROOT_DIR);
		});
		qs::windows::sys::test::setFileAttributesInvokerForTest([&](const QString& path) {
			if (path.compare(redirectedAncestor, Qt::CaseInsensitive) == 0) {
				ancestorChecked++;
				return DWORD(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT);
			}
			if (path.startsWith(redirectedAncestor + QStringLiteral("\\"), Qt::CaseInsensitive)) {
				probedBelowSymlink++;
			}
			return DWORD(INVALID_FILE_ATTRIBUTES);
		});
		qs::windows::sys::test::setReparsePointInvokerForTest(
		    [&](const QString& path, DWORD* tag, QString* target, bool* relative) {
			    if (path.compare(redirectedAncestor, Qt::CaseInsensitive) != 0) return false;
			    *tag = IO_REPARSE_TAG_SYMLINK;
			    *target = QStringLiteral("\\??\\UNC\\nonexistent-network-host.invalid\\share");
			    *relative = false;
			    return true;
		    }
		);
		qs::windows::sys::test::setShortcutRootsForTest({redirectedRoot});
		qs::windows::sys::test::setParsingNameForId(QStringLiteral("fixture-symlink-unc"), redirectedTarget);
		qs::windows::sys::test::setLinkTargetForId(QStringLiteral("fixture-symlink-unc"), redirectedTarget);

		AppActions actions;
		const auto id = QStringLiteral("fixture-symlink-unc");
		QCOMPARE(actions.info(id).value(QStringLiteral("canOpenProperties")).toBool(), false);
		QVERIFY(!actions.openProperties(id));
		QVERIFY(ancestorChecked > 0);
		QCOMPARE(probedBelowSymlink, 0);
	}

	void directorySymlinkToLocalTargetStillAllowsProperties() {
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto realTarget = writeFile(directory, QStringLiteral("fixture.exe"));
		QVERIFY(!realTarget.isEmpty());

		const auto aliasAncestor = QStringLiteral("C:\\LocalAlias");
		const auto aliasTarget = aliasAncestor + QStringLiteral("\\fixture.exe");
		const auto realDir = QDir::toNativeSeparators(directory.path());

		qs::windows::sys::test::setFileAttributesInvokerForTest([&](const QString& path) {
			if (path.compare(aliasAncestor, Qt::CaseInsensitive) == 0) {
				return DWORD(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT);
			}
			const auto wide = path.toStdWString();
			return GetFileAttributesW(wide.c_str());
		});
		qs::windows::sys::test::setReparsePointInvokerForTest(
		    [&](const QString& path, DWORD* tag, QString* target, bool* relative) {
			    if (path.compare(aliasAncestor, Qt::CaseInsensitive) != 0) return false;
			    *tag = IO_REPARSE_TAG_MOUNT_POINT;
			    *target = QStringLiteral("\\??\\") + realDir;
			    *relative = false;
			    return true;
		    }
		);
		qs::windows::sys::test::setParsingNameForId(QStringLiteral("fixture-symlink-local"), aliasTarget);

		InvokeLog invoke;
		installInvoker(invoke);
		AppActions actions;
		const auto id = QStringLiteral("fixture-symlink-local");
		QCOMPARE(actions.info(id).value(QStringLiteral("canOpenProperties")).toBool(), true);
		QVERIFY(actions.openProperties(id));
		QCOMPARE(invoke.calls, 1);
		QCOMPARE(invoke.path, aliasTarget);
	}

	void trailingSpaceOrDotSegmentsAreRejectedBeforeProbing_data() {
		QTest::addColumn<QString>("root");
		QTest::addColumn<QString>("target");
		QTest::newRow("trailing space segment")
		    << QStringLiteral("C:\\Redirected \\Start Menu")
		    << QStringLiteral("C:\\Redirected \\Start Menu\\app.exe");
		QTest::newRow("trailing dot segment")
		    << QStringLiteral("C:\\Redirected.\\Start Menu")
		    << QStringLiteral("C:\\Redirected.\\Start Menu\\app.exe");
	}

	void trailingSpaceOrDotSegmentsAreRejectedBeforeProbing() {
		QFETCH(QString, root);
		QFETCH(QString, target);
		auto filesystemProbes = 0;

		qs::windows::sys::test::setDriveTypeInvokerForTest([](const QString&) {
			return UINT(DRIVE_FIXED);
		});
		qs::windows::sys::test::setFileAttributesInvokerForTest([&](const QString&) {
			filesystemProbes++;
			return DWORD(FILE_ATTRIBUTE_DIRECTORY);
		});
		qs::windows::sys::test::setShortcutRootsForTest({root});
		qs::windows::sys::test::setParsingNameForId(QStringLiteral("fixture-trimmed-segment"), target);
		qs::windows::sys::test::setLinkTargetForId(QStringLiteral("fixture-trimmed-segment"), target);

		AppActions actions;
		const auto id = QStringLiteral("fixture-trimmed-segment");
		QCOMPARE(actions.info(id).value(QStringLiteral("canOpenProperties")).toBool(), false);
		QVERIFY(!actions.openProperties(id));
		QCOMPARE(filesystemProbes, 0);
	}

	void remoteStartMenuShortcutIsRejectedForPropertiesButUsedForElevation() {
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto target = writeFile(directory, QStringLiteral("fixture.exe"));
		const auto shortcut = QDir::toNativeSeparators(directory.filePath(QStringLiteral("Fixture.lnk")));
		QVERIFY(!target.isEmpty());
		QVERIFY(createShortcut(shortcut, target));

		const auto nativeRoot = QDir::toNativeSeparators(directory.path());
		const auto driveRoot = nativeRoot.left(2) + QStringLiteral("\\");
		qs::windows::sys::test::setDriveTypeInvokerForTest([&](const QString& root) {
			if (root.compare(driveRoot, Qt::CaseInsensitive) == 0) return UINT(DRIVE_REMOTE);
			const auto wide = root.toStdWString();
			return GetDriveTypeW(wide.c_str());
		});
		qs::windows::sys::test::setShortcutRootsForTest({nativeRoot});
		qs::windows::sys::test::setParsingNameForId(QStringLiteral("fixture-remote-shortcut"), target);
		qs::windows::sys::test::setLinkTargetForId(QStringLiteral("fixture-remote-shortcut"), target);

		InvokeLog properties;
		installInvoker(properties);
		ShellExecuteLog shell;
		qs::windows::sys::test::setShellExecuteTargetInvokerForTest(
		    [&](const QString& shellTarget,
		        const QString&,
		        const QString&,
		        const QString& verb,
		        HWND,
		        bool invokeIdList,
		        bool) {
			    shell.calls++;
			    shell.target = shellTarget;
			    shell.verb = verb;
			    shell.invokeIdList = invokeIdList;
			    return qs::windows::process::detail::launchResultForError(ERROR_SUCCESS);
		    }
		);

		AppActions actions;
		const auto id = QStringLiteral("fixture-remote-shortcut");
		QCOMPARE(actions.info(id).value(QStringLiteral("canOpenProperties")).toBool(), false);
		QVERIFY(!actions.openProperties(id));
		QCOMPARE(properties.calls, 0);

		QVERIFY(actions.runAsAdmin(id));
		QCOMPARE(shell.calls, 1);
		QCOMPARE(shell.target, shortcut);
		QCOMPARE(shell.verb, QStringLiteral("runas"));
		QVERIFY(shell.invokeIdList);
	}

	void synchronousInvocationFailureIsReported() {
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto target = writeFile(directory, QStringLiteral("fixture.exe"));
		QVERIFY(!target.isEmpty());
		qs::windows::sys::test::setParsingNameForId(QStringLiteral("fixture-failure"), target);

		InvokeLog invoke;
		invoke.error = ERROR_ACCESS_DENIED;
		installInvoker(invoke);
		AppActions actions;
		QVERIFY(!actions.openProperties(QStringLiteral("fixture-failure")));
		QCOMPARE(invoke.calls, 1);
		verifySynchronousFlags(invoke.flags);
		QVERIFY(!actions.lastError().isEmpty());
	}

	void packagedEntryRejectsPropertiesWithoutShellUi() {
		qs::windows::sys::test::setParsingNameForId(
		    QStringLiteral("fixture-packaged"),
		    QStringLiteral("Fixture.App_123456789abcd!App")
		);

		AppActions actions;
		const auto info = actions.info(QStringLiteral("fixture-packaged"));
		QCOMPARE(info.value(QStringLiteral("packaged")).toBool(), true);
		QCOMPARE(info.value(QStringLiteral("canOpenProperties")).toBool(), false);

		QVERIFY(!actions.openProperties(QStringLiteral("fixture-packaged")));
		QCOMPARE(
		    actions.lastError(),
		    QStringLiteral("Properties is not supported for this application")
		);
	}
};

} // namespace

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	AppActionsTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "app_actions_test.moc"

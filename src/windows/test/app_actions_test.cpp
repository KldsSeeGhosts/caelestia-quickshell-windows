#include <functional>

#include <qcoreapplication.h>
#include <qdir.h>
#include <qfile.h>
#include <qsignalspy.h>
#include <qtemporarydir.h>
#include <qtest.h>

#include <qt_windows.h>

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include "../system/app_actions.hpp"

using Microsoft::WRL::ComPtr;

namespace qs::windows::sys::test {
void setParsingNameForId(const QString& id, const QString& token);
void setLinkTargetForId(const QString& id, const QString& target);
void clearParsingNames();
void setPropertiesInvokerForTest(const std::function<DWORD(const QString&, HWND, DWORD)>& invoker);
void setShortcutRootsForTest(const QStringList& roots);
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
		qs::windows::sys::test::setShortcutRootsForTest({});
	}

	void cleanup() {
		qs::windows::sys::test::resetPropertiesTestHooks();
		qs::windows::sys::test::clearParsingNames();
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

	void deletionAfterInfoPreventsShellInvocation() {
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto target = writeFile(directory, QStringLiteral("fixture.exe"));
		QVERIFY(!target.isEmpty());
		qs::windows::sys::test::setParsingNameForId(QStringLiteral("fixture-deleted"), target);

		InvokeLog invoke;
		installInvoker(invoke);
		AppActions actions;
		QCOMPARE(
		    actions.info(QStringLiteral("fixture-deleted"))
		        .value(QStringLiteral("canOpenProperties"))
		        .toBool(),
		    true
		);

		QVERIFY(QFile::remove(target));
		QVERIFY(!actions.openProperties(QStringLiteral("fixture-deleted")));
		QCOMPARE(invoke.calls, 0);
		QCOMPARE(actions.lastError(), QStringLiteral("The application entry is not available"));
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

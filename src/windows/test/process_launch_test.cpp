#include <qbuffer.h>
#include <qcoreapplication.h>
#include <qdir.h>
#include <qfile.h>
#include <qstringlist.h>
#include <qtemporarydir.h>
#include <qtest.h>
#include <qtenvironmentvariables.h>

#include "../core/common.hpp"
#include "../process_launch.hpp"
#include "../system/appx_manifest.hpp"

using qs::windows::process::LaunchStatus;
using qs::windows::process::ProgramKind;

class ProcessLaunchTest: public QObject {
	Q_OBJECT

private slots:
	void commandQuotingPreservesSpaces() {
		using qs::windows::process::joinCommandArguments;
		using qs::windows::process::joinCommandLine;
		using qs::windows::process::quoteCommandArgument;

		QCOMPARE(quoteCommandArgument(QStringLiteral("plain")), QStringLiteral("plain"));
		QCOMPARE(quoteCommandArgument(QStringLiteral("two words")), QStringLiteral("\"two words\""));
		const auto line = joinCommandLine(
		    QStringLiteral("C:/Program Files/tool.exe"),
		    {QStringLiteral("argument one"), QStringLiteral("plain")}
		);
		QCOMPARE(line, QStringLiteral("\"C:/Program Files/tool.exe\" \"argument one\" plain"));
		QCOMPARE(joinCommandArguments({}), QString());
	}

	void unquotedSpacedPathUsesNamedExecutable() {
		QTemporaryDir root;
		QVERIFY(root.isValid());
		QVERIFY(QDir(root.path()).mkpath(QStringLiteral("bin dir")));
		const auto directory = QDir(root.path()).filePath(QStringLiteral("bin dir"));
		const auto executable = QDir(directory).filePath(QStringLiteral("tool copy.exe"));
		QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), executable));

		const auto command = QDir::toNativeSeparators(executable)
		    + QStringLiteral(" --flag value");
		const auto parsed = qs::windows::process::parseLaunchCommand(command);

		QCOMPARE(parsed.executable, QDir::toNativeSeparators(executable));
		QCOMPARE(parsed.argumentTail, QStringLiteral("--flag value"));
		QCOMPARE(parsed.kind, ProgramKind::Console);
	}

	void missingOrAmbiguousPathNeverLaunchesShorterPrefix() {
		using namespace qs::windows::process;
		QTemporaryDir root;
		QVERIFY(root.isValid());
		QDir directory(root.path());
		QVERIFY(directory.mkpath(QStringLiteral("Program Files/Vendor")));
		QVERIFY(QFile::copy(
		    QCoreApplication::applicationFilePath(), directory.filePath(QStringLiteral("Program.exe"))
		));

		// Same ambiguity as C:\Program Files\Vendor\Missing.exe when C:\Program.exe exists.
		const auto missing = QDir::toNativeSeparators(
		    directory.filePath(QStringLiteral("Program Files/Vendor/Missing.exe"))
		);
		const auto parsed = parseLaunchCommand(missing + QStringLiteral(" --flag"));
		QCOMPARE(parsed.program, missing);
		QVERIFY(parsed.executable.isEmpty());
		QCOMPARE(parsed.error, DWORD(ERROR_FILE_NOT_FOUND));

		const auto withoutExtension = missing.chopped(4) + QStringLiteral(" argument");
		QCOMPARE(parseLaunchCommand(withoutExtension).error, DWORD(ERROR_FILE_NOT_FOUND));

		const auto ambiguous = QDir::toNativeSeparators(
		    directory.filePath(QStringLiteral("Program.exe Files/Vendor/Missing.exe"))
		);
		QCOMPARE(parseLaunchCommand(ambiguous).error, DWORD(ERROR_INVALID_PARAMETER));

		const auto switchLike = QDir::toNativeSeparators(
		    directory.filePath(QStringLiteral("Program -Files/Vendor/Missing.exe"))
		);
		const auto switchLikeParsed = parseLaunchCommand(switchLike);
		QCOMPARE(switchLikeParsed.program, switchLike);
		QCOMPARE(switchLikeParsed.error, DWORD(ERROR_FILE_NOT_FOUND));
		const auto extensionless = directory.filePath(QStringLiteral("Program --switch value"));
		QCOMPARE(parseLaunchCommand(extensionless).error, DWORD(ERROR_INVALID_PARAMETER));
		QCOMPARE(
		    parseLaunchCommand(QStringLiteral("Program Files\\Vendor\\Missing.exe")).error,
		    DWORD(ERROR_INVALID_PARAMETER)
		);
	}

	void quotedSpacedPathKeepsArgumentsSeparate() {
		QTemporaryDir root;
		QVERIFY(root.isValid());
		const auto executable = QDir(root.path()).filePath(QStringLiteral("tool copy.exe"));
		QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), executable));

		const auto parsed = qs::windows::process::parseLaunchCommand(
		    QStringLiteral("\"") + QDir::fromNativeSeparators(executable)
		    + QStringLiteral("\" --flag \"two words\"")
		);

		QCOMPARE(parsed.executable, QDir::toNativeSeparators(executable));
		QCOMPARE(parsed.argumentTail, QStringLiteral("--flag \"two words\""));
	}

	void preservesEmptyArgumentsEscapedQuotesAndTrailingBackslashes() {
		using qs::windows::process::parseLaunchCommand;
		const auto tail = QStringLiteral("\"\" \"say \\\"hi\\\"\" \"two words\\\\\" tail  ");
		const auto bare = parseLaunchCommand(QStringLiteral("tool.exe  \t") + tail);
		QCOMPARE(bare.program, QStringLiteral("tool.exe"));
		QCOMPARE(bare.argumentTail, tail);

		QTemporaryDir root;
		QVERIFY(root.isValid());
		const auto executable = QDir(root.path()).filePath(QStringLiteral("tool copy.exe"));
		QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), executable));
		const auto parsed = parseLaunchCommand(executable + u' ' + tail);
		QCOMPARE(parsed.executable, QDir::toNativeSeparators(executable));
		QCOMPARE(parsed.argumentTail, tail);
	}

	void nonPeTargetsUseTheShell() {
		QTemporaryDir root;
		QVERIFY(root.isValid());
		const auto document = QDir(root.path()).filePath(QStringLiteral("notes.txt"));
		QFile file(document);
		QVERIFY(file.open(QIODevice::WriteOnly));
		file.write("notes");
		file.close();

		const auto parsed = qs::windows::process::parseLaunchCommand(QDir::fromNativeSeparators(document));
		QCOMPARE(parsed.kind, ProgramKind::Shell);
		QVERIFY(!parsed.useProcess());
	}

	void missingProgramsStayUnresolved() {
		const auto parsed = qs::windows::process::parseLaunchCommand(
		    QStringLiteral("qs-launch-fixture-missing-program-7f31 --flag")
		);
		QCOMPARE(parsed.program, QStringLiteral("qs-launch-fixture-missing-program-7f31"));
		QVERIFY(parsed.executable.isEmpty());
		QCOMPARE(parsed.kind, ProgramKind::Unresolved);
	}

	void explorerFailureFallsBackOnceWithCleanEnvironment() {
		using namespace qs::windows::process;
		const auto saved = qs::Common::INITIAL_ENVIRONMENT;
		const auto originalQpa = qgetenv("QT_QPA_PLATFORM");
		qs::Common::INITIAL_ENVIRONMENT.insert(
		    QStringLiteral("CAELESTIA_SUPERVISOR_PIPE"), QStringLiteral("pipe")
		);
		qs::Common::INITIAL_ENVIRONMENT.insert(
		    QStringLiteral("CAELESTIA_SUPERVISOR_TOKEN"), QStringLiteral("token")
		);
		qs::Common::INITIAL_ENVIRONMENT.insert(
		    QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("original")
		);
		qs::Common::INITIAL_ENVIRONMENT.insert(QStringLiteral("PATH"), QStringLiteral("original-path"));
		qputenv("QT_QPA_PLATFORM", "shell-pragma");

		auto calls = QStringList();
		auto entries = QStringList();
		auto cleanLaunch = [&](const std::wstring& environment) {
			calls.append(QStringLiteral("clean"));
			auto* cursor = environment.data();
			while (*cursor != L'\0') {
				const auto entry = QString::fromWCharArray(cursor);
				entries.append(entry);
				cursor += entry.size() + 1;
			}
			return detail::launchResultForError(ERROR_SUCCESS);
		};
		const auto result = detail::openWithFallback(
		    [&] {
			    calls.append(QStringLiteral("explorer"));
			    return detail::launchResultForError(ERROR_GEN_FAILURE);
		    },
		    cleanLaunch
		);
		qs::Common::INITIAL_ENVIRONMENT = saved;
		if (originalQpa.isNull()) qunsetenv("QT_QPA_PLATFORM");
		else qputenv("QT_QPA_PLATFORM", originalQpa);

		QVERIFY(result.started());
		QCOMPARE(calls, QStringList({QStringLiteral("explorer"), QStringLiteral("clean")}));
		QVERIFY(entries.contains(QStringLiteral("QT_QPA_PLATFORM=original")));
		QVERIFY(entries.contains(QStringLiteral("PATH=original-path")));
		for (const auto& entry: entries) {
			QVERIFY(!entry.startsWith(QStringLiteral("CAELESTIA_SUPERVISOR_PIPE=")));
			QVERIFY(!entry.startsWith(QStringLiteral("CAELESTIA_SUPERVISOR_TOKEN=")));
		}

		calls.clear();
		const auto explorerResult = detail::openWithFallback(
		    [&] {
			    calls.append(QStringLiteral("explorer"));
			    return detail::launchResultForError(ERROR_SUCCESS);
		    },
		    cleanLaunch
		);
		QVERIFY(explorerResult.started());
		QCOMPARE(calls, QStringList({QStringLiteral("explorer")}));

		calls.clear();
		const auto unknownResult = detail::openWithFallback(
		    [&] {
			    calls.append(QStringLiteral("explorer"));
			    return LaunchResult {
			        .status = LaunchStatus::Indeterminate, .error = ERROR_GEN_FAILURE,
			    };
		    },
		    cleanLaunch
		);
		QCOMPARE(unknownResult.status, LaunchStatus::Indeterminate);
		QCOMPARE(calls, QStringList({QStringLiteral("explorer")}));
	}

	void uacCancellationDoesNotRetry() {
		using namespace qs::windows::process;
		int fallbackCalls = 0;
		auto fallback = [&] {
			++fallbackCalls;
			return detail::launchResultForError(ERROR_SUCCESS);
		};

		const auto cancelled = detail::fallbackOnFailure(
		    detail::launchResultForError(ERROR_CANCELLED), fallback
		);
		QCOMPARE(cancelled.status, LaunchStatus::Cancelled);
		QCOMPARE(cancelled.error, DWORD(ERROR_CANCELLED));
		QCOMPARE(fallbackCalls, 0);

		const auto refused = detail::fallbackOnFailure(
		    detail::launchResultForError(ERROR_ACCESS_DENIED), fallback
		);
		QVERIFY(refused.started());
		QCOMPARE(fallbackCalls, 1);
	}

	void packagedDesktopAliasMustMatchApplicationId() {
		const auto manifest = QByteArray(R"xml(
<Package xmlns="urn:appx-test" xmlns:uap3="urn:uap3-test" xmlns:desktop="urn:desktop-test"
         xmlns:rescap="urn:rescap-test">
  <Applications>
    <Application Id="Other" EntryPoint="Windows.FullTrustApplication">
      <Extensions>
        <uap3:Extension Category="windows.appExecutionAlias"><uap3:AppExecutionAlias>
          <desktop:ExecutionAlias Alias="other.exe"/>
        </uap3:AppExecutionAlias></uap3:Extension>
      </Extensions>
    </Application>
    <Application Id="App" EntryPoint="Windows.FullTrustApplication">
      <Extensions>
        <uap3:Extension Category="windows.appExecutionAlias"><uap3:AppExecutionAlias>
          <desktop:ExecutionAlias Alias="..\bad.exe"/>
          <desktop:ExecutionAlias Alias="wt.exe"/>
        </uap3:AppExecutionAlias></uap3:Extension>
      </Extensions>
    </Application>
    <Application Id="Uwp">
      <Extensions>
        <uap3:Extension Category="windows.appExecutionAlias"><uap3:AppExecutionAlias>
          <desktop:ExecutionAlias Alias="uwp.exe"/>
        </uap3:AppExecutionAlias></uap3:Extension>
      </Extensions>
    </Application>
  </Applications>
  <Capabilities/>
</Package>)xml");
		auto read = [](const QByteArray& xml, QStringView id) {
			auto data = QBuffer();
			data.setData(xml);
			data.open(QIODevice::ReadOnly);
			return qs::windows::sys::readAppxManifest(&data, id);
		};
		const auto terminal = read(manifest, u"App");
		QVERIFY(terminal.applicationFound);
		QVERIFY(!terminal.allowElevation);
		QCOMPARE(terminal.desktopExecutionAliases, QStringList({QStringLiteral("wt.exe")}));
		const auto other = read(manifest, u"Other");
		QCOMPARE(other.desktopExecutionAliases, QStringList({QStringLiteral("other.exe")}));
		QVERIFY(!read(manifest, u"app").applicationFound);
		QVERIFY(read(manifest, u"Uwp").desktopExecutionAliases.isEmpty());

		auto allowed = manifest;
		allowed.replace(
		    "<Capabilities/>",
		    "<Capabilities><rescap:Capability Name=\"allowElevation\"/></Capabilities>"
		);
		QVERIFY(read(allowed, u"App").allowElevation);
		QVERIFY(!read(allowed + "<broken", u"App").applicationFound);
	}
};

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	ProcessLaunchTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "process_launch_test.moc"

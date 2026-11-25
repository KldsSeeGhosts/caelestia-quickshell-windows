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
#include "../system/app_exec_alias.hpp"
#include "../system/appx_manifest.hpp"
#include "../system/shell_action_request.hpp"


namespace {

void appendUtf16Le(QByteArray& data, const QString& value) {
	const auto* utf16 = value.utf16();
	for (qsizetype i = 0; i < value.size(); ++i) {
		const auto unit = static_cast<quint16>(utf16[i]);
		data.append(static_cast<char>(unit & 0xff));
		data.append(static_cast<char>(unit >> 8));
	}
	data.append('\0');
	data.append('\0');
}

QByteArray appExecPayload(
    const QString& family,
    const QString& aumid,
    const QString& target,
    quint32 version = 3
) {
	auto data = QByteArray();
	for (auto shift: {0, 8, 16, 24}) data.append(static_cast<char>((version >> shift) & 0xff));
	appendUtf16Le(data, family);
	appendUtf16Le(data, aumid);
	appendUtf16Le(data, target);
	appendUtf16Le(data, QStringLiteral("0"));
	return data;
}

} // namespace

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
		const auto ambiguousParsed = parseLaunchCommand(ambiguous);
		QCOMPARE(ambiguousParsed.error, DWORD(ERROR_SUCCESS));
		QCOMPARE(
		    ambiguousParsed.executable,
		    QDir::toNativeSeparators(directory.filePath(QStringLiteral("Program.exe")))
		);
		QCOMPARE(ambiguousParsed.argumentTail, QStringLiteral("Files\\Vendor\\Missing.exe"));
		QCOMPARE(parseElevatedLaunchCommand(ambiguous).error, DWORD(ERROR_INVALID_PARAMETER));

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
	void executableTokenEndsBeforeArgumentPaths() {
		using namespace qs::windows::process;
		QTemporaryDir root;
		QVERIFY(root.isValid());
		QVERIFY(QDir(root.path()).mkpath(QStringLiteral("tools")));
		const auto executable = QDir::toNativeSeparators(
		    QDir(root.path()).filePath(QStringLiteral("tools/editor.exe"))
		);
		QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), executable));

		const auto withDocument = parseLaunchCommand(executable + QStringLiteral(" notes.txt"));
		QCOMPARE(withDocument.error, DWORD(ERROR_SUCCESS));
		QCOMPARE(withDocument.executable, executable);
		QCOMPARE(withDocument.argumentTail, QStringLiteral("notes.txt"));

		const auto withRelativePath = parseLaunchCommand(QStringLiteral("tool.exe src\\file.cpp"));
		QCOMPARE(withRelativePath.error, DWORD(ERROR_SUCCESS));
		QCOMPARE(withRelativePath.program, QStringLiteral("tool.exe"));
		QCOMPARE(withRelativePath.argumentTail, QStringLiteral("src\\file.cpp"));
	}

	void elevatedParsingRejectsAmbiguousUnquotedPrefixes() {
		using namespace qs::windows::process;
		QTemporaryDir root;
		QVERIFY(root.isValid());
		const auto bin = QDir::toNativeSeparators(QDir(root.path()).filePath(QStringLiteral("Bin.exe")));
		QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), bin));

		const auto ambiguous = parseElevatedLaunchCommand(
		    bin + QStringLiteral(" - staging\\Target.exe")
		);
		QCOMPARE(ambiguous.error, DWORD(ERROR_INVALID_PARAMETER));
		QVERIFY(ambiguous.executable.isEmpty());

		QVERIFY(QDir(root.path()).mkpath(QStringLiteral("bin dir")));
		const auto target = QDir::toNativeSeparators(
		    QDir(root.path()).filePath(QStringLiteral("bin dir/target.exe"))
		);
		QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), target));
		const auto quoted = parseElevatedLaunchCommand(
		    u'"' + target + QStringLiteral("\" --flag")
		);
		QCOMPARE(quoted.error, DWORD(ERROR_SUCCESS));
		QCOMPARE(quoted.executable, target);
		QCOMPARE(quoted.argumentTail, QStringLiteral("--flag"));
	}

	void appExecAliasPayloadMatchesExactPackageIdentity() {
		using namespace qs::windows::sys;
		const auto family = QStringLiteral("Microsoft.WindowsTerminal_8wekyb3d8bbwe");
		const auto aumid = family + QStringLiteral("!App");
		const auto target = QStringLiteral(
		    "C:\\Program Files\\WindowsApps\\Microsoft.WindowsTerminal_1.0_x64__8wekyb3d8bbwe\\wt.exe"
		);
		const auto parsed = parseAppExecAliasPayload(appExecPayload(family, aumid, target));
		QVERIFY(parsed.has_value());
		QCOMPARE(parsed->packageFamilyName, family);
		QCOMPARE(parsed->aumid, aumid);
		QCOMPARE(parsed->targetPath, QDir::toNativeSeparators(target));
		QVERIFY(appExecAliasMatches(*parsed, family, aumid));
		QVERIFY(!appExecAliasMatches(*parsed, family, family + QStringLiteral("!Other")));
		QVERIFY(!appExecAliasMatches(
		    *parsed, QStringLiteral("Microsoft.Other_8wekyb3d8bbwe"), aumid
		));
		QVERIFY(!parseAppExecAliasPayload(appExecPayload(family, aumid, target, 2)).has_value());
		QVERIFY(!parseAppExecAliasPayload(appExecPayload(family, aumid, target).chopped(3)).has_value());
	}


	void elevatedBoundaryRulesAreSharedByExplicitAndAutomaticElevation() {
		using namespace qs::windows::process;
		QTemporaryDir root;
		QVERIFY(root.isValid());
		const auto bin = QDir::toNativeSeparators(QDir(root.path()).filePath(QStringLiteral("Bin.exe")));
		QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), bin));

		const auto ambiguous = bin + QStringLiteral(" Staging Target.exe");
		const auto parsed = parseLaunchCommand(ambiguous);
		QCOMPARE(parsed.error, DWORD(ERROR_SUCCESS));
		QVERIFY(!canElevateParsedCommand(parsed));
		QVERIFY(!canUseAutomaticElevation(parsed, ERROR_ELEVATION_REQUIRED));
		QCOMPARE(parseElevatedLaunchCommand(ambiguous).error, DWORD(ERROR_INVALID_PARAMETER));

		const auto quoted = parseElevatedLaunchCommand(
		    u'"' + bin + QStringLiteral("\" Staging Target.exe")
		);
		QCOMPARE(quoted.error, DWORD(ERROR_SUCCESS));
		QVERIFY(canElevateParsedCommand(quoted));

		const auto bare = parseElevatedLaunchCommand(QStringLiteral("wt.exe -p PowerShell"));
		QCOMPARE(bare.error, DWORD(ERROR_SUCCESS));
		QVERIFY(canElevateParsedCommand(bare));
		QCOMPARE(
		    parseElevatedLaunchCommand(QStringLiteral("wt.exe src\\file.cpp")).error,
		    DWORD(ERROR_INVALID_PARAMETER)
		);
		const auto quotedArgument = parseElevatedLaunchCommand(
		    QStringLiteral("wt.exe \"src\\file.cpp\"")
		);
		QCOMPARE(quotedArgument.error, DWORD(ERROR_SUCCESS));

		// A structured target has an explicit executable boundary, so trailing
		// arguments cannot make it part of a longer unquoted path.
		QCOMPARE(validateElevatedTarget(bin), DWORD(ERROR_SUCCESS));
		QCOMPARE(validateElevatedTarget(root.path()), DWORD(ERROR_INVALID_PARAMETER));
	}

	void bareCommandLooksUpPathBeforeRejectingPathArguments() {
		using namespace qs::windows::process;
		QTemporaryDir root;
		QVERIFY(root.isValid());
		const auto tool = QDir::toNativeSeparators(QDir(root.path()).filePath(QStringLiteral("tool.exe")));
		QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), tool));

		const auto oldPath = qgetenv("PATH");
		qputenv("PATH", QDir::toNativeSeparators(root.path()).toUtf8() + ';' + oldPath);
		const auto parsed = parseLaunchCommand(QStringLiteral("tool src\\file.cpp"));
		if (oldPath.isNull()) qunsetenv("PATH");
		else qputenv("PATH", oldPath);

		QCOMPARE(parsed.error, DWORD(ERROR_SUCCESS));
		QCOMPARE(parsed.program, QStringLiteral("tool"));
		QCOMPARE(parsed.executable, tool);
		QCOMPARE(parsed.argumentTail, QStringLiteral("src\\file.cpp"));
	}

	void existingDirectoryTargetsUseShellOpenButNeverElevation() {
		using namespace qs::windows::process;
		QTemporaryDir root;
		QVERIFY(root.isValid());
		const auto directory = QDir::toNativeSeparators(root.path());
		const auto quoted = parseLaunchCommand(u'"' + directory + u'"');
		QCOMPARE(quoted.error, DWORD(ERROR_SUCCESS));
		QCOMPARE(quoted.executable, directory);
		QCOMPARE(quoted.kind, ProgramKind::Shell);
		QCOMPARE(parseElevatedLaunchCommand(u'"' + directory + u'"').error, DWORD(ERROR_INVALID_PARAMETER));
		QCOMPARE(validateElevatedTarget(directory), DWORD(ERROR_INVALID_PARAMETER));
	}

	void packageTargetsAreCanonicalAndConfinedToInstallRoot() {
		using namespace qs::windows::sys;
		QTemporaryDir root;
		QVERIFY(root.isValid());
		QVERIFY(QDir(root.path()).mkpath(QStringLiteral("package")));
		QVERIFY(QDir(root.path()).mkpath(QStringLiteral("outside")));
		const auto packageRoot = QDir::toNativeSeparators(
		    QDir(root.path()).filePath(QStringLiteral("package"))
		);
		const auto inside = QDir::toNativeSeparators(
		    QDir(packageRoot).filePath(QStringLiteral("app.exe"))
		);
		const auto outside = QDir::toNativeSeparators(
		    QDir(root.path()).filePath(QStringLiteral("outside/evil.exe"))
		);
		QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), inside));
		QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), outside));

		const auto canonicalInside = canonicalPackageTargetInsideRoot(inside, packageRoot);
		QVERIFY(canonicalInside.has_value());
		QCOMPARE(*canonicalInside, canonicalFilesystemPath(inside));
		QVERIFY(!canonicalPackageTargetInsideRoot(outside, packageRoot).has_value());

		// Model a junction inside the package resolving outside it. The injected
		// canonicalizer makes the escape deterministic without requiring symlink rights.
		const auto junctionInput = QDir::toNativeSeparators(
		    QDir(packageRoot).filePath(QStringLiteral("junction/evil.exe"))
		);
		const auto outsideCanonical = canonicalFilesystemPath(outside);
		const CanonicalPathProvider resolver = [&](const QString& path) {
			if (QDir::toNativeSeparators(path) == junctionInput) return outsideCanonical;
			return canonicalFilesystemPath(path);
		};
		QVERIFY(!canonicalPackageTargetInsideRoot(junctionInput, packageRoot, resolver).has_value());
	}

	void shellActionRequestIdsAreMonotonic() {
		const auto first = qs::windows::sys::detail::nextShellActionRequestId();
		const auto second = qs::windows::sys::detail::nextShellActionRequestId();
		const auto third = qs::windows::sys::detail::nextShellActionRequestId();
		QVERIFY(second > first);
		QVERIFY(third > second);
		auto queued = QVariantMap();
		qs::windows::sys::detail::addShellActionRequestId(queued, second);
		QCOMPARE(queued.value(QStringLiteral("requestId")).value<quint64>(), second);
	}

};

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	ProcessLaunchTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "process_launch_test.moc"

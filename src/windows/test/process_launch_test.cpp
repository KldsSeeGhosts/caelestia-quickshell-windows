#include <qcoreapplication.h>
#include <qdir.h>
#include <qfile.h>
#include <qprocess.h>
#include <qstringlist.h>
#include <qtemporarydir.h>
#include <qtest.h>

#include "../core/common.hpp"
#include "../process_launch.hpp"

using qs::windows::process::LaunchCommand;
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

	void unquotedSpacedPathUsesLongestExistingPrefix() {
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
		QCOMPARE(parsed.arguments, QStringList({QStringLiteral("--flag"), QStringLiteral("value")}));
		QCOMPARE(parsed.kind, ProgramKind::Console);
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
		QCOMPARE(parsed.arguments, QStringList({QStringLiteral("--flag"), QStringLiteral("two words")}));
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

	void appExecutionAliasIsConsoleWhenPresent() {
		const auto local = qEnvironmentVariable("LOCALAPPDATA");
		if (local.isEmpty()) QSKIP("LOCALAPPDATA is unavailable");
		const auto alias = QDir(local).filePath(QStringLiteral("Microsoft/WindowsApps/wt.exe"));
		if (!QFileInfo::exists(alias)) QSKIP("Windows Terminal execution alias is unavailable");

		QVERIFY(qs::windows::process::isAppExecutionAlias(alias));
		QCOMPARE(qs::windows::process::classifyProgram(alias), ProgramKind::Console);
	}

	void cleanEnvironmentNeverContainsSupervisorCredentials() {
		const auto saved = qs::Common::INITIAL_ENVIRONMENT;
		qs::Common::INITIAL_ENVIRONMENT.insert(
		    QStringLiteral("CAELESTIA_SUPERVISOR_PIPE"), QStringLiteral("pipe")
		);
		qs::Common::INITIAL_ENVIRONMENT.insert(
		    QStringLiteral("CAELESTIA_SUPERVISOR_TOKEN"), QStringLiteral("token")
		);
		qs::Common::INITIAL_ENVIRONMENT.insert(
		    QStringLiteral("QS_PROCESS_LAUNCH_TEST"), QStringLiteral("present")
		);

		const auto block = qs::windows::process::cleanEnvironmentBlock();
		qs::Common::INITIAL_ENVIRONMENT = saved;

		auto entries = QStringList();
		const auto* cursor = block.data();
		while (*cursor != L'\0') {
			const auto entry = QString::fromWCharArray(cursor);
			entries.append(entry);
			cursor += entry.size() + 1;
		}

		QVERIFY(entries.contains(QStringLiteral("QS_PROCESS_LAUNCH_TEST=present")));
		for (const auto& entry: entries) {
			QVERIFY(!entry.startsWith(QStringLiteral("CAELESTIA_SUPERVISOR_PIPE=")));
			QVERIFY(!entry.startsWith(QStringLiteral("CAELESTIA_SUPERVISOR_TOKEN=")));
		}
	}
};

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	ProcessLaunchTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "process_launch_test.moc"

#include <memory>

#include <qcoreapplication.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qprocess.h>
#include <qstringlist.h>
#include <qtemporarydir.h>
#include <qtest.h>

#include "../../core/instanceinfo.hpp"
#include "../../core/paths.hpp"

namespace qs::log {
void initLogCategoryLevel(const char*, QtMsgType) {}
} // namespace qs::log

namespace {

int runChild(const QString& scenario) {
	InstanceInfo::CURRENT.instanceId = "runtime-path-instance";
	QsPaths::init("runtime-path-shell", "runtime-path-config", {}, {}, {});

	auto* paths = QsPaths::instance();
	QStringList failures;
	auto* baseDir = paths->baseRunDir();
	auto* instanceDir = paths->instanceRunDir();
	auto* shellDir = paths->shellRunDir();

	if (scenario == "base-file") {
		if (baseDir != nullptr) failures.append("base runtime directory unexpectedly ready");
		if (instanceDir != nullptr) failures.append("instance runtime directory unexpectedly ready");
		if (shellDir != nullptr) failures.append("shell runtime directory unexpectedly ready");
	} else if (scenario == "instance-file") {
		if (baseDir == nullptr) failures.append("base runtime directory was not ready");
		if (instanceDir != nullptr) failures.append("instance runtime directory unexpectedly ready");
		if (shellDir == nullptr) failures.append("shell runtime directory was not independently ready");
	} else if (scenario == "shell-file") {
		if (baseDir == nullptr) failures.append("base runtime directory was not ready");
		if (instanceDir == nullptr) failures.append("instance runtime directory was not independently ready");
		if (shellDir != nullptr) failures.append("shell runtime directory unexpectedly ready");
	} else if (scenario == "valid") {
		if (baseDir == nullptr) failures.append("base runtime directory was not ready");
		if (instanceDir == nullptr) failures.append("instance runtime directory was not ready");
		if (shellDir == nullptr) failures.append("shell runtime directory was not ready");
	} else {
		qCritical() << "Unknown runtime-path fixture scenario:" << scenario;
		return 2;
	}

	// These are the same operations performed during startup. Keep exercising
	// them after a failed prerequisite to catch the original null dereference.
	paths->linkRunDir();
	paths->linkPathDir();
	// Launch aborts on a shell-link failure before reaching lock creation.
	if (scenario != "shell-file") paths->createLock();

	const auto basePath = QsPaths::basePath("runtime-path-check");
	if (scenario == "base-file" && !basePath.isEmpty())
		failures.append("basePath returned a relative or otherwise usable fallback");
	if (scenario != "base-file" && basePath.isEmpty())
		failures.append("basePath was empty despite a ready base runtime directory");

	for (const auto& failure: failures) qCritical() << failure;
	return failures.isEmpty() ? 0 : 1;
}

struct ScenarioRun {
	std::unique_ptr<QTemporaryDir> temporary;
	QString root;
	QString runtime;
	QString workingDirectory;
	QString output;
	QProcess::ExitStatus exitStatus = QProcess::CrashExit;
	int exitCode = -1;
	bool started = false;
	bool finished = false;
};

ScenarioRun runScenario(const QString& scenario) {
	ScenarioRun result;
	result.temporary = std::make_unique<QTemporaryDir>();
	if (!result.temporary->isValid()) return result;

	result.root = result.temporary->path();
	result.runtime = QDir(result.root).filePath("runtime");
	result.workingDirectory = QDir(result.root).filePath("working");
	QDir().mkpath(result.workingDirectory);

	if (scenario == "base-file") {
		QFile blocker(result.runtime);
		if (!blocker.open(QFile::WriteOnly)) return result;
		blocker.close();
	} else {
		QDir().mkpath(result.runtime);

		if (scenario == "instance-file") {
			QDir().mkpath(QDir(result.runtime).filePath("run"));
			QFile blocker(QDir(result.runtime).filePath("run/by-id"));
			if (!blocker.open(QFile::WriteOnly)) return result;
			blocker.close();
		} else if (scenario == "shell-file") {
			QDir().mkpath(QDir(result.runtime).filePath("run"));
			QFile blocker(QDir(result.runtime).filePath("run/by-shell"));
			if (!blocker.open(QFile::WriteOnly)) return result;
			blocker.close();
		}
	}

	auto environment = QProcessEnvironment::systemEnvironment();
	environment.insert("QS_RUNTIME_DIR", result.runtime);
	environment.insert("QS_DISABLE_CRASH_HANDLER", "1");
	environment.insert("QS_WINDOWS_TRAY_HOOK", "0");
	environment.insert("CAELESTIA_WINDOWS_HYPR", "0");
	environment.insert("CAELESTIA_WINDOWS_HYPR_DISPATCH", "0");
	environment.insert("QT_QPA_PLATFORM", "offscreen");
	environment.insert("QT_QUICK_BACKEND", "software");
	environment.insert("QT_FORCE_STDERR_LOGGING", "1");

	QProcess process;
	process.setProcessEnvironment(environment);
	process.setWorkingDirectory(result.workingDirectory);
	process.setProcessChannelMode(QProcess::MergedChannels);
	process.start(
	    QCoreApplication::applicationFilePath(),
	    {"--runtime-path-child", scenario}
	);
	result.started = process.waitForStarted(5000);
	if (!result.started) return result;

	result.finished = process.waitForFinished(15000);
	result.output = QString::fromLocal8Bit(process.readAll());
	if (result.finished) {
		result.exitStatus = process.exitStatus();
		result.exitCode = process.exitCode();
	}

	return result;
}

} // namespace

class RuntimePathsTest final: public QObject {
	Q_OBJECT

private slots:
	void baseDirectoryFileBlockIsControlled();
	void instanceOnlyFailureDoesNotFallBackToWorkingDirectory();
	void shellOnlyFailureDoesNotPublishPartialLinks();
	void validRuntimeCreatesExpectedBookkeeping();
};

void RuntimePathsTest::baseDirectoryFileBlockIsControlled() {
	auto run = runScenario("base-file");
	QVERIFY(run.temporary);
	QVERIFY2(run.started, qPrintable(run.output));
	QVERIFY2(run.finished, qPrintable(run.output));
	QVERIFY2(run.exitStatus == QProcess::NormalExit, qPrintable(run.output));
	QVERIFY2(run.exitCode == 0, qPrintable(run.output));
	QVERIFY(run.output.contains("Could not create base runtime directory"));
	QVERIFY(!QFileInfo::exists(QDir(run.workingDirectory).filePath("instance.lock")));
	QVERIFY(!QFileInfo::exists(QDir(run.workingDirectory).filePath("log.log")));
	QVERIFY(!QFileInfo::exists(QDir(run.workingDirectory).filePath("log.qslog")));
	QVERIFY(!QFileInfo::exists(QDir(run.runtime).filePath("run")));
}

void RuntimePathsTest::instanceOnlyFailureDoesNotFallBackToWorkingDirectory() {
	auto run = runScenario("instance-file");
	QVERIFY(run.temporary);
	QVERIFY2(run.started, qPrintable(run.output));
	QVERIFY2(run.finished, qPrintable(run.output));
	QVERIFY2(run.exitStatus == QProcess::NormalExit, qPrintable(run.output));
	QVERIFY2(run.exitCode == 0, qPrintable(run.output));
	QVERIFY(run.output.contains("Could not create instance runtime directory"));
	QVERIFY(!QFileInfo::exists(QDir(run.workingDirectory).filePath("instance.lock")));
	QVERIFY(!QFileInfo::exists(QDir(run.workingDirectory).filePath("log.log")));
	QVERIFY(!QFileInfo::exists(QDir(run.workingDirectory).filePath("log.qslog")));
	QVERIFY(!QFileInfo::exists(QDir(run.runtime).filePath("run/by-pid")));
	QVERIFY(QFileInfo::exists(QDir(run.runtime).filePath("run/by-path/runtime-path-config")));
}

void RuntimePathsTest::shellOnlyFailureDoesNotPublishPartialLinks() {
	auto run = runScenario("shell-file");
	QVERIFY(run.temporary);
	QVERIFY2(run.started, qPrintable(run.output));
	QVERIFY2(run.finished, qPrintable(run.output));
	QVERIFY2(run.exitStatus == QProcess::NormalExit, qPrintable(run.output));
	QVERIFY2(run.exitCode == 0, qPrintable(run.output));
	QVERIFY(run.output.contains("Could not create runtime directory"));
	QVERIFY(!QFileInfo::exists(QDir(run.workingDirectory).filePath("instance.lock")));
	QVERIFY(!QFileInfo::exists(QDir(run.workingDirectory).filePath("log.log")));
	QVERIFY(!QFileInfo::exists(QDir(run.workingDirectory).filePath("log.qslog")));
	QVERIFY(!QFileInfo::exists(QDir(run.runtime).filePath("run/by-pid")));
	QVERIFY(!QFileInfo::exists(QDir(run.runtime).filePath("run/by-path")));
	QVERIFY(!QFileInfo::exists(QDir(run.runtime).filePath("run/by-id/runtime-path-instance/instance.lock")));
}

void RuntimePathsTest::validRuntimeCreatesExpectedBookkeeping() {
	auto run = runScenario("valid");
	QVERIFY(run.temporary);
	QVERIFY2(run.started, qPrintable(run.output));
	QVERIFY2(run.finished, qPrintable(run.output));
	QVERIFY2(run.exitStatus == QProcess::NormalExit, qPrintable(run.output));
	QVERIFY2(run.exitCode == 0, qPrintable(run.output));
	QVERIFY(!QFileInfo::exists(QDir(run.workingDirectory).filePath("instance.lock")));
	QVERIFY(!QFileInfo::exists(QDir(run.workingDirectory).filePath("log.log")));
	QVERIFY(!QFileInfo::exists(QDir(run.workingDirectory).filePath("log.qslog")));
	QVERIFY(QFileInfo::exists(QDir(run.runtime).filePath("run/by-id/runtime-path-instance/instance.lock")));
	QVERIFY(QFileInfo::exists(QDir(run.runtime).filePath("run/by-shell/runtime-path-shell/runtime-path-instance")));
	QVERIFY(QFileInfo::exists(QDir(run.runtime).filePath("run/by-path/runtime-path-config")));
	QVERIFY(QDir(QDir(run.runtime).filePath("run/by-pid")).entryList(QDir::Files | QDir::NoDotAndDotDot).size() == 1);
}

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);

	if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--runtime-path-child")
		return runChild(QString::fromLocal8Bit(argv[2]));

	RuntimePathsTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "runtime_paths_test.moc"

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
	// First operation matches launch.cpp, with all directory states still unknown.
	const auto linked = paths->linkRunDir();
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
		if (instanceDir == nullptr)
			failures.append("instance runtime directory was not independently ready");
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
	if (linked != (scenario == "valid")) failures.append("incorrect runtime linking result");
	if (paths->instanceRunDir() != instanceDir)
		failures.append("instance state changed after shell directory initialization");
	if (paths->linkPathDir() != (scenario == "valid" || scenario == "instance-file"))
		failures.append("incorrect path linking result");
	// A shell-only failure leaves the instance directory independently usable,
	// but launch must stop before lock creation.
	if (scenario != "shell-file" && paths->createLock() != (scenario == "valid"))
		failures.append("incorrect lock creation result");

	const auto basePath = QsPaths::basePath("runtime-path-check");
	if (scenario == "base-file" && !basePath.isEmpty())
		failures.append("basePath returned a relative or otherwise usable fallback");
	if (scenario != "base-file" && basePath.isEmpty())
		failures.append("basePath was empty despite a ready base runtime directory");

	for (const auto& failure: failures) qCritical() << failure;
	return failures.isEmpty() ? 0 : 1;
}

int runBookkeepingChild(const QString& scenario) {
	InstanceInfo::CURRENT.instanceId = "runtime-path-instance";
	QsPaths::init("runtime-path-shell", "runtime-path-config", {}, {}, {});
	auto* paths = QsPaths::instance();
	auto* base = paths->baseRunDir();
	if (!base || !paths->instanceRunDir() || !paths->shellRunDir()) return 2;

	QString blocker;
	if (scenario == "pid-directory") {
		blocker = base->filePath("by-pid");
		QFile file(blocker);
		if (!file.open(QFile::WriteOnly)) return 2;
	} else {
		if (scenario == "shell-link")
			blocker = paths->shellRunDir()->filePath(InstanceInfo::CURRENT.instanceId);
		else if (scenario == "pid-link")
			blocker = base->filePath("by-pid/" + QString::number(QCoreApplication::applicationPid()));
		else if (scenario == "path-directory") {
			QFile file(base->filePath("by-path"));
			if (!file.open(QFile::WriteOnly)) return 2;
		} else if (scenario == "path-link") blocker = base->filePath("by-path/runtime-path-config");
		else if (scenario == "lock") blocker = paths->instanceRunDir()->filePath("instance.lock");
		else return 2;
		if (!blocker.isEmpty() && !QDir().mkpath(blocker)) return 2;
	}

	// This is the required-bookkeeping gate used by real launch. No fake ready
	// or IPC server is substituted here; actual launch is checked separately.
	const auto ready = paths->linkRunDir() && paths->linkPathDir() && paths->createLock();
	if (ready) return 1;
	if (scenario == "pid-link"
	    && QFileInfo::exists(paths->shellRunDir()->filePath(InstanceInfo::CURRENT.instanceId)))
		return 1; // Failed PID publication must roll back the shell link.
	return 0;
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
	    {scenario.startsWith("bookkeeping-") ? "--bookkeeping-child" : "--runtime-path-child",
	     scenario.startsWith("bookkeeping-") ? scenario.sliced(12) : scenario}
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
	void requiredBookkeepingFailure_data();
	void requiredBookkeepingFailure();
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
	QVERIFY(!QFileInfo::exists(
	    QDir(run.runtime).filePath("run/by-id/runtime-path-instance/instance.lock")
	));
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
	QVERIFY(
	    QFileInfo::exists(QDir(run.runtime).filePath("run/by-id/runtime-path-instance/instance.lock"))
	);
	QVERIFY(
	    QFileInfo::exists(
	        QDir(run.runtime).filePath("run/by-shell/runtime-path-shell/runtime-path-instance")
	    )
	);
	QVERIFY(QFileInfo::exists(QDir(run.runtime).filePath("run/by-path/runtime-path-config")));
	QVERIFY(
	    QDir(QDir(run.runtime).filePath("run/by-pid"))
	        .entryList(QDir::Files | QDir::NoDotAndDotDot)
	        .size()
	    == 1
	);
}

void RuntimePathsTest::requiredBookkeepingFailure_data() {
	QTest::addColumn<QString>("scenario");
	for (const auto* scenario:
	     {"pid-directory", "shell-link", "pid-link", "path-directory", "path-link", "lock"})
		QTest::newRow(scenario) << QString("bookkeeping-") + scenario;
}

void RuntimePathsTest::requiredBookkeepingFailure() {
	QFETCH(QString, scenario);
	auto run = runScenario(scenario);
	QVERIFY(run.temporary);
	QVERIFY2(run.started && run.finished, qPrintable(run.output));
	QVERIFY2(run.exitStatus == QProcess::NormalExit && run.exitCode == 0, qPrintable(run.output));
	QVERIFY2(run.output.contains("Could not create"), qPrintable(run.output));
	QVERIFY(QDir(run.workingDirectory).isEmpty());
}

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);

	if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--runtime-path-child")
		return runChild(QString::fromLocal8Bit(argv[2]));
	if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--bookkeeping-child")
		return runBookkeepingChild(QString::fromLocal8Bit(argv[2]));

	RuntimePathsTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "runtime_paths_test.moc"

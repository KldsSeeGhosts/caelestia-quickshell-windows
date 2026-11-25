#pragma once

#include <qt_windows.h>

#include <optional>
#include <string>

#include <qstring.h>
#include <qstringlist.h>
#include <qtypes.h>

namespace qs::windows::process {

enum class LaunchStatus : quint8 {
	Started,
	Cancelled,
	Failed,
	Indeterminate, // Explorer disconnected after receiving the request; do not retry.
};

struct LaunchResult {
	LaunchStatus status = LaunchStatus::Failed;
	DWORD error = ERROR_GEN_FAILURE;
	bool usedExplorer = false;
	bool usedProcess = false;
	bool usedShell = false;

	[[nodiscard]] bool started() const { return this->status == LaunchStatus::Started; }
};

enum class ProgramKind : quint8 {
	Unresolved,
	Shell,
	Gui,
	Console,
};

struct LaunchCommand {
	QString program;
	QString argumentTail;
	QString executable;
	ProgramKind kind = ProgramKind::Unresolved;
	DWORD error = ERROR_FILE_NOT_FOUND;
	bool quotedProgram = false;

	[[nodiscard]] bool useProcess() const
	{
		return this->kind == ProgramKind::Gui || this->kind == ProgramKind::Console;
	}
};

// Parsing and classification are exposed for the offscreen fixtures. They are
// pure apart from executable lookup/inspection and never launch a process.
[[nodiscard]] LaunchCommand parseLaunchCommand(const QString& command);
// Elevated raw commands do not guess at ambiguous unquoted path boundaries.
[[nodiscard]] LaunchCommand parseElevatedLaunchCommand(const QString& command);
[[nodiscard]] QString quoteCommandArgument(const QString& argument);
[[nodiscard]] QString joinCommandArguments(const QStringList& arguments);
[[nodiscard]] QString joinCommandLine(const QString& program, const QStringList& arguments);
[[nodiscard]] QString resolveExecutable(const QString& program);
[[nodiscard]] bool isAppExecutionAlias(const QString& path);
[[nodiscard]] ProgramKind classifyProgram(const QString& path);

// UTF-16 environment block for a process detached from the shell. The block
// starts from Common::INITIAL_ENVIRONMENT and always strips supervisor credentials.
[[nodiscard]] std::wstring cleanEnvironmentBlock();

// Shell target launch. Open verbs try Explorer's IShellDispatch2 first so the
// child receives Explorer's environment and parentage. The local fallback
// runs ShellExecuteExW in a helper with the original environment.
[[nodiscard]] LaunchResult shellExecuteTarget(
    const QString& target,
    const QString& arguments = QString(),
    const QString& workingDirectory = QString(),
    const QString& verb = QStringLiteral("open"),
    HWND owner = nullptr,
    bool invokeIdList = false,
    bool useExplorer = true
);

// Internal early-entry mode for that helper. An ordinary invocation returns
// nullopt so it can continue through the normal CLI.
[[nodiscard]] std::optional<DWORD> runShellOpenHelper(int argc, char** argv);

// Unified command launch shared by keybinds and dispatch exec.
[[nodiscard]] LaunchResult launchCommand(
    const QString& command,
    const QString& workingDirectory = QString(),
    HWND owner = nullptr
);
[[nodiscard]] LaunchResult launchElevatedCommand(
    const QString& command,
    const QString& workingDirectory = QString(),
    HWND owner = nullptr
);

namespace detail {

[[nodiscard]] inline LaunchResult launchResultForError(DWORD error) {
	return {
	    .status = error == ERROR_SUCCESS ? LaunchStatus::Started
	                                      : error == ERROR_CANCELLED ? LaunchStatus::Cancelled
	                                                                 : LaunchStatus::Failed,
	    .error = error,
	};
}

template <typename Fallback>
[[nodiscard]] LaunchResult fallbackOnFailure(LaunchResult result, Fallback&& fallback) {
	return result.status == LaunchStatus::Failed ? fallback() : result;
}

template <typename ExplorerLaunch, typename CleanLaunch>
[[nodiscard]] LaunchResult openWithFallback(ExplorerLaunch&& explorer, CleanLaunch&& clean) {
	return fallbackOnFailure(explorer(), [&] { return clean(cleanEnvironmentBlock()); });
}

} // namespace detail

} // namespace qs::windows::process

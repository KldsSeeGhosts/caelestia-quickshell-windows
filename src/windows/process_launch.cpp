// The COM shell headers use the Windows SDK's interface macro, which
// qt_windows.h undefines. Include them before process_launch.hpp pulls it in.
#include <windows.h>

#include <objbase.h>
#include <exdisp.h>
#include <shellapi.h>
#include <shldisp.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include "process_launch.hpp"

#include <algorithm>
#include <vector>

#include <qcoreapplication.h>
#include <qdir.h>
#include <qendian.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qprocess.h>
#include <qstandardpaths.h>
#include <qtenvironmentvariables.h>

#include "../core/common.hpp"

using Microsoft::WRL::ComPtr;

namespace qs::windows::process {

namespace {

#ifndef IO_REPARSE_TAG_APPEXECLINK
constexpr DWORD IO_REPARSE_TAG_APPEXECLINK = 0x8000001BL;
#endif

bool looksLikePath(const QString& program) {
	return program.contains(u'/') || program.contains(u'\\')
	    || (program.size() > 2 && program.at(0).isLetter() && program.at(1) == u':');
}

QString normalizedExistingFile(const QString& path) {
	auto info = QFileInfo(path);
	if (!info.isFile()) return QString();
	return QDir::toNativeSeparators(info.absoluteFilePath());
}

QStringList executableExtensions() {
	auto extensions = QStringList();
	const auto pathext = qEnvironmentVariable("PATHEXT");
	for (const auto& extension: pathext.split(u';', Qt::SkipEmptyParts)) {
		extensions.append(extension.toLower());
	}
	for (const auto* extension: {".exe", ".com", ".bat", ".cmd"}) {
		const auto value = QString::fromLatin1(extension);
		if (!extensions.contains(value)) extensions.append(value);
	}
	return extensions;
}

QString resolvePathCandidate(const QString& candidate) {
	if (auto existing = normalizedExistingFile(candidate); !existing.isEmpty()) return existing;

	// Windows does not require typing PATHEXT. Mirror that lookup for explicit
	// paths as well so lpApplicationName receives the real file instead of a
	// best-effort prefix.
	for (const auto& extension: executableExtensions()) {
		if (candidate.endsWith(extension, Qt::CaseInsensitive)) continue;
		if (auto existing = normalizedExistingFile(candidate + extension); !existing.isEmpty()) {
			return existing;
		}
	}

	return QString();
}

QString resolvePathProgram(const QString& program) {
	return resolvePathCandidate(program);
}

QString resolvePathProgram(const QStringList& tokens, bool firstWasQuoted, qsizetype& consumed) {
	consumed = 1;
	if (tokens.isEmpty()) return QString();

	const auto& first = tokens.first();
	if (!looksLikePath(first) || firstWasQuoted) {
		return resolvePathCandidate(first);
	}

	// QProcess::splitCommand correctly removes quotes, but legacy exec lines
	// also carry unquoted paths such as C:\Program Files\app.exe. Choose the
	// longest existing prefix so a space in the path is not mistaken for the
	// first argument boundary.
	for (qsizetype count = tokens.size(); count >= 1; --count) {
		const auto candidate = tokens.first(count).join(u' ');
		if (auto resolved = resolvePathCandidate(candidate); !resolved.isEmpty()) {
			consumed = count;
			return resolved;
		}
	}

	return QString();
}

bool underWindowsApps(const QString& path) {
	const auto native = QDir::toNativeSeparators(path).toLower();
	return native.contains(QStringLiteral("\\microsoft\\windowsapps\\"));
}

class ComApartment {
public:
	explicit ComApartment(DWORD coinit): hr(CoInitializeEx(nullptr, coinit)) {}
	ComApartment(const ComApartment&) = delete;
	ComApartment& operator=(const ComApartment&) = delete;
	~ComApartment() {
		if (SUCCEEDED(this->hr)) CoUninitialize();
	}

	HRESULT hr;
};

class Bstr {
public:
	explicit Bstr(const QString& text): value(SysAllocStringLen(nullptr, static_cast<UINT>(text.size()))) {
		if (this->value != nullptr && !text.isEmpty()) {
			std::copy_n(reinterpret_cast<const wchar_t*>(text.utf16()), text.size(), this->value);
		}
	}
	Bstr(const Bstr&) = delete;
	Bstr& operator=(const Bstr&) = delete;
	~Bstr() {
		if (this->value != nullptr) SysFreeString(this->value);
	}

	BSTR get() const { return this->value; }

private:
	BSTR value;
};

struct Variant {
	Variant() { VariantInit(&this->value); }
	Variant(const Variant&) = delete;
	Variant& operator=(const Variant&) = delete;
	~Variant() { VariantClear(&this->value); }

	void setString(const QString& text) {
		VariantClear(&this->value);
		this->value.vt = VT_BSTR;
		auto wide = text.toStdWString();
		this->value.bstrVal = SysAllocStringLen(wide.data(), static_cast<UINT>(wide.size()));
	}

	void setInt(long number) {
		VariantClear(&this->value);
		this->value.vt = VT_I4;
		this->value.lVal = number;
	}

	VARIANT value;
};

HRESULT explorerShellExecute(
    const QString& target,
    const QString& arguments,
    const QString& workingDirectory,
    const QString& verb
) {
	auto com = ComApartment(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	if (FAILED(com.hr)) return com.hr;

	ComPtr<IShellWindows> windows;
	auto hr = CoCreateInstance(
	    CLSID_ShellWindows,
	    nullptr,
	    CLSCTX_ALL,
	    IID_PPV_ARGS(&windows)
	);
	if (FAILED(hr)) return hr;

	VARIANT location;
	VariantInit(&location);
	location.vt = VT_I4;
	location.lVal = CSIDL_DESKTOP;
	VARIANT empty;
	VariantInit(&empty);

	ComPtr<IDispatch> windowDispatch;
	long hwnd = 0;
	hr = windows->FindWindowSW(
	    &location,
	    &empty,
	    SWC_DESKTOP,
	    &hwnd,
	    SWFO_NEEDDISPATCH,
	    &windowDispatch
	);
	if (FAILED(hr) || windowDispatch == nullptr) return FAILED(hr) ? hr : E_FAIL;

	ComPtr<IServiceProvider> provider;
	hr = windowDispatch->QueryInterface(IID_PPV_ARGS(&provider));
	if (FAILED(hr)) return hr;

	ComPtr<IShellBrowser> browser;
	hr = provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser));
	if (FAILED(hr) || browser == nullptr) return FAILED(hr) ? hr : E_FAIL;

	ComPtr<IShellView> view;
	hr = browser->QueryActiveShellView(&view);
	if (FAILED(hr) || view == nullptr) return FAILED(hr) ? hr : E_FAIL;

	ComPtr<IDispatch> viewDispatch;
	hr = view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&viewDispatch));
	if (FAILED(hr) || viewDispatch == nullptr) return FAILED(hr) ? hr : E_FAIL;

	ComPtr<IShellFolderViewDual> folderView;
	hr = viewDispatch->QueryInterface(IID_PPV_ARGS(&folderView));
	if (FAILED(hr) || folderView == nullptr) return FAILED(hr) ? hr : E_FAIL;

	ComPtr<IDispatch> applicationDispatch;
	hr = folderView->get_Application(&applicationDispatch);
	if (FAILED(hr) || applicationDispatch == nullptr) return FAILED(hr) ? hr : E_FAIL;

	ComPtr<IShellDispatch2> shell;
	hr = applicationDispatch->QueryInterface(IID_PPV_ARGS(&shell));
	if (FAILED(hr) || shell == nullptr) return FAILED(hr) ? hr : E_FAIL;

	Bstr file(target);
	if (file.get() == nullptr) return E_OUTOFMEMORY;

	Variant args;
	if (!arguments.isEmpty()) args.setString(arguments);
	Variant directory;
	if (!workingDirectory.isEmpty()) directory.setString(workingDirectory);
	Variant operation;
	if (!verb.isEmpty()) operation.setString(verb);
	Variant show;
	show.setInt(SW_SHOWNORMAL);

	return shell->ShellExecute(file.get(), args.value, directory.value, operation.value, show.value);
}

LaunchResult shellExecuteExTarget(
    const QString& target,
    const QString& arguments,
    const QString& workingDirectory,
    const QString& verb,
    HWND owner,
    bool invokeIdList
) {
	auto result = LaunchResult();
	result.usedShell = true;

	auto wtarget = target.toStdWString();
	auto warguments = arguments.toStdWString();
	auto wdirectory = workingDirectory.toStdWString();
	auto wverb = verb.toStdWString();

	auto info = SHELLEXECUTEINFOW {};
	info.cbSize = sizeof(info);
	info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
	if (invokeIdList) info.fMask |= SEE_MASK_INVOKEIDLIST;
	info.hwnd = owner;
	info.lpVerb = wverb.empty() ? nullptr : wverb.c_str();
	info.lpFile = wtarget.c_str();
	info.lpParameters = warguments.empty() ? nullptr : warguments.c_str();
	info.lpDirectory = wdirectory.empty() ? nullptr : wdirectory.c_str();
	info.nShow = SW_SHOWNORMAL;

	if (ShellExecuteExW(&info)) {
		result.status = LaunchStatus::Started;
		result.error = ERROR_SUCCESS;
		return result;
	}

	result.error = GetLastError();
	result.status = result.error == ERROR_CANCELLED ? LaunchStatus::Cancelled : LaunchStatus::Failed;
	return result;
}

LaunchResult createProcess(const LaunchCommand& command, const QString& workingDirectory) {
	auto result = LaunchResult();
	result.usedProcess = true;

	if (command.executable.isEmpty()) {
		result.error = ERROR_FILE_NOT_FOUND;
		return result;
	}

	auto application = command.executable.toStdWString();
	auto commandLineText = joinCommandLine(command.executable, command.arguments);
	auto commandLine = commandLineText.toStdWString();
	auto directory = workingDirectory.toStdWString();
	auto environment = cleanEnvironmentBlock();

	STARTUPINFOW startup {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION processInformation {};

	DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP;
	if (command.kind == ProgramKind::Console) {
		flags |= CREATE_NEW_CONSOLE;
	} else {
		flags |= CREATE_NO_WINDOW;
	}

	const auto ok = CreateProcessW(
	    application.c_str(),
	    commandLine.data(),
	    nullptr,
	    nullptr,
	    FALSE,
	    flags,
	    environment.data(),
	    directory.empty() ? nullptr : directory.c_str(),
	    &startup,
	    &processInformation
	);

	if (!ok) {
		result.error = GetLastError();
		result.status = result.error == ERROR_CANCELLED ? LaunchStatus::Cancelled : LaunchStatus::Failed;
		return result;
	}

	CloseHandle(processInformation.hProcess);
	CloseHandle(processInformation.hThread);
	result.status = LaunchStatus::Started;
	result.error = ERROR_SUCCESS;
	return result;
}

QString targetFor(const LaunchCommand& command) {
	return command.executable.isEmpty() ? command.program : command.executable;
}

QString defaultWorkingDirectory(const QString& workingDirectory) {
	return workingDirectory.isEmpty() ? QDir::toNativeSeparators(QDir::homePath()) : workingDirectory;
}

} // namespace

QString quoteCommandArgument(const QString& argument) {
	if (!argument.isEmpty()
	    && std::ranges::none_of(argument, [](QChar c) { return c.isSpace() || c == u'"'; }))
	{
		return argument;
	}

	QString quoted;
	quoted.reserve(argument.size() + 2);
	quoted.append(u'"');

	auto backslashes = 0;
	for (const auto c: argument) {
		if (c == u'\\') {
			backslashes++;
			continue;
		}

		if (c == u'"') {
			quoted.append(QString((backslashes * 2) + 1, u'\\'));
			quoted.append(u'"');
		} else {
			if (backslashes != 0) quoted.append(QString(backslashes, u'\\'));
			quoted.append(c);
		}
		backslashes = 0;
	}

	if (backslashes != 0) quoted.append(QString(backslashes * 2, u'\\'));
	quoted.append(u'"');
	return quoted;
}

QString joinCommandArguments(const QStringList& arguments) {
	QStringList quoted;
	quoted.reserve(arguments.size());
	for (const auto& argument: arguments) quoted.append(quoteCommandArgument(argument));
	return quoted.join(u' ');
}

QString joinCommandLine(const QString& program, const QStringList& arguments) {
	auto line = quoteCommandArgument(program);
	const auto tail = joinCommandArguments(arguments);
	if (!tail.isEmpty()) line += u' ' + tail;
	return line;
}

QString resolveExecutable(const QString& program) {
	if (program.isEmpty()) return QString();

	if (looksLikePath(program)) return resolvePathProgram(program);

	// A file beside the shell wins over PATH, matching the keybind behavior that
	// shipped before the unified launcher.
	if (!QCoreApplication::applicationDirPath().isEmpty()) {
		for (const auto& candidate: {
		         QDir(QCoreApplication::applicationDirPath()).filePath(program),
		         QDir(QCoreApplication::applicationDirPath()).filePath(program + QStringLiteral(".exe")),
		     })
		{
			if (auto existing = normalizedExistingFile(candidate); !existing.isEmpty()) return existing;
		}
	}

	auto found = QStandardPaths::findExecutable(program);
	if (found.isEmpty() && QFileInfo(program).suffix().isEmpty()) {
		for (const auto& extension: executableExtensions()) {
			found = QStandardPaths::findExecutable(program + extension);
			if (!found.isEmpty()) break;
		}
	}

	return found.isEmpty() ? QString() : QDir::toNativeSeparators(found);
}

bool isAppExecutionAlias(const QString& path) {
	const auto wide = QDir::toNativeSeparators(path).toStdWString();
	WIN32_FIND_DATAW data {};
	auto* find = FindFirstFileW(wide.c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) return false;

	const auto isAlias = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
	                  && data.dwReserved0 == IO_REPARSE_TAG_APPEXECLINK;
	FindClose(find);
	return isAlias;
}

ProgramKind classifyProgram(const QString& path) {
	if (path.isEmpty()) return ProgramKind::Unresolved;
	if (isAppExecutionAlias(path)) return ProgramKind::Console;

	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		// App execution aliases are reparse points and some builds cannot open
		// the link target through QFile. Aliases under WindowsApps launch console
		// entry points often enough that hiding their console is never correct.
		if (underWindowsApps(path)) return ProgramKind::Console;
		return ProgramKind::Shell;
	}

	const auto dos = file.read(0x40);
	if (dos.size() < 0x40 || !dos.startsWith("MZ")) return ProgramKind::Shell;
	const auto peOffset = qFromLittleEndian<quint32>(dos.constData() + 0x3c);
	if (!file.seek(peOffset)) return ProgramKind::Shell;

	const auto header = file.read(4 + 20 + 70);
	if (header.size() < 4 + 20 + 70 || !header.startsWith(QByteArray("PE\0\0", 4))) {
		return ProgramKind::Shell;
	}

	const auto subsystem = qFromLittleEndian<quint16>(header.constData() + 4 + 20 + 68);
	return subsystem == IMAGE_SUBSYSTEM_WINDOWS_CUI ? ProgramKind::Console : ProgramKind::Gui;
}

LaunchCommand parseLaunchCommand(const QString& command) {
	auto parsed = LaunchCommand {.input = command};
	const auto tokens = QProcess::splitCommand(command);
	if (tokens.isEmpty()) return parsed;

	const auto trimmed = command.trimmed();
	const auto firstWasQuoted = trimmed.startsWith(u'"') || trimmed.startsWith(u'\'');
	qsizetype consumed = 1;
	parsed.executable = resolvePathProgram(tokens, firstWasQuoted, consumed);

	if (parsed.executable.isEmpty() && !looksLikePath(tokens.first())) {
		parsed.executable = resolveExecutable(tokens.first());
	}

	if (consumed == 1 || parsed.executable.isEmpty()) {
		parsed.program = tokens.first();
		parsed.arguments = tokens.mid(1);
	} else {
		parsed.program = tokens.first(consumed).join(u' ');
		parsed.arguments = tokens.mid(consumed);
	}

	parsed.kind = classifyProgram(parsed.executable);
	return parsed;
}

std::wstring cleanEnvironmentBlock() {
	auto environment = Common::INITIAL_ENVIRONMENT;
	if (environment.isEmpty()) environment = QProcessEnvironment::systemEnvironment();

	// Defense in depth: the launch path also removes these before capturing the
	// snapshot, but no process created through this helper may receive them.
	environment.remove(QStringLiteral("CAELESTIA_SUPERVISOR_PIPE"));
	environment.remove(QStringLiteral("CAELESTIA_SUPERVISOR_TOKEN"));

	auto keys = environment.keys();
	keys.sort(Qt::CaseInsensitive);

	std::wstring block;
	for (const auto& key: keys) {
		if (key.isEmpty() || key.contains(u'=')) continue;
		const auto line = key + u'=' + environment.value(key);
		const auto wide = line.toStdWString();
		block.append(wide);
		block.push_back(L'\0');
	}
	block.push_back(L'\0');
	return block;
}

LaunchResult shellExecuteTarget(
    const QString& target,
    const QString& arguments,
    const QString& workingDirectory,
    const QString& verb,
    HWND owner,
    bool invokeIdList,
    bool useExplorer
) {
	if (target.isEmpty()) {
		return {.status = LaunchStatus::Failed, .error = ERROR_FILE_NOT_FOUND};
	}

	const auto directory = defaultWorkingDirectory(workingDirectory);
	if (useExplorer && verb.compare(QStringLiteral("open"), Qt::CaseInsensitive) == 0) {
		const auto hr = explorerShellExecute(target, arguments, directory, verb);
		if (SUCCEEDED(hr)) {
			return {
			    .status = LaunchStatus::Started,
			    .error = ERROR_SUCCESS,
			    .usedExplorer = true,
			};
		}
	}

	return shellExecuteExTarget(target, arguments, directory, verb, owner, invokeIdList);
}

LaunchResult launchCommand(const QString& command, const QString& workingDirectory, HWND owner) {
	const auto parsed = parseLaunchCommand(command);
	if (parsed.program.isEmpty()) {
		return {.status = LaunchStatus::Failed, .error = ERROR_FILE_NOT_FOUND};
	}

	const auto directory = defaultWorkingDirectory(workingDirectory);
	const auto arguments = joinCommandArguments(parsed.arguments);
	const auto target = targetFor(parsed);

	// Explorer owns normal application launches when it is available. This keeps
	// shell-specific Qt/pragma state out of the child even when the shell itself
	// had to mutate its own process environment.
	const auto explorer = explorerShellExecute(target, arguments, directory, QStringLiteral("open"));
	if (SUCCEEDED(explorer)) {
		return {
		    .status = LaunchStatus::Started,
		    .error = ERROR_SUCCESS,
		    .usedExplorer = true,
		};
	}

	if (parsed.useProcess()) {
		auto process = createProcess(parsed, directory);
		if (process.started() || process.status == LaunchStatus::Cancelled) return process;
	}

	return shellExecuteExTarget(
	    target,
	    arguments,
	    directory,
	    QStringLiteral("open"),
	    owner,
	    false
	);
}

LaunchResult launchElevatedCommand(
    const QString& command,
    const QString& workingDirectory,
    HWND owner
) {
	const auto parsed = parseLaunchCommand(command);
	if (parsed.program.isEmpty()) {
		return {.status = LaunchStatus::Failed, .error = ERROR_FILE_NOT_FOUND};
	}

	return shellExecuteExTarget(
	    targetFor(parsed),
	    joinCommandArguments(parsed.arguments),
	    defaultWorkingDirectory(workingDirectory),
	    QStringLiteral("runas"),
	    owner,
	    false
	);
}

} // namespace qs::windows::process

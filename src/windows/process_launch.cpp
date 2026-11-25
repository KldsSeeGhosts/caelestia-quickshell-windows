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
#include <cwchar>

#include <qcoreapplication.h>
#include <qdir.h>
#include <qendian.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qstandardpaths.h>
#include <qtenvironmentvariables.h>

#include "../core/common.hpp"

using Microsoft::WRL::ComPtr;

namespace qs::windows::process {

namespace {

#ifndef IO_REPARSE_TAG_APPEXECLINK
constexpr DWORD IO_REPARSE_TAG_APPEXECLINK = 0x8000001BL;
#endif

constexpr auto SHELL_OPEN_HELPER_ARG = "--internal-windows-shell-open";

bool looksLikePath(QStringView program) {
	return program.contains(u'/') || program.contains(u'\\')
	    || (program.size() >= 2 && program.at(0).isLetter() && program.at(1) == u':');
}

bool isShellName(QStringView program) {
	const auto colon = program.indexOf(u':');
	if (colon < 2 || !program.front().isLetter()) return false;
	return std::ranges::all_of(program.first(colon), [](QChar c) {
		return c.isLetterOrNumber() || c == u'+' || c == u'-' || c == u'.';
	});
}

bool isCommandSpace(QChar c) {
	return c == u' ' || c == u'\t';
}

qsizetype skipCommandSpace(QStringView line, qsizetype position) {
	while (position < line.size() && isCommandSpace(line[position])) ++position;
	return position;
}

qsizetype endOfWord(QStringView line, qsizetype position) {
	while (position < line.size() && !isCommandSpace(line[position])) ++position;
	return position;
}

bool hasFileExtension(QStringView path) {
	const auto separator = std::max(path.lastIndexOf(u'\\'), path.lastIndexOf(u'/'));
	const auto dot = path.lastIndexOf(u'.');
	return dot > separator && dot + 1 < path.size();
}

bool hasExecutableExtension(QStringView path) {
	const auto separator = std::max(path.lastIndexOf(u'\\'), path.lastIndexOf(u'/'));
	const auto dot = path.lastIndexOf(u'.');
	if (dot <= separator || dot + 1 >= path.size()) return false;
	const auto extension = path.sliced(dot + 1);
	return extension.compare(u"exe", Qt::CaseInsensitive) == 0
	    || extension.compare(u"com", Qt::CaseInsensitive) == 0
	    || extension.compare(u"bat", Qt::CaseInsensitive) == 0
	    || extension.compare(u"cmd", Qt::CaseInsensitive) == 0;
}

bool isSeparateArgument(QStringView word) {
	if (word.startsWith(u'-') || word.startsWith(u'/')) {
		const auto option = word.sliced(1);
		return option.contains(u'=') || (!looksLikePath(option) && !hasFileExtension(option));
	}
	return word.startsWith(u'"') || word.startsWith(QStringLiteral("\\\\"))
	    || (word.size() >= 2 && word[0].isLetter() && word[1] == u':') || isShellName(word);
}

bool hasAmbiguousRelativePath(QStringView line, qsizetype firstEnd) {
	for (auto start = skipCommandSpace(line, firstEnd); start < line.size();) {
		const auto end = endOfWord(line, start);
		const auto word = line.sliced(start, end - start);
		if (isSeparateArgument(word)) break;
		if (looksLikePath(word)) {
			if (word.startsWith(QStringLiteral(".\\")) || word.startsWith(QStringLiteral("..\\"))
			    || word.startsWith(QStringLiteral("./")) || word.startsWith(QStringLiteral("../")))
			{
				break;
			}
			// "Program Files\tool.exe" could be a relative target or a
			// bare executable followed by a relative path. Require quotes.
			return true;
		}
		start = skipCommandSpace(line, end);
	}
	return false;
}

// An unquoted path with spaces needs an identifiable end. A file extension
// supplies that end. Anything after it is the argument tail; a longer path
// containing spaces must be quoted instead. Never use file existence to
// shorten an explicit path.
qsizetype explicitPathEnd(QStringView line, qsizetype firstEnd) {
	if (hasFileExtension(line.first(firstEnd))) return firstEnd;

	auto lastEnd = firstEnd;
	for (auto start = skipCommandSpace(line, firstEnd); start < line.size();
	     start = skipCommandSpace(line, lastEnd)) {
		const auto end = endOfWord(line, start);
		const auto word = line.sliced(start, end - start);
		if (isSeparateArgument(word) || word.contains(u'"')) return -1;
		if (hasFileExtension(word)) return end;
		lastEnd = end;
	}
	return lastEnd;
}

bool hasUnquotedPathSeparator(QStringView tail) {
	auto quoted = false;
	for (auto i = qsizetype(0); i < tail.size();) {
		const auto c = tail[i];
		if (c == u'\\') {
			auto slashes = qsizetype(0);
			while (i + slashes < tail.size() && tail[i + slashes] == u'\\') ++slashes;
			const auto escapedQuote = i + slashes < tail.size() && tail[i + slashes] == u'"'
			                       && slashes % 2 != 0;
			if (!quoted && !escapedQuote) return true;
			i += slashes + (escapedQuote ? 1 : 0);
			continue;
		}
		if (c == u'"') quoted = !quoted;
		if (!quoted && c == u'/') return true;
		++i;
	}
	return false;
}

QString normalizedExistingFile(const QString& path) {
	const auto absolute = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
	// Check the .lnk itself, even if its target is a folder or namespace item.
	const auto attributes = GetFileAttributesW(absolute.toStdWString().c_str());
	if ((attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
	    || isAppExecutionAlias(absolute))
	{
		return absolute;
	}
	return {};
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
	if (hasFileExtension(candidate)) return QString();

	// Allow omitted PATHEXT only on a target without a named extension.
	for (const auto& extension: executableExtensions()) {
		if (auto existing = normalizedExistingFile(candidate + extension); !existing.isEmpty()) {
			return existing;
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
    bool& mayHaveStarted
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
	operation.setString(QStringLiteral("open"));
	Variant show;
	show.setInt(SW_SHOWNORMAL);

	hr = shell->ShellExecute(file.get(), args.value, directory.value, operation.value, show.value);
	// A disconnect during the call is not proof that Explorer did not launch
	// the target. Failures during discovery, before ShellExecute, are retryable.
	mayHaveStarted = hr == RPC_E_SERVER_DIED || hr == RPC_E_DISCONNECTED
	              || hr == RPC_E_CALL_CANCELED || hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED);
	return hr;
}

LaunchResult shellExecuteExTarget(
    const QString& target,
    const QString& arguments,
    const QString& workingDirectory,
    const QString& verb,
    HWND owner,
    bool invokeIdList
) {
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

	auto error = DWORD(ERROR_SUCCESS);
	if (!ShellExecuteExW(&info)) {
		error = GetLastError();
		if (error == ERROR_SUCCESS) error = ERROR_GEN_FAILURE;
	}
	auto result = detail::launchResultForError(error);
	result.usedShell = true;
	return result;
}

LaunchResult cleanShellExecute(
    const QString& target,
    const QString& arguments,
    const QString& workingDirectory,
    bool invokeIdList,
    std::wstring& environment
) {
	// ShellExecuteExW cannot take an environment block. Re-enter the same
	// executable before CLI/GUI initialization, with no inherited handles.
	const auto executable = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
	if (executable.isEmpty()) return detail::launchResultForError(ERROR_FILE_NOT_FOUND);
	const auto application = executable.toStdWString();
	const auto helperDirectory = QDir::toNativeSeparators(QFileInfo(executable).absolutePath())
	                                 .toStdWString();
	auto commandLine = joinCommandLine(
	    executable,
	    {QString::fromLatin1(SHELL_OPEN_HELPER_ARG),
	     invokeIdList ? QStringLiteral("1") : QStringLiteral("0"),
	     target,
	     arguments,
	     workingDirectory}
	).toStdWString();

	STARTUPINFOW startup {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process {};
	if (!CreateProcessW(
	        application.c_str(),
	        commandLine.data(),
	        nullptr,
	        nullptr,
	        FALSE,
	        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
	        environment.data(),
	        helperDirectory.c_str(),
	        &startup,
	        &process
	    ))
	{
		return detail::launchResultForError(GetLastError());
	}

	CloseHandle(process.hThread);
	DWORD error = ERROR_GEN_FAILURE;
	if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0) {
		if (!GetExitCodeProcess(process.hProcess, &error)) error = GetLastError();
	} else {
		error = GetLastError();
	}
	CloseHandle(process.hProcess);
	auto result = detail::launchResultForError(error);
	result.usedShell = true;
	return result;
}

LaunchResult createProcess(
    const LaunchCommand& command,
    const QString& workingDirectory,
    std::wstring& environment
) {
	auto result = LaunchResult();
	result.usedProcess = true;

	if (command.executable.isEmpty()) {
		result.error = ERROR_FILE_NOT_FOUND;
		return result;
	}

	auto application = command.executable.toStdWString();
	auto commandLineText = quoteCommandArgument(command.executable);
	if (!command.argumentTail.isEmpty()) commandLineText += u' ' + command.argumentTail;
	auto commandLine = commandLineText.toStdWString();
	auto directory = workingDirectory.toStdWString();

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

LaunchResult explorerOpen(const QString& target, const QString& args, const QString& directory) {
	auto mayHaveStarted = false;
	const auto hr = explorerShellExecute(target, args, directory, mayHaveStarted);
	if (mayHaveStarted) {
		return {.status = LaunchStatus::Indeterminate, .error = ERROR_GEN_FAILURE};
	}
	if (FAILED(hr)) {
		return detail::launchResultForError(
		    hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) ? ERROR_CANCELLED : ERROR_GEN_FAILURE
		);
	}
	return {.status = LaunchStatus::Started, .error = ERROR_SUCCESS, .usedExplorer = true};
}

} // namespace

std::optional<DWORD> runShellOpenHelper(int argc, char** argv) {
	if (argc < 2 || qstrcmp(argv[1], SHELL_OPEN_HELPER_ARG) != 0) return std::nullopt;

	int count = 0;
	auto* args = CommandLineToArgvW(GetCommandLineW(), &count);
	if (args == nullptr) return ERROR_INVALID_PARAMETER;
	if (count != 6 || (std::wcscmp(args[2], L"0") != 0 && std::wcscmp(args[2], L"1") != 0)
	    || args[3][0] == L'\0')
	{
		LocalFree(args);
		return ERROR_INVALID_PARAMETER;
	}

	const auto invokeIdList = args[2][0] == L'1';
	const auto target = QString::fromWCharArray(args[3]);
	const auto parameters = QString::fromWCharArray(args[4]);
	const auto directory = QString::fromWCharArray(args[5]);
	LocalFree(args);

	auto com = ComApartment(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	if (FAILED(com.hr)) return ERROR_GEN_FAILURE;
	return shellExecuteExTarget(
	           target, parameters, directory, QStringLiteral("open"), nullptr, invokeIdList
	).error;
}

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

	if (looksLikePath(program)) return resolvePathCandidate(program);

	// A file beside the shell wins over PATH, matching the keybind behavior that
	// shipped before the unified launcher.
	if (!QCoreApplication::applicationDirPath().isEmpty()) {
		const auto candidate = QDir(QCoreApplication::applicationDirPath()).filePath(program);
		if (auto existing = resolvePathCandidate(candidate); !existing.isEmpty()) return existing;
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
	const auto name = QFileInfo(path).fileName();
	if (name.contains(u'*') || name.contains(u'?')) return false;
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
	auto parsed = LaunchCommand();
	const auto line = QStringView(command).sliced(skipCommandSpace(command, 0));
	if (line.isEmpty()) return parsed;

	parsed.error = ERROR_INVALID_PARAMETER;
	if (line.contains(QChar::Null)) return parsed;
	auto end = qsizetype(0);
	if (line.startsWith(u'"')) {
		const auto close = line.indexOf(u'"', 1);
		if (close < 0 || (close + 1 < line.size() && !isCommandSpace(line[close + 1]))) {
			return parsed;
		}
		parsed.program = line.sliced(1, close - 1).toString();
		parsed.quotedProgram = true;
		end = close + 1;
	} else {
		end = endOfWord(line, 0);
		const auto first = line.first(end);
		if (first.contains(u'"')) return parsed;
		if (!isShellName(first)) {
			if (looksLikePath(first)) {
				end = explicitPathEnd(line, end);
				if (end < 0) return parsed;
			} else if (!hasExecutableExtension(first) && hasAmbiguousRelativePath(line, end)) {
				return parsed;
			}
		}
		parsed.program = line.first(end).toString();
	}
	if (parsed.program.isEmpty()) return parsed;

	// Do not parse or rebuild the argument tail. Windows applications may use
	// their own parser; even empty arguments and escaped quotes must survive.
	parsed.argumentTail = line.sliced(skipCommandSpace(line, end)).toString();
	parsed.error = ERROR_SUCCESS;
	if (isShellName(parsed.program)) return parsed;

	parsed.executable = resolveExecutable(parsed.program);
	if (parsed.executable.isEmpty() && looksLikePath(parsed.program)) {
		parsed.error = ERROR_FILE_NOT_FOUND;
		return parsed;
	}
	parsed.kind = classifyProgram(parsed.executable);
	return parsed;
}

std::wstring cleanEnvironmentBlock() {
	auto environment = Common::INITIAL_ENVIRONMENT;

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
	if (block.empty()) block.push_back(L'\0');
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
		return detail::launchResultForError(ERROR_FILE_NOT_FOUND);
	}
	if (target.contains(QChar::Null) || arguments.contains(QChar::Null)
	    || workingDirectory.contains(QChar::Null))
	{
		return detail::launchResultForError(ERROR_INVALID_PARAMETER);
	}

	const auto directory = defaultWorkingDirectory(workingDirectory);
	if (verb == QStringLiteral("open")) {
		return detail::openWithFallback(
		    [&] { return useExplorer ? explorerOpen(target, arguments, directory) : LaunchResult(); },
		    [&](std::wstring environment) {
			    return cleanShellExecute(target, arguments, directory, invokeIdList, environment);
		    }
		);
	}
	if (verb == QStringLiteral("runas")) {
		return shellExecuteExTarget(target, arguments, directory, verb, owner, invokeIdList);
	}
	return detail::launchResultForError(ERROR_NOT_SUPPORTED);
}

LaunchResult launchCommand(const QString& command, const QString& workingDirectory, HWND owner) {
	const auto parsed = parseLaunchCommand(command);
	if (parsed.error != ERROR_SUCCESS) return detail::launchResultForError(parsed.error);

	const auto directory = defaultWorkingDirectory(workingDirectory);
	const auto target = targetFor(parsed);
	return detail::openWithFallback(
	    [&] { return explorerOpen(target, parsed.argumentTail, directory); },
	    [&](std::wstring environment) {
		    if (parsed.useProcess()) {
			    const auto result = createProcess(parsed, directory, environment);
			    if (result.error == ERROR_ELEVATION_REQUIRED) {
				    return shellExecuteTarget(
				        target, parsed.argumentTail, directory, QStringLiteral("runas"), owner
				    );
			    }
			    if (result.status != LaunchStatus::Failed) return result;
		    }
		    return cleanShellExecute(target, parsed.argumentTail, directory, false, environment);
	    }
	);
}

LaunchCommand parseElevatedLaunchCommand(const QString& command) {
	auto parsed = parseLaunchCommand(command);
	if (parsed.error != ERROR_SUCCESS || parsed.quotedProgram) return parsed;

	// For elevation, an unquoted executable path containing spaces is not a
	// safe guess. Path separators in unquoted tail text can also mean that the
	// first executable-looking token was only a prefix of the intended path.
	if (parsed.program.contains(u' ') || parsed.program.contains(u'\t')
	    || hasUnquotedPathSeparator(parsed.argumentTail))
	{
		parsed.executable.clear();
		parsed.kind = ProgramKind::Unresolved;
		parsed.error = ERROR_INVALID_PARAMETER;
	}
	return parsed;
}

LaunchResult launchElevatedCommand(
    const QString& command,
    const QString& workingDirectory,
    HWND owner
) {
	const auto parsed = parseElevatedLaunchCommand(command);
	if (parsed.error != ERROR_SUCCESS) return detail::launchResultForError(parsed.error);

	return shellExecuteTarget(
	    targetFor(parsed),
	    parsed.argumentTail,
	    defaultWorkingDirectory(workingDirectory),
	    QStringLiteral("runas"),
	    owner
	);
}

} // namespace qs::windows::process

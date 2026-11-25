#include "equalizer_apo.hpp"
#include <algorithm>
#include <cstring>
#include <vector>

#include <qt_windows.h>

#include <shellapi.h>

#include <qbytearray.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qquickitem.h>
#include <qquickwindow.h>
#include <qstringlist.h>
#include <qtimer.h>
#include <quuid.h>
#include <qwindow.h>

namespace qs::windows::sys {

namespace {

const auto kApoClsidGfx = QStringLiteral("{EC1CC9CE-FAED-4822-828A-82A81A6F018F}");
const auto kApoClsidPostMix = QStringLiteral("{EACD2258-FCAC-4FF4-B36D-419E924A6D79}");

QString readRegString(HKEY root, const wchar_t* subkey, const wchar_t* name) {
	wchar_t buffer[1024];
	DWORD size = sizeof(buffer);
	auto status = RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, nullptr, buffer, &size);
	if (status != ERROR_SUCCESS) return QString();
	return equalizer_apo_detail::decodeRegistryString(
	    reinterpret_cast<const BYTE*>(buffer),
	    std::min<DWORD>(size, sizeof(buffer))
	);
}

bool writeFileAtomically(const QString& path, const QByteArray& data) {
	auto dir = QFileInfo(path).absoluteDir();
	if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) return false;

	auto tempPath = path + u".tmp-" + QUuid::createUuid().toString(QUuid::Id128);
	QFile tempFile(tempPath);
	if (!tempFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
	if (tempFile.write(data) != data.size()) {
		tempFile.close();
		QFile::remove(tempPath);
		return false;
	}
	tempFile.close();

	QFile::remove(path);
	if (!QFile::rename(tempPath, path)) {
		QFile::remove(tempPath);
		return false;
	}
	return true;
}

QString psSingleQuote(const QString& text) {
	QString escaped = text;
	escaped.replace(u'\'', QStringLiteral("''"));
	return escaped;
}

bool unquoteIncludeTarget(const QString& rawTarget, QString& target) {
	target = rawTarget.trimmed();
	if (target.isEmpty()) return false;
	const auto quote = target.front();
	if (quote != u'"' && quote != u'\'') return true;
	if (target.size() < 2 || target.back() != quote) return false;
	target = target.mid(1, target.size() - 2);
	return !target.isEmpty();
}

QString cleanPathAgainstBase(const QString& path, const QString& baseDir) {
	const auto separatorsNormalized = QDir::fromNativeSeparators(path);
	const QFileInfo info(separatorsNormalized);
	const auto absolute = info.isAbsolute() ? separatorsNormalized
	                                       : QDir(baseDir).filePath(separatorsNormalized);
	return QDir::cleanPath(absolute);
}

HWND parentHwnd(QObject* parentWindow) {
	auto* window = qobject_cast<QWindow*>(parentWindow);
	if (window == nullptr && parentWindow != nullptr) {
		auto* content = parentWindow->property("contentItem").value<QQuickItem*>();
		if (content != nullptr) window = content->window();
	}
	if (window == nullptr || window->handle() == nullptr) return nullptr;
	return reinterpret_cast<HWND>(window->winId()); // NOLINT(performance-no-int-to-ptr)
}

class ProcessHandle: public QObject {
public:
	explicit ProcessHandle(HANDLE handle, QObject* parent): QObject(parent), handle(handle) {}
	~ProcessHandle() override {
		if (this->handle != nullptr) CloseHandle(this->handle);
	}

	HANDLE handle;
};

} // namespace

namespace equalizer_apo_detail {

QString decodeRegistryString(const BYTE* data, DWORD size) {
	if (data == nullptr || size < sizeof(wchar_t)) return {};
	auto chars = size / sizeof(wchar_t);
	std::wstring wide(chars, L'\0');
	std::memcpy(wide.data(), data, chars * sizeof(wchar_t));
	while (!wide.empty() && wide.back() == L'\0') wide.pop_back();
	return QString::fromStdWString(wide);
}

bool scanRegistryStringValues(
    const RegistryEnumValue& enumerate,
    const std::function<bool(const QString& name, const QString& value)>& predicate,
    bool* scanComplete
) {
	constexpr DWORD initialDataBytes = 4096;
	constexpr DWORD maxDataBytes = 1024 * 1024;
	constexpr size_t maxNameChars = 16384;
	if (scanComplete != nullptr) *scanComplete = false;

	std::vector<BYTE> valueData(initialDataBytes);
	std::vector<wchar_t> valueName(512);
	for (DWORD index = 0;;) {
		// First read only the name and metadata. A huge or unreadable data item
		// must not corrupt, truncate, or end enumeration of the value name.
		DWORD nameLen = 0;
		DWORD requiredDataLen = 0;
		DWORD type = 0;
		LSTATUS status = ERROR_SUCCESS;
		for (;;) {
			nameLen = static_cast<DWORD>(valueName.size());
			requiredDataLen = 0;
			type = 0;
			status = enumerate(index, valueName.data(), nameLen, type, nullptr, requiredDataLen);
			if (status == ERROR_MORE_DATA && nameLen >= valueName.size()
			    && valueName.size() < maxNameChars)
			{
				valueName.resize(std::min(valueName.size() * 2, maxNameChars));
				continue;
			}
			break;
		}

		if (status == ERROR_NO_MORE_ITEMS) {
			if (scanComplete != nullptr) *scanComplete = true;
			return false;
		}
		if (status != ERROR_SUCCESS) return false;

		auto name = QString::fromWCharArray(valueName.data(), static_cast<qsizetype>(nameLen));
		QString value;
		if ((type == REG_SZ || type == REG_EXPAND_SZ) && requiredDataLen <= maxDataBytes
		    && requiredDataLen > 0)
		{
			if (requiredDataLen > valueData.size()) valueData.resize(requiredDataLen);
			for (;;) {
				DWORD readNameLen = static_cast<DWORD>(valueName.size());
				DWORD dataLen = static_cast<DWORD>(valueData.size());
				DWORD readType = 0;
				status = enumerate(
				    index,
				    valueName.data(),
				    readNameLen,
				    readType,
				    valueData.data(),
				    dataLen
				);
				if (status == ERROR_MORE_DATA) {
					// A concurrent writer can grow the value between the metadata
					// and data reads. Resize within the cap; above the cap skip only
					// this value's data and keep scanning later names.
					if (dataLen > valueData.size() && dataLen <= maxDataBytes) {
						valueData.resize(dataLen);
						continue;
					}
					break;
				}
				if (status != ERROR_SUCCESS) return false;
				if (readType == REG_SZ || readType == REG_EXPAND_SZ)
					value = decodeRegistryString(valueData.data(), dataLen);
				break;
			}
		}

		// The name is inspected for every registry type, including values whose
		// data is oversized, binary, or absent.
		if (predicate(name, value)) {
			if (scanComplete != nullptr) *scanComplete = true;
			return true;
		}
		++index;
	}
}

bool registryStringValueMatches(
    HKEY key,
    const std::function<bool(const QString& name, const QString& value)>& predicate,
    bool* scanComplete
) {
	return scanRegistryStringValues(
	    [key](DWORD index, wchar_t* name, DWORD& nameLen, DWORD& type, BYTE* data, DWORD& dataLen) {
		    return RegEnumValueW(key, index, name, &nameLen, nullptr, &type, data, &dataLen);
	    },
	    predicate,
	    scanComplete
	);
}

bool registryTextContainsClsid(const QString& text, const QString& clsid) {
	return !clsid.isEmpty() && text.contains(clsid, Qt::CaseInsensitive);
}

bool hasActiveIncludeDirective(
    const QString& text,
    const QString& configPath,
    const QString& includePath
) {
	const auto configDir = QFileInfo(configPath).absolutePath();
	const auto includeFullPath = cleanPathAgainstBase(includePath, configDir);
	for (const auto& rawLine: text.split(u'\n')) {
		auto line = rawLine.trimmed();
		if (line.startsWith(u'#')) continue;
		if (!line.startsWith(QStringLiteral("Include"), Qt::CaseInsensitive)) continue;
		auto directive = line.mid(7).trimmed();
		if (!directive.startsWith(u':')) continue;
		QString target;
		if (!unquoteIncludeTarget(directive.mid(1), target)) continue;
		const auto targetFullPath = cleanPathAgainstBase(target, configDir);
		if (targetFullPath.compare(includeFullPath, Qt::CaseInsensitive) == 0) return true;
	}
	return false;
}

QString elevatedHookScript(const QString& configPath, const QString& includePath) {
	return QStringLiteral(
	           "$configPath = '%1'\n"
	           "$includePath = '%2'\n"
	           "$line = 'Include: %2'\n"
	           "try {\n"
	           "    $utf8 = New-Object Text.UTF8Encoding $false\n"
	           "    if (!(Test-Path -LiteralPath $includePath -PathType Leaf)) {\n"
	           "        [IO.File]::WriteAllText($includePath, \"Preamp: 0 dB`n\", $utf8)\n"
	           "    }\n"
	           "    if (!(Test-Path -LiteralPath $includePath -PathType Leaf)) { exit 3 }\n"
	           "    $configLines = [IO.File]::ReadAllLines($configPath, [System.Text.Encoding]::UTF8)\n"
	           "    $configDir = Split-Path $configPath -Parent\n"
	           "    if ([System.IO.Path]::IsPathRooted($includePath)) {\n"
	           "        $includeFullPath = [System.IO.Path]::GetFullPath($includePath)\n"
	           "    } else {\n"
	           "        $includeFullPath = [System.IO.Path]::GetFullPath((Join-Path $configDir $includePath))\n"
	           "    }\n"
	           "    $active = $false\n"
	           "    foreach ($rawLine in $configLines) {\n"
	           "        $entry = $rawLine.Trim()\n"
	           "        if ($entry.StartsWith('#')) { continue }\n"
	           "        if ($entry.StartsWith('Include', [System.StringComparison]::OrdinalIgnoreCase)) {\n"
	           "            $directive = $entry.Substring(7).TrimStart()\n"
	           "            if ($directive.StartsWith(':')) {\n"
	           "                $target = $directive.Substring(1).Trim()\n"
	           "                $valid = $target.Length -gt 0\n"
	           "                if ($valid) {\n"
	           "                    $quote = $target[0]\n"
	           "                    if (($quote -eq [char]34) -or ($quote -eq [char]39)) {\n"
	           "                        if (($target.Length -lt 2) -or ($target[$target.Length - 1] -ne $quote)) {\n"
	           "                            $valid = $false\n"
	           "                        } else {\n"
	           "                            $target = $target.Substring(1, $target.Length - 2)\n"
	           "                            $valid = $target.Length -gt 0\n"
	           "                        }\n"
	           "                    }\n"
	           "                }\n"
	           "                if ($valid) {\n"
	           "                    try {\n"
	           "                        if ([System.IO.Path]::IsPathRooted($target)) {\n"
	           "                            $targetFullPath = [System.IO.Path]::GetFullPath($target)\n"
	           "                        } else {\n"
	           "                            $targetFullPath = [System.IO.Path]::GetFullPath((Join-Path $configDir $target))\n"
	           "                        }\n"
	           "                        if ($targetFullPath -ieq $includeFullPath) { $active = $true }\n"
	           "                    } catch {\n"
	           "                    }\n"
	           "                }\n"
	           "            }\n"
	           "        }\n"
	           "    }\n"
	           "    if (!$active) {\n"
	           "        [IO.File]::AppendAllText($configPath, \"`n$line`n\", $utf8)\n"
	           "    }\n"
	           "    exit 0\n"
	           "} catch {\n"
	           "    exit 2\n"
	           "}\n"
	       )
	    .arg(psSingleQuote(configPath), psSingleQuote(includePath));
}

ElevationCompletion elevationCompletion(bool hasProcess, DWORD waitResult, DWORD exitCode) {
	if (!hasProcess) return {false, QStringLiteral("completion-unverified")};
	auto success = waitResult == WAIT_OBJECT_0 && exitCode == 0;
	return {success, success ? QString() : QStringLiteral("exit-%1").arg(exitCode)};
}

} // namespace equalizer_apo_detail

EqualizerApo::EqualizerApo(QObject* parent): QObject(parent) {
	this->detectInstall();
	this->detectWritable();
	this->detectHooked();
}

QString EqualizerApo::configFilePath() const {
	if (this->mConfigPath.isEmpty()) return QString();
	return QDir(this->mConfigPath).filePath(QStringLiteral("config.txt"));
}

QString EqualizerApo::includeFilePath() const {
	if (this->mConfigPath.isEmpty()) return QString();
	return QDir(this->mConfigPath).filePath(QStringLiteral("ii-equalizer.txt"));
}

void EqualizerApo::detectInstall() {
	auto installPath =
	    readRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\EqualizerAPO", L"InstallPath");
	this->mInstalled = !installPath.isEmpty() && QDir(installPath).exists();
	this->mInstallPath = installPath;

	if (!this->mInstalled) {
		this->mConfigPath = QString();
		return;
	}

	auto configPath =
	    readRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\EqualizerAPO", L"ConfigPath");
	this->mConfigPath = configPath.isEmpty() ? QDir(installPath).filePath(QStringLiteral("config"))
	                                          : configPath;
}

void EqualizerApo::detectWritable() {
	auto path = this->configFilePath();
	if (path.isEmpty() || !QFileInfo::exists(path)) {
		this->mConfigWritable = false;
		return;
	}

	QFile file(path);
	auto ok = file.open(QIODevice::Append);
	if (ok) file.close();
	this->mConfigWritable = ok;
}

void EqualizerApo::detectHooked() {
	auto path = this->configFilePath();
	if (path.isEmpty() || !QFileInfo::exists(path)) {
		this->mHooked = false;
		return;
	}

	QFile file(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		this->mHooked = false;
		return;
	}
	auto text = QString::fromUtf8(file.readAll());
	file.close();

	this->mHooked = equalizer_apo_detail::hasActiveIncludeDirective(
	    text,
	    this->configFilePath(),
	    this->includeFilePath()
	);
}

void EqualizerApo::detectActiveOnDevice() {
	if (this->mDefaultDeviceId.isEmpty()) {
		this->mActiveOnDevice = false;
		return;
	}

	auto subkey = QStringLiteral(
	                  "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\"
	              )
	              + this->mDefaultDeviceId + QStringLiteral("\\FxProperties");
	auto wsubkey = subkey.toStdWString();

	HKEY key = nullptr;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, wsubkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
		this->mActiveOnDevice = false;
		return;
	}

	auto found = equalizer_apo_detail::registryStringValueMatches(
	    key,
	    [](const QString& name, const QString& value) {
		    using equalizer_apo_detail::registryTextContainsClsid;
		    return registryTextContainsClsid(value, kApoClsidGfx)
		        || registryTextContainsClsid(value, kApoClsidPostMix)
		        || registryTextContainsClsid(name, kApoClsidGfx)
		        || registryTextContainsClsid(name, kApoClsidPostMix);
	    }
	);

	RegCloseKey(key);
	this->mActiveOnDevice = found;
}

void EqualizerApo::refresh() {
	this->detectInstall();
	this->detectWritable();
	this->detectHooked();
	this->detectActiveOnDevice();
	emit this->changed();
}

void EqualizerApo::setDefaultDeviceId(const QString& id) {
	if (this->mDefaultDeviceId == id) return;
	this->mDefaultDeviceId = id;
	this->detectActiveOnDevice();
	emit this->changed();
}

bool EqualizerApo::writeCurve(const QString& content) const {
	auto path = this->includeFilePath();
	if (path.isEmpty()) return false;
	return writeFileAtomically(path, content.toUtf8());
}

bool EqualizerApo::hookDirectly() {
	if (!this->mConfigWritable) return false;
	if (this->mHooked) return true;

	if (!QFileInfo(this->includeFilePath()).isFile()
	    && !this->writeCurve(QStringLiteral("Preamp: 0 dB\n")))
		return false;
	if (!QFileInfo(this->includeFilePath()).isFile()) return false;

	QFile file(this->configFilePath());
	if (!file.open(QIODevice::Append | QIODevice::Text)) return false;
	auto line = QStringLiteral("\nInclude: %1\n").arg(this->includeFilePath());
	auto written = file.write(line.toUtf8());
	file.close();
	if (written <= 0) return false;

	this->detectHooked();
	emit this->changed();
	return this->mHooked;
}

void EqualizerApo::hookElevated(QObject* parentWindow) {
	if (this->mConfigPath.isEmpty()) return;

	// Try the unelevated write first, but do not treat its failure as fatal: the
	// elevated script performs and verifies the same creation in the protected directory.
	if (!QFileInfo(this->includeFilePath()).isFile())
		(void) this->writeCurve(QStringLiteral("Preamp: 0 dB\n"));

	auto script = equalizer_apo_detail::elevatedHookScript(
	    this->configFilePath(),
	    this->includeFilePath()
	);

	auto encoded = QByteArray(
	                   reinterpret_cast<const char*>(script.utf16()),
	                   static_cast<qsizetype>(script.size() * sizeof(char16_t))
	)
	                   .toBase64();

	wchar_t systemDirectory[MAX_PATH] {};
	auto systemLength = GetSystemDirectoryW(systemDirectory, MAX_PATH);
	if (systemLength == 0 || systemLength >= MAX_PATH) {
		emit this->elevationRefused(QStringLiteral("launch-failed"));
		return;
	}
	auto powerShell = QString::fromWCharArray(systemDirectory, static_cast<qsizetype>(systemLength))
	                  + QStringLiteral("\\WindowsPowerShell\\v1.0\\powershell.exe");
	auto arguments = QStringLiteral("-NoProfile -EncodedCommand %1").arg(QString::fromLatin1(encoded));

	auto file = powerShell.toStdWString();
	auto parameters = arguments.toStdWString();
	SHELLEXECUTEINFOW info {};
	info.cbSize = sizeof(info);
	info.fMask = SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS;
	info.hwnd = parentHwnd(parentWindow);
	info.lpVerb = L"runas";
	info.lpFile = file.c_str();
	info.lpParameters = parameters.c_str();
	info.nShow = SW_HIDE;
	if (!ShellExecuteExW(&info)) {
		auto error = GetLastError();
		emit this->elevationRefused(
		    error == ERROR_CANCELLED ? QStringLiteral("cancelled") : QStringLiteral("launch-failed")
		);
		return;
	}
	if (info.hProcess == nullptr) {
		this->refresh();
		auto completion = equalizer_apo_detail::elevationCompletion(false, 0, 1);
		emit this->elevationFinished(completion.success, completion.reason);
		return;
	}

	auto* waitTimer = new QTimer(this);
	new ProcessHandle(info.hProcess, waitTimer);
	waitTimer->setInterval(200);
	QObject::connect(waitTimer, &QTimer::timeout, this, [this, waitTimer, process = info.hProcess] {
		auto wait = WaitForSingleObject(process, 0);
		if (wait == WAIT_TIMEOUT) return;

		DWORD exitCode = 1;
		if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process, &exitCode);
		waitTimer->stop();
		waitTimer->deleteLater();
		this->refresh();
		auto completion = equalizer_apo_detail::elevationCompletion(true, wait, exitCode);
		emit this->elevationFinished(completion.success, completion.reason);
	});
	waitTimer->start();
}

} // namespace qs::windows::sys

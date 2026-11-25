#pragma once

#include <functional>

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qt_windows.h>
#include <qtmetamacros.h>

namespace qs::windows::sys {

namespace equalizer_apo_detail {

// Registry REG_SZ/REG_EXPAND_SZ storage is byte-counted and need not end in a
// NUL. The decoder uses exactly the stored byte count and strips terminators.
[[nodiscard]] QString decodeRegistryString(const BYTE* data, DWORD size);
using RegistryEnumValue = std::function<LSTATUS(
    DWORD index,
    wchar_t* name,
    DWORD& nameLen,
    DWORD& type,
    BYTE* data,
    DWORD& dataLen
)>;

// Scans string values without ending enumeration on ERROR_MORE_DATA. Returns
// true when the predicate matched; scanComplete is false when enumeration ended
// because of an error rather than ERROR_NO_MORE_ITEMS. The enumeration callback
// seam keeps the resize-and-continue policy portable-testable without registry writes.
[[nodiscard]] bool scanRegistryStringValues(
    const RegistryEnumValue& enumerate,
    const std::function<bool(const QString& name, const QString& value)>& predicate,
    bool* scanComplete = nullptr
);
[[nodiscard]] bool registryStringValueMatches(
    HKEY key,
    const std::function<bool(const QString& name, const QString& value)>& predicate,
    bool* scanComplete = nullptr
);
[[nodiscard]] bool registryTextContainsClsid(const QString& text, const QString& clsid);
[[nodiscard]] QString elevatedHookScript(const QString& configPath, const QString& includePath);
struct ElevationCompletion {
	bool success = false;
	QString reason;
};
[[nodiscard]] ElevationCompletion
elevationCompletion(bool hasProcess, DWORD waitResult, DWORD exitCode);

} // namespace equalizer_apo_detail

class EqualizerApo: public QObject {
	Q_OBJECT;
	// clang-format off
	Q_PROPERTY(bool installed READ installed NOTIFY changed);
	Q_PROPERTY(QString installPath READ installPath NOTIFY changed);
	Q_PROPERTY(QString configPath READ configPath NOTIFY changed);
	Q_PROPERTY(QString configFilePath READ configFilePath NOTIFY changed);
	Q_PROPERTY(QString includeFilePath READ includeFilePath NOTIFY changed);
	Q_PROPERTY(bool configWritable READ configWritable NOTIFY changed);
	Q_PROPERTY(bool hooked READ hooked NOTIFY changed);
	Q_PROPERTY(bool activeOnDefaultDevice READ activeOnDefaultDevice NOTIFY changed);
	Q_PROPERTY(QString defaultDeviceId READ defaultDeviceId NOTIFY changed);
	Q_PROPERTY(bool ready READ ready NOTIFY changed);
	// clang-format on
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit EqualizerApo(QObject* parent = nullptr);

	[[nodiscard]] bool installed() const { return this->mInstalled; }
	[[nodiscard]] QString installPath() const { return this->mInstallPath; }
	[[nodiscard]] QString configPath() const { return this->mConfigPath; }
	[[nodiscard]] QString configFilePath() const;
	[[nodiscard]] QString includeFilePath() const;
	[[nodiscard]] bool configWritable() const { return this->mConfigWritable; }
	[[nodiscard]] bool hooked() const { return this->mHooked; }
	[[nodiscard]] bool activeOnDefaultDevice() const { return this->mActiveOnDevice; }
	[[nodiscard]] QString defaultDeviceId() const { return this->mDefaultDeviceId; }
	[[nodiscard]] bool ready() const { return this->mInstalled && this->mActiveOnDevice; }

	Q_INVOKABLE void refresh();

	Q_INVOKABLE void setDefaultDeviceId(const QString& id);

	Q_INVOKABLE bool writeCurve(const QString& content) const;

	Q_INVOKABLE bool hookDirectly();

	Q_INVOKABLE void hookElevated(QObject* parentWindow = nullptr);

signals:
	void changed();
	// Refused before the elevated process starts. reason is "cancelled" for a
	// declined UAC prompt and "launch-failed" for other ShellExecuteEx failures.
	void elevationRefused(const QString& reason);
	// The elevated hook process exited. success means exit code zero.
	void elevationFinished(bool success, const QString& reason);

private:
	void detectInstall();
	void detectWritable();
	void detectHooked();
	void detectActiveOnDevice();

	bool mInstalled = false;
	QString mInstallPath;
	QString mConfigPath;
	bool mConfigWritable = false;
	bool mHooked = false;
	bool mActiveOnDevice = false;
	QString mDefaultDeviceId;
};

} // namespace qs::windows::sys

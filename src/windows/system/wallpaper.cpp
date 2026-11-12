#include "wallpaper.hpp"
#include <atomic>
#include <string>
#include <thread>
#include <utility>

#include <objbase.h>
#include <qcolor.h>
#include <qcoreapplication.h>
#include <qdir.h>
#include <qfileinfo.h>
#include <qguiapplication.h>
#include <qhash.h>
#include <qloggingcategory.h>
#include <qmetaobject.h>
#include <qpointer.h>
#include <qstring.h>
#include <qt_windows.h>
#include <qvariant.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.UserProfile.h>
#include <winstring.h>
#include <wrl/client.h>

#include "../redact.hpp"
#include "../services/message_window.hpp"
#include "../util.hpp"
#include "../virtual_desktops.hpp"
#include "wallpaper_target.hpp"
#include "wallpaper_worker.hpp"

using Microsoft::WRL::ComPtr;

namespace qs::windows::sys {

namespace {
Q_LOGGING_CATEGORY(logWallpaper, "quickshell.windows.wallpaper", QtWarningMsg);

std::atomic<bool> gShuttingDown = false;

QString wallpaperDevicePath(const QString& screenName) {
	if (screenName.isEmpty() || !qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
		return {};

	auto* registry = ScreenIdentityRegistry::instance();
	auto screens = QGuiApplication::screens();
	auto* screen = registry->screenByName(screenName);
	if (!screen || !screens.contains(screen)) return {};

	QList<DisplayIdentity> live;
	live.reserve(screens.size());
	for (auto* current: screens) live.append(registry->identity(current));
	return wallpaper_target::devicePath(live, screenName);
}

std::wstring
wallpaperMonitorPath(IDesktopWallpaper* wallpaper, const QString& target, HRESULT& error) {
	UINT count = 0;
	error = wallpaper->GetMonitorDevicePathCount(&count);
	if (FAILED(error)) return {};

	QStringList paths;
	for (UINT index = 0; index < count; ++index) {
		LPWSTR path = nullptr;
		auto hr = wallpaper->GetMonitorDevicePathAt(index, &path);
		if (FAILED(hr) || !path) {
			error = hr;
			CoTaskMemFree(path);
			return {};
		}
		paths.append(QString::fromWCharArray(path));
		CoTaskMemFree(path);
	}
	return wallpaper_target::monitorPath(paths, target).toStdWString();
}

const wchar_t* const kPersonalizeKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
const wchar_t* const kDwmKey = L"Software\\Microsoft\\Windows\\DWM";
const wchar_t* const kContentDeliveryKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager";
const wchar_t* const kLockScreenKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Lock Screen";
const wchar_t* const kVirtualDesktopsKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops\\Desktops\\";

const CLSID CLSID_IMMERSIVE_SHELL =
    {0xc2f03a33, 0x21f5, 0x47fa, {0xb4, 0xbb, 0x15, 0x63, 0x62, 0xa2, 0xf2, 0x39}};
const GUID SID_VIRTUAL_DESKTOP_MANAGER_INTERNAL =
    {0xc5e0cdca, 0x7b6e, 0x41b2, {0x9f, 0xc4, 0xd9, 0x39, 0x75, 0xcc, 0x46, 0x7b}};

MIDL_INTERFACE("3F07F4BE-B107-441A-AF0F-39D82529072C")
ShellVirtualDesktop: public IUnknown {};

MIDL_INTERFACE("53F5CA0B-158F-4124-900C-057158060B27")
ShellVirtualDesktopManager: public IUnknown {
public:
	virtual HRESULT STDMETHODCALLTYPE GetCount(UINT * count) = 0;
	virtual HRESULT STDMETHODCALLTYPE
	    MoveViewToDesktop(IUnknown * view, ShellVirtualDesktop * desktop) = 0;
	virtual HRESULT STDMETHODCALLTYPE CanViewMoveDesktops(IUnknown * view, BOOL * canMove) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetCurrentDesktop(ShellVirtualDesktop * *desktop) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetDesktops(IObjectArray * *desktops) = 0;
	virtual HRESULT STDMETHODCALLTYPE
	GetAdjacentDesktop(ShellVirtualDesktop * from, int direction, ShellVirtualDesktop** desktop) = 0;
	virtual HRESULT STDMETHODCALLTYPE SwitchDesktop(ShellVirtualDesktop * desktop) = 0;
	virtual HRESULT STDMETHODCALLTYPE SwitchDesktopAndMoveForegroundView(
	    ShellVirtualDesktop * desktop
	) = 0;
	virtual HRESULT STDMETHODCALLTYPE CreateDesktop(ShellVirtualDesktop * *desktop) = 0;
	virtual HRESULT STDMETHODCALLTYPE MoveDesktop(ShellVirtualDesktop * desktop, int index) = 0;
	virtual HRESULT STDMETHODCALLTYPE
	    RemoveDesktop(ShellVirtualDesktop * desktop, ShellVirtualDesktop * fallback) = 0;
	virtual HRESULT STDMETHODCALLTYPE FindDesktop(const GUID* id, ShellVirtualDesktop** desktop) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetDesktopSwitchIncludeExcludeViews(
	    ShellVirtualDesktop * desktop,
	    IObjectArray * *include,
	    IObjectArray * *exclude
	) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetDesktopName(ShellVirtualDesktop * desktop, HSTRING name) = 0;
	virtual HRESULT STDMETHODCALLTYPE
	SetDesktopWallpaper(ShellVirtualDesktop * desktop, HSTRING path) = 0;
};

class NativeWallpaperProvider: public wallpaper_worker::Provider {
public:
	wallpaper_worker::Result execute(const wallpaper_worker::Request& request) override;

private:
	QString currentWallpaper();
	QString currentWallpaperForScreen(const QString& target);
	bool setWallpaper(const QString& path, bool animate);
	bool setWallpaperForScreen(const QString& path, const QString& target);
	bool setDesktopWallpapers(const QList<wallpaper_worker::DesktopPath>& paths);
	HRESULT record(HRESULT hr) {
		if (FAILED(hr)) this->error = hr;
		return hr;
	}
	HRESULT error = S_OK;
	bool readSucceeded = false;
	QHash<QString, QString> appliedDesktopWallpapers;
};

QList<QPointer<Wallpaper>>& wallpaperObservers() {
	static auto* observers = new QList<QPointer<Wallpaper>>();
	return *observers;
}

wallpaper_worker::Channel& wallpaperChannel() {
	// Process-lived, like the desktop worker. A stuck worker must never retain
	// a GUI object, nor run Release during static destruction.
	static auto* channel = [] {
		auto* app = QCoreApplication::instance();
		auto* result = new wallpaper_worker::Channel(
		    [] { return std::make_unique<NativeWallpaperProvider>(); },
		    app
		);
		QObject::connect(app, &QCoreApplication::aboutToQuit, app, [result] { result->shutdown(); });
		auto* window = services::ServiceMessageWindow::instance();
		window->addHandler(RegisterWindowMessageW(L"TaskbarCreated"), app, [result](WPARAM, LPARAM) {
			result->invalidate();
		});
		return result;
	}();
	return *channel;
}

void broadcastSettingChange(const wchar_t* setting) {
	std::thread([setting] {
		DWORD_PTR result = 0;
		SendMessageTimeoutW(
		    HWND_BROADCAST,
		    WM_SETTINGCHANGE,
		    0,
		    reinterpret_cast<LPARAM>(setting),
		    SMTO_ABORTIFHUNG,
		    2000,
		    &result
		);
	}).detach();
}

bool readDword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD& out) {
	HKEY key = nullptr;
	if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
	DWORD value = 0;
	DWORD size = sizeof(value);
	DWORD type = 0;
	auto ok = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
	RegCloseKey(key);
	if (ok != ERROR_SUCCESS || type != REG_DWORD) return false;
	out = value;
	return true;
}

bool writeDword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD value) {
	HKEY key = nullptr;
	if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr)
	    != ERROR_SUCCESS)
	{
		return false;
	}

	auto ok =
	    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));

	RegCloseKey(key);
	return ok == ERROR_SUCCESS;
}

bool writeString(HKEY root, const wchar_t* subkey, const wchar_t* name, const wchar_t* value) {
	HKEY key = nullptr;
	if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr)
	    != ERROR_SUCCESS)
	{
		return false;
	}

	auto bytes = static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t));
	auto ok = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value), bytes);

	RegCloseKey(key);
	return ok == ERROR_SUCCESS;
}

QString readString(HKEY root, const wchar_t* subkey, const wchar_t* name) {
	HKEY key = nullptr;
	if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) return {};

	wchar_t buffer[MAX_PATH * 2] = {};
	DWORD size = sizeof(buffer) - sizeof(wchar_t);
	DWORD type = 0;
	auto ok = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer), &size);
	RegCloseKey(key);
	if (ok != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
	return QString::fromWCharArray(buffer);
}

bool setWallpaperWithoutAnimation(const std::wstring& path) {
	writeString(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WallpaperStyle", L"10");
	writeString(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"TileWallpaper", L"0");

	auto ok = SystemParametersInfoW(
	    SPI_SETDESKWALLPAPER,
	    0,
	    const_cast<wchar_t*>(path.c_str()),
	    SPIF_UPDATEINIFILE | SPIF_SENDCHANGE
	);

	if (!ok) {
		qCWarning(logWallpaper) << "SystemParametersInfo(SPI_SETDESKWALLPAPER) failed:"
		                        << GetLastError();
	}

	return ok != FALSE;
}

bool parseHexColor(const QString& hex, QColor& out) {
	auto trimmed = hex.startsWith(u'#') ? hex.mid(1) : hex;
	if (trimmed.length() != 6) return false;

	bool ok = false;
	auto value = trimmed.toUInt(&ok, 16);
	if (!ok) return false;

	out = QColor(
	    static_cast<int>((value >> 16) & 0xFF),
	    static_cast<int>((value >> 8) & 0xFF),
	    static_cast<int>(value & 0xFF)
	);

	return true;
}

void openUri(const wchar_t* uri) {
	runLauncher([uri = std::wstring(uri)] {
		auto result = reinterpret_cast<INT_PTR>( // NOLINT
		    ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL)
		);

		if (result <= 32) {
			qCWarning(logWallpaper) << "Could not open"
			                        << qs::windows::logPath(QString::fromStdWString(uri)) << ":"
			                        << result;
		}
	});
}

struct LockScreenResult {
	bool ok = false;
	QString errorMessage;
};

LockScreenResult setLockScreenImageWorker(const QString& path) {
	auto apartment = true;
	try {
		winrt::init_apartment(winrt::apartment_type::multi_threaded);
	} catch (...) {
		apartment = false;
	}

	LockScreenResult result;
	try {
		using namespace winrt::Windows::Storage;
		using namespace winrt::Windows::System::UserProfile;

		auto file = StorageFile::GetFileFromPathAsync(winrt::hstring(path.toStdWString())).get();
		LockScreen::SetImageFileAsync(file).get();
		result.ok = true;
	} catch (const winrt::hresult_error& e) {
		result.errorMessage = QString::fromWCharArray(e.message().c_str());
	} catch (...) {
		result.errorMessage = QStringLiteral("unknown error");
	}

	if (apartment) winrt::uninit_apartment();
	return result;
}

} // namespace

QString Wallpaper::screenTargetId(const QString& screenName) {
	return wallpaper_target::targetId(wallpaperDevicePath(screenName));
}

QString NativeWallpaperProvider::currentWallpaperForScreen(const QString& target) {
	ComPtr<IDesktopWallpaper> wallpaper;
	auto hr = this->record(
	    CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wallpaper))
	);
	if (FAILED(hr) || !wallpaper) return {};

	auto monitor = wallpaperMonitorPath(wallpaper.Get(), target, this->error);
	if (monitor.empty()) return {};

	LPWSTR path = nullptr;
	hr = this->record(wallpaper->GetWallpaper(monitor.c_str(), &path));
	auto result = SUCCEEDED(hr) && path ? QString::fromWCharArray(path) : QString();
	CoTaskMemFree(path);
	return result;
}

bool NativeWallpaperProvider::setWallpaperForScreen(const QString& path, const QString& target) {
	if (path.isEmpty()) return false;
	if (target.isEmpty()) return false;

	auto nativePath = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()).toStdWString();
	ComPtr<IDesktopWallpaper> wallpaper;
	auto hr = this->record(
	    CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wallpaper))
	);
	if (FAILED(hr) || !wallpaper) return false;

	auto monitor = wallpaperMonitorPath(wallpaper.Get(), target, this->error);
	if (monitor.empty()) return false;

	// Position and SPI are global. This route must affect only the exact target.
	return SUCCEEDED(this->record(wallpaper->SetWallpaper(monitor.c_str(), nativePath.c_str())));
}

QString NativeWallpaperProvider::currentWallpaper() {
	ComPtr<IDesktopWallpaper> wallpaper;
	auto hr = this->record(
	    CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wallpaper))
	);

	if (SUCCEEDED(hr) && wallpaper) {
		LPWSTR path = nullptr;
		hr = this->record(wallpaper->GetWallpaper(nullptr, &path));
		if (SUCCEEDED(hr) && path != nullptr) {
			this->readSucceeded = true;
			auto result = QString::fromWCharArray(path);
			CoTaskMemFree(path);
			if (!result.isEmpty()) return result;
		} else CoTaskMemFree(path);

		UINT count = 0;
		if (SUCCEEDED(this->record(wallpaper->GetMonitorDevicePathCount(&count)))) {
			for (UINT i = 0; i < count; i++) {
				LPWSTR monitorId = nullptr;
				if (FAILED(this->record(wallpaper->GetMonitorDevicePathAt(i, &monitorId)))
				    || monitorId == nullptr)
				{
					CoTaskMemFree(monitorId);
					continue;
				}

				LPWSTR monitorPath = nullptr;
				auto ok = SUCCEEDED(this->record(wallpaper->GetWallpaper(monitorId, &monitorPath)))
				       && monitorPath != nullptr;
				CoTaskMemFree(monitorId);
				if (!ok) {
					CoTaskMemFree(monitorPath);
					continue;
				}
				this->readSucceeded = true;

				auto result = QString::fromWCharArray(monitorPath);
				CoTaskMemFree(monitorPath);
				if (!result.isEmpty() && QFileInfo::exists(result)) return result;
			}
		}
	} else {
		qCDebug(logWallpaper) << "IDesktopWallpaper unavailable for read:" << Qt::hex << hr;
	}

	wchar_t buf[MAX_PATH] = {};
	if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, buf, 0)) {
		this->readSucceeded = true;
		return QString::fromWCharArray(buf);
	}

	return {};
}

bool NativeWallpaperProvider::setWallpaper(const QString& path, bool animate) {
	if (path.isEmpty()) return false;

	auto wpath = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()).toStdWString();
	this->appliedDesktopWallpapers.clear();

	if (!animate && setWallpaperWithoutAnimation(wpath)) return true;

	ComPtr<IDesktopWallpaper> wallpaper;
	auto hr = this->record(
	    CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wallpaper))
	);

	if (FAILED(hr) || !wallpaper) {
		qCWarning(logWallpaper) << "IDesktopWallpaper unavailable:" << Qt::hex << hr;
		return false;
	}

	this->record(wallpaper->SetPosition(DWPOS_FILL));

	bool anyOk = false;

	UINT count = 0;
	if (SUCCEEDED(this->record(wallpaper->GetMonitorDevicePathCount(&count))) && count > 0) {
		for (UINT i = 0; i < count; i++) {
			LPWSTR monitorId = nullptr;
			if (FAILED(this->record(wallpaper->GetMonitorDevicePathAt(i, &monitorId)))
			    || monitorId == nullptr)
			{
				CoTaskMemFree(monitorId);
				continue;
			}

			auto setHr = this->record(wallpaper->SetWallpaper(monitorId, wpath.c_str()));
			if (SUCCEEDED(setHr)) {
				anyOk = true;
			} else {
				qCWarning(logWallpaper) << "SetWallpaper failed for monitor" << i << ":" << Qt::hex
				                        << setHr;
			}

			CoTaskMemFree(monitorId);
		}
	}

	if (!anyOk) {
		auto setHr = this->record(wallpaper->SetWallpaper(nullptr, wpath.c_str()));
		anyOk = SUCCEEDED(setHr);
		if (!anyOk) qCWarning(logWallpaper) << "SetWallpaper(all monitors) failed:" << Qt::hex << setHr;
	}

	return anyOk;
}

bool NativeWallpaperProvider::setDesktopWallpapers(const QList<wallpaper_worker::DesktopPath>& paths
) {
	if (windowsBuild() < 22000) return false;

	ComPtr<IServiceProvider> shell;
	auto hr = this->record(
	    CoCreateInstance(CLSID_IMMERSIVE_SHELL, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&shell))
	);

	if (FAILED(hr) || !shell) {
		qCInfo(logWallpaper) << "Immersive shell unavailable for per desktop wallpapers:" << Qt::hex
		                     << hr;
		return false;
	}

	ComPtr<ShellVirtualDesktopManager> manager;
	hr =
	    this->record(shell->QueryService(SID_VIRTUAL_DESKTOP_MANAGER_INTERNAL, IID_PPV_ARGS(&manager))
	    );

	if (FAILED(hr) || !manager) {
		qCInfo(logWallpaper) << "Per desktop wallpapers aren't available on Windows build"
		                     << windowsBuild() << ":" << Qt::hex << hr;
		return false;
	}

	static const GUID null {};
	auto attempts = 0;
	auto applied = 0;

	for (const auto& desktop: paths) {
		if (IsEqualGUID(desktop.id, null)) continue;

		auto id = VirtualDesktops::guidToString(desktop.id);
		auto target = desktop.path;
		if (target.isEmpty()) continue;

		auto native = QDir::toNativeSeparators(QFileInfo(target).absoluteFilePath());
		auto key = (QString::fromWCharArray(kVirtualDesktopsKey) + id).toStdWString();
		auto current = readString(HKEY_CURRENT_USER, key.c_str(), L"Wallpaper");

		if (current.isEmpty()) current = this->appliedDesktopWallpapers.value(id);
		if (current.compare(native, Qt::CaseInsensitive) == 0) continue;

		ComPtr<ShellVirtualDesktop> shellDesktop;
		hr = this->record(manager->FindDesktop(&desktop.id, &shellDesktop));
		if (FAILED(hr) || !shellDesktop) {
			qCWarning(logWallpaper) << "FindDesktop failed for" << qs::windows::logId(id) << ":"
			                        << Qt::hex << hr;
			continue;
		}

		auto value = winrt::hstring(native.toStdWString());
		auto* rawValue = static_cast<HSTRING>(winrt::get_abi(value));
		attempts++;
		hr = this->record(manager->SetDesktopWallpaper(shellDesktop.Get(), rawValue));
		if (FAILED(hr)) {
			qCWarning(logWallpaper) << "SetDesktopWallpaper failed for" << qs::windows::logId(id) << ":"
			                        << Qt::hex << hr;
			continue;
		}

		applied++;
		this->appliedDesktopWallpapers.insert(id, native);
	}

	return attempts == 0 || applied > 0;
}

wallpaper_worker::Result NativeWallpaperProvider::execute(const wallpaper_worker::Request& request
) {
	using namespace wallpaper_worker;
	this->error = S_OK;
	Result result;
	switch (request.kind) {
	case Kind::Read:
		this->readSucceeded = false;
		result.path = this->currentWallpaper();
		result.ok = this->readSucceeded;
		break;
	case Kind::ReadScreen:
		result.path = this->currentWallpaperForScreen(request.target);
		result.ok = SUCCEEDED(this->error);
		break;
	case Kind::Set: result.ok = this->setWallpaper(request.path, request.animate); break;
	case Kind::SetScreen:
		result.ok = this->setWallpaperForScreen(request.path, request.target);
		break;
	case Kind::SetDesktops: result.ok = this->setDesktopWallpapers(request.desktops); break;
	case Kind::Background:
		this->appliedDesktopWallpapers.clear();
		result.ok = setWallpaperWithoutAnimation(request.path.toStdWString());
		break;
	}
	result.disconnected = this->error == RPC_E_DISCONNECTED
	                   || this->error == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE)
	                   || this->error == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED)
	                   || this->error == CO_E_OBJNOTCONNECTED;
	return result;
}

QString Wallpaper::currentWallpaper() { return wallpaperChannel().read({}); }

QString Wallpaper::currentWallpaperForScreen(const QString& screenName) {
	auto target = wallpaperDevicePath(screenName);
	if (target.isEmpty()) return {};
	wallpaper_worker::Request request;
	request.kind = wallpaper_worker::Kind::ReadScreen;
	request.target = target;
	return wallpaperChannel().read(std::move(request));
}

bool Wallpaper::setWallpaperForScreen(const QString& path, const QString& screenName) {
	auto target = wallpaperDevicePath(screenName);
	if (path.isEmpty() || target.isEmpty()) return false;
	wallpaper_worker::Request request;
	request.kind = wallpaper_worker::Kind::SetScreen;
	request.path = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
	request.target = target;
	return wallpaperChannel().command(std::move(request));
}

bool Wallpaper::setWallpaper(const QString& path, bool animate) {
	if (path.isEmpty()) return false;
	wallpaper_worker::Request request;
	request.kind = wallpaper_worker::Kind::Set;
	request.path = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
	request.animate = animate;
	return wallpaperChannel().command(std::move(request));
}

void Wallpaper::setWallpaperInBackground(const QString& path) {
	if (path.isEmpty()) return;
	wallpaper_worker::Request request;
	request.kind = wallpaper_worker::Kind::Background;
	request.path = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
	request.animate = false;
	wallpaperChannel().background(std::move(request));
}

bool Wallpaper::setDesktopWallpapers(const QVariantMap& paths, const QString& fallback) {
	if (windowsBuild() < 22000) return false;
	wallpaper_worker::Request request;
	request.kind = wallpaper_worker::Kind::SetDesktops;
	for (const auto& desktop: VirtualDesktops::instance()->desktops()) {
		auto path = paths.value(VirtualDesktops::guidToString(desktop.id)).toString();
		if (path.isEmpty()) path = fallback;
		if (!path.isEmpty()) request.desktops.append({desktop.id, path});
	}
	return wallpaperChannel().command(std::move(request));
}

void Wallpaper::trackDesktops() {
	if (this->mTrackingDesktops) return;
	this->mTrackingDesktops = true;

	auto* virtualDesktops = VirtualDesktops::instance();
	QObject::connect(
	    virtualDesktops,
	    &VirtualDesktops::currentChanged,
	    this,
	    &Wallpaper::currentDesktopChanged
	);
	QObject::connect(
	    virtualDesktops,
	    &VirtualDesktops::desktopsChanged,
	    this,
	    &Wallpaper::desktopsChanged
	);
}

QString Wallpaper::currentDesktop() {
	this->trackDesktops();

	static const GUID null {};
	auto* virtualDesktops = VirtualDesktops::instance();
	auto index = virtualDesktops->currentIndex();
	if (index < 0 || index >= virtualDesktops->count()) return {};

	const auto& id = virtualDesktops->desktops().at(index).id;
	return IsEqualGUID(id, null) ? QString() : VirtualDesktops::guidToString(id);
}

QVariantList Wallpaper::desktops() {
	this->trackDesktops();

	static const GUID null {};
	QVariantList list;
	for (const auto& desktop: VirtualDesktops::instance()->desktops()) {
		auto id = IsEqualGUID(desktop.id, null) ? QString() : VirtualDesktops::guidToString(desktop.id);
		list.append(QVariantMap {{"id", id}, {"name", desktop.name}});
	}

	return list;
}

bool Wallpaper::isDarkMode() {
	DWORD value = 1;
	if (!readDword(HKEY_CURRENT_USER, kPersonalizeKey, L"AppsUseLightTheme", value)) return false;
	return value == 0;
}

void Wallpaper::setDarkMode(bool dark) {
	DWORD value = dark ? 0 : 1;
	auto ok1 = writeDword(HKEY_CURRENT_USER, kPersonalizeKey, L"AppsUseLightTheme", value);
	auto ok2 = writeDword(HKEY_CURRENT_USER, kPersonalizeKey, L"SystemUsesLightTheme", value);

	if (!ok1 || !ok2) qCWarning(logWallpaper) << "failed writing theme registry values";

	broadcastSettingChange(L"ImmersiveColorSet");
}

bool Wallpaper::setAccentColor(const QString& hex) {
	QColor color;
	if (!parseHexColor(hex, color)) {
		qCWarning(logWallpaper) << "setAccentColor: not a hex color:" << hex;
		return false;
	}

	DWORD accentColor = 0xFF000000u | (static_cast<DWORD>(color.blue()) << 16)
	                  | (static_cast<DWORD>(color.green()) << 8) | static_cast<DWORD>(color.red());

	DWORD colorizationColor = 0xC4000000u | (static_cast<DWORD>(color.red()) << 16)
	                        | (static_cast<DWORD>(color.green()) << 8)
	                        | static_cast<DWORD>(color.blue());

	DWORD current = 0;
	auto unchanged = readDword(HKEY_CURRENT_USER, kDwmKey, L"AccentColor", current)
	              && current == accentColor
	              && readDword(HKEY_CURRENT_USER, kDwmKey, L"ColorizationColor", current)
	              && current == colorizationColor
	              && readDword(HKEY_CURRENT_USER, kDwmKey, L"ColorizationAfterglow", current)
	              && current == colorizationColor;
	if (unchanged) return true;

	auto ok1 = writeDword(HKEY_CURRENT_USER, kDwmKey, L"AccentColor", accentColor);
	auto ok2 = writeDword(HKEY_CURRENT_USER, kDwmKey, L"ColorizationColor", colorizationColor);
	auto ok3 = writeDword(HKEY_CURRENT_USER, kDwmKey, L"ColorizationAfterglow", colorizationColor);

	if (!ok1 || !ok2 || !ok3) {
		qCWarning(logWallpaper) << "failed writing accent color registry values";
	}

	broadcastSettingChange(L"ImmersiveColorSet");
	return ok1 && ok2;
}

QString Wallpaper::matugenPath() {
	auto path = QCoreApplication::applicationDirPath() + QStringLiteral("/matugen.exe");
	return QFileInfo::exists(path) ? path : QString();
}

Wallpaper::Wallpaper(QObject* parent): QObject(parent) {
	QObject::connect(qApp, &QCoreApplication::aboutToQuit, []() { gShuttingDown.store(true); });

	this->mSystemDarkMode = Wallpaper::isDarkMode();
	// Existing bool calls still report confirmed success. A false result at
	// the GUI deadline is not cancellation; this signal reports the outcome.
	wallpaperObservers().append(this);
	wallpaperChannel().finished =
	    [](const wallpaper_worker::Request& request, bool ok, const QString& reason) {
		    QString operation;
		    switch (request.kind) {
		    case wallpaper_worker::Kind::Set: operation = QStringLiteral("setWallpaper"); break;
		    case wallpaper_worker::Kind::SetScreen:
			    operation = QStringLiteral("setWallpaperForScreen");
			    break;
		    case wallpaper_worker::Kind::SetDesktops:
			    operation = QStringLiteral("setDesktopWallpapers");
			    break;
		    case wallpaper_worker::Kind::Background:
			    operation = QStringLiteral("setWallpaperInBackground");
			    break;
		    default: return;
		    }
		    auto& observers = wallpaperObservers();
		    observers.removeIf([](const auto& observer) { return observer.isNull(); });
		    const auto snapshot = observers;
		    for (const auto& observer: snapshot)
			    if (observer)
				    emit observer
				        ->wallpaperOperationFinished(operation, request.path, request.target, ok, reason);
	    };

	auto* window = qs::windows::services::ServiceMessageWindow::instance();
	window->addHandler(
	    WM_SETTINGCHANGE,
	    this,
	    [self = QPointer(this)](WPARAM /*wparam*/, LPARAM lparam) {
		    if (!self || lparam == 0) return;

		    auto name = QString::fromWCharArray(reinterpret_cast<const wchar_t*>(lparam)); // NOLINT
		    if (name.compare(QStringLiteral("ImmersiveColorSet"), Qt::CaseInsensitive) == 0) {
			    self->refreshSystemDarkMode();
		    }
	    }
	);
}

void Wallpaper::refreshSystemDarkMode() {
	auto value = Wallpaper::isDarkMode();
	if (value != this->mSystemDarkMode) {
		this->mSystemDarkMode = value;
		emit this->systemDarkModeChanged();
	}
}

void Wallpaper::setLockScreenImage(const QString& path) {
	if (path.isEmpty() || this->mLockScreenAttemptRunning) return;
	this->mLockScreenAttemptRunning = true;

	auto absolutePath = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
	auto self = QPointer<Wallpaper>(this);

	std::thread([self, absolutePath]() {
		auto result = setLockScreenImageWorker(absolutePath);

		if (gShuttingDown.load()) return;
		auto* app = QCoreApplication::instance();
		if (app == nullptr) return;

		QMetaObject::invokeMethod(
		    app,
		    [self, result]() {
			    if (self) self->applyLockScreenResult(result.ok, result.errorMessage);
		    },
		    Qt::QueuedConnection
		);
	}).detach();
}

void Wallpaper::applyLockScreenResult(bool ok, const QString& errorMessage) {
	this->mLockScreenAttemptRunning = false;

	if (!ok) {
		qCWarning(logWallpaper) << "setLockScreenImage failed:" << errorMessage;
	}

	if (ok != this->mLockScreenImageSupported) {
		this->mLockScreenImageSupported = ok;
		emit this->lockScreenImageSupportedChanged();
	}

	emit this->lockScreenImageApplied(ok);
}

QString Wallpaper::lockScreenMode() {
	DWORD value = 0;
	if (readDword(HKEY_CURRENT_USER, kContentDeliveryKey, L"RotatingLockScreenEnabled", value)
	    && value != 0)
	{
		return QStringLiteral("spotlight");
	}

	value = 0;
	if (readDword(HKEY_CURRENT_USER, kLockScreenKey, L"SlideshowEnabled", value) && value != 0) {
		return QStringLiteral("slideshow");
	}

	return QStringLiteral("picture");
}

void Wallpaper::openLockScreenPersonalizationSettings() { openUri(L"ms-settings:lockscreen"); }

} // namespace qs::windows::sys

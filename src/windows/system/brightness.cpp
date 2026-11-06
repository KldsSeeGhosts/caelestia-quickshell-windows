#include "brightness.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

#include <lowlevelmonitorconfigurationapi.h>
#include <ntddvdeo.h>
#include <oleauto.h>
#include <physicalmonitorenumerationapi.h>
#include <qcoreapplication.h>
#include <qguiapplication.h>
#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qmetaobject.h>
#include <qmutex.h>
#include <qpointer.h>
#include <qrunnable.h>
#include <qscopeguard.h>
#include <qscreen.h>
#include <qstringlist.h>
#include <qt_windows.h>
#include <qthreadpool.h>
#include <qtimer.h>
#include <wbemidl.h>
#include <winioctl.h>
#include <wrl/client.h>

#include "../../core/logcat.hpp"
#include "../redact.hpp"
#include "../screen_identity.hpp"
#include "../startup.hpp"
#include "../util.hpp"
#include "brightness_backend.hpp"
#include "gamma.hpp"

using Microsoft::WRL::ComPtr;

namespace qs::windows::sys {

namespace {
QS_LOGGING_CATEGORY(logBrightness, "quickshell.windows.brightness", QtInfoMsg);

constexpr int POOL_STOP_TIMEOUT_MS = 2000;
constexpr long WMI_WAIT_MS = 5000;
constexpr long WMI_PROBE_WAIT_MS = 15000;
constexpr int SETTLE_MS = 1200;
constexpr int EVENT_ATTEMPTS = 5;
constexpr DWORD EVENT_RETRY_MS = 5000;
constexpr qreal SAME_VALUE = 0.005;
constexpr BYTE VCP_BRIGHTNESS = 0x10;
constexpr BYTE NO_SYSTEM_BATTERY = 128;
constexpr DWORD MAX_LCD_LEVELS = 256;

LONG readSdrWhite(LUID adapter, UINT32 id, UINT32& raw) {
	DISPLAYCONFIG_SDR_WHITE_LEVEL info {};
	info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
	info.header.size = sizeof(info);
	info.header.adapterId = adapter;
	info.header.id = id;
	auto result = DisplayConfigGetDeviceInfo(&info.header);
	if (result == ERROR_SUCCESS) raw = info.SDRWhiteLevel;
	return result;
}

// Undocumented Windows Settings request. See ledoge/set_maxtml main_sdrwhite.c.
LONG setSdrWhite(LUID adapter, UINT32 id, UINT32 raw) {
	struct SetWhite {
		DISPLAYCONFIG_DEVICE_INFO_HEADER header;
		UINT32 level;
		BYTE finalValue;
	} info {};
	info.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(0xFFFFFFEEu);
	info.header.size = sizeof(info);
	info.header.adapterId = adapter;
	info.header.id = id;
	info.level = raw;
	info.finalValue = 1;
	return DisplayConfigSetDeviceInfo(&info.header);
}

QString hexCode(quint32 code) { return QStringLiteral("0x%1").arg(code, 8, 16, QLatin1Char('0')); }

// Process-wide so request ids stay unique and strictly increasing across
// controller instances (an adapter may drop and recreate its controller while
// native queries from the previous one are still outstanding). 64-bit, so the
// counter cannot wrap to a non-positive id in any realistic process lifetime.
qint64 nextQueryRequestId() {
	static std::atomic<qint64> next {1};
	return next.fetch_add(1, std::memory_order_relaxed);
}

QString hrText(HRESULT hr) {
	struct Name {
		HRESULT hr;
		const char* text;
	};

	static const Name names[] = {
	    {static_cast<HRESULT>(WBEM_E_FAILED), "failed"},
	    {static_cast<HRESULT>(WBEM_E_NOT_FOUND), "not found"},
	    {static_cast<HRESULT>(WBEM_E_ACCESS_DENIED), "access denied"},
	    {static_cast<HRESULT>(WBEM_E_PROVIDER_FAILURE), "provider failure"},
	    {static_cast<HRESULT>(WBEM_E_TYPE_MISMATCH), "type mismatch"},
	    {static_cast<HRESULT>(WBEM_E_INVALID_PARAMETER), "invalid parameter"},
	    {static_cast<HRESULT>(WBEM_E_NOT_SUPPORTED), "not supported"},
	    {static_cast<HRESULT>(WBEM_E_INVALID_CLASS), "invalid class"},
	    {static_cast<HRESULT>(WBEM_E_INVALID_METHOD), "invalid method"},
	    {static_cast<HRESULT>(WBEM_E_INVALID_METHOD_PARAMETERS), "invalid method parameters"},
	    {static_cast<HRESULT>(WBEM_E_TRANSPORT_FAILURE), "transport failure"},
	    {HRESULT_FROM_WIN32(ERROR_TIMEOUT), "timed out"},
	    {RPC_E_DISCONNECTED, "disconnected"},
	    {E_ACCESSDENIED, "access denied"},
	};

	auto text = hexCode(static_cast<quint32>(hr));
	for (const auto& name: names) {
		if (name.hr == hr) return QStringLiteral("%1 (%2)").arg(text, QLatin1StringView(name.text));
	}

	return text;
}

QString win32Text(DWORD error) {
	if (error < 0x10000) return QStringLiteral("error %1").arg(error);
	return QStringLiteral("error %1").arg(hexCode(error));
}

QString percentText(qreal value) { return QStringLiteral("%1%").arg(std::lround(value * 100.0)); }

struct PhysicalMonitors {
	explicit PhysicalMonitors(HMONITOR hMonitor) {
		if (hMonitor == nullptr) {
			this->error = ERROR_INVALID_HANDLE;
			return;
		}

		DWORD count = 0;
		if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMonitor, &count)) {
			this->error = GetLastError();
			return;
		}

		if (count == 0) {
			this->error = ERROR_NOT_FOUND;
			return;
		}

		this->monitors.resize(count);
		if (!GetPhysicalMonitorsFromHMONITOR(hMonitor, count, this->monitors.data())) {
			this->error = GetLastError();
			this->monitors.clear();
		}
	}

	~PhysicalMonitors() {
		if (!this->monitors.empty()) {
			DestroyPhysicalMonitors(static_cast<DWORD>(this->monitors.size()), this->monitors.data());
		}
	}

	PhysicalMonitors(const PhysicalMonitors&) = delete;
	PhysicalMonitors(PhysicalMonitors&&) = delete;
	PhysicalMonitors& operator=(const PhysicalMonitors&) = delete;
	PhysicalMonitors& operator=(PhysicalMonitors&&) = delete;

	std::vector<PHYSICAL_MONITOR> monitors;
	DWORD error = ERROR_SUCCESS;
};

bool ddcQuery(HMONITOR hMonitor, qreal& outBrightness, DWORD& outError) {
	PhysicalMonitors physical(hMonitor);
	if (physical.monitors.empty()) {
		outError = physical.error;
		return false;
	}

	MC_VCP_CODE_TYPE vcpType {};
	DWORD current = 0;
	DWORD maxValue = 0;
	if (!GetVCPFeatureAndVCPFeatureReply(
	        physical.monitors[0].hPhysicalMonitor,
	        VCP_BRIGHTNESS,
	        &vcpType,
	        &current,
	        &maxValue
	    ))
	{
		outError = GetLastError();
		return false;
	}

	if (maxValue == 0) {
		outError = ERROR_INVALID_DATA;
		return false;
	}

	outBrightness = std::clamp(qreal(current) / qreal(maxValue), 0.0, 1.0);
	return true;
}

bool ddcSet(HMONITOR hMonitor, qreal value, QString& outError) {
	PhysicalMonitors physical(hMonitor);
	if (physical.monitors.empty()) {
		outError = win32Text(physical.error);
		return false;
	}

	std::vector<HANDLE> targets;
	targets.reserve(physical.monitors.size());
	for (const auto& monitor: physical.monitors) targets.push_back(monitor.hPhysicalMonitor);

	auto report = ddcWriteTargets(
	    value,
	    targets,
	    [](HANDLE target, DWORD& maximum, DWORD& error) {
		    MC_VCP_CODE_TYPE vcpType {};
		    DWORD current = 0;
		    if (!GetVCPFeatureAndVCPFeatureReply(
		            target,
		            VCP_BRIGHTNESS,
		            &vcpType,
		            &current,
		            &maximum
		        ))
		    {
			    error = GetLastError();
			    return false;
		    }
		    return true;
	    },
	    [](HANDLE target, DWORD level, DWORD& error) {
		    if (!SetVCPFeature(target, VCP_BRIGHTNESS, level)) {
			    error = GetLastError();
			    return false;
		    }
		    return true;
	    }
	);
	if (report.ok()) return true;

	QStringList failures;
	for (const auto& failure: report.failures) {
		failures << QStringLiteral("physical monitor %1 %2 (%3)")
		                .arg(
		                    QString::number(failure.target),
		                    failure.range
		                        ? QStringLiteral("refused: the VCP brightness range could not be read")
		                        : QStringLiteral("write failed"),
		                    win32Text(failure.error)
		                );
	}
	outError = failures.isEmpty()
	    ? win32Text(ERROR_NOT_FOUND)
	    : failures.join(QStringLiteral("; "));
	return false;
}

const char* panelKindName(PanelKind kind) {
	switch (kind) {
	case PanelKind::Internal: return "built-in";
	case PanelKind::External: return "external";
	default: return "unknown output";
	}
}

bool isInternalOutput(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY technology) {
	switch (technology) {
	case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL:
	case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED:
	case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED:
	case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS: return true;
	default: return false;
	}
}

const char* technologyName(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY technology) {
	switch (technology) {
	case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15: return "VGA";
	case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI: return "DVI";
	case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI: return "HDMI";
	case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS: return "LVDS";
	case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL: return "DisplayPort";
	case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED: return "eDP";
	case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_USB_TUNNEL: return "USB-C DisplayPort";
	case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EXTERNAL: return "UDI";
	case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED: return "embedded UDI";
	case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST: return "Miracast";
	case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED: return "indirect";
	case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL: return "virtual";
	case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL: return "internal";
	default: return "other";
	}
}

QString instanceIdFromDevicePath(const QString& devicePath) {
	auto id = devicePath;
	if (id.startsWith(QStringLiteral("\\\\?\\"))) id.remove(0, 4);

	auto guid = id.lastIndexOf(QStringLiteral("#{"));
	if (guid >= 0) id.truncate(guid);

	id.replace(QLatin1Char('#'), QLatin1Char('\\'));
	return id.toUpper();
}

bool instanceMatches(const QString& wmiInstance, const QString& instanceId) {
	if (instanceId.isEmpty()) return false;

	auto name = wmiInstance.toUpper();
	return name == instanceId || name.startsWith(instanceId + QLatin1Char('_'));
}

MonitorDescription describeMonitor(HMONITOR hMonitor) {
	MonitorDescription description;
	if (hMonitor == nullptr) return description;

	MONITORINFOEXW info {};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfoW(hMonitor, &info)) return description;

	std::vector<DISPLAYCONFIG_PATH_INFO> paths;
	std::vector<DISPLAYCONFIG_MODE_INFO> modes;
	LONG result = ERROR_INSUFFICIENT_BUFFER;

	for (auto attempt = 0; attempt < 3 && result == ERROR_INSUFFICIENT_BUFFER; attempt++) {
		UINT32 pathCount = 0;
		UINT32 modeCount = 0;
		result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
		if (result != ERROR_SUCCESS) return description;

		paths.resize(pathCount);
		modes.resize(modeCount);
		result = QueryDisplayConfig(
		    QDC_ONLY_ACTIVE_PATHS,
		    &pathCount,
		    paths.data(),
		    &modeCount,
		    modes.data(),
		    nullptr
		);
		paths.resize(pathCount);
	}

	if (result != ERROR_SUCCESS) return description;

	for (const auto& path: paths) {
		if (isInternalOutput(path.targetInfo.outputTechnology)) description.internalTargets++;
	}

	auto matched = false;
	for (const auto& path: paths) {
		DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
		source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		source.header.size = sizeof(source);
		source.header.adapterId = path.sourceInfo.adapterId;
		source.header.id = path.sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
		if (std::wcscmp(source.viewGdiDeviceName, info.szDevice) != 0) continue;

		matched = true;

		MonitorTarget target;
		target.technology = path.targetInfo.outputTechnology;
		target.adapterId = path.targetInfo.adapterId;
		target.targetId = path.targetInfo.id;

		// INFO_2 is hidden by older NTDDI build targets even with a recent SDK.
		// Its documented ABI is header, flags, encoding, bit depth, color mode.
		struct ColorInfo2 {
			DISPLAYCONFIG_DEVICE_INFO_HEADER header;
			UINT32 flags;
			UINT32 encoding;
			UINT32 bitsPerChannel;
			UINT32 activeMode;
		} color2 {};
		color2.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(15);
		color2.header.size = sizeof(color2);
		color2.header.adapterId = target.adapterId;
		color2.header.id = target.targetId;
		auto colorResult = DisplayConfigGetDeviceInfo(&color2.header);
		if (colorResult == ERROR_SUCCESS) {
			target.colorKnown = true;
			target.advancedColor = (color2.flags & 2u) != 0;
			target.hdrActive = isHdrColorMode(color2.activeMode);
		} else {
			DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color {};
			color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
			color.header.size = sizeof(color);
			color.header.adapterId = target.adapterId;
			color.header.id = target.targetId;
			if (DisplayConfigGetDeviceInfo(&color.header) == ERROR_SUCCESS) {
				target.colorKnown = true;
				target.advancedColor = color.advancedColorEnabled;
				target.hdrActive = color.advancedColorEnabled && !color.wideColorEnforced;
			}
		}
		if (target.hdrActive)
			target.whiteKnown =
			    readSdrWhite(target.adapterId, target.targetId, target.whiteLevel) == ERROR_SUCCESS;

		DISPLAYCONFIG_TARGET_DEVICE_NAME name {};
		name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
		name.header.size = sizeof(name);
		name.header.adapterId = path.targetInfo.adapterId;
		name.header.id = path.targetInfo.id;
		if (DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS) {
			target.instanceId = instanceIdFromDevicePath(QString::fromWCharArray(name.monitorDevicePath));
			target.friendlyName = QString::fromWCharArray(name.monitorFriendlyDeviceName);
		}

		if (isInternalOutput(target.technology)) description.kind = PanelKind::Internal;
		description.targets.push_back(std::move(target));
	}

	if (matched && description.kind != PanelKind::Internal) description.kind = PanelKind::External;
	return description;
}

bool hasSystemBattery() {
	SYSTEM_POWER_STATUS status {};
	if (!GetSystemPowerStatus(&status)) return true;
	return status.BatteryFlag != NO_SYSTEM_BATTERY;
}

struct MtaScope {
	MtaScope(): hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}

	MtaScope(const MtaScope&) = delete;
	MtaScope(MtaScope&&) = delete;
	MtaScope& operator=(const MtaScope&) = delete;
	MtaScope& operator=(MtaScope&&) = delete;

	~MtaScope() {
		if (SUCCEEDED(this->hr)) CoUninitialize();
	}

	HRESULT hr;
};

struct Bstr {
	explicit Bstr(const wchar_t* text): value(SysAllocString(text)) {}
	explicit Bstr(const QString& text)
	    : value(SysAllocStringLen(
	          reinterpret_cast<const OLECHAR*>(text.utf16()), // NOLINT
	          static_cast<UINT>(text.size())
	      )) {}

	Bstr(const Bstr&) = delete;
	Bstr(Bstr&&) = delete;
	Bstr& operator=(const Bstr&) = delete;
	Bstr& operator=(Bstr&&) = delete;

	~Bstr() { SysFreeString(this->value); }

	BSTR value;
};

struct Variant {
	Variant() { VariantInit(&this->value); }

	Variant(const Variant&) = delete;
	Variant(Variant&&) = delete;
	Variant& operator=(const Variant&) = delete;
	Variant& operator=(Variant&&) = delete;

	~Variant() { VariantClear(&this->value); }

	VARIANT value;
};

void secure(IUnknown* proxy) {
	if (proxy == nullptr) return;

	CoSetProxyBlanket(
	    proxy,
	    RPC_C_AUTHN_WINNT,
	    RPC_C_AUTHZ_NONE,
	    nullptr,
	    RPC_C_AUTHN_LEVEL_CALL,
	    RPC_C_IMP_LEVEL_IMPERSONATE,
	    nullptr,
	    EOAC_NONE
	);
}

HRESULT connectWmi(IWbemServices** out) {
	*out = nullptr;

	ComPtr<IWbemLocator> locator;
	auto hr = CoCreateInstance(
	    CLSID_WbemLocator,
	    nullptr,
	    CLSCTX_INPROC_SERVER,
	    IID_IWbemLocator,
	    reinterpret_cast<LPVOID*>(locator.GetAddressOf()) // NOLINT
	);
	if (FAILED(hr)) return hr;
	if (!locator) return E_FAIL;

	Bstr ns(L"ROOT\\WMI");
	hr = locator->ConnectServer(ns.value, nullptr, nullptr, nullptr, 0, nullptr, nullptr, out);
	if (FAILED(hr)) return hr;
	if (*out == nullptr) return E_FAIL;

	secure(*out);
	return S_OK;
}

bool wmiConnectionLost(HRESULT hr) {
	return hr == RPC_E_DISCONNECTED || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE)
	    || hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED) || hr == WBEM_E_TRANSPORT_FAILURE
	    || hr == WBEM_E_SHUTTING_DOWN;
}

struct WmiCache {
	QMutex mutex;
	IWbemServices* services = nullptr;
	bool mtaHeld = false;
	bool mtaAttempted = false;
};

WmiCache& wmiCache() {
	static auto* cache = new WmiCache();
	return *cache;
}

template <typename Fn>
HRESULT withWmi(Fn&& fn) {
	MtaScope com;

	auto runOnce = [&fn]() -> HRESULT {
		ComPtr<IWbemServices> services;
		auto hr = connectWmi(services.GetAddressOf());
		if (FAILED(hr)) return hr;
		return fn(services.Get());
	};

	if (com.hr == RPC_E_CHANGED_MODE) return runOnce();
	if (FAILED(com.hr)) return com.hr;

	auto& cache = wmiCache();
	QMutexLocker locker(&cache.mutex);

	if (!cache.mtaAttempted) {
		cache.mtaAttempted = true;
		CO_MTA_USAGE_COOKIE cookie = nullptr;
		cache.mtaHeld = SUCCEEDED(CoIncrementMTAUsage(&cookie));
	}

	if (!cache.mtaHeld) return runOnce();

	if (cache.services == nullptr) {
		auto hr = connectWmi(&cache.services);
		if (FAILED(hr)) return hr;
	}

	auto hr = fn(cache.services);
	if (wmiConnectionLost(hr)) {
		qCDebug(logBrightness) << "WMI connection lost, reconnecting";
		cache.services->Release();
		cache.services = nullptr;
		auto reconnect = connectWmi(&cache.services);
		if (FAILED(reconnect)) return reconnect;
		hr = fn(cache.services);
	}

	return hr;
}

HRESULT execQuery(IWbemServices* services, const QString& wql, ComPtr<IEnumWbemClassObject>& out) {
	Bstr language(L"WQL");
	Bstr query(wql);
	auto hr = services->ExecQuery(
	    language.value,
	    query.value,
	    WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
	    nullptr,
	    out.ReleaseAndGetAddressOf()
	);
	if (FAILED(hr)) return hr;
	if (!out) return E_FAIL;

	secure(out.Get());
	return S_OK;
}

HRESULT nextObject(
    IEnumWbemClassObject* enumerator,
    ComPtr<IWbemClassObject>& out,
    long timeout = WMI_WAIT_MS
) {
	ULONG returned = 0;
	auto hr = enumerator->Next(timeout, 1, out.ReleaseAndGetAddressOf(), &returned);
	if (hr == WBEM_S_TIMEDOUT) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
	if (FAILED(hr)) return hr;
	return returned == 1 && out ? S_OK : S_FALSE;
}

QString stringProperty(IWbemClassObject* object, const wchar_t* name) {
	Variant value;
	if (FAILED(object->Get(name, 0, &value.value, nullptr, nullptr))) return {};
	if (value.value.vt != VT_BSTR || value.value.bstrVal == nullptr) return {};

	return QString::fromWCharArray(
	    value.value.bstrVal,
	    static_cast<qsizetype>(SysStringLen(value.value.bstrVal))
	);
}

int intProperty(IWbemClassObject* object, const wchar_t* name, int fallback) {
	Variant value;
	if (FAILED(object->Get(name, 0, &value.value, nullptr, nullptr))) return fallback;

	switch (value.value.vt) {
	case VT_UI1: return value.value.bVal;
	case VT_I2: return value.value.iVal;
	case VT_UI2: return value.value.uiVal;
	case VT_I4: return static_cast<int>(value.value.lVal);
	case VT_UI4: return static_cast<int>(value.value.ulVal);
	case VT_INT: return value.value.intVal;
	case VT_UINT: return static_cast<int>(value.value.uintVal);
	default: return fallback;
	}
}

bool boolProperty(IWbemClassObject* object, const wchar_t* name, bool fallback) {
	Variant value;
	if (FAILED(object->Get(name, 0, &value.value, nullptr, nullptr))) return fallback;
	if (value.value.vt != VT_BOOL) return fallback;
	return value.value.boolVal != VARIANT_FALSE;
}

QString wqlString(const QString& text) {
	auto escaped = text;
	escaped.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
	escaped.replace(QStringLiteral("'"), QStringLiteral("\\'"));
	return escaped;
}

QString methodsPathFor(const QString& instanceName) {
	auto escaped = instanceName;
	escaped.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
	escaped.replace(QStringLiteral("\""), QStringLiteral("\\\""));
	return QStringLiteral("WmiMonitorBrightnessMethods.InstanceName=\"%1\"").arg(escaped);
}

HRESULT listWmiPanels(IWbemServices* services, std::vector<WmiPanel>& out) {
	out.clear();

	ComPtr<IEnumWbemClassObject> enumerator;
	auto hr = execQuery(services, QStringLiteral("SELECT * FROM WmiMonitorBrightness"), enumerator);
	if (FAILED(hr)) return hr;

	ComPtr<IWbemClassObject> object;
	while ((hr = nextObject(enumerator.Get(), object, WMI_PROBE_WAIT_MS)) == S_OK) {
		WmiPanel panel;
		panel.instanceName = stringProperty(object.Get(), L"InstanceName");
		panel.active = boolProperty(object.Get(), L"Active", true);
		panel.current = intProperty(object.Get(), L"CurrentBrightness", -1);
		panel.levels = intProperty(object.Get(), L"Levels", 0);
		if (!panel.instanceName.isEmpty()) out.push_back(std::move(panel));
	}

	if (FAILED(hr)) return hr;
	if (out.empty()) return S_OK;

	if (FAILED(execQuery(
	        services,
	        QStringLiteral("SELECT * FROM WmiMonitorBrightnessMethods"),
	        enumerator
	    )))
	{
		return S_OK;
	}

	while (nextObject(enumerator.Get(), object, WMI_PROBE_WAIT_MS) == S_OK) {
		auto name = stringProperty(object.Get(), L"InstanceName");
		auto path = stringProperty(object.Get(), L"__RELPATH");
		if (name.isEmpty() || path.isEmpty()) continue;

		for (auto& panel: out) {
			if (panel.instanceName.compare(name, Qt::CaseInsensitive) == 0) panel.methodsPath = path;
		}
	}

	return S_OK;
}

HRESULT readWmiPanel(IWbemServices* services, const QString& instanceName, int& outPercent) {
	ComPtr<IEnumWbemClassObject> enumerator;
	auto hr = execQuery(
	    services,
	    QStringLiteral("SELECT * FROM WmiMonitorBrightness WHERE InstanceName = '%1'")
	        .arg(wqlString(instanceName)),
	    enumerator
	);
	if (FAILED(hr)) return hr;

	ComPtr<IWbemClassObject> object;
	hr = nextObject(enumerator.Get(), object);
	if (FAILED(hr)) return hr;
	if (hr != S_OK) return WBEM_E_NOT_FOUND;

	outPercent = intProperty(object.Get(), L"CurrentBrightness", -1);
	return outPercent >= 0 ? S_OK : WBEM_E_TYPE_MISMATCH;
}

HRESULT writeWmiPanel(IWbemServices* services, const QString& methodsPath, int percent) {
	Bstr className(L"WmiMonitorBrightnessMethods");
	Bstr methodName(L"WmiSetBrightness");

	ComPtr<IWbemClassObject> classObject;
	auto hr = services->GetObject(className.value, 0, nullptr, classObject.GetAddressOf(), nullptr);
	if (FAILED(hr)) return hr;
	if (!classObject) return E_FAIL;

	ComPtr<IWbemClassObject> signature;
	hr = classObject->GetMethod(methodName.value, 0, signature.GetAddressOf(), nullptr);
	if (FAILED(hr)) return hr;
	if (!signature) return WBEM_E_INVALID_METHOD;

	ComPtr<IWbemClassObject> params;
	hr = signature->SpawnInstance(0, params.GetAddressOf());
	if (FAILED(hr)) return hr;
	if (!params) return E_FAIL;

	Variant timeout;
	timeout.value.vt = VT_I4;
	timeout.value.lVal = 0;
	hr = params->Put(L"Timeout", 0, &timeout.value, 0);
	if (FAILED(hr)) return hr;

	Variant level;
	level.value.vt = VT_UI1;
	level.value.bVal = static_cast<BYTE>(std::clamp(percent, 0, 100));
	hr = params->Put(L"Brightness", 0, &level.value, 0);
	if (FAILED(hr)) return hr;

	Bstr path(methodsPath);
	ComPtr<IWbemCallResult> call;
	hr = services->ExecMethod(
	    path.value,
	    methodName.value,
	    WBEM_FLAG_RETURN_IMMEDIATELY,
	    nullptr,
	    params.Get(),
	    nullptr,
	    call.GetAddressOf()
	);
	if (FAILED(hr)) return hr;
	if (!call) return S_OK;

	secure(call.Get());
	long status = WBEM_S_NO_ERROR;
	hr = call->GetCallStatus(WMI_WAIT_MS, &status);
	if (hr == WBEM_S_TIMEDOUT) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
	if (FAILED(hr)) return hr;
	return status;
}

struct DeviceHandle {
	DeviceHandle()
	    : handle(CreateFileW(
	          L"\\\\.\\LCD",
	          GENERIC_READ | GENERIC_WRITE,
	          FILE_SHARE_READ | FILE_SHARE_WRITE,
	          nullptr,
	          OPEN_EXISTING,
	          0,
	          nullptr
	      )) {
		if (this->handle == INVALID_HANDLE_VALUE) this->error = GetLastError();
	}

	DeviceHandle(const DeviceHandle&) = delete;
	DeviceHandle(DeviceHandle&&) = delete;
	DeviceHandle& operator=(const DeviceHandle&) = delete;
	DeviceHandle& operator=(DeviceHandle&&) = delete;

	~DeviceHandle() {
		if (this->handle != INVALID_HANDLE_VALUE) CloseHandle(this->handle);
	}

	[[nodiscard]] bool valid() const { return this->handle != INVALID_HANDLE_VALUE; }

	HANDLE handle;
	DWORD error = ERROR_SUCCESS;
};

LcdPanel queryLcd(const DeviceHandle& device) {
	LcdPanel panel;
	if (!device.valid()) {
		panel.error = device.error;
		return panel;
	}

	UCHAR levels[MAX_LCD_LEVELS] {}; // NOLINT
	DWORD bytes = 0;
	if (DeviceIoControl(
	        device.handle,
	        IOCTL_VIDEO_QUERY_SUPPORTED_BRIGHTNESS,
	        nullptr,
	        0,
	        levels,
	        sizeof(levels),
	        &bytes,
	        nullptr
	    ))
	{
		panel.levels.assign(levels, levels + std::min<DWORD>(bytes, MAX_LCD_LEVELS)); // NOLINT
	}

	DISPLAY_BRIGHTNESS state {};
	bytes = 0;
	if (!DeviceIoControl(
	        device.handle,
	        IOCTL_VIDEO_QUERY_DISPLAY_BRIGHTNESS,
	        nullptr,
	        0,
	        &state,
	        sizeof(state),
	        &bytes,
	        nullptr
	    ))
	{
		panel.error = GetLastError();
		return panel;
	}

	panel.usable = true;
	panel.current =
	    (state.ucDisplayPolicy & DISPLAYPOLICY_DC) != 0 ? state.ucDCBrightness : state.ucACBrightness;
	return panel;
}

LcdPanel queryLcd() {
	DeviceHandle device;
	return queryLcd(device);
}

bool lcdSet(int percent, int& outApplied, DWORD& outError) {
	DeviceHandle device;
	auto panel = queryLcd(device);
	if (!panel.usable) {
		outError = panel.error;
		return false;
	}

	auto level = std::clamp(percent, 0, 100);
	if (!panel.levels.empty()) {
		auto best = static_cast<int>(panel.levels.front());
		for (auto candidate: panel.levels) {
			if (std::abs(static_cast<int>(candidate) - level) < std::abs(best - level)) best = candidate;
		}
		level = best;
	}

	DISPLAY_BRIGHTNESS state {};
	state.ucDisplayPolicy = static_cast<UCHAR>(DISPLAYPOLICY_BOTH);
	state.ucACBrightness = static_cast<UCHAR>(level);
	state.ucDCBrightness = static_cast<UCHAR>(level);
	DWORD bytes = 0;
	if (!DeviceIoControl(
	        device.handle,
	        IOCTL_VIDEO_SET_DISPLAY_BRIGHTNESS,
	        &state,
	        sizeof(state),
	        nullptr,
	        0,
	        &bytes,
	        nullptr
	    ))
	{
		outError = GetLastError();
		return false;
	}

	auto after = queryLcd(device);
	if (after.usable && std::abs(after.current - level) > 1) {
		outError = ERROR_INVALID_DATA;
		return false;
	}

	outApplied = level;
	return true;
}

struct WmiMatch {
	const WmiPanel* panel = nullptr;
	// More than one WMI instance matches a display target. Both are device
	// paths, so the correlation cannot name one panel and must not guess.
	bool ambiguous = false;
};

WmiMatch matchingPanel(const Probe& probe) {
	WmiMatch match;
	for (const auto& target: probe.monitor.targets) {
		if (target.instanceId.isEmpty()) continue;
		for (const auto& panel: probe.wmi) {
			if (!instanceMatches(panel.instanceName, target.instanceId)) continue;
			if (match.panel != nullptr
			    && match.panel->instanceName.compare(panel.instanceName, Qt::CaseInsensitive) != 0)
			{
				return {nullptr, true};
			}
			match.panel = &panel;
		}
	}

	return match;
}

// The global \\.\LCD device provably addresses this screen's panel only when
// the screen is one active built-in target and it is the topology's only one.
bool isProvenBuiltIn(const MonitorDescription& monitor) {
	return monitor.kind == PanelKind::Internal && monitor.targets.size() == 1
	    && monitor.internalTargets == 1;
}

Probe runProbe(HMONITOR hMonitor, bool readDdc) {
	Probe probe;
	probe.monitor = describeMonitor(hMonitor);
	for (const auto& target: probe.monitor.targets) {
		if (target.hdrActive) return probe;
	}
	auto kind = probe.monitor.kind;

	if (kind == PanelKind::Internal || (kind == PanelKind::External && hasSystemBattery())) {
		probe.wmiAsked = true;
		probe.wmiResult =
		    withWmi([&probe](IWbemServices* services) { return listWmiPanels(services, probe.wmi); });
	}

	auto matched = matchingPanel(probe).panel != nullptr;

	if (!matched && isProvenBuiltIn(probe.monitor)) {
		probe.lcdAsked = true;
		probe.lcd = queryLcd();
	}

	if (!matched && readDdc && kind != PanelKind::Internal) {
		probe.ddcAsked = true;
		probe.ddcOk = ddcQuery(hMonitor, probe.ddcBrightness, probe.ddcError);
	}

	return probe;
}

struct Decision {
	BrightnessRoute route;
	bool available = false;
	qreal brightness = 1.0;
	QString reason;
};

Decision decide(const Probe& probe, const QSet<QString>& claimedWmi, bool lcdClaimed) {
	Decision decision;
	for (const auto& target: probe.monitor.targets) {
		if (!target.hdrActive) continue;
		decision.route.method = BrightnessMethod::SdrWhite;
		decision.route.adapterId = target.adapterId;
		decision.route.targetId = target.targetId;
		decision.available = target.whiteKnown && probe.monitor.targets.size() == 1;
		decision.brightness = target.whiteKnown ? sdrWhiteToSlider(target.whiteLevel) : 0;
		decision.reason =
		    decision.available
		        ? QStringLiteral("HDR color mode is active")
		        : QStringLiteral("SDR white is unreadable or the display has multiple targets");
		return decision;
	}

	if (probe.monitor.targets.empty()) {
		// No display target names a panel for this screen, so no method can be
		// chosen for it. A later probe after a display change can resolve it.
		decision.reason = QStringLiteral("no display target identifies its panel");
		return decision;
	}

	auto builtIn = isProvenBuiltIn(probe.monitor);
	decision.route.builtIn = builtIn;

	auto useWmi = [&decision](const WmiPanel& panel, const QString& reason) {
		decision.route.method = BrightnessMethod::Wmi;
		decision.route.wmiInstance = panel.instanceName;
		decision.route.wmiMethodsPath =
		    panel.methodsPath.isEmpty() ? methodsPathFor(panel.instanceName) : panel.methodsPath;
		decision.available = panel.current >= 0;
		decision.brightness = panel.current >= 0 ? std::clamp(panel.current / 100.0, 0.0, 1.0) : 1.0;
		decision.reason = reason;
	};

	auto match = matchingPanel(probe);
	if (match.panel != nullptr && !claimedWmi.contains(match.panel->instanceName.toUpper())) {
		useWmi(
		    *match.panel,
		    QStringLiteral("its WMI brightness instance matches the monitor's device path")
		);
		return decision;
	}

	if (builtIn && probe.lcd.usable && !lcdClaimed) {
		decision.route.method = BrightnessMethod::Lcd;
		decision.route.lcdTried = true;
		decision.available = probe.lcd.current >= 0;
		decision.brightness =
		    probe.lcd.current >= 0 ? std::clamp(probe.lcd.current / 100.0, 0.0, 1.0) : 1.0;
		decision.reason =
		    match.ambiguous
		        ? QStringLiteral(
		              "several WMI brightness instances match its device path, so none is guessed; it "
		              "is the only built-in panel and \\\\.\\LCD answers"
		          )
		        : QStringLiteral(
		              "no WMI brightness instance matches its device path; it is the only built-in panel "
		              "and \\\\.\\LCD answers"
		          );
		return decision;
	}

	if (probe.monitor.kind == PanelKind::Internal) {
		decision.route.method = BrightnessMethod::Software;
		decision.reason =
		    QStringLiteral(
		        "built-in panel, but %1 and \\\\.\\LCD can't be proven to address it; DDC/CI isn't "
		        "tried on built-in panels"
		    )
		        .arg(
		            match.ambiguous
		                ? QStringLiteral("several WMI brightness instances match its device path")
		                : QStringLiteral("no WMI brightness instance matches its device path")
		        );
		return decision;
	}

	decision.route.method = BrightnessMethod::Ddc;
	if (probe.ddcAsked && probe.ddcOk) {
		decision.available = true;
		decision.brightness = probe.ddcBrightness;
		decision.reason = QStringLiteral("it answers DDC/CI");
	} else if (probe.ddcAsked) {
		decision.reason =
		    QStringLiteral(
		        "DDC/CI didn't answer (%1); it's tried again on the first change, then software dimming"
		    )
		        .arg(win32Text(probe.ddcError));
	} else {
		decision.reason = QStringLiteral(
		    "not a built-in panel; DDC/CI is tried on the first change, then software dimming if the "
		    "monitor refuses"
		);
	}

	return decision;
}

const char* methodKey(BrightnessMethod method) {
	switch (method) {
	case BrightnessMethod::Wmi: return "wmi";
	case BrightnessMethod::Lcd: return "lcd";
	case BrightnessMethod::Ddc: return "ddc";
	case BrightnessMethod::SdrWhite: return "sdr-white";
	case BrightnessMethod::Software: return "software";
	default: return "";
	}
}

const char* methodLabel(BrightnessMethod method) {
	switch (method) {
	case BrightnessMethod::Wmi: return "WMI";
	case BrightnessMethod::Lcd: return "\\\\.\\LCD";
	case BrightnessMethod::Ddc: return "DDC/CI";
	case BrightnessMethod::SdrWhite: return "SDR white level";
	case BrightnessMethod::Software: return "software dimming";
	default: return "nothing";
	}
}

const char* methodDescription(BrightnessMethod method) {
	switch (method) {
	case BrightnessMethod::Wmi: return "the backlight through WMI";
	case BrightnessMethod::Lcd: return "the backlight through \\\\.\\LCD";
	case BrightnessMethod::Ddc: return "DDC/CI";
	case BrightnessMethod::SdrWhite: return "SDR white level";
	case BrightnessMethod::Software:
		return "software dimming (gamma ramp, the backlight stays as it is)";
	default: return "nothing";
	}
}

QString describeProbe(const QString& screenName, const Probe& probe, const Decision& decision) {
	QStringList parts;

	QStringList targets;
	auto targetIndex = 0;
	for (const auto& target: probe.monitor.targets) {
		targetIndex++;
		targets << QStringLiteral("target %1 over %2 [%3]")
		               .arg(
		                   QString::number(targetIndex),
		                   QLatin1StringView(technologyName(target.technology)),
		                   target.instanceId.isEmpty() ? QStringLiteral("no device path")
		                                               : qs::windows::logId(target.instanceId)
		               );
	}

	parts << QStringLiteral("%1, %2").arg(
	    QLatin1StringView(panelKindName(probe.monitor.kind)),
	    targets.isEmpty() ? QStringLiteral("no active display path")
	                      : targets.join(QStringLiteral(", "))
	);

	if (!probe.wmiAsked) {
		parts
		    << (probe.monitor.kind == PanelKind::Unknown
		            ? QStringLiteral("WMI not asked (no display target identifies this screen)")
		            : QStringLiteral("WMI not asked (external output on a machine without a battery)"));
	} else if (FAILED(probe.wmiResult)) {
		parts << QStringLiteral("WMI brightness query failed with %1").arg(hrText(probe.wmiResult));
	} else if (probe.wmi.empty()) {
		parts << QStringLiteral("WMI has no brightness instances");
	} else {
		QStringList panels;
		auto panelIndex = 0;
		for (const auto& panel: probe.wmi) {
			panelIndex++;
			panels << QStringLiteral("panel %1 (%2, %3%, %4 levels)")
			              .arg(
			                  QString::number(panelIndex),
			                  panel.active ? QStringLiteral("active") : QStringLiteral("inactive"),
			                  QString::number(panel.current),
			                  QString::number(panel.levels)
			              );
		}
		parts << QStringLiteral("WMI brightness instances: %1").arg(panels.join(QStringLiteral(", ")));
	}

	if (probe.lcdAsked) {
		parts
		    << (probe.lcd.usable
		            ? QStringLiteral("\\\\.\\LCD at %1% (%2 levels)")
		                  .arg(
		                      QString::number(probe.lcd.current),
		                      QString::number(probe.lcd.levels.size())
		                  )
		            : QStringLiteral("no \\\\.\\LCD (%1)").arg(win32Text(probe.lcd.error)));
	}

	if (probe.ddcAsked) {
		parts
		    << (probe.ddcOk ? QStringLiteral("DDC/CI at %1").arg(percentText(probe.ddcBrightness))
		                    : QStringLiteral("DDC/CI read failed (%1)").arg(win32Text(probe.ddcError)));
	}

	return QStringLiteral("%1: %2 -> %3, because %4")
	    .arg(
	        qs::windows::logId(screenName),
	        parts.join(QStringLiteral("; ")),
	        QLatin1StringView(methodDescription(decision.route.method)),
	        decision.reason
	    );
}

struct ScreenRef {
	QScreen* screen = nullptr;
	// Opaque target key: a persistent interface-path id or a session-local id.
	// All per-screen state is keyed by it.
	QString id;
	// Unique QML-facing name used in signals and logs.
	QString name;
};

ScreenRef resolveScreen(qs::windows::ScreenIdentityRegistry* registry, const QString& requested) {
	ScreenRef ref;
	ref.name = requested;
	ref.id = requested;
	if (registry == nullptr) return ref;
	auto* screen = registry->screenByName(requested);
	if (screen == nullptr) return ref;
	auto identity = registry->identity(screen);
	ref.screen = screen;
	if (!identity.uniqueName.isEmpty()) ref.name = identity.uniqueName;
	if (!identity.stableId.isEmpty()) ref.id = identity.stableId;
	return ref;
}

ScreenRef resolveId(
    qs::windows::ScreenIdentityRegistry* registry,
    const QString& id,
    const QString& fallbackName
) {
	ScreenRef ref;
	ref.id = id;
	ref.name = fallbackName;
	if (registry == nullptr) return ref;
	auto* screen = registry->screenById(id);
	if (screen == nullptr) return ref;
	auto identity = registry->identity(screen);
	ref.screen = screen;
	if (!identity.uniqueName.isEmpty()) ref.name = identity.uniqueName;
	return ref;
}

struct EventWatch {
	std::atomic<bool> started = false;
};

EventWatch& eventWatch() {
	static auto* watch = new EventWatch();
	return *watch;
}

QPointer<Brightness>& activeBrightness() {
	static auto* instance = new QPointer<Brightness>();
	return *instance;
}

void watchBrightnessEvents(const std::function<void(const QString&, int)>& deliver) {
	auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (FAILED(com)) {
		qCWarning(logBrightness).noquote()
		    << "Can't watch brightness changes: CoInitializeEx failed with" << hrText(com);
		return;
	}

	auto failures = 0;
	auto announced = false;

	while (failures < EVENT_ATTEMPTS) {
		ComPtr<IWbemServices> services;
		auto hr = connectWmi(services.GetAddressOf());

		ComPtr<IEnumWbemClassObject> events;
		if (SUCCEEDED(hr)) {
			Bstr language(L"WQL");
			Bstr query(L"SELECT * FROM WmiMonitorBrightnessEvent");
			hr = services->ExecNotificationQuery(
			    language.value,
			    query.value,
			    WBEM_FLAG_RETURN_IMMEDIATELY | WBEM_FLAG_FORWARD_ONLY,
			    nullptr,
			    events.GetAddressOf()
			);
			if (SUCCEEDED(hr) && !events) hr = E_FAIL;
		}

		if (SUCCEEDED(hr)) {
			secure(events.Get());
			if (!announced) {
				announced = true;
				qCInfo(logBrightness) << "Watching WmiMonitorBrightnessEvent for brightness changes made "
				                         "outside ii (Fn keys, Settings, adaptive brightness)";
			}

			while (true) {
				ComPtr<IWbemClassObject> event;
				ULONG returned = 0;
				hr = events->Next(WBEM_INFINITE, 1, event.GetAddressOf(), &returned);
				if (FAILED(hr) || returned == 0 || !event) break;

				failures = 0;
				auto instanceName = stringProperty(event.Get(), L"InstanceName");
				auto level = intProperty(event.Get(), L"Brightness", -1);
				if (!instanceName.isEmpty() && level >= 0) deliver(instanceName, level);
			}
		}

		failures++;
		qCWarning(logBrightness).noquote()
		    << "Brightness change events stopped or couldn't start:" << hrText(hr);
		if (failures < EVENT_ATTEMPTS) Sleep(EVENT_RETRY_MS * static_cast<DWORD>(failures));
	}

	qCWarning(logBrightness) << "Gave up on brightness change events; changes made outside ii show "
	                            "up the next time a sidebar opens";
	CoUninitialize();
}

class RealBrightnessBackend final: public BrightnessBackend {
public:
	Probe probeScreen(HMONITOR monitor, bool readDdc) override { return runProbe(monitor, readDdc); }

	bool writeBrightness(
	    const BrightnessRoute& route,
	    HMONITOR monitor,
	    qreal value,
	    QString& error
	) override {
		auto percent = static_cast<int>(std::lround(value * 100.0));
		// Recheck immediately before every hardware write. A missed display
		// notification must never send a stale DDC or SDR-white request.
		auto current = describeMonitor(monitor);
		if (current.targets.size() == 1 && current.targets[0].colorKnown
		    && current.targets[0].hdrActive != (route.method == BrightnessMethod::SdrWhite))
		{
			error = QStringLiteral("display color mode changed; refresh the brightness route");
			return false;
		}

		switch (route.method) {
		case BrightnessMethod::SdrWhite: {
			if (current.targets.size() != 1 || !current.targets[0].colorKnown
			    || !current.targets[0].hdrActive || current.targets[0].targetId != route.targetId
			    || current.targets[0].adapterId.HighPart != route.adapterId.HighPart
			    || current.targets[0].adapterId.LowPart != route.adapterId.LowPart)
			{
				error = QStringLiteral("display color mode changed; refresh the brightness route");
				return false;
			}
			auto result = setSdrWhite(route.adapterId, route.targetId, sliderToSdrWhite(value));
			if (result != ERROR_SUCCESS) {
				error =
				    QStringLiteral(
				        "SDR white route unavailable: Windows rejected the undocumented set request (%1)"
				    )
				        .arg(result);
				return false;
			}
			return true;
		}
		case BrightnessMethod::Wmi: {
			auto hr = withWmi([&route, percent](IWbemServices* services) {
				return writeWmiPanel(services, route.wmiMethodsPath, percent);
			});
			if (FAILED(hr)) {
				error = hrText(hr);
				return false;
			}

			return true;
		}
		case BrightnessMethod::Lcd: {
			DWORD code = ERROR_SUCCESS;
			auto applied = 0;
			if (!lcdSet(percent, applied, code)) {
				error = win32Text(code);
				return false;
			}

			return true;
		}
		case BrightnessMethod::Ddc: {
			if (!ddcSet(monitor, value, error)) return false;
			return true;
		}
		default: error = QStringLiteral("no brightness method"); return false;
		}
	}

	int readBrightness(const BrightnessRoute& route, QString& error) override {
		if (route.method == BrightnessMethod::SdrWhite) {
			UINT32 raw = 0;
			auto result = readSdrWhite(route.adapterId, route.targetId, raw);
			if (result != ERROR_SUCCESS) {
				error = QStringLiteral("SDR white read-back failed (%1)").arg(result);
				return -1;
			}
			return int(raw);
		}
		if (route.method == BrightnessMethod::Wmi) {
			auto percent = -1;
			auto hr = withWmi([&route, &percent](IWbemServices* services) {
				return readWmiPanel(services, route.wmiInstance, percent);
			});
			if (FAILED(hr)) {
				error = hrText(hr);
				return -1;
			}

			return percent;
		}

		auto panel = queryLcd();
		if (panel.usable) return panel.current;
		error = win32Text(panel.error);
		return -1;
	}

	[[nodiscard]] bool gammaDims(const QString& screenName) const override {
		return GammaController::instance()->dims(screenName);
	}

	bool gammaSetBrightness(const QString& screenName, qreal value) override {
		if (value < 1.0) {
			auto* screen = qs::windows::screenByName(screenName);
			auto description = describeMonitor(screen ? qs::windows::monitorForScreen(screen) : nullptr);
			for (const auto& target: description.targets) {
				if (target.advancedColor) return false;
			}
		}
		return GammaController::instance()->trySetBrightness(screenName, value);
	}

	void startEventWatch(const std::function<void(const QString&, int)>& deliver) override {
		std::thread([deliver]() { watchBrightnessEvents(deliver); }).detach();
	}
};

BrightnessBackend* realBrightnessBackend() {
	static auto* backend = new RealBrightnessBackend();
	return backend;
}

} // namespace

Brightness::Brightness(QObject* parent): Brightness(*realBrightnessBackend(), parent) {
	QCoreApplication::instance()->installNativeEventFilter(this);
	QObject::connect(
	    ScreenIdentityRegistry::instance(),
	    &ScreenIdentityRegistry::screenChanged,
	    this,
	    &Brightness::displayConfigurationChanged
	);
}

bool Brightness::nativeEventFilter(const QByteArray&, void* message, qintptr*) {
	auto* msg = static_cast<MSG*>(message);
	// A wallpaper switch is not a display configuration change. Ignore its
	// broadcast so it cannot trigger a brightness query.
	if (msg != nullptr && msg->message == WM_SETTINGCHANGE && msg->wParam == SPI_SETDESKWALLPAPER)
		return false;
	if (msg && (msg->message == WM_DISPLAYCHANGE || msg->message == WM_SETTINGCHANGE)) {
		// Debounce a burst of topology/color-mode messages.
		if (!this->property("displayChangePending").toBool()) {
			this->setProperty("displayChangePending", true);
			QTimer::singleShot(250, this, [this] {
				this->setProperty("displayChangePending", false);
				emit this->displayConfigurationChanged();
			});
		}
	}
	return false;
}

Brightness::Brightness(BrightnessBackend& backend, QObject* parent)
    : QObject(parent)
    , backend(&backend)
    , identities(
          qobject_cast<QGuiApplication*>(QCoreApplication::instance())
              ? ScreenIdentityRegistry::instance()
              : nullptr
      )
    , pool(new QThreadPool()) {
	this->pool->setMaxThreadCount(1);
	if (this->identities != nullptr) this->watchIdentities();
	activeBrightness() = this;
}

Brightness::Brightness(
    BrightnessBackend& backend,
    qs::windows::ScreenIdentityRegistry& identities,
    QObject* parent
)
    : QObject(parent)
    , backend(&backend)
    , identities(&identities)
    , pool(new QThreadPool()) {
	this->pool->setMaxThreadCount(1);
	this->watchIdentities();
	activeBrightness() = this;
}

void Brightness::watchIdentities() {
	connect(
	    this->identities,
	    &ScreenIdentityRegistry::identityRetired,
	    this,
	    [this](const QString& id, QScreen*) { this->retire(id); }
	);
}

Brightness::~Brightness() {
	if (QCoreApplication::instance()) QCoreApplication::instance()->removeNativeEventFilter(this);
	this->pool->clear();
	if (!this->pool->waitForDone(POOL_STOP_TIMEOUT_MS)) {
		qCWarning(logBrightness) << "A brightness call didn't finish in time";
		return;
	}

	delete this->pool;
}

[[nodiscard]] bool Brightness::strictRoutes() const { return this->strictRoutesEnabled; }

void Brightness::setStrictRoutes(bool strict) {
	if (this->strictRoutesEnabled == strict) return;
	if (!strict) {
		if (!this->strictRoutesDisableWarned) {
			this->strictRoutesDisableWarned = true;
			qCWarning(logBrightness) << "strictRoutes cannot be disabled once enabled";
		}
		return;
	}
	if (!this->inFlight.isEmpty() || !this->pending.isEmpty() || this->legacyWriteDepth != 0) {
		qCWarning(logBrightness) << "Cannot enable strictRoutes while brightness writes are active; "
		                            "retry when idle";
		return;
	}
	this->strictRoutesEnabled = strict;
	emit this->strictRoutesChanged();
}

void Brightness::query(const QString& screenName) { this->startProbe(screenName, true, 0); }

void Brightness::probe(const QString& screenName) { this->startProbe(screenName, false, 0); }

qint64 Brightness::queryRequest(const QString& screenName) {
	const auto requestId = nextQueryRequestId();
	this->startProbe(screenName, true, requestId);
	return requestId;
}

void Brightness::startProbe(const QString& screenName, bool readDdc, qint64 requestId) {
	if (!startup::settled()) {
		// The id travels with the deferred work: the gate may run it after
		// requests that were issued later and dispatched immediately.
		startup::afterFirstFrame(this, [this, screenName, readDdc, requestId]() {
			this->startProbe(screenName, readDdc, requestId);
		});
		return;
	}

	auto ref = resolveScreen(this->identities, screenName);
	auto hMonitor = ref.screen != nullptr ? qs::windows::monitorForScreen(ref.screen) : nullptr;
	auto key = ref.id;
	auto name = ref.name;

	auto guard = QPointer<Brightness>(this);
	auto* backend = this->backend;
	const auto lifetime = this->lifetimeOf(key);
	auto* task = QRunnable::create([guard, backend, key, name, hMonitor, readDdc, requestId, lifetime]() {
		auto probe = backend->probeScreen(hMonitor, readDdc);

		QMetaObject::invokeMethod(
		    QCoreApplication::instance(),
		    [guard, key, name, probe, requestId, lifetime]() {
			    auto* self = guard.data();
			    if (self == nullptr) return;
			    if (self->lifetimeOf(key) != lifetime) {
				    // The screen was removed while this probe ran. Report the request as
				    // unavailable, once, and publish nothing else.
				    if (requestId > 0) emit self->queryFinished(requestId, name, false, false, 1.0);
				    return;
			    }

			    auto previous = self->routes.take(key);

			    QSet<QString> present;
			    if (self->identities != nullptr) {
				    for (auto* screen: QGuiApplication::screens())
					    present.insert(self->identities->identity(screen).stableId);
			    }
			    self->routes.removeIf([&present](QHash<QString, BrightnessRoute>::iterator entry) {
				    return !present.contains(entry.key());
			    });

			    QSet<QString> claimedWmi;
			    auto lcdClaimed = false;
			    for (const auto& route: std::as_const(self->routes)) {
				    if (route.method == BrightnessMethod::Wmi)
					    claimedWmi.insert(route.wmiInstance.toUpper());
				    if (route.method == BrightnessMethod::Lcd) lcdClaimed = true;
			    }

			    auto decision = decide(probe, claimedWmi, lcdClaimed);
			    if (previous.method == BrightnessMethod::Software
			        && decision.route.method != BrightnessMethod::SdrWhite
			        && decision.route.method != BrightnessMethod::Software)
			    {
				    decision.route.method = BrightnessMethod::Software;
				    decision.available = false;
				    decision.brightness = 1.0;
				    decision.reason =
				        QStringLiteral("it already fell back to software dimming this session");
			    }

			    const auto currentName = self->nameFor(key, name);
			    qCInfo(logBrightness).noquote() << describeProbe(currentName, probe, decision);

			    if (previous.method == BrightnessMethod::Software
			        && decision.route.method == BrightnessMethod::Software
			        && self->lastValue.contains(key))
			    {
				    decision.available = true;
				    decision.brightness = self->lastValue.value(key);
			    } else if (!self->strictRoutesEnabled && self->backend->gammaDims(currentName)) {
				    qCInfo(logBrightness).noquote()
				        << qs::windows::logId(currentName)
				        << "drops the software dimming left from before, so the screen matches the slider";
				    self->backend->gammaSetBrightness(currentName, 1.0);
			    }

			    self->routes.insert(key, decision.route);
			    if (decision.available) self->lastValue.insert(key, decision.brightness);
			    else self->lastValue.remove(key);

			    if (decision.route.method == BrightnessMethod::Wmi) self->ensureEventWatch();

			    emit self->routeChanged(currentName, QLatin1StringView(methodKey(decision.route.method)));
			    if (!guard) return;

			    const auto isDdc = decision.route.method == BrightnessMethod::Ddc
			                    || decision.route.method == BrightnessMethod::Software;

			    // queryFinished() goes first: a queried() handler that destroys the
			    // controller must not suppress the correlated completion. Reaching
			    // here is the only way a request completes, so every outcome
			    // (resolved, unavailable, unknown screen, deferred by the startup
			    // gate) reports exactly once while the controller lives.
			    if (requestId > 0) {
				    emit self->queryFinished(
				        requestId,
				        currentName,
				        decision.available,
				        isDdc,
				        decision.brightness
				    );
				    if (!guard) return;
			    }

			    emit self->queried(currentName, decision.available, isDdc, decision.brightness);
		    },
		    Qt::QueuedConnection
		);
	});

	this->pool->start(task);
}

void Brightness::refresh(const QString& screenName) {
	auto ref = resolveScreen(this->identities, screenName);
	auto method = this->routes.value(ref.id).method;
	if (method != BrightnessMethod::Wmi && method != BrightnessMethod::Lcd) return;
	if (this->busy(ref.id)) return;

	this->startReadBack(ref.id, ref.name);
}

QString Brightness::methodFor(const QString& screenName) const {
	auto ref = resolveScreen(this->identities, screenName);
	return QLatin1StringView(methodKey(this->routes.value(ref.id).method));
}

void Brightness::setBrightness(const QString& screenName, bool isDdc, qreal value) {
	auto clamped = std::clamp(value, 0.0, 1.0);
	auto ref = resolveScreen(this->identities, screenName);
	auto key = ref.id;
	auto name = ref.name;

	if (this->strictRoutesEnabled) {
		this->strictSet(key, name, clamped);
		return;
	}

	// Synchronous legacy write callbacks must not change policy mid-dispatch.
	auto guard = QPointer<Brightness>(this);
	this->legacyWriteDepth++;
	const auto legacyWrite = qScopeGuard([guard] {
		if (guard) guard->legacyWriteDepth--;
	});
	auto& route = this->routes[key];
	if (route.method == BrightnessMethod::Unresolved) {
		route.method = BrightnessMethod::Ddc;
		qCInfo(logBrightness).noquote() << qs::windows::logId(name) << "was never probed; trying DDC/CI"
		                                << (isDdc ? "as asked" : "anyway");
	}

	this->lastValue.insert(key, clamped);
	this->generation[key]++;
	if (auto* timer = this->settleTimers.value(key)) timer->stop();

	if (route.method == BrightnessMethod::Software) {
		this->backend->gammaSetBrightness(name, clamped);
		emit this->brightnessSetFinished(name, true);
		return;
	}

	if (this->inFlight.contains(key)) {
		this->pending.insert(key, clamped);
		return;
	}

	this->startSet(key, name, clamped);
}

void Brightness::strictSet(const QString& key, const QString& name, qreal value) {
	auto guard = QPointer<Brightness>(this);
	auto route = this->routes.value(key);
	auto unresolved = route.method == BrightnessMethod::Unresolved;
	if (unresolved || this->inFlight.contains(key)) {
		emit this->brightnessSetFinished(name, false);
		if (!guard) return;
		emit this->brightnessWriteResult(
		    name,
		    value,
		    false,
		    unresolved ? QStringLiteral("unresolved") : QLatin1StringView(methodKey(route.method)),
		    unresolved ? QStringLiteral("no route resolved for this screen; query it before writing")
		               : QStringLiteral("a brightness write is already in flight for this screen")
		);
		return;
	}

	this->generation[key]++;
	if (auto* timer = this->settleTimers.value(key)) timer->stop();

	if (route.method == BrightnessMethod::Software) {
		auto ok = this->backend->gammaSetBrightness(name, value);
		if (ok) this->lastValue.insert(key, value);
		emit this->brightnessSetFinished(name, ok);
		if (!guard) return;
		emit this->brightnessWriteResult(
		    name,
		    value,
		    ok,
		    QLatin1StringView(methodKey(route.method)),
		    ok ? QString() : QStringLiteral("software gamma write failed")
		);
		return;
	}

	this->startSet(key, name, value, true);
}

void Brightness::startSet(const QString& key, const QString& name, qreal value, bool strict) {
	auto route = this->routes.value(key);
	if (route.method == BrightnessMethod::Software) {
		this->backend->gammaSetBrightness(name, value);
		emit this->brightnessSetFinished(name, true);
		return;
	}

	auto ref = resolveId(this->identities, key, name);
	auto hMonitor = ref.screen != nullptr ? qs::windows::monitorForScreen(ref.screen) : nullptr;

	this->inFlight.insert(key);

	auto guard = QPointer<Brightness>(this);
	auto* backend = this->backend;
	const auto lifetime = this->lifetimeOf(key);
	auto* task = QRunnable::create([guard, backend, key, name, route, hMonitor, value, strict, lifetime]() {
		QString error;
		auto ok = backend->writeBrightness(route, hMonitor, value, error);
		auto confirmed = value;
		if (!ok) {
			// One bounded mode recheck on refusal catches a toggle racing the
			// initial check without retrying a potentially wrong write.
			auto observed = backend->probeScreen(hMonitor, false);
			for (const auto& target: observed.monitor.targets) {
				if (target.colorKnown && target.hdrActive != (route.method == BrightnessMethod::SdrWhite)) {
					error = QStringLiteral("display color mode changed; refresh the brightness route");
					break;
				}
			}
		}
		if (ok && route.method == BrightnessMethod::SdrWhite) {
			auto read = backend->readBrightness(route, error);
			ok = read >= 0;
			if (ok) confirmed = sdrWhiteToSlider(UINT32(read));
		}

		QMetaObject::invokeMethod(
		    QCoreApplication::instance(),
		    [guard, key, name, method = route.method, value = confirmed, ok, error, strict, lifetime]() {
			    auto* self = guard.data();
			    if (self == nullptr) return;
			    self->finishSet(key, name, method, value, ok, error, strict, lifetime);
		    },
		    Qt::QueuedConnection
		);
	});

	this->pool->start(task);
}

void Brightness::finishSet(
    const QString& key,
    const QString& capturedName,
    BrightnessMethod method,
    qreal value,
    bool ok,
    const QString& error,
    bool strict,
    quint64 lifetime
) {
	auto guard = QPointer<Brightness>(this);
	if (this->lifetimeOf(key) != lifetime) {
		// The screen was removed while the write ran. Its busy state was dropped
		// with it, so this must not touch the state of a screen that came back with
		// the same id. The request is over and failed.
		emit this->brightnessSetFinished(capturedName, false);
		if (!guard || !strict) return;
		emit this->brightnessWriteResult(
		    capturedName,
		    value,
		    false,
		    QLatin1StringView(methodKey(method)),
		    QStringLiteral("the screen was removed during the write")
		);
		return;
	}
	auto name = this->nameFor(key, capturedName);
	this->inFlight.remove(key);

	if (strict) {
		// Finalize state before callbacks can submit another strict write.
		if (ok) {
			this->lastValue.insert(key, value);
			this->settle(key, name);
		}
		emit this->brightnessSetFinished(name, ok);
		if (!guard) return;
		emit this->brightnessWriteResult(
		    name,
		    value,
		    ok,
		    QLatin1StringView(methodKey(method)),
		    ok ? QString() : error
		);
		return;
	}
	if (method == BrightnessMethod::SdrWhite
	    || error.startsWith(QStringLiteral("display color mode changed;")))
	{
		emit this->brightnessSetFinished(name, ok);
		return; // Never fall back to gamma while advanced color is active.
	}

	// Fallback emits routeChanged before dispatching its next legacy write.
	// Refuse policy changes from handlers in that gap too.
	this->legacyWriteDepth++;
	const auto legacyWrite = qScopeGuard([guard] {
		if (guard) guard->legacyWriteDepth--;
	});
	auto latest = this->pending.contains(key) ? this->pending.take(key) : value;

	if (ok) {
		if (latest != value) {
			this->startSet(key, name, latest);
			return;
		}

		this->settle(key, name);
		emit this->brightnessSetFinished(name, true);
		return;
	}

	qCWarning(logBrightness).noquote() << QStringLiteral("%1: setting %2 with %3 failed: %4")
	                                          .arg(
	                                              qs::windows::logId(name),
	                                              percentText(value),
	                                              QLatin1StringView(methodLabel(method)),
	                                              error
	                                          );

	auto found = this->routes.find(key);
	if (found == this->routes.end()) return;

	auto& route = *found;
	if (route.method != method) {
		this->startSet(key, name, latest);
		return;
	}

	auto lcdFree = true;
	for (auto iter = this->routes.cbegin(); iter != this->routes.cend(); ++iter) {
		if (iter.key() != key && iter->method == BrightnessMethod::Lcd) lcdFree = false;
	}

	if (method == BrightnessMethod::Wmi && route.builtIn && !route.lcdTried && lcdFree) {
		route.lcdTried = true;
		route.method = BrightnessMethod::Lcd;
		qCInfo(logBrightness).noquote()
		    << qs::windows::logId(name) << "tries \\\\.\\LCD instead of WMI";
		emit this->routeChanged(name, QLatin1StringView(methodKey(route.method)));
		if (!guard) return;
		this->startSet(key, name, latest);
		return;
	}

	this->useSoftware(
	    key,
	    name,
	    latest,
	    QStringLiteral("%1 refused (%2)").arg(QLatin1StringView(methodLabel(method)), error)
	);
	if (!guard) return;
	emit this->brightnessSetFinished(name, true);
}

void Brightness::useSoftware(
    const QString& key,
    const QString& capturedName,
    qreal value,
    const QString& reason
) {
	auto guard = QPointer<Brightness>(this);
	auto name = this->nameFor(key, capturedName);
	auto& route = this->routes[key];
	route.method = BrightnessMethod::Software;

	qCWarning(logBrightness).noquote()
	    << QStringLiteral(
	           "%1: %2; dimming it in software through the gamma ramp from now on, the "
	           "backlight stays as it is"
	       )
	           .arg(qs::windows::logId(name), reason);

	emit this->routeChanged(name, QLatin1StringView(methodKey(route.method)));
	if (!guard) return;
	this->backend->gammaSetBrightness(name, value);
}

void Brightness::clearSoftwareDimming(const QString& screenName) {
	auto ref = resolveScreen(this->identities, screenName);
	if (!this->backend->gammaDims(ref.name)) return;

	if (this->backend->gammaSetBrightness(ref.name, 1.0)
	    && this->routes.value(ref.id).method == BrightnessMethod::Software)
	{
		this->lastValue.insert(ref.id, 1.0);
	}
}

void Brightness::settle(const QString& key, const QString& capturedName) {
	auto method = this->routes.value(key).method;
	if (method != BrightnessMethod::Wmi && method != BrightnessMethod::Lcd) return;

	auto* timer = this->settleTimers.value(key);
	if (timer == nullptr) {
		timer = new QTimer(this);
		timer->setSingleShot(true);
		timer->setInterval(SETTLE_MS);
		QObject::connect(timer, &QTimer::timeout, this, [this, key, capturedName]() {
			auto name = this->nameFor(key, capturedName);
			this->startReadBack(key, name);
		});
		this->settleTimers.insert(key, timer);
	}

	timer->start();
}

bool Brightness::busy(const QString& key) const {
	if (this->inFlight.contains(key) || this->pending.contains(key)) return true;

	auto* timer = this->settleTimers.value(key);
	return timer != nullptr && timer->isActive();
}

void Brightness::startReadBack(const QString& key, const QString& capturedName) {
	auto route = this->routes.value(key);
	if (route.method != BrightnessMethod::Wmi && route.method != BrightnessMethod::Lcd) return;
	if (this->inFlight.contains(key) || this->pending.contains(key)) return;

	auto expected = this->generation.value(key);
	auto guard = QPointer<Brightness>(this);
	auto* backend = this->backend;
	const auto lifetime = this->lifetimeOf(key);
	auto* task = QRunnable::create([guard, backend, key, capturedName, route, expected, lifetime]() {
		QString error;
		auto percent = backend->readBrightness(route, error);

		QMetaObject::invokeMethod(
		    QCoreApplication::instance(),
		    [guard, key, capturedName, method = route.method, expected, percent, error, lifetime]() {
			    auto* self = guard.data();
			    if (self == nullptr) return;
			    // Removed while reading: nothing is published for a screen that is gone.
			    if (self->lifetimeOf(key) != lifetime) return;
			    auto name = self->nameFor(key, capturedName);

			    if (percent < 0) {
				    qCWarning(logBrightness).noquote()
				        << QStringLiteral("%1: reading the brightness back with %2 failed: %3")
				               .arg(
				                   qs::windows::logId(name),
				                   QLatin1StringView(methodLabel(method)),
				                   error
				               );
				    return;
			    }

			    if (self->generation.value(key) != expected) return;
			    if (self->routes.value(key).method != method) return;
			    if (self->inFlight.contains(key) || self->pending.contains(key)) return;

			    auto value = std::clamp(percent / 100.0, 0.0, 1.0);
			    auto previous = self->lastValue.value(key, -1.0);
			    self->lastValue.insert(key, value);
			    if (previous >= 0.0 && std::abs(previous - value) <= SAME_VALUE) return;

			    qCDebug(logBrightness).noquote()
			        << qs::windows::logId(name) << "reads back at" << percentText(value);
			    emit self->systemBrightnessChanged(name, value);
		    },
		    Qt::QueuedConnection
		);
	});

	this->pool->start(task);
}

void Brightness::onSystemBrightness(const QString& instanceName, int percent) {
	QString key;
	for (auto iter = this->routes.cbegin(); iter != this->routes.cend(); ++iter) {
		if (iter->method == BrightnessMethod::Wmi
		    && iter->wmiInstance.compare(instanceName, Qt::CaseInsensitive) == 0)
		{
			key = iter.key();
			break;
		}
	}

	if (key.isEmpty() || this->busy(key)) return;
	auto name = this->nameFor(key, key);

	auto value = std::clamp(percent / 100.0, 0.0, 1.0);
	auto previous = this->lastValue.value(key, -1.0);
	if (previous >= 0.0 && std::abs(previous - value) <= SAME_VALUE) return;

	this->lastValue.insert(key, value);
	qCDebug(logBrightness).noquote()
	    << qs::windows::logId(name) << "changed outside ii to" << percentText(value);
	emit this->systemBrightnessChanged(name, value);
}

QString Brightness::nameFor(const QString& key, const QString& fallback) const {
	if (this->identities != nullptr) {
		if (auto* screen = this->identities->screenById(key)) {
			auto name = this->identities->identity(screen).uniqueName;
			if (!name.isEmpty()) return name;
		}
	}
	return fallback;
}

void Brightness::retire(const QString& key) {
	// The screen left, or has another identity now, so this id's route, values
	// and outstanding work are gone with it. A worker that is still running
	// finds a different lifetime when it completes and is dropped; it can then
	// neither clear the busy state of a later screen with the same id nor
	// publish for a screen that is no longer there.
	++this->lifetimes[key];
	this->routes.remove(key);
	this->pending.remove(key);
	this->lastValue.remove(key);
	this->generation.remove(key);
	this->inFlight.remove(key);
	if (auto* timer = this->settleTimers.take(key)) {
		timer->stop();
		timer->deleteLater();
	}
}

quint64 Brightness::lifetimeOf(const QString& key) const { return this->lifetimes.value(key, 0); }

void Brightness::ensureEventWatch() {
	auto& watch = eventWatch();
	if (watch.started.exchange(true)) return;

	this->backend->startEventWatch([](const QString& instanceName, int percent) {
		auto* app = QCoreApplication::instance();
		if (app == nullptr) return;

		QMetaObject::invokeMethod(
		    app,
		    [instanceName, percent]() {
			    auto* self = activeBrightness().data();
			    if (self != nullptr) self->onSystemBrightness(instanceName, percent);
		    },
		    Qt::QueuedConnection
		);
	});
}

} // namespace qs::windows::sys

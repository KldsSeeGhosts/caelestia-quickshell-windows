#include "native_desktop_accessor.hpp"

#include <objectarray.h>
#include <qloggingcategory.h>
#include <qpointer.h>
#include <servprov.h>
#include <shobjidl_core.h>
#include <wrl/client.h>

#include "desktop_recovery.hpp"
#include "native_desktop_interfaces.hpp"
#include "util.hpp"

namespace qs::windows {
namespace {
Q_LOGGING_CATEGORY(logNativeDesktops, "quickshell.windows.desktops.native", QtWarningMsg);
using Microsoft::WRL::ComPtr;

using namespace native_desktop;

constexpr GUID immersiveShell =
    {0xc2f03a33, 0x21f5, 0x47fa, {0xb4, 0xbb, 0x15, 0x63, 0x62, 0xa2, 0xf2, 0x39}};
constexpr GUID desktopService =
    {0xc5e0cdca, 0x7b6e, 0x41b2, {0x9f, 0xc4, 0xd9, 0x39, 0x75, 0xcc, 0x46, 0x7b}};
constexpr GUID publicManager =
    {0xaa509086, 0x5ca9, 0x4c25, {0x8f, 0x95, 0x58, 0x9d, 0x3c, 0x07, 0xb4, 0x8a}};

struct Provider {
#ifdef QS_TEST
	NativeDesktopFactory factory;
#endif
	ComPtr<DesktopManager> manager;
	ComPtr<ViewCollection> views;
	ComPtr<IVirtualDesktopManager> publicApi;
	QList<ComPtr<Desktop>> snapshot;
	QList<GUID> ids;
	int current = -1;
	bool supported = false;
	DesktopAvailability availability = DesktopAvailability::MissingProvider;
	HRESULT lastError = S_OK;
	QPointer<QObject> context;
	std::function<void()> recovered;
	DesktopRecovery recovery {
	    [this] { return this->acquire(); },
	    [this] {
		    if (this->context && this->recovered) this->recovered();
	    }
	};

	void drop() {
		this->snapshot.clear();
		this->ids.clear();
		this->current = -1;
		this->publicApi.Reset();
		this->views.Reset();
		this->manager.Reset();
	}

	bool check(HRESULT hr, const char* operation) {
		if (SUCCEEDED(hr)) return true;
		this->lastError = hr;
		qCWarning(logNativeDesktops) << operation << "refused, HRESULT" << Qt::hex << hr;
		if (hr == RPC_E_DISCONNECTED || hr == CO_E_OBJNOTCONNECTED
		    || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE))
		{
			this->drop();
			this->availability = DesktopAvailability::Disconnected;
			this->recovery.disconnected();
		}
		return false;
	}

	bool acquire() {
		this->drop();
		this->lastError = S_OK;
		ComPtr<IServiceProvider> shell;
		bool ok;
#ifdef QS_TEST
		if (this->factory) {
			ok = this->check(
			    this->factory(
			        this->manager.GetAddressOf(),
			        this->views.GetAddressOf(),
			        this->publicApi.GetAddressOf()
			    ),
			    "Test desktop acquisition"
			);
		} else
#endif
		{
			ok = this->check(
			         CoCreateInstance(immersiveShell, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&shell)),
			         "ImmersiveShell"
			     )
			  && this->check(
			      shell->QueryService(desktopService, IID_PPV_ARGS(&this->manager)),
			      "DesktopManager 24H2 GUID"
			  )
			  && this->check(
			      shell->QueryService(__uuidof(ViewCollection), IID_PPV_ARGS(&this->views)),
			      "ApplicationViewCollection GUID"
			  )
			  && this->check(
			      CoCreateInstance(
			          publicManager,
			          nullptr,
			          CLSCTX_INPROC_SERVER,
			          IID_PPV_ARGS(&this->publicApi)
			      ),
			      "PublicDesktopManager"
			  );
		}
		ok = ok && this->refresh();
		if (!ok) {
			this->drop();
			if (this->lastError == E_ACCESSDENIED) {
				this->availability = DesktopAvailability::PermissionDenied;
			} else if (this->lastError == RPC_E_DISCONNECTED || this->lastError == CO_E_OBJNOTCONNECTED
			           || this->lastError == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE))
			{
				this->availability = DesktopAvailability::Disconnected;
			} else {
				this->availability = DesktopAvailability::MissingProvider;
			}
		} else {
			this->availability = DesktopAvailability::Available;
		}
		return ok;
	}

	bool refresh() {
		if (!this->manager) return false;
		ComPtr<IObjectArray> array;
		UINT count = 0;
		if (!this->check(this->manager->GetDesktops(&array), "GetDesktops")
		    || !this->check(array->GetCount(&count), "GetDesktopCount"))
			return false;
		QList<ComPtr<Desktop>> desktops;
		QList<GUID> newIds;
		if (count == 0) {
			this->snapshot.clear();
			this->ids.clear();
			this->current = -1;
			return true;
		}
		for (UINT i = 0; i < count; ++i) {
			ComPtr<Desktop> desktop;
			GUID id {};
			if (!this->check(array->GetAt(i, IID_PPV_ARGS(&desktop)), "GetDesktop")
			    || !this->check(desktop->GetId(&id), "GetDesktopId"))
				return false;
			desktops.append(desktop);
			newIds.append(id);
		}
		ComPtr<Desktop> active;
		GUID activeId {};
		if (!this->check(this->manager->GetCurrentDesktop(&active), "GetCurrentDesktop")
		    || !this->check(active->GetId(&activeId), "GetCurrentDesktopId"))
			return false;
		int activeIndex = -1;
		for (qsizetype i = 0; i < newIds.size(); ++i) {
			if (IsEqualGUID(newIds[i], activeId)) activeIndex = static_cast<int>(i);
		}
		if (activeIndex < 0) return false;
		this->snapshot = std::move(desktops);
		this->ids = std::move(newIds);
		this->current = activeIndex;
		return true;
	}

	ComPtr<Desktop> desktop(int index) {
		if (!this->manager || index < 0 || index >= this->snapshot.size()) return {};
		return this->snapshot[index];
	}
};

Provider& provider() {
	static Provider instance;
	return instance;
}

int desktopCount() {
	auto& p = provider();
	if (p.refresh()) return static_cast<int>(p.ids.size());
	if (p.availability != DesktopAvailability::Disconnected) {
		p.availability = p.lastError == E_ACCESSDENIED
		    ? DesktopAvailability::PermissionDenied : DesktopAvailability::MissingProvider;
		p.drop();
		p.recovery.disconnected();
	}
	return -1;
}

GUID desktopId(int index) {
	auto& ids = provider().ids;
	return index >= 0 && index < ids.size() ? ids[index] : GUID {};
}

int indexOf(const GUID& id) {
	if (IsEqualGUID(id, GUID {})) return -1;
	auto& ids = provider().ids;
	for (qsizetype i = 0; i < ids.size(); ++i) {
		if (IsEqualGUID(id, ids[i])) return static_cast<int>(i);
	}
	return -1;
}

int currentDesktop() { return provider().current; }

int windowDesktop(HWND window) {
	GUID id {};
	auto& p = provider();
	if (!p.manager || !p.check(p.publicApi->GetWindowDesktopId(window, &id), "GetWindowDesktopId"))
		return -1;
	return indexOf(id);
}

int onCurrent(HWND window) {
	BOOL current = FALSE;
	auto& p = provider();
	return p.manager
	            && p.check(
	                p.publicApi->IsWindowOnCurrentVirtualDesktop(window, &current),
	                "IsWindowOnCurrentDesktop"
	            )
	         ? static_cast<int>(current)
	         : -1;
}

int switchDesktop(int index) {
	auto& p = provider();
	auto desktop = p.desktop(index);
	return desktop && p.check(p.manager->SwitchDesktop(desktop.Get()), "SwitchDesktop") ? 0 : -1;
}

int createDesktop() {
	auto& p = provider();
	ComPtr<Desktop> desktop;
	if (!p.manager || !p.check(p.manager->CreateDesktop(&desktop), "CreateDesktop")) return -1;
	return desktopCount() - 1;
}

int moveWindow(HWND window, int index) {
	auto& p = provider();
	auto desktop = p.desktop(index);
	if (!desktop || !IsWindow(window)) return -1;
	ApplicationView* view = nullptr;
	if (!p.check(p.views->GetViewForHwnd(window, &view), "GetViewForHwnd") || !view) return -1;
	// The returned IApplicationView begins with IUnknown, even though its full
	// interface derives from IInspectable.
	auto* unknown = reinterpret_cast<IUnknown*>(view);
	BOOL movable = FALSE;
	auto ok = p.check(p.manager->CanViewMoveDesktops(view, &movable), "CanViewMoveDesktops")
	       && movable
	       && p.check(p.manager->MoveViewToDesktop(view, desktop.Get()), "MoveViewToDesktop");
	unknown->Release();
	return ok ? 0 : -1;
}
} // namespace

bool loadNativeDesktopAccessor(
    VirtualDesktops::Accessor& accessor,
    QObject* context,
    std::function<void()> recovered
) {
	auto& p = provider();
	const auto build = windowsBuild();
	// These build families share the 24H2 ABI. Never guess a vtable on another
	// build, or try unrelated interface GUIDs after QueryService refuses it.
	if (build != 26100 && build != 26200 && build != 26300) {
		qCWarning(logNativeDesktops) << "Desktop dispatch refused on unrecognized Windows build"
		                             << build;
		p.availability = DesktopAvailability::UnsupportedBuild;
		return false;
	}
	p.supported = true;
	p.context = context;
	p.recovered = std::move(recovered);
	accessor.getDesktopCount = &desktopCount;
	accessor.getCurrentDesktopNumber = &currentDesktop;
	accessor.getDesktopIdByNumber = &desktopId;
	accessor.getWindowDesktopNumber = &windowDesktop;
	accessor.isWindowOnCurrentVirtualDesktop = &onCurrent;
	accessor.goToDesktopNumber = &switchDesktop;
	accessor.moveWindowToDesktopNumber = &moveWindow;
	accessor.createDesktop = &createDesktop;
	// Removal, pinning and notification hooks intentionally remain absent.
	if (p.acquire()) return true;
	p.recovery.disconnected();
	return false;
}

bool retryNativeDesktopAccessor() {
	auto& p = provider();
	if (!p.supported) return false;
	p.recovery.retryOnDispatch();
	return p.availability == DesktopAvailability::Available && p.manager && !p.recovery.active();
}

void invalidateNativeDesktopAccessor() {
	auto& p = provider();
	if (!p.supported) return;
	p.drop();
	p.availability = DesktopAvailability::Disconnected;
	p.recovery.disconnected();
}

DesktopAvailability nativeDesktopAvailability() { return provider().availability; }
HRESULT nativeDesktopLastError() { return provider().lastError; }

#ifdef QS_TEST
void setNativeDesktopFactoryForTest(NativeDesktopFactory factory) {
	auto& p = provider();
	p.drop();
	p.recovery.reset();
	p.context = nullptr;
	p.recovered = {};
	p.supported = false;
	p.availability = DesktopAvailability::MissingProvider;
	p.lastError = S_OK;
	p.factory = std::move(factory);
}
#endif

} // namespace qs::windows

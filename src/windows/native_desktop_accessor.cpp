#include "native_desktop_accessor.hpp"
#include <utility>

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

// Catch at the COM boundary, not around the whole accessor operation: an SEH
// unwind under /EHsc would otherwise skip local ComPtr destructors.
template <typename F>
bool sehCall(F&& call, HRESULT& result) {
	__try
	{
		result = call();
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

template <typename T>
bool release(ComPtr<T>& pointer) {
	auto* detached = pointer.Detach();
	if (!detached) return true;
	HRESULT ignored = S_OK;
	// Detach first, so even a broken Release is attempted only once and cannot
	// leave a stale interface in the provider.
	return sehCall(
	    [detached] {
		    detached->Release();
		    return S_OK;
	    },
	    ignored
	);
}

template <typename T>
bool release(QList<ComPtr<T>>& pointers) {
	bool ok = true;
	for (auto& pointer: pointers) ok = release(pointer) && ok;
	pointers.clear();
	return ok;
}

struct Provider {
#ifdef QS_TEST
	NativeDesktopFactory factory;
	std::function<bool(HWND)> windowValidator;
#endif
	ComPtr<DesktopManager> manager;
	ComPtr<ViewCollection> views;
	ComPtr<IVirtualDesktopManager> publicApi;
	QList<ComPtr<Desktop>> snapshot;
	QList<GUID> ids;
	int current = -1;
	bool supported = false;
	bool structuredException = false;
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
		release(this->snapshot);
		this->ids.clear();
		this->current = -1;
		release(this->publicApi);
		release(this->views);
		release(this->manager);
	}

	void exception(const char* operation) {
		qCWarning(logNativeDesktops) << operation << "raised a structured exception";
		this->structuredException = true;
		this->lastError = E_UNEXPECTED;
		this->availability = DesktopAvailability::Disconnected;
		this->drop();
		this->recovery.disconnected();
	}

	template <typename T>
	bool releaseLocal(T& pointer) {
		if (release(pointer)) return true;
		this->exception("Local COM Release");
		return false;
	}

	template <typename F>
	bool invoke(F&& call, const char* operation) {
		HRESULT hr = S_OK;
		if (sehCall(std::forward<F>(call), hr)) return this->check(hr, operation);
		this->exception(operation);
		return false;
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
		this->structuredException = false;
		ComPtr<IServiceProvider> shell;
		bool ok;
#ifdef QS_TEST
		if (this->factory) {
			ok = this->invoke(
			    [&] {
				    return this->factory(
				        this->manager.GetAddressOf(),
				        this->views.GetAddressOf(),
				        this->publicApi.GetAddressOf()
				    );
			    },
			    "Test desktop acquisition"
			);
		} else
#endif
		{
			ok = this->invoke(
			         [&] {
				         return CoCreateInstance(
				             immersiveShell,
				             nullptr,
				             CLSCTX_LOCAL_SERVER,
				             IID_PPV_ARGS(&shell)
				         );
			         },
			         "ImmersiveShell"
			     )
			  && this->invoke(
			      [&] { return shell->QueryService(desktopService, IID_PPV_ARGS(&this->manager)); },
			      "DesktopManager 24H2 GUID"
			  )
			  && this->invoke(
			      [&] {
				      return shell->QueryService(__uuidof(ViewCollection), IID_PPV_ARGS(&this->views));
			      },
			      "ApplicationViewCollection GUID"
			  )
			  && this->invoke(
			      [&] {
				      return CoCreateInstance(
				          publicManager,
				          nullptr,
				          CLSCTX_INPROC_SERVER,
				          IID_PPV_ARGS(&this->publicApi)
				      );
			      },
			      "PublicDesktopManager"
			  );
		}
		ok = ok && this->refresh();
		// Finish local cleanup before publishing acquisition success, including
		// timer-driven acquisitions which have no outer facade SEH guard.
		ok = this->releaseLocal(shell) && ok;
		if (!ok) {
			this->drop();
			if (this->lastError == E_ACCESSDENIED) {
				this->availability = DesktopAvailability::PermissionDenied;
			} else if (this->structuredException || this->lastError == RPC_E_DISCONNECTED
			           || this->lastError == CO_E_OBJNOTCONNECTED
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
		QList<ComPtr<Desktop>> desktops;
		ComPtr<Desktop> desktop, active;
		QList<GUID> newIds;
		// Keep every local COM owner outside this body, so every early return
		// reaches guarded cleanup before an operation can report success.
		bool ok = [&] {
			UINT count = 0;
			if (!this->invoke([&] { return this->manager->GetDesktops(&array); }, "GetDesktops")
			    || !this->invoke([&] { return array->GetCount(&count); }, "GetDesktopCount"))
				return false;
			for (UINT i = 0; i < count; ++i) {
				GUID id {};
				if (!this->invoke([&] { return array->GetAt(i, IID_PPV_ARGS(&desktop)); }, "GetDesktop")
				    || !this->invoke([&] { return desktop->GetId(&id); }, "GetDesktopId"))
					return false;
				desktops.append(std::move(desktop));
				newIds.append(id);
			}
			int activeIndex = -1;
			if (count != 0) {
				GUID activeId {};
				if (!this->invoke(
				        [&] { return this->manager->GetCurrentDesktop(&active); },
				        "GetCurrentDesktop"
				    )
				    || !this->invoke([&] { return active->GetId(&activeId); }, "GetCurrentDesktopId"))
					return false;
				for (qsizetype i = 0; i < newIds.size(); ++i) {
					if (IsEqualGUID(newIds[i], activeId)) activeIndex = static_cast<int>(i);
				}
				if (activeIndex < 0) return false;
			}
			if (!this->releaseLocal(this->snapshot)) return false;
			this->snapshot = std::move(desktops);
			this->ids = std::move(newIds);
			this->current = activeIndex;
			return true;
		}();
		ok = this->releaseLocal(active) && ok;
		ok = this->releaseLocal(desktop) && ok;
		ok = this->releaseLocal(desktops) && ok;
		ok = this->releaseLocal(array) && ok;
		return ok;
	}

	bool validWindow(HWND window) {
#ifdef QS_TEST
		if (this->windowValidator) return this->windowValidator(window);
#endif
		return IsWindow(window);
	}

	Desktop* desktop(int index) {
		if (!this->manager || index < 0 || index >= this->snapshot.size()) return nullptr;
		return this->snapshot[index].Get();
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
		p.availability = p.lastError == E_ACCESSDENIED ? DesktopAvailability::PermissionDenied
		                                               : DesktopAvailability::MissingProvider;
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
	if (!p.manager
	    || !p.invoke(
	        [&] { return p.publicApi->GetWindowDesktopId(window, &id); },
	        "GetWindowDesktopId"
	    ))
		return -1;
	return indexOf(id);
}

int onCurrent(HWND window) {
	BOOL current = FALSE;
	auto& p = provider();
	return p.manager
	            && p.invoke(
	                [&] { return p.publicApi->IsWindowOnCurrentVirtualDesktop(window, &current); },
	                "IsWindowOnCurrentDesktop"
	            )
	         ? static_cast<int>(current)
	         : -1;
}

int switchDesktop(int index) {
	auto& p = provider();
	auto desktop = p.desktop(index);
	return desktop && p.invoke([&] { return p.manager->SwitchDesktop(desktop); }, "SwitchDesktop")
	         ? 0
	         : -1;
}

int createDesktop() {
	auto& p = provider();
	ComPtr<Desktop> desktop;
	const auto ok =
	    p.manager && p.invoke([&] { return p.manager->CreateDesktop(&desktop); }, "CreateDesktop");
	if (!p.releaseLocal(desktop) || !ok) return -1;
	return desktopCount() - 1;
}

int moveWindow(HWND window, int index) {
	auto& p = provider();
	auto desktop = p.desktop(index);
	if (!desktop || !p.validWindow(window)) return -1;
	ApplicationView* view = nullptr;
	const auto gotView =
	    p.invoke([&] { return p.views->GetViewForHwnd(window, &view); }, "GetViewForHwnd");
	// Failed calls do not transfer ownership of a potentially half-written out
	// pointer, including a structured exception in GetViewForHwnd.
	if (!gotView || !view) return -1;
	// The returned IApplicationView begins with IUnknown, even though its full
	// interface derives from IInspectable.
	ComPtr<IUnknown> unknown;
	unknown.Attach(reinterpret_cast<IUnknown*>(view));
	BOOL movable = FALSE;
	auto ok =
	    p.invoke(
	        [&] { return p.manager->CanViewMoveDesktops(view, &movable); },
	        "CanViewMoveDesktops"
	    )
	    && movable
	    && p.invoke([&] { return p.manager->MoveViewToDesktop(view, desktop); }, "MoveViewToDesktop");
	return p.releaseLocal(unknown) && ok ? 0 : -1;
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

void invalidateNativeDesktopAccessor(HRESULT error) {
	auto& p = provider();
	if (!p.supported) return;
	if (FAILED(error)) p.lastError = error;
	p.availability = DesktopAvailability::Disconnected;
	p.drop();
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
	p.windowValidator = {};
}

void setNativeDesktopWindowValidatorForTest(std::function<bool(HWND)> validator) {
	provider().windowValidator = std::move(validator);
}
#endif

} // namespace qs::windows

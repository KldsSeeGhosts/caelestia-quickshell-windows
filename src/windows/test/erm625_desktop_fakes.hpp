#pragma once

#include <memory>

#include <objectarray.h>
#include <qlist.h>
#include <servprov.h>

#include "native_desktop_interfaces.hpp"

// Synthetic, stack-owned COM fixtures. Release counts ownership without
// deleting the fixture, allowing assertions after the provider drops it.
namespace erm625 {
using namespace qs::windows::native_desktop;

inline void fault() { RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr); }

// A fake COM call blocks only its worker. Tests release it before destroying
// stack-owned fake interfaces. No Explorer object or window is involved.
struct BlockingCall {
	HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	HANDLE resume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	~BlockingCall() {
		CloseHandle(this->entered);
		CloseHandle(this->resume);
	}
	void operator()() const {
		SetEvent(this->entered);
		WaitForSingleObject(this->resume, INFINITE);
	}
	void release() const { SetEvent(this->resume); }
};
struct Unblock {
	std::shared_ptr<BlockingCall> call;
	~Unblock() { this->call->release(); }
};

template <typename Interface>
struct FakeCom: Interface {
	ULONG refs = 1;
	int releases = 0;
	bool faultOnRelease = false;
	std::function<void()> beforeRelease;
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
		*out = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override {
		return static_cast<ULONG>(InterlockedIncrement(reinterpret_cast<volatile LONG*>(&this->refs)));
	}
	ULONG STDMETHODCALLTYPE Release() override {
		if (this->beforeRelease) this->beforeRelease();
		InterlockedIncrement(reinterpret_cast<volatile LONG*>(&this->releases));
		const auto remaining =
		    static_cast<ULONG>(InterlockedDecrement(reinterpret_cast<volatile LONG*>(&this->refs)));
		if (this->faultOnRelease) fault();
		return remaining;
	}
};
struct FakeView: FakeCom<IUnknown> {};
struct FakeDesktop: FakeCom<Desktop> {
	GUID id {};
	bool faultOnId = false;
	explicit FakeDesktop(ULONG value) { this->id.Data1 = value; }
	HRESULT STDMETHODCALLTYPE IsViewVisible(ApplicationView*, BOOL*) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetId(GUID* out) override {
		if (this->faultOnId) fault();
		*out = this->id;
		return S_OK;
	}
};
struct FakeArray: FakeCom<IObjectArray> {
	QList<FakeDesktop*> desktops;
	HRESULT STDMETHODCALLTYPE GetCount(UINT* out) override {
		*out = static_cast<UINT>(this->desktops.size());
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetAt(UINT index, REFIID, void** out) override {
		if (index >= this->desktops.size()) return E_INVALIDARG;
		auto* desktop = this->desktops[index];
		desktop->AddRef();
		*out = desktop;
		return S_OK;
	}
};
struct FakeManager: FakeCom<DesktopManager> {
	FakeArray array;
	HRESULT result = S_OK;
	int enumerations = 0;
	int switches = 0;
	int creates = 0;
	bool faultOnEnumeration = false;
	bool faultOnSwitch = false;
	bool faultOnCreate = false;
	bool faultOnCurrent = false;
	bool faultOnCanMove = false;
	bool faultOnMove = false;
	bool canMove = true;
	int canMoves = 0;
	int moves = 0;
	HRESULT moveResult = S_OK;
	HRESULT switchResult = S_OK;
	bool publishSwitch = false;
	bool publishCreation = false;
	ApplicationView* expectedView = nullptr;
	FakeDesktop* current = nullptr;
	FakeDesktop* created = nullptr;
	FakeDesktop* lastMovedDesktop = nullptr;
	std::function<void()> beforeEnumeration, beforeSwitch, beforeCreate, beforeCanMove, beforeMove;
	HRESULT STDMETHODCALLTYPE GetCount(UINT* out) override { return this->array.GetCount(out); }
	HRESULT STDMETHODCALLTYPE MoveViewToDesktop(ApplicationView* view, Desktop* desktop) override {
		if (this->beforeMove) this->beforeMove();
		++this->moves;
		this->lastMovedDesktop = static_cast<FakeDesktop*>(desktop);
		if (this->faultOnMove) fault();
		return view == this->expectedView ? this->moveResult : E_INVALIDARG;
	}
	HRESULT STDMETHODCALLTYPE CanViewMoveDesktops(ApplicationView* view, BOOL* out) override {
		if (this->beforeCanMove) this->beforeCanMove();
		++this->canMoves;
		if (this->faultOnCanMove) fault();
		if (view != this->expectedView) return E_INVALIDARG;
		*out = this->canMove ? TRUE : FALSE;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetCurrentDesktop(Desktop** out) override {
		this->current->AddRef();
		*out = this->current;
		if (this->faultOnCurrent) fault();
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetDesktops(IObjectArray** out) override {
		if (this->beforeEnumeration) this->beforeEnumeration();
		++this->enumerations;
		if (this->faultOnEnumeration) fault();
		if (FAILED(this->result) || this->result == RPC_S_SERVER_UNAVAILABLE) return this->result;
		this->array.AddRef();
		*out = &this->array;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetAdjacentDesktop(Desktop*, int, Desktop**) override {
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE SwitchDesktop(Desktop* desktop) override {
		if (this->beforeSwitch) this->beforeSwitch();
		++this->switches;
		if (this->faultOnSwitch) fault();
		if (SUCCEEDED(this->switchResult) && this->publishSwitch)
			this->current = static_cast<FakeDesktop*>(desktop);
		return this->switchResult;
	}
	HRESULT STDMETHODCALLTYPE SwitchDesktopAndMoveForegroundView(Desktop*) override {
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE CreateDesktop(Desktop** out) override {
		if (this->beforeCreate) this->beforeCreate();
		++this->creates;
		if (!this->created) return E_NOTIMPL;
		this->created->AddRef();
		*out = this->created;
		if (this->faultOnCreate) fault(); // Also test ownership of a partial output.
		if (this->publishCreation) this->array.desktops.append(this->created);
		return S_OK;
	}
};
struct FakeViews: FakeCom<ViewCollection> {
	FakeView view;
	int gets = 0;
	bool faultOnGet = false;
	HRESULT result = S_OK;
	HRESULT STDMETHODCALLTYPE GetViews(IObjectArray**) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetViewsByZOrder(IObjectArray**) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetViewsByAppUserModelId(PCWSTR, IObjectArray**) override {
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE GetViewForHwnd(HWND, ApplicationView** out) override {
		++this->gets;
		// Failed calls deliberately leave a non-owning output. The caller must
		// neither adopt nor release it without a successful ownership transfer.
		*out = reinterpret_cast<ApplicationView*>(&this->view);
		if (this->faultOnGet) fault();
		if (FAILED(this->result)) return this->result;
		this->view.AddRef();
		return S_OK;
	}
};
struct FakePinnedApps: FakeCom<PinnedApps> {
	ApplicationView* expectedView = nullptr;
	BOOL pinned = FALSE;
	int queries = 0;
	int pins = 0;
	int unpins = 0;
	HRESULT queryResult = S_OK;
	HRESULT pinResult = S_OK;
	HRESULT unpinResult = S_OK;
	bool faultOnQuery = false;
	bool faultOnPin = false;
	bool faultOnUnpin = false;
	std::function<void()> beforeQuery;
	std::function<void()> beforePin;
	std::function<void()> beforeUnpin;
	HRESULT STDMETHODCALLTYPE IsAppIdPinned(PCWSTR, BOOL*) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE PinAppID(PCWSTR) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE UnpinAppID(PCWSTR) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE IsViewPinned(ApplicationView* view, BOOL* out) override {
		if (this->beforeQuery) this->beforeQuery();
		++this->queries;
		if (this->faultOnQuery) fault();
		if (FAILED(this->queryResult)) return this->queryResult;
		if (view != this->expectedView) return E_INVALIDARG;
		*out = this->pinned;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE PinView(ApplicationView* view) override {
		if (this->beforePin) this->beforePin();
		++this->pins;
		if (this->faultOnPin) fault();
		if (view != this->expectedView) return E_INVALIDARG;
		if (this->pinResult == S_OK) this->pinned = TRUE;
		return this->pinResult;
	}
	HRESULT STDMETHODCALLTYPE UnpinView(ApplicationView* view) override {
		if (this->beforeUnpin) this->beforeUnpin();
		++this->unpins;
		if (this->faultOnUnpin) fault();
		if (view != this->expectedView) return E_INVALIDARG;
		if (this->unpinResult == S_OK) this->pinned = FALSE;
		return this->unpinResult;
	}
};
struct FakePinnedAppsShell: FakeCom<IServiceProvider> {
	FakePinnedApps* pinnedApps = nullptr;
	HRESULT result = S_OK;
	int queries = 0;
	bool faultOnQuery = false;
	bool writeBogusOnFail = false;
	std::function<void()> beforeQuery;
	HRESULT STDMETHODCALLTYPE QueryService(REFGUID, REFIID, void** out) override {
		if (this->beforeQuery) this->beforeQuery();
		++this->queries;
		if (this->writeBogusOnFail) {
			// Deliberately write a non-owning pointer on failure or SEH so the
			// caller must not adopt or Release it without a successful return.
			*out = this->pinnedApps;
		}
		if (this->faultOnQuery) fault();
		if (FAILED(this->result) || !this->pinnedApps) {
			if (!this->writeBogusOnFail) *out = nullptr;
			return FAILED(this->result) ? this->result : E_NOINTERFACE;
		}
		this->pinnedApps->AddRef();
		*out = this->pinnedApps;
		return S_OK;
	}
};
struct FakePublic: FakeCom<IVirtualDesktopManager> {
	bool faultOnRead = false;
	std::function<void()> beforeQuery, beforeMove;
	GUID desktopId {};
	BOOL onCurrent = TRUE;
	HRESULT STDMETHODCALLTYPE IsWindowOnCurrentVirtualDesktop(HWND, BOOL* out) override {
		if (this->beforeQuery) this->beforeQuery();
		if (this->faultOnRead) fault();
		*out = this->onCurrent;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetWindowDesktopId(HWND, GUID* out) override {
		if (this->beforeQuery) this->beforeQuery();
		if (this->faultOnRead) fault();
		*out = this->desktopId;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE MoveWindowToDesktop(HWND, REFGUID id) override {
		if (this->beforeMove) this->beforeMove();
		this->desktopId = id;
		return S_OK;
	}
};

struct Fixture {
	FakeDesktop one {1}, two {2}, three {3};
	FakeManager manager;
	FakeViews views;
	FakePublic publicApi;

	Fixture() {
		this->manager.array.desktops = {&this->one, &this->two};
		this->manager.current = &this->one;
		this->manager.created = &this->three;
		this->manager.expectedView = reinterpret_cast<ApplicationView*>(&this->views.view);
		this->publicApi.desktopId = this->one.id;
	}
	void clearFaults() {
		this->manager.publishSwitch = this->manager.publishCreation = false;
		this->manager.switchResult = this->manager.moveResult = S_OK;
		this->manager.canMove = TRUE;
		this->manager.beforeEnumeration = this->manager.beforeSwitch = {};
		this->manager.beforeCreate = this->manager.beforeCanMove = this->manager.beforeMove = {};
		this->manager.beforeRelease = this->manager.array.beforeRelease = {};
		this->one.beforeRelease = this->two.beforeRelease = this->three.beforeRelease = {};
		this->views.beforeRelease = this->views.view.beforeRelease = {};
		this->publicApi.beforeRelease = this->publicApi.beforeQuery = this->publicApi.beforeMove = {};
		this->one.faultOnRelease = this->two.faultOnRelease = this->three.faultOnRelease = false;
		this->one.faultOnId = this->two.faultOnId = this->three.faultOnId = false;
		this->manager.faultOnRelease = this->manager.array.faultOnRelease = false;
		this->manager.faultOnEnumeration = this->manager.faultOnSwitch = false;
		this->manager.faultOnCreate = this->manager.faultOnCurrent = false;
		this->manager.faultOnCanMove = this->manager.faultOnMove = false;
		this->views.faultOnRelease = this->views.faultOnGet = this->views.view.faultOnRelease = false;
		this->publicApi.faultOnRelease = this->publicApi.faultOnRead = false;
	}
	HRESULT acquire(DesktopManager** m, ViewCollection** v, IVirtualDesktopManager** p) {
		this->manager.AddRef();
		this->views.AddRef();
		this->publicApi.AddRef();
		*m = &this->manager;
		*v = &this->views;
		*p = &this->publicApi;
		return S_OK;
	}
};
} // namespace erm625

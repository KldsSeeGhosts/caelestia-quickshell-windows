#pragma once

#include <objectarray.h>
#include <qlist.h>

#include "native_desktop_interfaces.hpp"

// Synthetic, stack-owned COM fixtures. Release counts ownership without
// deleting the fixture, allowing assertions after the provider drops it.
namespace erm625 {
using namespace qs::windows::native_desktop;

inline void fault() { RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr); }

template <typename Interface>
struct FakeCom: Interface {
	ULONG refs = 1;
	int releases = 0;
	bool faultOnRelease = false;
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
		*out = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++this->refs; }
	ULONG STDMETHODCALLTYPE Release() override {
		++this->releases;
		const auto remaining = --this->refs;
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
	int canMoves = 0;
	int moves = 0;
	HRESULT moveResult = S_OK;
	ApplicationView* expectedView = nullptr;
	FakeDesktop* current = nullptr;
	FakeDesktop* created = nullptr;
	HRESULT STDMETHODCALLTYPE GetCount(UINT* out) override { return this->array.GetCount(out); }
	HRESULT STDMETHODCALLTYPE MoveViewToDesktop(ApplicationView* view, Desktop*) override {
		++this->moves;
		if (this->faultOnMove) fault();
		return view == this->expectedView ? this->moveResult : E_INVALIDARG;
	}
	HRESULT STDMETHODCALLTYPE CanViewMoveDesktops(ApplicationView* view, BOOL* out) override {
		++this->canMoves;
		if (this->faultOnCanMove) fault();
		if (view != this->expectedView) return E_INVALIDARG;
		*out = TRUE;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetCurrentDesktop(Desktop** out) override {
		this->current->AddRef();
		*out = this->current;
		if (this->faultOnCurrent) fault();
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetDesktops(IObjectArray** out) override {
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
	HRESULT STDMETHODCALLTYPE SwitchDesktop(Desktop*) override {
		++this->switches;
		if (this->faultOnSwitch) fault();
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SwitchDesktopAndMoveForegroundView(Desktop*) override {
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE CreateDesktop(Desktop** out) override {
		++this->creates;
		if (!this->created) return E_NOTIMPL;
		this->created->AddRef();
		*out = this->created;
		if (this->faultOnCreate) fault(); // Also test ownership of a partial output.
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
struct FakePublic: FakeCom<IVirtualDesktopManager> {
	bool faultOnRead = false;
	HRESULT STDMETHODCALLTYPE IsWindowOnCurrentVirtualDesktop(HWND, BOOL*) override {
		if (this->faultOnRead) fault();
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE GetWindowDesktopId(HWND, GUID*) override {
		if (this->faultOnRead) fault();
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE MoveWindowToDesktop(HWND, REFGUID) override { return E_NOTIMPL; }
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
	}
	void clearFaults() {
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

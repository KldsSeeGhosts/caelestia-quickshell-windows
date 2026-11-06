#pragma once

#include <functional>

#include <objectarray.h>
#include <qt_windows.h>
#include <shobjidl_core.h>

namespace qs::windows::native_desktop {
// ABI declarations adapted from Markus Scholtes' MIT VirtualDesktop 1.21,
// VirtualDesktop11-24H2.cs at a725cbd3cdb9e977678eeaf034a7cc96d2e74bc6.
// See third_party/virtual_desktop/LICENSE and README.md.
// Only the prefixes we call are declared. IApplicationView is opaque: its
// IInspectable methods are never invoked by this provider.
struct ApplicationView;

struct __declspec(uuid("3F07F4BE-B107-441A-AF0F-39D82529072C")) Desktop: IUnknown {
	virtual HRESULT STDMETHODCALLTYPE IsViewVisible(ApplicationView*, BOOL*) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetId(GUID*) = 0;
};

struct __declspec(uuid("1841C6D7-4F9D-42C0-AF41-8747538F10E5")) ViewCollection: IUnknown {
	virtual HRESULT STDMETHODCALLTYPE GetViews(IObjectArray**) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetViewsByZOrder(IObjectArray**) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetViewsByAppUserModelId(PCWSTR, IObjectArray**) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetViewForHwnd(HWND, ApplicationView**) = 0;
};

struct __declspec(uuid("53F5CA0B-158F-4124-900C-057158060B27")) DesktopManager: IUnknown {
	virtual HRESULT STDMETHODCALLTYPE GetCount(UINT*) = 0;
	virtual HRESULT STDMETHODCALLTYPE MoveViewToDesktop(ApplicationView*, Desktop*) = 0;
	virtual HRESULT STDMETHODCALLTYPE CanViewMoveDesktops(ApplicationView*, BOOL*) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetCurrentDesktop(Desktop**) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetDesktops(IObjectArray**) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetAdjacentDesktop(Desktop*, int, Desktop**) = 0;
	virtual HRESULT STDMETHODCALLTYPE SwitchDesktop(Desktop*) = 0;
	virtual HRESULT STDMETHODCALLTYPE SwitchDesktopAndMoveForegroundView(Desktop*) = 0;
	virtual HRESULT STDMETHODCALLTYPE CreateDesktop(Desktop**) = 0;
};

} // namespace qs::windows::native_desktop

#ifdef QS_TEST
namespace qs::windows {
using NativeDesktopFactory = std::function<HRESULT(
    native_desktop::DesktopManager**,
    native_desktop::ViewCollection**,
    IVirtualDesktopManager**
)>;
void setNativeDesktopFactoryForTest(NativeDesktopFactory factory);
} // namespace qs::windows
#endif

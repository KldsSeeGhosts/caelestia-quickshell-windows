#include "blur.hpp"
#include <algorithm>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <qt_windows.h>

#include <qcoreapplication.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qjsonvalue.h>
#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qpointer.h>
#include <qqmlengine.h>
#include <qquickwindow.h>
#include <qscopedvaluerollback.h>
#include <qstandardpaths.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtimer.h>
#include <qvariant.h>
#include <qwineventnotifier.h>

#include "../core/generation.hpp"
#include "blur_shapes.hpp"
#include "panel_window.hpp"
#include "redact.hpp"
#include "startup.hpp"
#include "util.hpp"

#include <unknwn.h>
#include <inspectable.h>

#include <DispatcherQueue.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.h>

#include <dwmapi.h>
#include <windows.ui.composition.interop.h>

namespace wuc = winrt::Windows::UI::Composition;
namespace wucd = winrt::Windows::UI::Composition::Desktop;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;
using AbiDesktopInterop = ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop;
using AbiDesktopWindowTarget = ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget;
using AbiDispatcherQueueController = ABI::Windows::System::IDispatcherQueueController;

namespace qs::windows {

namespace {

Q_LOGGING_CATEGORY(logBlur, "quickshell.windows.blur", QtInfoMsg);

constexpr auto DEFAULT_FILE = "defaults/windows/layerrules.json";
constexpr auto PERSONALIZE_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr auto BACKDROP_CLASS = L"QuickshellBlurBackdrop";
// The SDK declares these as enum members, so use local aliases with the documented values.
constexpr DWORD SYSTEM_BACKDROP_TYPE = 38; // DWMWA_SYSTEMBACKDROP_TYPE
constexpr int BACKDROP_NONE = 1; // DWMSBT_NONE
constexpr int BACKDROP_MICA = 2; // DWMSBT_MAINWINDOW
constexpr int BACKDROP_ACRYLIC = 3; // DWMSBT_TRANSIENTWINDOW
constexpr int BACKDROP_MICA_ALT = 4; // DWMSBT_TABBEDWINDOW
constexpr qreal OPAQUE_ALPHA = 0.995; // Match the shape collector's cutoff.

bool gManagerDestroyed = false; // NOLINT

int systemBackdropType(BlurMode mode) {
	switch (mode) {
	case BlurMode::SystemAcrylic: return BACKDROP_ACRYLIC;
	case BlurMode::SystemMica: return BACKDROP_MICA;
	case BlurMode::SystemMicaAlt: return BACKDROP_MICA_ALT;
	case BlurMode::Disabled:
	case BlurMode::HostBackdrop: return BACKDROP_NONE;
	}

	return BACKDROP_NONE;
}

QString hresultString(const winrt::hresult_error& error) {
	return QString("0x%1 %2")
	    .arg(static_cast<quint32>(error.code().value), 8, 16, QChar('0'))
	    .arg(QString::fromWCharArray(error.message().c_str()));
}

} // namespace

class BackdropWindow {
public:
	BackdropWindow() = default;
	~BackdropWindow();
	Q_DISABLE_COPY_MOVE(BackdropWindow);

	static std::unique_ptr<BackdropWindow> create(
	    const wuc::Compositor& compositor,
	    bool tint,
	    QString& error,
	    bool& unsupported
	);

	void setPanel(HWND panel) { this->mPanel = panel; }

	bool setShapes(const QList<BlurShape>& shapes);

	void show();
	void hide();

private:
	static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	static bool registerClass();
	static HWND createWindow(BackdropWindow* owner, QString& error);
	void hideWindow();

	struct VisualSlot {
		wuc::ContainerVisual holder {nullptr};
		wuc::SpriteVisual sprite {nullptr};
		wuc::CompositionRoundedRectangleGeometry geometry {nullptr};
	};

	HWND mHwnd = nullptr;
	HWND mPanel = nullptr;
	bool syncing = false;
	bool shown = false;
	RECT lastRect {};

	wuc::Compositor compositor {nullptr};
	wucd::DesktopWindowTarget target {nullptr};
	wuc::ContainerVisual root {nullptr};
	wuc::CompositionBrush brush {nullptr};
	wuc::CompositionBrush tint {nullptr};
	std::vector<VisualSlot> visuals;
	QList<BlurShape> applied;
};

bool BackdropWindow::registerClass() {
	static auto registered = false; // NOLINT
	if (registered) return true;

	WNDCLASSEXW wndClass {};
	wndClass.cbSize = sizeof(wndClass);
	wndClass.lpfnWndProc = &BackdropWindow::wndProc;
	wndClass.hInstance = GetModuleHandleW(nullptr);
	wndClass.lpszClassName = BACKDROP_CLASS;

	registered = RegisterClassExW(&wndClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
	return registered;
}

HWND BackdropWindow::createWindow(BackdropWindow* owner, QString& error) {
	if (!registerClass()) {
		error = QString("RegisterClassEx failed (%1)").arg(GetLastError());
		return nullptr;
	}

	constexpr DWORD exStyle =
	    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP;

	auto* hwnd = CreateWindowExW(
	    exStyle,
	    BACKDROP_CLASS,
	    L"",
	    WS_POPUP,
	    0,
	    0,
	    0,
	    0,
	    nullptr,
	    nullptr,
	    GetModuleHandleW(nullptr),
	    nullptr
	);

	if (hwnd != nullptr) markNonRude(hwnd);

	if (hwnd == nullptr) {
		error = QString("CreateWindowEx failed (%1)").arg(GetLastError());
		return nullptr;
	}

	SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owner));

	return hwnd;
}

std::unique_ptr<BackdropWindow> BackdropWindow::create(
    const wuc::Compositor& compositor,
    bool tint,
    QString& error,
    bool& unsupported
) {
	unsupported = false;

	auto backdrop = std::make_unique<BackdropWindow>();
	backdrop->mHwnd = createWindow(backdrop.get(), error);
	if (backdrop->mHwnd == nullptr) return nullptr;
	auto* hwnd = backdrop->mHwnd;

	BOOL enable = TRUE;
	auto hr = DwmSetWindowAttribute(hwnd, DWMWA_USE_HOSTBACKDROPBRUSH, &enable, sizeof(enable));
	if (FAILED(hr)) {
		error = QString("DWMWA_USE_HOSTBACKDROPBRUSH was refused (0x%1)")
		            .arg(static_cast<quint32>(hr), 8, 16, QChar('0'));
		unsupported = true;
		return nullptr;
	}

	auto corner = static_cast<DWM_WINDOW_CORNER_PREFERENCE>(DWMWCP_DONOTROUND);
	DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
	COLORREF border = DWMWA_COLOR_NONE;
	DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
	DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &enable, sizeof(enable));
	DwmSetWindowAttribute(hwnd, DWMWA_EXCLUDED_FROM_PEEK, &enable, sizeof(enable));

	try {
		backdrop->compositor = compositor;

		auto interop = compositor.as<AbiDesktopInterop>();
		winrt::check_hresult(interop->CreateDesktopWindowTarget(
		    hwnd,
		    TRUE,
		    reinterpret_cast<AbiDesktopWindowTarget**>(winrt::put_abi(backdrop->target)) // NOLINT
		));

		backdrop->root = compositor.CreateContainerVisual();
		backdrop->root.RelativeSizeAdjustment(float2(1.0f, 1.0f));
		backdrop->target.Root(backdrop->root);

		backdrop->brush = compositor.CreateHostBackdropBrush();

		if (tint) {
			backdrop->tint = compositor.CreateColorBrush(
			    winrt::Windows::UI::Color {.A = 0x60, .R = 0xff, .G = 0x20, .B = 0x20}
			);
		}
	} catch (const winrt::hresult_error& e) {
		error = "Composition setup failed: " + hresultString(e);
		return nullptr;
	}

	return backdrop;
}

BackdropWindow::~BackdropWindow() {
	try {
		this->visuals.clear();
		if (this->root != nullptr) this->root.Children().RemoveAll();

		if (this->target != nullptr) {
			this->target.Root(nullptr);
			this->target.Close();
		}
	} catch (const winrt::hresult_error& e) {
		qCDebug(logBlur) << "Releasing a backdrop:" << hresultString(e);
	}

	this->tint = nullptr;
	this->brush = nullptr;
	this->root = nullptr;
	this->target = nullptr;
	this->compositor = nullptr;

	if (this->mHwnd != nullptr) {
		SetWindowLongPtrW(this->mHwnd, GWLP_USERDATA, 0);
		DestroyWindow(this->mHwnd);
		this->mHwnd = nullptr;
	}
}

bool BackdropWindow::setShapes(const QList<BlurShape>& shapes) {
	try {
		auto children = this->root.Children();
		// collectBlurShapes caps the list at MAX_SHAPES. Retain slots while the
		// panel is shown so animations can reuse them as the count changes.
		auto count = static_cast<size_t>(shapes.size());
		auto previousCount = static_cast<size_t>(this->applied.size());

		while (this->visuals.size() < count) {
			VisualSlot slot;
			slot.holder = this->compositor.CreateContainerVisual();
			slot.holder.Clip(this->compositor.CreateInsetClip());
			slot.holder.IsVisible(false);

			slot.sprite = this->compositor.CreateSpriteVisual();
			slot.sprite.Brush(this->brush);
			slot.geometry = this->compositor.CreateRoundedRectangleGeometry();
			slot.sprite.Clip(this->compositor.CreateGeometricClip(slot.geometry));

			if (this->tint != nullptr) {
				auto overlay = this->compositor.CreateSpriteVisual();
				overlay.Brush(this->tint);
				overlay.RelativeSizeAdjustment(float2(1.0f, 1.0f));
				slot.sprite.Children().InsertAtTop(overlay);
			}

			slot.holder.Children().InsertAtTop(slot.sprite);
			children.InsertAtTop(slot.holder);
			this->visuals.push_back(std::move(slot));
		}

		for (auto i = count; i < previousCount; i++) this->visuals.at(i).holder.IsVisible(false);

		for (size_t i = 0; i < count; i++) {
			auto index = static_cast<qsizetype>(i);
			const auto& shape = shapes.at(index);
			if (index < this->applied.size() && this->applied.at(index).fuzzyEquals(shape)) continue;

			auto& slot = this->visuals.at(i);

			auto size = float2(static_cast<float>(shape.rect.width()), static_cast<float>(shape.rect.height()));
			auto radius = static_cast<float>(shape.radius);

			slot.holder.Offset(float3(static_cast<float>(shape.clip.x()), static_cast<float>(shape.clip.y()), 0));
			slot.holder.Size(float2(static_cast<float>(shape.clip.width()), static_cast<float>(shape.clip.height())));

			slot.sprite.Offset(float3(
			    static_cast<float>(shape.rect.x() - shape.clip.x()),
			    static_cast<float>(shape.rect.y() - shape.clip.y()),
			    0
			));
			slot.sprite.Size(size);
			slot.geometry.Size(size);
			slot.geometry.CornerRadius(float2(radius, radius));
			if (i >= previousCount) slot.holder.IsVisible(true);
		}
	} catch (const winrt::hresult_error& e) {
		qCWarning(logBlur) << "Updating the blur shapes failed:" << hresultString(e);
		this->applied.clear();
		return false;
	}

	this->applied = shapes;
	if (shapes.isEmpty()) this->hideWindow();
	return true;
}

void BackdropWindow::show() {
	if (this->mHwnd == nullptr || this->mPanel == nullptr) return;

	RECT rect {};
	if (!GetWindowRect(this->mPanel, &rect)) {
		this->hideWindow();
		return;
	}

	auto panelTopmost = (GetWindowLongPtrW(this->mPanel, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
	auto ownTopmost = (GetWindowLongPtrW(this->mHwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;

	if (this->shown && IsWindowVisible(this->mHwnd) && EqualRect(&rect, &this->lastRect)
	    && panelTopmost == ownTopmost && GetWindow(this->mPanel, GW_HWNDNEXT) == this->mHwnd)
	{
		return;
	}

	this->syncing = true;
	constexpr UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER;

	if (panelTopmost != ownTopmost) {
		SetWindowPos(
		    this->mHwnd,
		    panelTopmost ? HWND_TOPMOST : HWND_NOTOPMOST,
		    0,
		    0,
		    0,
		    0,
		    flags | SWP_NOMOVE | SWP_NOSIZE
		);
	}

	SetWindowPos(
	    this->mHwnd,
	    this->mPanel,
	    rect.left,
	    rect.top,
	    rect.right - rect.left,
	    rect.bottom - rect.top,
	    flags | SWP_SHOWWINDOW
	);

	this->syncing = false;
	this->lastRect = rect;
	this->shown = true;
}

void BackdropWindow::hide() {
	this->hideWindow();
	if (this->visuals.empty()) return;

	try {
		this->root.Children().RemoveAll();
		this->visuals.clear();
		this->applied.clear();
	} catch (const winrt::hresult_error& e) {
		qCDebug(logBlur) << "Releasing hidden blur visuals:" << hresultString(e);
	}
}

void BackdropWindow::hideWindow() {
	this->shown = false;
	if (this->mHwnd == nullptr || !IsWindowVisible(this->mHwnd)) return;

	this->syncing = true;
	SetWindowPos(
	    this->mHwnd,
	    nullptr,
	    0,
	    0,
	    0,
	    0,
	    SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER
	);
	this->syncing = false;
}

LRESULT CALLBACK BackdropWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	// NOLINTNEXTLINE(performance-no-int-to-ptr)
	auto* self = reinterpret_cast<BackdropWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

	switch (msg) {
	case WM_NCHITTEST: return HTTRANSPARENT;
	case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
	case WM_ERASEBKGND: return 1;
	case WM_PAINT: ValidateRect(hwnd, nullptr); return 0;
	case WM_DPICHANGED: return 0;
	case WM_WINDOWPOSCHANGING:
		if (self != nullptr && !self->syncing && self->mPanel != nullptr && IsWindow(self->mPanel)) {
			auto* pos = reinterpret_cast<WINDOWPOS*>(lParam); // NOLINT(performance-no-int-to-ptr)
			if ((pos->flags & SWP_NOZORDER) == 0) pos->hwndInsertAfter = self->mPanel;
		}
		break;
	default: break;
	}

	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

struct BlurManager::Composition {
	winrt::Windows::System::DispatcherQueueController controller {nullptr};
	wuc::Compositor compositor {nullptr};
};

BlurManager* BlurManager::instance() {
	static QPointer<BlurManager> manager; // NOLINT

	if (gManagerDestroyed) return nullptr;

	if (manager.isNull()) {
		manager = new BlurManager(QCoreApplication::instance());
	}

	return manager.data();
}

BlurManager::BlurManager(QObject* parent): QObject(parent) {
	auto env = qEnvironmentVariable("QS_WINDOWS_BLUR").trimmed().toLower();
	this->envEnabled = !(env == "0" || env == "false" || env == "off" || env == "no");
	this->mDebugTint = qEnvironmentVariableIntValue("QS_WINDOWS_BLUR_DEBUG") != 0;
	this->mWindowsBuild = windowsBuild();

	if (this->mWindowsBuild < HOST_BACKDROP_MIN_BUILD) {
		qCInfo(logBlur) << "Blur behind panels requires Windows 11. Current build:"
		               << this->mWindowsBuild;
	} else if (!this->envEnabled) {
		qCInfo(logBlur) << "Blur behind panels is turned off by QS_WINDOWS_BLUR.";
	}

	this->reloadTimer.setSingleShot(true);
	this->reloadTimer.setInterval(200);
	QObject::connect(&this->reloadTimer, &QTimer::timeout, this, &BlurManager::loadFile);

	// clang-format off
	QObject::connect(&this->watcher, &QFileSystemWatcher::fileChanged, this, &BlurManager::scheduleReload);
	QObject::connect(&this->watcher, &QFileSystemWatcher::directoryChanged, this, &BlurManager::scheduleReload);
	// clang-format on

	if (auto* app = QCoreApplication::instance()) {
		QObject::connect(app, &QCoreApplication::aboutToQuit, this, &BlurManager::shutdown);
	}

	if (this->mWindowsBuild >= HOST_BACKDROP_MIN_BUILD && this->envEnabled) {
		this->checkSystem();
		this->watchRegistry();
	}
}

BlurManager::~BlurManager() {
	gManagerDestroyed = true;
	this->shutdown();

	delete this->registryNotifier;
	this->registryNotifier = nullptr;
	if (this->registryEvent != nullptr) CloseHandle(this->registryEvent);
	if (this->personalizeKey != nullptr) RegCloseKey(this->personalizeKey);
}

void BlurManager::shutdown() {
	if (this->mShutDown) return;
	this->mShutDown = true;

	for (auto* panel: QList(this->panels)) panel->updateActive();

	if (this->composition != nullptr) {
		this->closeCompositor();
		auto state = std::move(this->composition);
		if (state->controller == nullptr) return;

		// We created this queue on the GUI thread. Windows requires its
		// message loop to run until ShutdownQueueAsync completes.
		std::optional<int> quitCode;
		try {
			using winrt::Windows::Foundation::AsyncStatus;
			auto action = state->controller.ShutdownQueueAsync();
			const auto deadline = GetTickCount64() + 250;
			while (action.Status() == AsyncStatus::Started) {
				auto now = GetTickCount64();
				if (now >= deadline) {
					qCDebug(logBlur) << "Dispatcher queue shutdown did not finish within its deadline";
					break;
				}
				MSG msg {};
				if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
					if (msg.message == WM_QUIT) quitCode = static_cast<int>(msg.wParam);
					else {
						TranslateMessage(&msg);
						DispatchMessageW(&msg);
					}
				} else {
					MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(deadline - now),
					    QS_ALLINPUT, MWMO_INPUTAVAILABLE);
				}
			}
		} catch (const winrt::hresult_error& e) {
			qCDebug(logBlur) << "Closing the dispatcher queue:" << hresultString(e);
		}
		if (quitCode) PostQuitMessage(*quitCode);
	}
}

void BlurManager::closeCompositor() {
	if (this->composition == nullptr || this->composition->compositor == nullptr) return;

	try {
		this->composition->compositor.Close();
	} catch (const winrt::hresult_error& e) {
		qCDebug(logBlur) << "Closing the compositor:" << hresultString(e);
	}

	this->composition->compositor = nullptr;
}

BlurManager::Composition* BlurManager::ensureComposition() {
	if (this->hostUnsupported || !this->available()) return nullptr;

	if (this->composition == nullptr) this->composition = std::make_unique<Composition>();
	auto* state = this->composition.get();
	if (state->compositor != nullptr) return state;

	try {
		if (state->controller == nullptr
		    && winrt::Windows::System::DispatcherQueue::GetForCurrentThread() == nullptr)
		{
			DispatcherQueueOptions options {
			    .dwSize = sizeof(DispatcherQueueOptions),
			    .threadType = DQTYPE_THREAD_CURRENT,
			    .apartmentType = DQTAT_COM_NONE,
			};

			winrt::check_hresult(CreateDispatcherQueueController(
			    options,
			    reinterpret_cast<AbiDispatcherQueueController**>(winrt::put_abi(state->controller)) // NOLINT
			));
		}

		state->compositor = wuc::Compositor();
	} catch (const winrt::hresult_error& e) {
		qCWarning(logBlur).noquote() << "Could not create a compositor for blur:" << hresultString(e);
		state->compositor = nullptr;
		return nullptr;
	}

	qCDebug(logBlur) << "Compositor created.";
	return state;
}

bool BlurManager::deferComposition() {
	if (this->hostUnsupported || !this->available()) return false;
	if (startup::settled()) return false;
	if (this->composition != nullptr && this->composition->compositor != nullptr) return false;

	if (!this->compositionWaiting) {
		this->compositionWaiting = true;
		qCDebug(logBlur) << "Creating the compositor after the first panel frame.";

		startup::afterFirstFrame(this, [this]() {
			this->compositionWaiting = false;
			this->scheduleNotifyPanels();
		});
	}

	return true;
}

void BlurManager::markHostUnsupported(const QString& reason) {
	if (this->hostUnsupported) return;
	auto wasAvailable = this->available();
	this->hostUnsupported = true;

	qCWarning(logBlur).noquote() << "HostBackdrop is unavailable:" << reason;
	if (wasAvailable != this->available()) emit this->stateChanged();
	this->scheduleNotifyPanels();
}

void BlurManager::registerPanel(PanelBlur* panel) {
	if (!this->panels.contains(panel)) this->panels.append(panel);
}

void BlurManager::unregisterPanel(PanelBlur* panel) { this->panels.removeOne(panel); }

void BlurManager::setEnabled(bool enabled) {
	if (enabled == this->mEnabled) return;
	this->mEnabled = enabled;

	emit this->stateChanged();
	this->scheduleNotifyPanels();
}

void BlurManager::setPanelAnimations(bool enabled) {
	if (enabled == this->mPanelAnimations) return;
	this->mPanelAnimations = enabled;

	qCInfo(logBlur) << "Panel open and close animations" << (enabled ? "on" : "off");
	emit this->panelAnimationsChanged();
}

void BlurManager::setExpressiveAnimations(bool expressive) {
	if (expressive == this->mExpressiveAnimations) return;
	this->mExpressiveAnimations = expressive;

	qCInfo(logBlur) << "Panel animation style" << (expressive ? "expressive" : "standard");
	emit this->panelAnimationsChanged();
}

bool BlurManager::available() const {
	return this->mWindowsBuild >= HOST_BACKDROP_MIN_BUILD
	    && (!this->hostUnsupported || this->mWindowsBuild >= SYSTEM_BACKDROP_MIN_BUILD)
	    && !this->mShutDown && this->envEnabled && this->rules.enabled()
	    && this->transparencyEffects && !this->highContrast && !this->energySaver;
}

void BlurManager::scheduleNotifyPanels() {
	if (this->notifyPending) return;
	this->notifyPending = true;

	QMetaObject::invokeMethod(
	    this,
	    [this]() {
		    this->notifyPending = false;
		    this->notifyPanels();
	    },
	    Qt::QueuedConnection
	);
}

void BlurManager::notifyPanels() {
	for (auto* panel: QList(this->panels)) panel->updateActive();
}

void BlurManager::scheduleSystemCheck(bool recreate) {
	if (this->mWindowsBuild < HOST_BACKDROP_MIN_BUILD || !this->envEnabled || this->mShutDown) return;

	if (recreate) this->recreatePending = true;
	if (this->systemCheckPending) return;
	this->systemCheckPending = true;

	QTimer::singleShot(0, this, [this]() {
		this->systemCheckPending = false;
		this->checkSystem();

		if (this->recreatePending) {
			this->recreatePending = false;
			if (this->mShutDown) return;

			qCInfo(logBlur) << "Desktop composition changed, recreating the blur backdrops.";

			for (auto* panel: QList(this->panels)) panel->recreate();

			this->closeCompositor();
			this->notifyPanels();
		}
	});
}

void BlurManager::checkSystem() {
	DWORD value = 1;
	DWORD size = sizeof(value);
	auto status = RegGetValueW(
	    HKEY_CURRENT_USER,
	    PERSONALIZE_KEY,
	    L"EnableTransparency",
	    RRF_RT_REG_DWORD,
	    nullptr,
	    &value,
	    &size
	);
	auto transparency = status != ERROR_SUCCESS || value != 0;

	HIGHCONTRASTW contrast {};
	contrast.cbSize = sizeof(contrast);
	auto highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)
	                 && (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;

	SYSTEM_POWER_STATUS power {};
	auto energySaver = GetSystemPowerStatus(&power) && power.SystemStatusFlag == 1;

	if (transparency == this->transparencyEffects && highContrast == this->highContrast
	    && energySaver == this->energySaver)
	{
		return;
	}

	this->transparencyEffects = transparency;
	this->highContrast = highContrast;
	this->energySaver = energySaver;

	qCInfo(logBlur) << "System state: transparency effects" << transparency << "high contrast"
	                << highContrast << "energy saver" << energySaver;

	emit this->stateChanged();
	this->scheduleNotifyPanels();
}

void BlurManager::watchRegistry() {
	auto status = RegOpenKeyExW(
	    HKEY_CURRENT_USER,
	    PERSONALIZE_KEY,
	    0,
	    KEY_NOTIFY | KEY_QUERY_VALUE,
	    &this->personalizeKey
	);

	if (status != ERROR_SUCCESS) {
		this->personalizeKey = nullptr;
		qCDebug(logBlur) << "Can't watch the Personalize key:" << status;
		return;
	}

	this->registryEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (this->registryEvent == nullptr) return;

	this->registryNotifier = new QWinEventNotifier(this->registryEvent, this);
	QObject::connect(this->registryNotifier, &QWinEventNotifier::activated, this, [this]() {
		this->armRegistryNotification();
		this->scheduleSystemCheck();
	});

	this->armRegistryNotification();
}

void BlurManager::armRegistryNotification() {
	if (this->personalizeKey == nullptr || this->registryEvent == nullptr) return;

	RegNotifyChangeKeyValue(
	    this->personalizeKey,
	    FALSE,
	    REG_NOTIFY_CHANGE_LAST_SET,
	    this->registryEvent,
	    TRUE
	);
}

void BlurManager::setShellDir(const QString& shellDir) {
	if (shellDir.isEmpty() || QDir::isRelativePath(shellDir)) return;
	if (this->loaded && shellDir == this->shellDir) return;

	this->shellDir = shellDir;
	this->loaded = true;
	this->parsed = false;
	this->loadFile();
}

void BlurManager::reload() {
	this->parsed = false;
	this->loadFile();
}

QString BlurManager::userFilePath() const {
	if (this->userFile.isEmpty()) return QString();
	if (!QDir::isRelativePath(this->userFile)) return this->userFile;
	auto base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
	if (base.isEmpty()) return QString();
	return base + '/' + this->userFile;
}

QString BlurManager::readUserFile() const {
	auto file = QFile(this->defaultFilePath());
	if (this->defaultFilePath().isEmpty() || !file.open(QFile::ReadOnly)) return QString();
	return QJsonDocument::fromJson(file.readAll()).object().value("userFile").toString().trimmed();
}

QString BlurManager::defaultFilePath() const {
	if (this->shellDir.isEmpty()) return QString();
	return this->shellDir + '/' + DEFAULT_FILE;
}

void BlurManager::scheduleReload() { this->reloadTimer.start(); }

void BlurManager::loadFile() {
	this->userFile = this->readUserFile();
	auto path = this->userFilePath();
	if (path.isEmpty() || !QFileInfo::exists(path)) path = this->defaultFilePath();

	QByteArray data;
	auto file = QFile(path);
	if (path.isEmpty() || !file.open(QFile::ReadOnly)) path.clear();
	else data = file.readAll();

	if (!this->parsed || path != this->mConfigPath || data != this->loadedData) {
		if (path.isEmpty()) {
			auto looked = QStringList {this->userFilePath(), this->defaultFilePath()};
			looked.removeAll(QString());
			qCInfo(logBlur).noquote() << "No layer rules file found (looked for"
			                          << qs::windows::logPath(looked.join(" and "))
			                          << "), blur behind panels is off.";
		}

		auto wasAvailable = this->available();

		this->parsed = true;
		this->mConfigPath = path;
		this->loadedData = data;
		this->parse(data, path);

		emit this->rulesChanged();
		if (this->available() != wasAvailable) emit this->stateChanged();
		this->scheduleNotifyPanels();
	}

	this->updateWatches();
}

void BlurManager::updateWatches() {
	auto userPath = this->userFilePath();

	QStringList wanted;
	if (!this->mConfigPath.isEmpty()) wanted.append(this->mConfigPath);
	auto defaultPath = this->defaultFilePath();
	if (!defaultPath.isEmpty() && !wanted.contains(defaultPath)) wanted.append(defaultPath);
	if (!userPath.isEmpty()) {
		auto userDir = QFileInfo(userPath).absolutePath();
		if (QFileInfo::exists(userDir)) wanted.append(userDir);
		else wanted.append(QFileInfo(userDir).absolutePath());
	}

	auto current = this->watcher.files() + this->watcher.directories();
	for (const auto& path: current) {
		if (!wanted.contains(path)) this->watcher.removePath(path);
	}

	for (const auto& path: wanted) {
		if (!current.contains(path) && QFileInfo::exists(path)) this->watcher.addPath(path);
	}
}

void BlurManager::parse(const QByteArray& data, const QString& path) {
	QStringList warnings;
	this->rules = LayerRules::parse(data, qs::windows::logPath(path), &warnings);

	for (const auto& warning: warnings) qCWarning(logBlur).noquote() << warning;

	if (this->rules.valid()) {
		qCInfo(logBlur).noquote() << "Loaded" << this->rules.size() << "layer rules from"
		                          << qs::windows::logPath(path);
	}
}

LayerRule BlurManager::ruleFor(const QString& ns) const { return this->rules.ruleFor(ns); }

QVariantList BlurManager::rulesInfo() const { return this->rules.info(); }

PanelBlur::PanelBlur(WinPanelWindow* panel): QObject(panel), panel(panel) {
	if (auto* manager = BlurManager::instance()) manager->registerPanel(this);
	QObject::connect(panel, &WinPanelWindow::namespaceChanged, this, &PanelBlur::updateActive);
}

PanelBlur::~PanelBlur() {
	this->release();
	if (auto* manager = BlurManager::instance()) manager->unregisterPanel(this);
}

void PanelBlur::adopt(PanelBlur* other) {
	if (other == nullptr || other == this) return;

	this->recreate();
	other->disconnectFrames();
	QObject::disconnect(std::exchange(other->colorConnection, {}));

	// The backing window survives a reload. Transfer its live DWM state before
	// the old panel releases its native state and disowns the window.
	this->mode = std::exchange(other->mode, BlurMode::Disabled);
	this->appliedSystemMode = std::exchange(other->appliedSystemMode, BlurMode::Disabled);
	this->systemBackdropHwnd = std::exchange(other->systemBackdropHwnd, nullptr);
	this->rejectedSystemHwnd = std::exchange(other->rejectedSystemHwnd, nullptr);
	this->backdrop = std::move(other->backdrop);
	this->shapes = std::move(other->shapes);
	this->mask = other->mask;
	this->hasMask = other->hasMask;
}

void PanelBlur::attach() {
	auto* window = this->panel->backingWindow();
	if (this->mWindow != window) {
		this->disconnectFrames();
		QObject::disconnect(std::exchange(this->colorConnection, {}));
		this->mWindow = window;
	}

	this->updateActive();
}

void PanelBlur::release() {
	this->recreate();
	this->mWindow = nullptr;
}

void PanelBlur::recreate() {
	this->setMode(BlurMode::Disabled);
	this->rejectedSystemHwnd = nullptr;
}

void PanelBlur::setSuspended(bool suspended) {
	if (suspended == this->suspended) return;
	this->suspended = suspended;
	this->syncPlacement();
}

void PanelBlur::destroyBackdrop() {
	this->backdrop.reset();
	this->shapes.clear();
	this->shapesForced = true;
}

void PanelBlur::connectFrames() {
	auto* window = this->mWindow.data();
	if (window == nullptr || this->frameConnection) return;

	this->frameConnection =
	    QObject::connect(window, &QQuickWindow::afterAnimating, this, &PanelBlur::updateShapes);
	this->scheduleShapes();
}

void PanelBlur::disconnectFrames() {
	QObject::disconnect(std::exchange(this->frameConnection, {}));
}

void PanelBlur::updateActive() {
	auto* manager = BlurManager::instance();

	if (manager != nullptr && !manager->shutDown()) {
		if (auto* generation = EngineGeneration::findObjectGeneration(this->panel)) {
			manager->setShellDir(generation->rootPath.path());
		}

		this->rule = manager->ruleFor(this->panel->ns());
	} else {
		this->rule = LayerRule();
	}

	this->updateMode();
}

void PanelBlur::updateMode() {
	auto* manager = BlurManager::instance();
	auto* hwnd = this->panel->hwnd();
	auto next = chooseBlurMode({
	    .windowsBuild = manager != nullptr ? manager->build() : 0u,
	    .enabled = manager != nullptr && manager->enabled() && this->rule.blur && hwnd != nullptr
	            && this->mWindow != nullptr && !this->panel->isEmbedded(),
	    .forceDisabled = manager == nullptr || !manager->available(),
	    .requested = this->rule.backdrop,
	    .hasIgnoreAlpha = this->rule.ignoreAlpha.has_value(),
	    .hasInputMask = this->hasMask,
	});

	if (systemBackdropType(next) != BACKDROP_NONE && hwnd == this->rejectedSystemHwnd) {
		next = BlurMode::HostBackdrop;
	}
	if (next == BlurMode::HostBackdrop
	    && (!manager->hostBackdropAvailable() || this->rule.ignoreAlpha.value_or(0) >= 1))
	{
		next = BlurMode::Disabled;
	}

	this->setMode(next);
	if (this->backdrop != nullptr) this->backdrop->setPanel(hwnd);
	this->syncPlacement();
	if (this->mode == BlurMode::HostBackdrop && this->frameConnection) this->scheduleShapes();
}

void PanelBlur::setMode(BlurMode mode) {
	if (this->mode != mode) {
		this->mode = mode;
		qCDebug(logBlur) << "Blur mode for" << qs::windows::logId(this->panel->ns())
		                 << blurModeName(mode);
	}

	if (mode != BlurMode::HostBackdrop) {
		this->disconnectFrames();
		this->destroyBackdrop();
	}

	if (systemBackdropType(mode) == BACKDROP_NONE) {
		QObject::disconnect(std::exchange(this->colorConnection, {}));
		this->clearSystemBackdrop();
	} else if (!this->colorConnection) {
		this->colorConnection =
		    QObject::connect(this->mWindow.data(), &QQuickWindow::colorChanged, this, &PanelBlur::syncPlacement);
	}
}

bool PanelBlur::applySystemBackdrop(HWND hwnd) {
	if (this->systemBackdropHwnd == hwnd && this->appliedSystemMode == this->mode) return true;

	// Extending the frame may deliver native placement messages synchronously.
	const auto syncing = QScopedValueRollback(this->syncingSystemBackdrop, true);
	HRESULT hr = S_OK;

	if (this->systemBackdropHwnd != hwnd) {
		this->clearSystemBackdrop();
		constexpr MARGINS margins {-1, -1, -1, -1};
		hr = DwmExtendFrameIntoClientArea(hwnd, &margins);
		if (SUCCEEDED(hr)) this->systemBackdropHwnd = hwnd;
	}

	if (SUCCEEDED(hr)) {
		const auto type = systemBackdropType(this->mode);
		hr = DwmSetWindowAttribute(hwnd, SYSTEM_BACKDROP_TYPE, &type, sizeof(type));
	}

	if (SUCCEEDED(hr)) {
		this->appliedSystemMode = this->mode;
		return true;
	}

	this->clearSystemBackdrop();
	qCDebug(logBlur) << "System backdrop for" << qs::windows::logId(this->panel->ns())
	                 << "failed. Trying HostBackdrop. HRESULT:" << Qt::hex << hr;
	return false;
}

void PanelBlur::clearSystemBackdrop() {
	auto* hwnd = std::exchange(this->systemBackdropHwnd, nullptr);
	this->appliedSystemMode = BlurMode::Disabled;
	if (hwnd == nullptr || !IsWindow(hwnd)) return;

	const auto syncing = QScopedValueRollback(this->syncingSystemBackdrop, true);
	constexpr int type = BACKDROP_NONE;
	constexpr MARGINS margins {};
	DwmSetWindowAttribute(hwnd, SYSTEM_BACKDROP_TYPE, &type, sizeof(type));
	DwmExtendFrameIntoClientArea(hwnd, &margins);
}

bool PanelBlur::ensureBackdrop() {
	if (this->backdrop != nullptr) return true;

	auto* manager = BlurManager::instance();
	if (manager == nullptr || !manager->hostBackdropAvailable()) return false;
	auto* composition = manager->ensureComposition();
	if (composition == nullptr) return false;

	QString error;
	auto unsupported = false;
	this->backdrop =
	    BackdropWindow::create(composition->compositor, manager->debugTint(), error, unsupported);

	if (this->backdrop == nullptr) {
		if (unsupported) manager->markHostUnsupported(error);
		else qCWarning(logBlur).noquote() << "No blur for" << qs::windows::logId(this->panel->ns())
		                                  << ":" << error;
		return false;
	}

	this->backdrop->setPanel(this->panel->hwnd());
	this->shapes.clear();
	return true;
}

void PanelBlur::setInputMask(const QRegion& region, bool hasMask) {
	if (hasMask == this->hasMask && region == this->mask) return;

	auto changed = hasMask != this->hasMask;
	this->mask = region;
	this->hasMask = hasMask;
	if (changed) this->updateMode();
	else if (this->frameConnection) this->scheduleShapes();
}

void PanelBlur::scheduleShapes() {
	this->shapesForced = true;
	if (this->shapesPending) return;
	this->shapesPending = true;

	QTimer::singleShot(0, this, [this]() {
		this->shapesPending = false;
		if (this->frameConnection && framePending(this->mWindow.data())) return;
		this->updateShapes();
	});
}

bool PanelBlur::panelShown() const {
	auto* hwnd = this->panel->hwnd();
	return hwnd != nullptr && IsWindowVisible(hwnd) && !IsIconic(hwnd);
}

void PanelBlur::updateShapes() {
	auto* window = this->mWindow.data();
	if (this->mode != BlurMode::HostBackdrop || this->suspended || window == nullptr) return;

	if (!this->panelShown()) {
		this->syncPlacement();
		return;
	}

	auto inputs = ShapeInputs {
	    .size = window->size(),
	    .alpha = static_cast<qreal>(window->color().alphaF()),
	    .dpr = window->devicePixelRatio(),
	};
	if (!this->shapesForced && inputs == this->lastInputs
	    && !(this->rule.ignoreAlpha.has_value() && itemTreeDirty(window)))
	{
		this->syncPlacement();
		return;
	}

	this->shapesForced = false;
	this->lastInputs = inputs;

	QList<BlurShape> next;
	this->collectShapes(next);

	if (this->backdrop == nullptr) {
		if (next.isEmpty()) return;

		auto* manager = BlurManager::instance();
		if (manager != nullptr && manager->deferComposition()) return;

		if (!this->ensureBackdrop()) {
			this->setMode(BlurMode::Disabled);
			return;
		}
	}

	auto same = next.size() == this->shapes.size()
	         && std::equal(next.begin(), next.end(), this->shapes.begin(), [](const auto& a, const auto& b) {
		            return a.fuzzyEquals(b);
	            });

	if (!same) {
		if (!this->backdrop->setShapes(next)) {
			this->setMode(BlurMode::Disabled);
			return;
		}

		if (logBlur().isDebugEnabled()) {
			QStringList list;
			for (const auto& shape: next) {
				list.append(QString("%1x%2+%3+%4 r%5")
				                .arg(shape.rect.width())
				                .arg(shape.rect.height())
				                .arg(shape.rect.x())
				                .arg(shape.rect.y())
				                .arg(shape.radius));
			}

			qCDebug(logBlur).noquote() << "Blur shapes of" << qs::windows::logId(this->panel->ns()) << ":"
			                           << (list.isEmpty() ? "none" : list.join(", "));
		}

		this->shapes = std::move(next);
	}

	this->syncPlacement();
}

void PanelBlur::syncPlacement() {
	if (this->mode == BlurMode::Disabled || this->syncingSystemBackdrop) return;

	auto shown = !this->suspended && this->panelShown();
	if (this->mode == BlurMode::HostBackdrop) {
		if (shown) {
			this->connectFrames();
			if (this->backdrop != nullptr && !this->shapes.isEmpty()) this->backdrop->show();
		} else {
			this->disconnectFrames();
			if (this->backdrop != nullptr) this->backdrop->hide();
			this->shapes.clear();
		}
		return;
	}

	// An opaque Qt clear color would cover the native material.
	auto* window = this->mWindow.data();
	if (!shown || window == nullptr || window->color().alphaF() >= OPAQUE_ALPHA) {
		this->clearSystemBackdrop();
	} else {
		auto* hwnd = this->panel->hwnd();
		if (!this->applySystemBackdrop(hwnd)) {
			this->rejectedSystemHwnd = hwnd;
			this->updateMode();
		}
	}
}

void PanelBlur::collectShapes(QList<BlurShape>& shapes) {
	auto* window = this->mWindow.data();
	if (window == nullptr) return;

	auto query = BlurShapeQuery {
	    .ignoreAlpha = this->rule.ignoreAlpha,
	    .mask = this->mask,
	    .hasMask = this->hasMask,
	    .dpr = window->devicePixelRatio(),
	};

	auto result = collectBlurShapes(window, query);
	shapes = std::move(result.shapes);

	if (result.truncated && !this->truncatedWarned) {
		this->truncatedWarned = true;
		qCWarning(logBlur) << "Item tree of" << qs::windows::logId(this->panel->ns())
		                   << "is too large to walk every frame, blur only covers part of it.";
	}
}

BackdropBlur::BackdropBlur(QObject* parent): QObject(parent) {
	if (auto* manager = BlurManager::instance()) {
		// clang-format off
		QObject::connect(manager, &BlurManager::stateChanged, this, &BackdropBlur::stateChanged);
		QObject::connect(manager, &BlurManager::rulesChanged, this, &BackdropBlur::rulesChanged);
		// clang-format on
		QObject::connect(
		    manager,
		    &BlurManager::panelAnimationsChanged,
		    this,
		    &BackdropBlur::panelAnimationsChanged
		);
	}
}

BackdropBlur* BackdropBlur::create(QQmlEngine* engine, QJSEngine* /*jsEngine*/) {
	auto* blur = new BackdropBlur(nullptr);

	auto* manager = BlurManager::instance();
	auto* generation = EngineGeneration::findEngineGeneration(engine);
	if (manager != nullptr && generation != nullptr) manager->setShellDir(generation->rootPath.path());

	return blur;
}

bool BackdropBlur::enabled() {
	auto* manager = BlurManager::instance();
	return manager != nullptr && manager->enabled();
}

void BackdropBlur::setEnabled(bool enabled) {
	if (auto* manager = BlurManager::instance()) manager->setEnabled(enabled);
}

bool BackdropBlur::available() {
	auto* manager = BlurManager::instance();
	return manager != nullptr && manager->available();
}

bool BackdropBlur::panelAnimations() {
	auto* manager = BlurManager::instance();
	return manager == nullptr || manager->panelAnimations();
}

void BackdropBlur::setPanelAnimations(bool enabled) {
	if (auto* manager = BlurManager::instance()) manager->setPanelAnimations(enabled);
}

QString BackdropBlur::panelAnimationStyle() {
	auto* manager = BlurManager::instance();
	return manager != nullptr && manager->expressiveAnimations() ? "expressive" : "standard";
}

void BackdropBlur::setPanelAnimationStyle(const QString& style) {
	if (auto* manager = BlurManager::instance()) {
		auto expressive = style.trimmed().compare("expressive", Qt::CaseInsensitive) == 0;
		manager->setExpressiveAnimations(expressive);
	}
}

QString BackdropBlur::configPath() {
	auto* manager = BlurManager::instance();
	return manager != nullptr ? manager->configPath() : QString();
}

QVariantList BackdropBlur::rules() {
	auto* manager = BlurManager::instance();
	return manager != nullptr ? manager->rulesInfo() : QVariantList();
}

void BackdropBlur::reload() {
	if (auto* manager = BlurManager::instance()) manager->reload();
}

} // namespace qs::windows

#pragma once

#include <qnamespace.h>
#include <qobject.h>
#include <qproperty.h>
#include <qsize.h>
#include <qtmetamacros.h>

#include "proxywindow.hpp"
#include "windowinterface.hpp"
#ifdef _WIN32
#include "../windows/activation_policy.hpp"
#endif

// see #include <qpa/qplatformwindow.h>
static const int QWINDOWSIZE_MAX = ((1 << 24) - 1);

class ProxyFloatingWindow: public ProxyWindowBase {
	Q_OBJECT;

public:
	explicit ProxyFloatingWindow(QObject* parent = nullptr);

	void onReload(QObject* oldInstance) override;
	ProxiedWindow* createQQuickWindow() override;
	void connectWindow() override;
	void completeWindow() override;
	void postCompleteWindow() override;
	void setVisible(bool visible) override;
	void setVisibleDirect(bool visible) override;

	/// Bring the window to the front and request keyboard focus.
	void raiseAndActivate();

	[[nodiscard]] QObject* parentWindow() const;
	void setParentWindow(QObject* window);

	// Setting geometry while the window is visible makes the content item shrink but not the window
	// which is awful so we disable it for floating windows.
	void trySetWidth(qint32 implicitWidth) override;
	void trySetHeight(qint32 implicitHeight) override;

signals:
	void minimumSizeChanged();
	void maximumSizeChanged();
	void titleChanged();
	void framelessChanged();
	void parentWindowChanged();

private slots:
	void onParentDestroyed();

private:
	void onMinimumSizeChanged();
	void onMaximumSizeChanged();
	void onTitleChanged();
	void onFramelessChanged();
	void targetVisibleChanged();
	void applyFrameless();
#ifdef _WIN32
	void activateAs(qs::windows::ActivationSource source);

	// The dispatch that opened the window, kept for its first show.
	qs::windows::PendingActivation pendingOpen;
#endif

	QObject* mParentWindow = nullptr;

	Q_OBJECT_BINDABLE_PROPERTY(ProxyFloatingWindow, ProxyWindowBase*, bParentProxyWindow);
	Q_OBJECT_BINDABLE_PROPERTY_WITH_ARGS(ProxyFloatingWindow, bool, bWantsVisible, true);

	Q_OBJECT_BINDABLE_PROPERTY(
	    ProxyFloatingWindow,
	    bool,
	    bTargetVisible,
	    &ProxyFloatingWindow::targetVisibleChanged
	);

public:
	Q_OBJECT_BINDABLE_PROPERTY(
	    ProxyFloatingWindow,
	    QString,
	    bTitle,
	    &ProxyFloatingWindow::onTitleChanged
	);

	Q_OBJECT_BINDABLE_PROPERTY(
	    ProxyFloatingWindow,
	    QSize,
	    bMinimumSize,
	    &ProxyFloatingWindow::onMinimumSizeChanged
	);

	Q_OBJECT_BINDABLE_PROPERTY_WITH_ARGS(
	    ProxyFloatingWindow,
	    QSize,
	    bMaximumSize,
	    QSize(QWINDOWSIZE_MAX, QWINDOWSIZE_MAX),
	    &ProxyFloatingWindow::onMaximumSizeChanged
	);

	Q_OBJECT_BINDABLE_PROPERTY(
	    ProxyFloatingWindow,
	    bool,
	    bFrameless,
	    &ProxyFloatingWindow::onFramelessChanged
	);
};

///! Standard toplevel operating system window that looks like any other application.
class FloatingWindowInterface: public WindowInterface {
	Q_OBJECT;
	// clang-format off
	/// Window title.
	Q_PROPERTY(QString title READ default WRITE default NOTIFY titleChanged BINDABLE bindableTitle);
	/// Minimum window size given to the window system.
	Q_PROPERTY(QSize minimumSize READ default WRITE default NOTIFY minimumSizeChanged BINDABLE bindableMinimumSize);
	/// Maximum window size given to the window system.
	Q_PROPERTY(QSize maximumSize READ default WRITE default NOTIFY maximumSizeChanged BINDABLE bindableMaximumSize);
	/// Remove the system title bar and frame so the content fills the whole window. Defaults to false.
	///
	/// Only has an effect on Windows, where the window keeps its resize borders, Aero Snap,
	/// minimize and maximize, taskbar entry, shadow and rounded corners. Nothing is drawn for
	/// the title, so the content has to provide a way to close the window and an area that calls
	/// @@startSystemMove() when dragged.
	Q_PROPERTY(bool frameless READ default WRITE default NOTIFY framelessChanged BINDABLE bindableFrameless);
	/// Whether the window is currently minimized.
	Q_PROPERTY(bool minimized READ isMinimized WRITE setMinimized NOTIFY minimizedChanged);
	/// Whether the window is currently maximized.
	Q_PROPERTY(bool maximized READ isMaximized WRITE setMaximized NOTIFY maximizedChanged);
	/// Whether the window is currently fullscreen.
	Q_PROPERTY(bool fullscreen READ isFullscreen WRITE setFullscreen NOTIFY fullscreenChanged);
	/// The parent window of this window. Setting this makes the window a child of the parent,
	/// which affects window stacking behavior.
	///
	/// > [!NOTE] This property cannot be changed after the window is visible.
	Q_PROPERTY(QObject* parentWindow READ parentWindow WRITE setParentWindow NOTIFY parentWindowChanged);
	// clang-format on
	QML_NAMED_ELEMENT(FloatingWindow);

public:
	explicit FloatingWindowInterface(QObject* parent = nullptr);

	void onReload(QObject* oldInstance) override;

	[[nodiscard]] ProxyWindowBase* proxyWindow() const override;

	[[nodiscard]] QBindable<QSize> bindableMinimumSize() { return &this->window->bMinimumSize; }
	[[nodiscard]] QBindable<QSize> bindableMaximumSize() { return &this->window->bMaximumSize; }
	[[nodiscard]] QBindable<QString> bindableTitle() { return &this->window->bTitle; }
	[[nodiscard]] QBindable<bool> bindableFrameless() { return &this->window->bFrameless; }

	[[nodiscard]] bool isMinimized() const;
	void setMinimized(bool minimized);
	[[nodiscard]] bool isMaximized() const;
	void setMaximized(bool maximized);
	[[nodiscard]] bool isFullscreen() const;
	void setFullscreen(bool fullscreen);

	/// Start a system move operation. Must be called during a pointer press/drag.
	Q_INVOKABLE [[nodiscard]] bool startSystemMove() const;
	/// Start a system resize operation. Must be called during a pointer press/drag.
	Q_INVOKABLE [[nodiscard]] bool startSystemResize(Qt::Edges edges) const;

	/// On Windows, bring an existing window to the front and request keyboard
	/// focus. Other platforms leave focus and stacking to the compositor.
	///
	/// Use this when an already open window is requested again. Unlike changing
	/// @@visible, it neither recreates the window nor changes its content.
	Q_INVOKABLE void requestActivate();

	[[nodiscard]] QObject* parentWindow() const;
	void setParentWindow(QObject* window);

signals:
	void minimumSizeChanged();
	void maximumSizeChanged();
	void titleChanged();
	void framelessChanged();
	void minimizedChanged();
	void maximizedChanged();
	void fullscreenChanged();
	void parentWindowChanged();

private slots:
	void onWindowConnected();
	void onWindowStateChanged();

private:
	ProxyFloatingWindow* window;
	bool mMinimized = false;
	bool mMaximized = false;
	bool mFullscreen = false;
	bool mWasMinimized = false;
	bool mWasMaximized = false;
	bool mWasFullscreen = false;
};

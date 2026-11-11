#include "floatingwindow.hpp"

#include <qguiapplication.h>
#include <qnamespace.h>
#include <qobject.h>
#include <qqmlengine.h>
#include <qqmlinfo.h>
#include <qtmetamacros.h>
#include <qtypes.h>
#include <qwindow.h>

#include "../core/generation.hpp"
#include "proxywindow.hpp"
#include "windowinterface.hpp"
#ifdef _WIN32
#include "../windows/util.hpp"
#include "floatingframe.hpp"
#endif

ProxyFloatingWindow::ProxyFloatingWindow(QObject* parent): ProxyWindowBase(parent) {
	this->bTargetVisible.setBinding([this] {
		if (!this->bWantsVisible) return false;
		auto* parent = this->bParentProxyWindow.value();
		if (!parent) return true;
		return parent->bindableBackerVisibility().value();
	});

#ifdef _WIN32
	// A window created from QML while a key dispatch runs is that dispatch's open
	// request, but Reloadable shows it only after the dispatch has ended.
	this->pendingOpen.capture();
#endif
}

void ProxyFloatingWindow::onReload(QObject* oldInstance) {
	this->ProxyWindowBase::onReload(oldInstance);

#ifdef _WIN32
	// The first show happens inside the reload. Whatever it did not consume
	// belongs to an open that never showed and must not reach a later one.
	this->pendingOpen.clear();
#endif
}

void ProxyFloatingWindow::targetVisibleChanged() {
	if (this->window && this->bParentProxyWindow) {
		auto* bw = this->bParentProxyWindow.value()->backingWindow();

		if (bw != this->window->transientParent()) {
			this->window->setTransientParent(bw);
		}
	}

	this->ProxyWindowBase::setVisible(this->bTargetVisible);
}

ProxiedWindow* ProxyFloatingWindow::createQQuickWindow() {
#ifdef _WIN32
	return new FloatingFrameWindow(this);
#else
	return this->ProxyWindowBase::createQQuickWindow();
#endif
}

void ProxyFloatingWindow::connectWindow() {
	this->ProxyWindowBase::connectWindow();

	this->window->setTitle(this->bTitle);
	this->window->setMinimumSize(this->bMinimumSize);
	this->window->setMaximumSize(this->bMaximumSize);
	this->applyFrameless();
}

void ProxyFloatingWindow::completeWindow() {
	this->ProxyWindowBase::completeWindow();

	auto* parent = this->bParentProxyWindow.value();
	this->window->setTransientParent(parent ? parent->backingWindow() : nullptr);
}

void ProxyFloatingWindow::postCompleteWindow() {
	this->ProxyWindowBase::setVisible(this->bTargetVisible);
}

void ProxyFloatingWindow::onParentDestroyed() {
	this->mParentWindow = nullptr;
	this->bParentProxyWindow = nullptr;
	emit this->parentWindowChanged();
}

void ProxyFloatingWindow::setVisible(bool visible) { this->bWantsVisible = visible; }

void ProxyFloatingWindow::setVisibleDirect(bool visible) {
#ifdef _WIN32
	const bool wasVisible = this->isVisibleDirect();
#endif

	this->ProxyWindowBase::setVisibleDirect(visible);

#ifdef _WIN32
	// Windows has no compositor that raises and focuses a new toplevel, and
	// Qt's show (SW_SHOWNORMAL) only activates the window when the foreground
	// rules allow it. Ask for the foreground explicitly as soon as the native
	// window exists, which is the earliest a new window can be asked.
	//
	// The source decides the path: a WM_HOTKEY or a click on a shell surface
	// has the documented permission and asks once; a swallowed hook key, or an
	// IPC request with no input event, reports a refusal. There is deliberately
	// no event-loop retry: a second attempt after the dispatch would carry a
	// permission the shell may no longer have.
	//
	// A first show deferred by Reloadable runs after the dispatch, so it uses the
	// source captured when the open was requested, once.
	if (visible && !wasVisible) {
		auto source =
		    this->pendingOpen.take().value_or(qs::windows::activationSourceForCurrentState());
		this->activateAs(source);
	}
#endif
}

void ProxyFloatingWindow::raiseAndActivate() {
#ifdef _WIN32
	this->activateAs(qs::windows::activationSourceForCurrentState());
#endif
}

#ifdef _WIN32
void ProxyFloatingWindow::activateAs(qs::windows::ActivationSource source) {
	if (this->window == nullptr || !this->isVisibleDirect()) return;

	// Only the Windows QPA implements raise/activate here. The offscreen QPA
	// used by the Nexus smoke has no raise support and logs a warning.
	if (!QGuiApplication::platformName().startsWith(QStringLiteral("windows"))) return;

	this->window->raise();

	if (!qs::windows::requestForegroundWindow(qs::windows::hwndOf(this->window), source)) {
		qWarning() << "The foreground lock refused activation for the floating window";
	}
}
#endif

void ProxyFloatingWindow::trySetWidth(qint32 implicitWidth) {
	if (!this->window->isVisible()) {
		this->ProxyWindowBase::trySetWidth(implicitWidth);
	}
}

void ProxyFloatingWindow::trySetHeight(qint32 implicitHeight) {
	if (!this->window->isVisible()) {
		this->ProxyWindowBase::trySetHeight(implicitHeight);
	}
}

void ProxyFloatingWindow::onTitleChanged() {
	if (this->window) this->window->setTitle(this->bTitle);
	emit this->titleChanged();
}

void ProxyFloatingWindow::onFramelessChanged() {
	this->applyFrameless();
	emit this->framelessChanged();
}

void ProxyFloatingWindow::applyFrameless() {
#ifdef _WIN32
	if (!this->window) return;
	this->window->setFlag(Qt::FramelessWindowHint, this->bFrameless);
	FloatingFrameWindow::applyFrame(this->window);
#endif
}

void ProxyFloatingWindow::onMinimumSizeChanged() {
	if (this->window) this->window->setMinimumSize(this->bMinimumSize);
	emit this->minimumSizeChanged();
}

void ProxyFloatingWindow::onMaximumSizeChanged() {
	if (this->window) this->window->setMaximumSize(this->bMaximumSize);
	emit this->maximumSizeChanged();
}

QObject* ProxyFloatingWindow::parentWindow() const { return this->mParentWindow; }

void ProxyFloatingWindow::setParentWindow(QObject* window) {
	if (window == this->mParentWindow) return;

	if (this->window && this->window->isVisible()) {
		qmlWarning(this) << "parentWindow cannot be changed after the window is visible.";
		return;
	}

	if (this->bParentProxyWindow) {
		QObject::disconnect(this->bParentProxyWindow, nullptr, this, nullptr);
	}

	if (this->mParentWindow) {
		QObject::disconnect(this->mParentWindow, nullptr, this, nullptr);
	}

	this->mParentWindow = nullptr;
	this->bParentProxyWindow = nullptr;

	if (auto* proxy = ProxyWindowBase::forObject(window)) {
		this->mParentWindow = window;
		this->bParentProxyWindow = proxy;

		QObject::connect(
		    this->mParentWindow,
		    &QObject::destroyed,
		    this,
		    &ProxyFloatingWindow::onParentDestroyed
		);
	}

	emit this->parentWindowChanged();
}

// FloatingWindowInterface

FloatingWindowInterface::FloatingWindowInterface(QObject* parent)
    : WindowInterface(parent)
    , window(new ProxyFloatingWindow(this)) {
	this->connectSignals();

	// clang-format off
	QObject::connect(this->window, &ProxyFloatingWindow::titleChanged, this, &FloatingWindowInterface::titleChanged);
	QObject::connect(this->window, &ProxyFloatingWindow::framelessChanged, this, &FloatingWindowInterface::framelessChanged);
	QObject::connect(this->window, &ProxyFloatingWindow::minimumSizeChanged, this, &FloatingWindowInterface::minimumSizeChanged);
	QObject::connect(this->window, &ProxyFloatingWindow::maximumSizeChanged, this, &FloatingWindowInterface::maximumSizeChanged);
	QObject::connect(this->window, &ProxyFloatingWindow::parentWindowChanged, this, &FloatingWindowInterface::parentWindowChanged);
	QObject::connect(this->window, &ProxyWindowBase::windowConnected, this, &FloatingWindowInterface::onWindowConnected);
	// clang-format on
}

void FloatingWindowInterface::onReload(QObject* oldInstance) {
	QQmlEngine::setContextForObject(this->window, QQmlEngine::contextForObject(this));

	auto* old = qobject_cast<FloatingWindowInterface*>(oldInstance);
	this->window->reload(old != nullptr ? old->window : nullptr);
}

ProxyWindowBase* FloatingWindowInterface::proxyWindow() const { return this->window; }

void FloatingWindowInterface::onWindowConnected() {
	auto* qw = this->window->backingWindow();
	if (qw) {
		QObject::connect(
		    qw,
		    &QWindow::windowStateChanged,
		    this,
		    &FloatingWindowInterface::onWindowStateChanged
		);
		this->setMinimized(this->mMinimized);
		this->setMaximized(this->mMaximized);
		this->setFullscreen(this->mFullscreen);
		this->onWindowStateChanged();
	}
}

void FloatingWindowInterface::onWindowStateChanged() {
	auto* qw = this->window->backingWindow();
	auto states = qw ? qw->windowStates() : Qt::WindowStates();

	auto minimized = states.testFlag(Qt::WindowMinimized);
	auto maximized = states.testFlag(Qt::WindowMaximized);
	auto fullscreen = states.testFlag(Qt::WindowFullScreen);

	if (minimized != this->mWasMinimized) {
		this->mWasMinimized = minimized;
		emit this->minimizedChanged();
	}

	if (maximized != this->mWasMaximized) {
		this->mWasMaximized = maximized;
		emit this->maximizedChanged();
	}

	if (fullscreen != this->mWasFullscreen) {
		this->mWasFullscreen = fullscreen;
		emit this->fullscreenChanged();
	}
}

bool FloatingWindowInterface::isMinimized() const {
	auto* qw = this->window->backingWindow();
	if (!qw) return this->mWasMinimized;
	return qw->windowStates().testFlag(Qt::WindowMinimized);
}

void FloatingWindowInterface::setMinimized(bool minimized) {
	this->mMinimized = minimized;

	if (auto* qw = this->window->backingWindow()) {
		auto states = qw->windowStates();
		states.setFlag(Qt::WindowMinimized, minimized);
		qw->setWindowStates(states);
	}
}

bool FloatingWindowInterface::isMaximized() const {
	auto* qw = this->window->backingWindow();
	if (!qw) return this->mWasMaximized;
	return qw->windowStates().testFlag(Qt::WindowMaximized);
}

void FloatingWindowInterface::setMaximized(bool maximized) {
	this->mMaximized = maximized;

	if (auto* qw = this->window->backingWindow()) {
		auto states = qw->windowStates();
		states.setFlag(Qt::WindowMaximized, maximized);
		qw->setWindowStates(states);
	}
}

bool FloatingWindowInterface::isFullscreen() const {
	auto* qw = this->window->backingWindow();
	if (!qw) return this->mWasFullscreen;
	return qw->windowStates().testFlag(Qt::WindowFullScreen);
}

void FloatingWindowInterface::setFullscreen(bool fullscreen) {
	this->mFullscreen = fullscreen;

	if (auto* qw = this->window->backingWindow()) {
		auto states = qw->windowStates();
		states.setFlag(Qt::WindowFullScreen, fullscreen);
		qw->setWindowStates(states);
	}
}

bool FloatingWindowInterface::startSystemMove() const {
	auto* qw = this->window->backingWindow();
	if (!qw) return false;
	return qw->startSystemMove();
}

bool FloatingWindowInterface::startSystemResize(Qt::Edges edges) const {
	auto* qw = this->window->backingWindow();
	if (!qw) return false;
	return qw->startSystemResize(edges);
}

void FloatingWindowInterface::requestActivate() {
#ifdef _WIN32
	// createObject() has completed the QML object, but Reloadable normally waits
	// for the next event-loop turn to show its backer. Complete that first show
	// while the opening input dispatch/source panel still owns foreground rights.
	// Never bypass an in-progress generation reload, which may supply an old window.
	if (!this->reloadComplete && this->engineGeneration != nullptr
	    && this->engineGeneration->reloadComplete)
	{
		this->reload();
		// The first show already made its one source-aware activation request.
		return;
	}
#endif
	this->window->raiseAndActivate();
}

QObject* FloatingWindowInterface::parentWindow() const { return this->window->parentWindow(); }

void FloatingWindowInterface::setParentWindow(QObject* window) {
	this->window->setParentWindow(window);
}

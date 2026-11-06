#include "qmlscreen.hpp"

#include <qdebug.h>
#include <qlogging.h>
#include <qnamespace.h>
#include <qobject.h>
#include <qscreen.h>
#include <qtypes.h>

#ifdef Q_OS_WIN
#include "../windows/screen_identity.hpp"
#endif

QuickshellScreenInfo::QuickshellScreenInfo(
    QObject* parent,
    QScreen* screen
#ifdef Q_OS_WIN
    ,
    qs::windows::ScreenIdentityRegistry* identities
#endif
)
    : QObject(parent)
    , screen(screen)
#ifdef Q_OS_WIN
    , identities(identities ? identities : qs::windows::ScreenIdentityRegistry::instance())
#endif
{

	if (this->screen != nullptr) {
		// clang-format off
		QObject::connect(this->screen, &QScreen::geometryChanged, this, &QuickshellScreenInfo::geometryChanged);
		QObject::connect(this->screen, &QScreen::physicalDotsPerInchChanged, this, &QuickshellScreenInfo::physicalPixelDensityChanged);
		QObject::connect(this->screen, &QScreen::logicalDotsPerInchChanged, this, &QuickshellScreenInfo::logicalPixelDensityChanged);
		QObject::connect(this->screen, &QScreen::orientationChanged, this, &QuickshellScreenInfo::orientationChanged);
		QObject::connect(this->screen, &QScreen::primaryOrientationChanged, this, &QuickshellScreenInfo::primaryOrientationChanged);
		QObject::connect(this->screen, &QObject::destroyed, this, &QuickshellScreenInfo::screenDestroyed);
		QObject::connect(this->screen, &QScreen::geometryChanged, this, &QuickshellScreenInfo::devicePixelRatioChanged);
		QObject::connect(this->screen, &QScreen::logicalDotsPerInchChanged, this, &QuickshellScreenInfo::devicePixelRatioChanged);
		QObject::connect(this->screen, &QScreen::physicalDotsPerInchChanged, this, &QuickshellScreenInfo::devicePixelRatioChanged);
		// clang-format on
#ifdef Q_OS_WIN
		QObject::connect(
		    this->identities,
		    &qs::windows::ScreenIdentityRegistry::screenChanged,
		    this,
		    [this](QScreen* changed) {
			    if (changed != this->screen) return;
			    emit this->nameChanged();
			    emit this->geometryChanged();
			    emit this->devicePixelRatioChanged();
		    }
		);
#endif
	}
}

bool QuickshellScreenInfo::operator==(QuickshellScreenInfo& other) const {
	return this->screen == other.screen;
}

void QuickshellScreenInfo::warnDangling() const {
	if (this->dangling) {
		qWarning() << "attempted to use dangling screen object";
	}
}

QString QuickshellScreenInfo::name() const {
	if (this->screen == nullptr) {
		this->warnDangling();
		return "{ NULL SCREEN }";
	}

#ifdef Q_OS_WIN
	// Unique and stable even when two identical monitors share a friendly
	// label. Per-screen config and the QML API use this name; native routing
	// resolves it back through the identity registry.
	auto identity = this->identities->identity(this->screen);
	return identity.uniqueName.isEmpty() ? identity.friendlyName : identity.uniqueName;
#else
	return this->screen->name();
#endif
}

QString QuickshellScreenInfo::model() const {
	if (this->screen == nullptr) {
		this->warnDangling();
		return "{ NULL SCREEN }";
	}

	return this->screen->model();
}

QString QuickshellScreenInfo::serialNumber() const {
	if (this->screen == nullptr) {
		this->warnDangling();
		return "{ NULL SCREEN }";
	}

	return this->screen->serialNumber();
}

qint32 QuickshellScreenInfo::x() const {
	if (this->screen == nullptr) {
		this->warnDangling();
		return 0;
	}

	return this->screen->geometry().x();
}

qint32 QuickshellScreenInfo::y() const {
	if (this->screen == nullptr) {
		this->warnDangling();
		return 0;
	}

	return this->screen->geometry().y();
}

qint32 QuickshellScreenInfo::width() const {
	if (this->screen == nullptr) {
		this->warnDangling();
		return 0;
	}

	return this->screen->size().width();
}

qint32 QuickshellScreenInfo::height() const {
	if (this->screen == nullptr) {
		this->warnDangling();
		return 0;
	}

	return this->screen->size().height();
}

qreal QuickshellScreenInfo::physicalPixelDensity() const {
	if (this->screen == nullptr) {
		this->warnDangling();
		return 0.0;
	}

	return this->screen->physicalDotsPerInch() / 25.4;
}

qreal QuickshellScreenInfo::logicalPixelDensity() const {
	if (this->screen == nullptr) {
		this->warnDangling();
		return 0.0;
	}

	return this->screen->logicalDotsPerInch() / 25.4;
}

qreal QuickshellScreenInfo::devicePixelRatio() const {
	if (this->screen == nullptr) {
		this->warnDangling();
		return 0.0;
	}

	return this->screen->devicePixelRatio();
}

Qt::ScreenOrientation QuickshellScreenInfo::orientation() const {
	if (this->screen == nullptr) {
		this->warnDangling();
		return Qt::PrimaryOrientation;
	}

	return this->screen->orientation();
}

Qt::ScreenOrientation QuickshellScreenInfo::primaryOrientation() const {
	if (this->screen == nullptr) {
		this->warnDangling();
		return Qt::PrimaryOrientation;
	}

	return this->screen->primaryOrientation();
}

void QuickshellScreenInfo::screenDestroyed() {
	this->screen = nullptr;
	this->dangling = true;
}

QDebug operator<<(QDebug debug, const QuickshellScreenInfo* screen) {
	if (screen == nullptr) {
		debug.nospace() << "QuickshellScreenInfo(nullptr)";
		return debug;
	}

	debug.nospace() << screen->metaObject()->className() << '(' << static_cast<const void*>(screen)
	                << ", screen=" << screen->screen << ')';

	return debug;
}

QString QuickshellScreenInfo::toString() const {
	QString str;
	QDebug(&str) << this;
	return str;
}

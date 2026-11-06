#pragma once

#include <vector>

#include <qlist.h>
#include <qobject.h>
#include <qproperty.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qt_windows.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qvariantlist.h>

namespace qs::windows::sys {

// One input entry: a keyboard layout or a TSF text service (IME). `id` is the
// TSF profile identity for a text service and the HKL for a layout; `hkl` is
// the handle a switch carries and may be null for a text service. `code`
// is the locale name, and the two names are localized by Windows: `name` is
// the language/region display name, `shortName` is its short form, and
// `layoutName` is the keyboard layout's own displayed name (for example "US"
// or "United States-Dvorak"). Keeping all four distinguishes layouts that
// share a language, such as US QWERTY and US Dvorak.
struct KeyboardLayout {
	HKL hkl = nullptr;
	QString id;
	QString code;
	QString name;
	QString shortName;
	QString layoutName;
};

class KeyboardLayoutWatcher: public QObject {
	Q_OBJECT;

public:
	static KeyboardLayoutWatcher* instance();
	Q_DISABLE_COPY_MOVE(KeyboardLayoutWatcher);

	[[nodiscard]] HKL current() const { return this->mCurrent; }
	[[nodiscard]] const std::vector<HKL>& layouts() const { return this->mLayouts; }
	[[nodiscard]] bool capsLock() const { return this->mCapsLock; }
	[[nodiscard]] bool numLock() const { return this->mNumLock; }

	void check();

signals:
	void currentChanged();
	void layoutsChanged();
	void locksChanged();

private:
	explicit KeyboardLayoutWatcher(QObject* parent);

	HKL mCurrent = nullptr;
	std::vector<HKL> mLayouts;
	bool mCapsLock = false;
	bool mNumLock = false;
	QTimer pollTimer;
};

class Keyboard: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	// clang-format off
	Q_PROPERTY(QString currentLayoutCode READ default NOTIFY currentLayoutChanged BINDABLE bindableCurrentLayoutCode);
	Q_PROPERTY(QString currentLayoutName READ default NOTIFY currentLayoutChanged BINDABLE bindableCurrentLayoutName);
	Q_PROPERTY(int currentLayoutIndex READ default NOTIFY currentLayoutChanged BINDABLE bindableCurrentLayoutIndex);
	Q_PROPERTY(QVariantList layouts READ default NOTIFY layoutsChanged BINDABLE bindableLayouts);
	Q_PROPERTY(QStringList layoutCodes READ default NOTIFY layoutsChanged BINDABLE bindableLayoutCodes);
	Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged);
	Q_PROPERTY(bool capsLock READ capsLock NOTIFY locksChanged);
	Q_PROPERTY(bool numLock READ numLock NOTIFY locksChanged);
	// clang-format on

public:
	explicit Keyboard(QObject* parent = nullptr);
	~Keyboard() override;
	Q_DISABLE_COPY_MOVE(Keyboard);

	[[nodiscard]] bool active() const { return this->mActive; }
	void setActive(bool active);
	[[nodiscard]] bool capsLock() const {
		return this->mActive && KeyboardLayoutWatcher::instance()->capsLock();
	}
	[[nodiscard]] bool numLock() const {
		return this->mActive && KeyboardLayoutWatcher::instance()->numLock();
	}

	[[nodiscard]] QBindable<QString> bindableCurrentLayoutCode() const {
		return &this->bCurrentLayoutCode;
	}
	[[nodiscard]] QBindable<QString> bindableCurrentLayoutName() const {
		return &this->bCurrentLayoutName;
	}
	[[nodiscard]] QBindable<int> bindableCurrentLayoutIndex() const {
		return &this->bCurrentLayoutIndex;
	}
	[[nodiscard]] QBindable<QVariantList> bindableLayouts() const { return &this->bLayouts; }
	[[nodiscard]] QBindable<QStringList> bindableLayoutCodes() const { return &this->bLayoutCodes; }

	// Switches by position in `layouts`, so layouts that share a locale are
	// distinguishable. Returns "requested", "no-target", "denied" or
	// "unsupported" (an IME with no installed handle to switch through); requested
	// only means the message was posted, not that the target changed layout.
	// `activateLayout(code)` keeps its original first-match behavior for
	// callers that only know the language.
	Q_INVOKABLE QString activateLayoutAt(int index);
	Q_INVOKABLE void activateLayout(const QString& code);

	void refresh();

signals:
	void currentLayoutChanged();
	void layoutsChanged();
	void activeChanged();
	void locksChanged();

private:
	void initDeferred();
	void refreshLayoutList();

	bool mActive = true;
	QList<KeyboardLayout> mLayouts;

	// clang-format off
	Q_OBJECT_BINDABLE_PROPERTY(Keyboard, QString, bCurrentLayoutCode, &Keyboard::currentLayoutChanged);
	Q_OBJECT_BINDABLE_PROPERTY(Keyboard, QString, bCurrentLayoutName, &Keyboard::currentLayoutChanged);
	Q_OBJECT_BINDABLE_PROPERTY_WITH_ARGS(Keyboard, int, bCurrentLayoutIndex, -1, &Keyboard::currentLayoutChanged);
	Q_OBJECT_BINDABLE_PROPERTY(Keyboard, QVariantList, bLayouts, &Keyboard::layoutsChanged);
	Q_OBJECT_BINDABLE_PROPERTY(Keyboard, QStringList, bLayoutCodes, &Keyboard::layoutsChanged);
	// clang-format on
};

} // namespace qs::windows::sys

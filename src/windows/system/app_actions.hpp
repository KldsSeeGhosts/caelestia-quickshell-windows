#pragma once

#include <qhash.h>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>
#include <qvariant.h>

namespace qs::windows::sys {

struct AppActionInfo {
	QString token;
	QString name;
	QString target;
	QString packageFamilyName;
	QString executionAlias;
	bool packaged = false;
	bool canRunAsAdmin = false;
	bool canOpenLocation = false;
	bool canUninstall = false;
};

class AppActions: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
	enum class ActionResult : quint8 {
		Started,
		Cancelled,
		Refused,
		NotSupported,
	};
	Q_ENUM(ActionResult)

	explicit AppActions(QObject* parent = nullptr);

	[[nodiscard]] QString lastError() const { return this->mLastError; }

	Q_INVOKABLE QVariantMap info(const QString& entryId);
	Q_INVOKABLE bool runAsAdmin(const QString& entryId, QObject* parentWindow = nullptr);
	Q_INVOKABLE QVariantMap runAsAdminDetailed(const QString& entryId, QObject* parentWindow = nullptr);
	Q_INVOKABLE QVariantMap launchShellAction(const QVariantMap& spec, QObject* parentWindow = nullptr);
	Q_INVOKABLE bool isInlineSudoEnabled() const;
	Q_INVOKABLE bool openFileLocation(const QString& entryId);
	Q_INVOKABLE bool openAppSettings(const QString& entryId);
	Q_INVOKABLE bool uninstall(const QString& entryId);

signals:
	void lastErrorChanged();

private:
	AppActionInfo details(const QString& entryId);
	void setLastError(const QString& error);

	QHash<QString, AppActionInfo> mCache;
	QString mLastError;
};

} // namespace qs::windows::sys

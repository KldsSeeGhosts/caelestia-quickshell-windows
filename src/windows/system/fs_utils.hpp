#pragma once

#include <memory>
#include <vector>

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>
#include <qvariantlist.h>

namespace qs::windows::sys {

class FsUtils: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit FsUtils(QObject* parent = nullptr);
	~FsUtils() override;

	Q_INVOKABLE static QString classify(const QString& path);

	Q_INVOKABLE static bool isAccessibleDir(const QString& path);

	// Lists path on a worker thread and returns a request id. The result arrives
	// through listDirFinished on the GUI thread. Every request is answered.
	// Listings run on a small shared pool; requests for the same path that
	// arrive before its listing starts are answered by that one listing.
	// Each entry is { name, onDisk }; onDisk is false for a cloud placeholder
	// whose content is not local.
	Q_INVOKABLE int listDirAsync(const QString& path);

	Q_INVOKABLE static QString findExecutable(const QString& name);

	Q_INVOKABLE static bool makePath(const QString& path);

	Q_INVOKABLE static bool removeFile(const QString& path);

	Q_INVOKABLE static QString canonicalPath(const QString& path);

signals:
	void listDirFinished(int request, const QString& path, const QVariantList& entries);

private:
	struct Bridge;

	const std::shared_ptr<Bridge> m_bridge;
	int m_nextRequest = 1;

	static void deliver(
	    const std::shared_ptr<Bridge>& bridge,
	    FsUtils* owner,
	    const std::vector<int>& requests,
	    const QString& path,
	    const QVariantList& entries
	);
	static std::vector<int> takeWaiting(const std::shared_ptr<Bridge>& bridge, const QString& path);
};

} // namespace qs::windows::sys

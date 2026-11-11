#include "fs_utils.hpp"
#include "listing_pool.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>

#include <qt_windows.h>

#include <qcoreapplication.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qlist.h>
#include <qmetaobject.h>
#include <qstandardpaths.h>
#include <qvariant.h>
#include <qvariantlist.h>
#include <qvariantmap.h>

namespace qs::windows::sys {

namespace {

struct FoundEntry {
	QString name;
	bool onDisk = true;
};

// A cloud placeholder has a local directory entry but its content lives with
// the provider. It still belongs in the listing; the flag only tells the UI not
// to decode it until the user opens it.
bool isOnDisk(DWORD attributes) {
	constexpr DWORD cloudOnly =
	    FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
	return (attributes & cloudOnly) == 0;
}

QList<FoundEntry> listDirOnDisk(const QString& path) {
	auto entries = QList<FoundEntry>();
	if (path.isEmpty()) return entries;

	auto pattern = QDir::toNativeSeparators(path);
	if (!pattern.endsWith(QLatin1Char('\\')) && !pattern.endsWith(QLatin1Char('/'))) pattern += QLatin1Char('\\');
	pattern += QLatin1Char('*');

	WIN32_FIND_DATAW findData {};
	auto rawHandle = FindFirstFileExW(
	    reinterpret_cast<LPCWSTR>(pattern.utf16()),
	    FindExInfoBasic,
	    &findData,
	    FindExSearchNameMatch,
	    nullptr,
	    FIND_FIRST_EX_ON_DISK_ENTRIES_ONLY
	);
	if (rawHandle == INVALID_HANDLE_VALUE) return entries;

	struct FindHandle {
		HANDLE value = INVALID_HANDLE_VALUE;
		~FindHandle() {
			if (value != INVALID_HANDLE_VALUE) FindClose(value);
		}
	} handle {rawHandle};

	do {
		if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
		// QDir::Files without QDir::Hidden or QDir::System skipped these.
		if (findData.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
		entries.append({QString::fromWCharArray(findData.cFileName), isOnDisk(findData.dwFileAttributes)});
	} while (FindNextFileW(handle.value, &findData));

	std::sort(entries.begin(), entries.end(), [](const FoundEntry& a, const FoundEntry& b) {
		const auto byName = QString::compare(a.name, b.name, Qt::CaseInsensitive);
		return byName != 0 ? byName < 0 : a.name < b.name;
	});

	return entries;
}

} // namespace

// Joint ownership of the hand-off between a listing worker and the GUI thread.
// The receiver is touched only under the mutex, so a worker never posts to a
// destroyed object.
struct FsUtils::Bridge {
	std::mutex mutex;
	QObject* receiver = nullptr;
	int latestRequest = 0;
	std::shared_ptr<ListingPool::Slot> slot = std::make_shared<ListingPool::Slot>();

	void close() {
		std::lock_guard lock(mutex);
		receiver = nullptr;
	}
};

FsUtils::FsUtils(QObject* parent)
    : QObject(parent)
    , m_bridge(std::make_shared<Bridge>()) {
	// The delivery receiver lives and dies with this object on the GUI thread.
	m_bridge->receiver = new QObject(this);
	if (auto* app = QCoreApplication::instance()) {
		connect(app, &QCoreApplication::aboutToQuit, this, [bridge = m_bridge] { bridge->close(); });
	}
}

FsUtils::~FsUtils() { m_bridge->close(); }

QString FsUtils::classify(const QString& path) {
	auto wpath = path.toStdWString();
	auto attrs = GetFileAttributesW(wpath.c_str());
	if (attrs == INVALID_FILE_ATTRIBUTES) return QStringLiteral("invalid");
	return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? QStringLiteral("dir") : QStringLiteral("file");
}

bool FsUtils::isAccessibleDir(const QString& path) {
	if (path.isEmpty()) return false;

	auto wpath = path.toStdWString();
	auto attrs = GetFileAttributesW(wpath.c_str());
	if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) return false;

	auto pattern = wpath;
	if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') pattern += L'\\';
	pattern += L'*';

	WIN32_FIND_DATAW findData;
	auto handle = FindFirstFileExW(
	    pattern.c_str(),
	    FindExInfoBasic,
	    &findData,
	    FindExSearchNameMatch,
	    nullptr,
	    0
	);

	if (handle == INVALID_HANDLE_VALUE) return false;
	FindClose(handle);
	return true;
}

int FsUtils::listDirAsync(const QString& path) {
	const auto request = m_nextRequest++;
	auto bridge = m_bridge;
	auto* owner = this;
	{
		std::lock_guard lock(bridge->mutex);
		bridge->latestRequest = request;
	}

	try {
		static ListingPool pool;
		pool.submit(bridge->slot, [weak = std::weak_ptr(bridge), owner, request, path] {
			auto bridge = weak.lock();
			if (!bridge) return;
			{
				std::lock_guard lock(bridge->mutex);
				if (!bridge->receiver || bridge->latestRequest != request) return;
			}
			auto entries = QVariantList();
			for (const auto& entry: listDirOnDisk(path)) {
				QVariantMap map;
				map.insert(QStringLiteral("name"), entry.name);
				map.insert(QStringLiteral("onDisk"), entry.onDisk);
				entries.append(map);
			}
			deliver(bridge, owner, request, path, entries);
		});
	} catch (const std::system_error&) {
		// No worker could start: finish the request with an empty answer so a
		// caller never treats it as pending.
		deliver(bridge, owner, request, path, {});
	}

	return request;
}

void FsUtils::deliver(
    const std::shared_ptr<Bridge>& bridge,
    FsUtils* owner,
    int request,
    const QString& path,
    const QVariantList& entries
) {
	std::lock_guard lock(bridge->mutex);
	if (!bridge->receiver || bridge->latestRequest != request) return;
	QMetaObject::invokeMethod(
	    bridge->receiver,
	    [bridge, owner, request, path, entries] {
		    // Check again after the GUI queue, where a newer request may have arrived.
		    if (bridge->latestRequest == request)
			    emit owner->listDirFinished(request, path, entries);
	    },
	    Qt::QueuedConnection
	);
}

QString FsUtils::findExecutable(const QString& name) {
	if (name.isEmpty()) return QString();

	auto local = QFileInfo(QDir(QCoreApplication::applicationDirPath()), name);
	if (local.isFile()) return local.absoluteFilePath();

	return QStandardPaths::findExecutable(name);
}

bool FsUtils::makePath(const QString& path) {
	if (path.isEmpty()) return false;
	return QDir().mkpath(path);
}

bool FsUtils::removeFile(const QString& path) {
	auto info = QFileInfo(path);
	if (!info.exists()) return true;
	if (!info.isFile()) return false;
	if (!info.isWritable()) QFile::setPermissions(path, info.permissions() | QFileDevice::WriteOwner | QFileDevice::WriteUser);
	return QFile::remove(path);
}

QString FsUtils::canonicalPath(const QString& path) {
	if (path.isEmpty()) return QString();
	auto native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
	auto wide = native.toStdWString();
	auto size = GetLongPathNameW(wide.c_str(), nullptr, 0);
	if (size > 0) {
		auto buffer = std::wstring(size, L'\0');
		auto written = GetLongPathNameW(wide.c_str(), buffer.data(), size);
		if (written > 0 && written < size) native = QString::fromWCharArray(buffer.c_str(), static_cast<qsizetype>(written));
	}
	auto canonical = QFileInfo(native).canonicalFilePath();
	return canonical.isEmpty() ? QDir::fromNativeSeparators(native) : canonical;
}

} // namespace qs::windows::sys

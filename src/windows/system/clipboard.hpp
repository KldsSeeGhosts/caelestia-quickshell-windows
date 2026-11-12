#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <qobject.h>
#include <qproperty.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include "clipboard_capture.hpp"
#include "clipboard_write.hpp"

class QImage;
class QThreadPool;

namespace qs::windows::sys {

class ClipboardWriteController;

// Write-only singleton. IDs identify asynchronous outcomes, not acceptance.
class ClipboardWriter: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit ClipboardWriter(QObject* parent = nullptr);
	~ClipboardWriter() override;
	Q_DISABLE_COPY_MOVE(ClipboardWriter);

	Q_INVOKABLE int copyImageFile(const QString& path);
	Q_INVOKABLE int copyText(const QString& text);

signals:
	void writeFinished(int id, bool ok, const QString& error);

private:
	friend class ClipboardTestAccess;
	ClipboardWriter(std::shared_ptr<ClipboardWriteProvider> provider, QThreadPool* pool);
	int nextOperationId();
	int lastId = 0;
	std::unique_ptr<ClipboardWriteController> writer;
};

struct ClipboardEntry {
	qint64 id = 0;
	bool isImage = false;
	QString text;
	QString textPath;
	qsizetype textLength = 0;
	size_t textHash = 0;
	QString preview;
	QString imagePath;
	int width = 0;
	int height = 0;
};

struct ClipboardCapture {
	std::shared_ptr<ClipboardCaptureGeneration> generation;
	quint32 sequence = 0;
	bool captured = false;
	ClipboardEntry entry;
};

class Clipboard: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	Q_PROPERTY(QStringList entries READ default NOTIFY entriesChanged BINDABLE bindableEntries);
	Q_PROPERTY(bool historyEnabled READ historyEnabled WRITE setHistoryEnabled NOTIFY
	               historyEnabledChanged);

public:
	explicit Clipboard(QObject* parent = nullptr);
	~Clipboard() override;
	Q_DISABLE_COPY_MOVE(Clipboard);

	[[nodiscard]] QBindable<QStringList> bindableEntries() const { return &this->bEntries; }
	[[nodiscard]] bool historyEnabled() const { return this->mHistoryEnabled; }
	void setHistoryEnabled(bool enabled);

	Q_INVOKABLE void copy(qint64 id);
	Q_INVOKABLE void deleteEntry(qint64 id);
	Q_INVOKABLE void wipe();

	Q_INVOKABLE QString imagePath(qint64 id) const;

	Q_INVOKABLE bool copyImageFile(const QString& path);

	Q_INVOKABLE void copyText(const QString& text);

signals:
	void entriesChanged();
	void historyEnabledChanged();

private:
	friend class ClipboardTestAccess;
	// Non-QML construction seam. Does not register a listener, prune caches or
	// resolve a personal path. Only focused fixtures supply this boundary.
	Clipboard(
	    std::shared_ptr<ClipboardReadProvider> provider,
	    std::shared_ptr<ClipboardListenerProvider> listener,
	    std::shared_ptr<ClipboardWriteProvider> writeProvider,
	    const QString& directory,
	    QThreadPool* pool
	);

	void onClipboardUpdate();
	void startCapture();
	void finishCapture(const ClipboardCapture& capture);
	void rebuildEntriesProperty();
	void trimHistory();
	void captureText(ClipboardEntry entry);
	void captureImage(const ClipboardEntry& entry);
	void noteOwnWrite();
	QString cacheDir();

	std::vector<ClipboardEntry> storage;
	QString mCacheDir;
	qint64 nextId = 1;
	qint64 suppressNextCaptureFor = -1;
	bool capturing = false;
	bool captureAgain = false;
	quint32 capturedSequence = 0;
	quint32 writtenSequence = 0;
	quint32 suppressedSequence = 0;
	bool cleared = false;
	quint32 clearSequence = 0;
	std::shared_ptr<ClipboardReadProvider> readProvider;
	std::shared_ptr<ClipboardCaptureGeneration> captureGeneration;
	QThreadPool* readPool = nullptr;
	bool mHistoryEnabled = false;
	bool production = true;
	std::shared_ptr<ClipboardListenerProvider> listener;
	std::unique_ptr<QObject> listenerContext;
	std::unique_ptr<ClipboardWriteController> writer;

	Q_OBJECT_BINDABLE_PROPERTY(Clipboard, QStringList, bEntries, &Clipboard::entriesChanged);
};

} // namespace qs::windows::sys

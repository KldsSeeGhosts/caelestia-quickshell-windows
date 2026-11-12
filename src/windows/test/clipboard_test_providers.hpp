#pragma once

#include <utility>

#include <qimage.h>
#include <qpointer.h>
#include <qset.h>

#include "../system/clipboard_write.hpp"

namespace qs::windows::sys {

class FakeClipboardListener final: public ClipboardListenerProvider {
public:
	int starts = 0;
	int stops = 0;
	bool allow = true;
	QPointer<QObject> context;
	std::function<void()> update;

	bool start(QObject* context, std::function<void()> update) override {
		++this->starts;
		if (!this->allow) return false;
		this->context = context;
		this->update = std::move(update);
		return true;
	}
	void stop() override { ++this->stops; }
	void notify() {
		if (this->context) this->update();
	}
};

class FakeClipboardWrite final: public ClipboardWriteProvider {
public:
	bool allocate = true;
	bool allowOpen = true;
	bool allowEmpty = true;
	bool allowSet = true;
	int failedOpens = 0;
	int opens = 0;
	int empties = 0;
	int sets = 0;
	int closes = 0;
	int frees = 0;
	int transfers = 0;
	int sequences = 0;
	QImage image;
	QString text;
	QSet<void*> owned;

	~FakeClipboardWrite() override {
		if (!this->owned.isEmpty()) qFatal("Clipboard fixture leaked caller-owned write data");
	}
	void* imageData(const QImage& image) override {
		this->image = image;
		return this->data();
	}
	void* textData(const QString& text) override {
		this->text = text;
		return this->data();
	}
	void freeData(void* data) override {
		if (!this->owned.remove(data)) qFatal("Clipboard fixture freed transferred or unknown data");
		delete static_cast<int*>(data);
		++this->frees;
	}
	bool open() override {
		++this->opens;
		return this->allowOpen && this->opens > this->failedOpens;
	}
	bool empty() override {
		++this->empties;
		return this->allowEmpty;
	}
	bool set(quint32, void* data) override {
		++this->sets;
		if (!this->allowSet) return false;
		// Model the OS taking ownership. No Win32 clipboard calls exist here.
		if (!this->owned.remove(data)) qFatal("Clipboard fixture transferred unknown data");
		delete static_cast<int*>(data);
		++this->transfers;
		return true;
	}
	void close() override { ++this->closes; }
	quint32 sequence() override {
		++this->sequences;
		return 500;
	}

private:
	void* data() {
		if (!this->allocate) return nullptr;
		auto* data = new int(1);
		this->owned.insert(data);
		return data;
	}
};

} // namespace qs::windows::sys

#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include <qlist.h>
#include <qobject.h>
#include <qstring.h>
#include <qt_windows.h>

namespace qs::windows::sys::wallpaper_worker {

enum class Kind { Read, ReadScreen, Set, SetScreen, SetDesktops, Background };

struct DesktopPath {
	GUID id {};
	QString path;
};

struct Request {
	Kind kind = Kind::Read;
	QString path;
	QString target;
	QList<DesktopPath> desktops;
	bool animate = true;
};

struct Result {
	bool ok = false;
	QString path;
	bool disconnected = false;
};

class Provider {
public:
	virtual ~Provider() = default;
	virtual Result execute(const Request& request) = 0;
};

// The factory, provider, and its destructor run only in the worker apartment.
using Factory = std::function<std::unique_ptr<Provider>()>;

class Channel {
public:
	explicit Channel(Factory factory, QObject* context);
	~Channel();
	Channel(const Channel&) = delete;
	Channel& operator=(const Channel&) = delete;

	QString read(Request request);
	bool command(Request request);
	void background(Request request);
	void invalidate();
	void shutdown();

	// Delivered on the GUI, once per accepted mutation, including retirement.
	std::function<void(const Request&, bool, const QString&)> finished;

#ifdef QS_TEST
	void setStuckBound(std::chrono::milliseconds bound);
	static int liveCount();
#endif

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace qs::windows::sys::wallpaper_worker

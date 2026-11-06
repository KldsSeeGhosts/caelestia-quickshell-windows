// Standalone diagnostic using the same Brightness controller as the shell.
// It never creates shell surfaces or changes desktop/work-area state.
#include <functional>
#include <vector>
#include <qt_windows.h>
#include <qdir.h>
#include <qeventloop.h>
#include <qfile.h>
#include <qguiapplication.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qlockfile.h>
#include <qsavefile.h>
#include <qscreen.h>
#include <qscreen_platform.h>
#include <qtextstream.h>
#include <qtimer.h>

#include "../system/brightness.hpp"
#include "../system/brightness_backend.hpp"
#include "../startup.hpp"
#include "../util.hpp"

namespace qs::windows {
HMONITOR monitorForScreen(QScreen* screen) {
	auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
	return native ? native->handle() : nullptr;
}
}
namespace qs::windows::startup {
bool settled() { return true; }
void afterFirstFrame(QObject*, std::function<void()> callback) { callback(); }
}
namespace qs::log {
void initLogCategoryLevel(const char*, QtMsgType) {}
}

struct Path {
	QString source;
	QString name;
	QString identity;
	LUID adapter {};
	UINT32 id = 0;
	bool hdrKnown = false;
	bool hdr = false;
	bool whiteKnown = false;
	UINT32 white = 0;
};

static LONG readWhite(const Path& path, UINT32& raw) {
	DISPLAYCONFIG_SDR_WHITE_LEVEL info {};
	info.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof(info), path.adapter, path.id};
	auto rc = DisplayConfigGetDeviceInfo(&info.header);
	if (rc == ERROR_SUCCESS) raw = info.SDRWhiteLevel;
	return rc;
}

static LONG restoreWhite(const Path& path, UINT32 raw) {
	struct SetWhite { DISPLAYCONFIG_DEVICE_INFO_HEADER header; UINT32 level; BYTE finalValue; } info {};
	info.header = {static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(0xFFFFFFEEu), sizeof(info), path.adapter, path.id};
	info.level = raw;
	info.finalValue = 1;
	return DisplayConfigSetDeviceInfo(&info.header);
}

static bool paths(std::vector<Path>& result) {
	for (int attempt = 0; attempt < 3; ++attempt) {
		UINT32 count = 0, modesCount = 0;
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &count, &modesCount)) return false;
		std::vector<DISPLAYCONFIG_PATH_INFO> native(count);
		std::vector<DISPLAYCONFIG_MODE_INFO> modes(modesCount);
		auto rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &count, native.data(), &modesCount, modes.data(), nullptr);
		if (rc == ERROR_INSUFFICIENT_BUFFER) continue;
		if (rc != ERROR_SUCCESS) return false;
		for (UINT32 i = 0; i < count; ++i) {
			auto& p = native[i];
			Path path;
			path.adapter = p.targetInfo.adapterId;
			path.id = p.targetInfo.id;
			DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
			source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), p.sourceInfo.adapterId, p.sourceInfo.id};
			DISPLAYCONFIG_TARGET_DEVICE_NAME target {};
			target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target), path.adapter, path.id};
			if (DisplayConfigGetDeviceInfo(&source.header) || DisplayConfigGetDeviceInfo(&target.header)) return false;
			path.source = QString::fromWCharArray(source.viewGdiDeviceName);
			path.name = QString::fromWCharArray(target.monitorFriendlyDeviceName);
			path.identity = QString::fromWCharArray(target.monitorDevicePath);
			struct Color2 { DISPLAYCONFIG_DEVICE_INFO_HEADER header; UINT32 flags, encoding, bits, mode; } color2 {};
			color2.header = {static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(15), sizeof(color2), path.adapter, path.id};
			if (DisplayConfigGetDeviceInfo(&color2.header) == ERROR_SUCCESS) {
				path.hdrKnown = true;
				path.hdr = qs::windows::sys::isHdrColorMode(color2.mode);
			} else {
				DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color {};
				color.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO, sizeof(color), path.adapter, path.id};
				if (DisplayConfigGetDeviceInfo(&color.header) == ERROR_SUCCESS) {
					path.hdrKnown = true;
					path.hdr = color.advancedColorEnabled && !color.wideColorEnforced;
				}
			}
			path.whiteKnown = readWhite(path, path.white) == ERROR_SUCCESS;
			result.push_back(path);
		}
		return true;
	}
	return false;
}

int main(int argc, char** argv) {
	QGuiApplication app(argc, argv);
	QTextStream out(stdout), err(stderr);
	QString display;
	int set = -1;
	bool restore = false;
	const auto args = app.arguments();
	for (int i = 1; i < args.size(); ++i) {
		if (args[i].compare("-Display", Qt::CaseInsensitive) == 0 && i + 1 < args.size()) display = args[++i];
		else if (args[i].compare("-Set", Qt::CaseInsensitive) == 0 && i + 1 < args.size()) {
			bool ok = false;
			set = args[++i].toInt(&ok);
			if (!ok || set < 0 || set > 100) { err << "Set must be an integer from 0 to 100\n"; return 2; }
		} else if (args[i].compare("-Restore", Qt::CaseInsensitive) == 0) restore = true;
		else { err << "Usage: brightness-probe [-Display name -Set 0..100 | -Display name -Restore]\n"; return 2; }
	}
	if ((set >= 0 && restore) || ((set >= 0 || restore) && display.isEmpty())) {
		err << "Specify one of -Set or -Restore and an exact -Display name.\n"; return 2;
	}
	std::vector<Path> all;
	if (!paths(all)) { err << "Cannot enumerate active display paths\n"; return 1; }
	qs::windows::sys::Brightness brightness;
	brightness.setStrictRoutes(true);
	int matches = 0;
	for (const auto& path: all)
		if (path.source.compare(display, Qt::CaseInsensitive) == 0 || path.name.compare(display, Qt::CaseInsensitive) == 0) ++matches;
	if (!display.isEmpty() && matches != 1) { err << "Display name must identify exactly one active path\n"; return 1; }
	for (const auto& path: all) {
		QString screenName;
		for (auto* screen: QGuiApplication::screens()) {
			MONITORINFOEXW info {};
			info.cbSize = sizeof(info);
			auto monitor = qs::windows::monitorForScreen(screen);
			if (monitor && GetMonitorInfoW(monitor, &info)
			    && path.source.compare(QString::fromWCharArray(info.szDevice), Qt::CaseInsensitive) == 0) {
				screenName = qs::windows::screenName(screen);
				break;
			}
		}
		if (screenName.isEmpty()) { err << "No Qt screen for " << path.source << '\n'; return 1; }
		QEventLoop loop;
		QTimer timer;
		timer.setSingleShot(true);
		bool queried = false;
		QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
		auto queryConnection = QObject::connect(&brightness, &qs::windows::sys::Brightness::queried, &loop,
		    [&](const QString& name, bool, bool, qreal) { if (name == screenName) { queried = true; loop.quit(); } });
		timer.start(20000);
		brightness.query(screenName);
		loop.exec();
		QObject::disconnect(queryConnection);
		if (!queried) { err << "Route query timed out for " << path.source << '\n'; return 1; }
		out << path.source << " | " << path.name << " | HDR=" << (path.hdrKnown ? (path.hdr ? "active" : "off") : "unknown")
		    << " | SDR-white=" << (path.whiteKnown ? QString::number(path.white) + " raw / " + QString::number(path.white * 0.08) + " nits" : "unavailable")
		    << " | route=" << brightness.methodFor(screenName) << '\n';
		if (display.isEmpty() || (path.source.compare(display, Qt::CaseInsensitive) && path.name.compare(display, Qt::CaseInsensitive))) continue;
		if (set < 0 && !restore) continue;
		if (!path.hdrKnown || !path.hdr || !path.whiteKnown || path.identity.isEmpty()
		    || brightness.methodFor(screenName) != "sdr-white") {
			err << "Writes require one active HDR path with a readable SDR white level.\n"; return 1;
		}
		const auto journalPath = QDir::temp().filePath("caelestia-brightness-sdr-white-journal.json");
		QLockFile lock(journalPath + ".lock");
		if (!lock.tryLock(1000)) { err << "Cannot lock journal\n"; return 1; }
		QJsonObject journal;
		QFile existing(journalPath);
		if (existing.exists()) {
			if (!existing.open(QIODevice::ReadOnly)) { err << "Cannot read journal\n"; return 1; }
			QJsonParseError parse;
			auto doc = QJsonDocument::fromJson(existing.readAll(), &parse);
			if (parse.error != QJsonParseError::NoError || !doc.isObject()) { err << "Invalid journal\n"; return 1; }
			journal = doc.object();
		}
		if (restore) {
			if (!journal.contains(path.identity) || !journal.value(path.identity).isDouble()) {
				err << "No original value recorded for this display\n"; return 1;
			}
			auto raw = UINT32(journal.value(path.identity).toInt());
			auto rc = restoreWhite(path, raw);
			UINT32 actual = 0;
			if (rc || readWhite(path, actual) || actual != raw) {
				err << "Restore or exact read-back failed; journal retained. Windows error " << rc << '\n'; return 1;
			}
			journal.remove(path.identity);
			QSaveFile save(journalPath);
			if (!save.open(QIODevice::WriteOnly) || save.write(QJsonDocument(journal).toJson()) < 0 || !save.commit()) {
				err << "Restored, but could not update journal\n"; return 1;
			}
			out << "Restored and confirmed raw=" << actual << '\n';
		} else {
			if (!journal.contains(path.identity)) {
				journal.insert(path.identity, int(path.white));
				QSaveFile save(journalPath);
				if (!save.open(QIODevice::WriteOnly) || save.write(QJsonDocument(journal).toJson()) < 0 || !save.commit()) {
					err << "Cannot record original value; no write performed\n"; return 1;
				}
			}
			bool finished = false, success = false;
			QString failure;
			auto writeConnection = QObject::connect(&brightness, &qs::windows::sys::Brightness::brightnessWriteResult, &loop,
			    [&](const QString& name, qreal, bool ok, const QString&, const QString& error) {
				    if (name == screenName) { finished = true; success = ok; failure = error; loop.quit(); }
			    });
			timer.start(20000);
			brightness.setBrightness(screenName, false, set / 100.0);
			loop.exec();
			QObject::disconnect(writeConnection);
			UINT32 actual = 0;
			if (!finished || !success || readWhite(path, actual)) {
				err << "Set failed or read-back unavailable: " << failure << '\n'; return 1;
			}
			out << "Confirmed raw=" << actual << " nits=" << actual * 0.08 << '\n';
		}
	}
	return 0;
}

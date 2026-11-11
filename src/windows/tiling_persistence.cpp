#include "tiling_persistence.hpp"
#include <algorithm>
#include <cmath>
#include <functional>

#include <qcryptographichash.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qsavefile.h>

#include "tiling_layout.hpp"

namespace qs::windows::tiling {
namespace {
constexpr qsizetype MAX_WINDOWS = 4096;
constexpr qint64 MAX_BYTES = 4 * 1024 * 1024;

QString number(quint64 value) { return QString::number(value, 16); }
quint64 integer(const QJsonValue& value) { return value.toString().toULongLong(nullptr, 16); }
double bounded(const QJsonValue& value, double fallback, double low, double high) {
	auto result = value.toDouble(fallback);
	return std::isfinite(result) ? qBound(low, result, high) : fallback;
}

bool sameProcess(const WindowIdentity& a, const WindowIdentity& b) {
	return a.pid != 0 && a.started != 0 && a.pid == b.pid && a.started == b.started;
}
} // namespace

QString persistenceHint(const QString& value) {
	if (value.isEmpty()) return {};
	return QString::fromLatin1(
	    QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex()
	);
}

RestorePlan planRestore(
    const QList<SavedWindow>& saved,
    const QList<WindowIdentity>& current,
    const QSet<QString>& monitors,
    const QSet<QString>& desktops
) {
	RestorePlan plan;
	QSet<qsizetype> used;
	QSet<qsizetype> restored;
	auto available = [&](const SavedWindow& entry) {
		return monitors.contains(entry.monitor) && desktops.contains(entry.desktop);
	};
	// Exact matches reserve their windows before any hint matching.
	for (qsizetype s = 0; s < saved.size(); ++s) {
		const auto& entry = saved.at(s);
		if (!available(entry)) continue;
		for (qsizetype c = 0; c < current.size(); ++c) {
			const auto& live = current.at(c);
			if (!used.contains(c) && entry.identity.hwnd != 0
			    && entry.identity.hwnd == live.hwnd && sameProcess(entry.identity, live))
			{
				plan.matched.append({s, c});
				used.insert(c);
				restored.insert(s);
				break;
			}
		}
	}
	for (qsizetype s = 0; s < saved.size(); ++s) {
		const auto& entry = saved.at(s);
		const auto& old = entry.identity;
		if (restored.contains(s) || !available(entry) || old.app.isEmpty() || old.title.isEmpty())
			continue;
		// A reused handle is evidence against a fallback, even if hints match.
		bool handleExists = false;
		for (const auto& live: current) handleExists |= live.hwnd == old.hwnd;
		if (handleExists) continue;
		qsizetype candidate = -1;
		int candidates = 0;
		for (qsizetype c = 0; c < current.size(); ++c) {
			const auto& live = current.at(c);
			if (!used.contains(c) && sameProcess(old, live)
			    && old.app == live.app && old.title == live.title)
			{
				candidate = c;
				++candidates;
			}
		}
		int hints = 0;
		for (const auto& other: saved) {
			if (sameProcess(old, other.identity) && old.app == other.identity.app
			    && old.title == other.identity.title) ++hints;
		}
		if (candidates == 1 && hints == 1) {
			plan.matched.append({s, candidate});
			used.insert(candidate);
		}
	}
	std::sort(plan.matched.begin(), plan.matched.end(), [](auto a, auto b) {
		return a.saved < b.saved;
	});
	for (qsizetype c = 0; c < current.size(); ++c) {
		if (!used.contains(c)) plan.unmatched.append(c);
	}
	return plan;
}

QJsonObject encodeSnapshot(const LayoutSnapshot& snapshot) {
	QJsonArray windows;
	for (const auto& w: snapshot.windows) {
		windows.append(QJsonObject {
		    {"hwnd", number(w.identity.hwnd)}, {"pid", number(w.identity.pid)},
		    {"started", number(w.identity.started)}, {"app", w.identity.app},
		    {"title", w.identity.title}, {"monitor", w.monitor}, {"desktop", w.desktop},
		    {"override", w.override}, {"floating", w.floating}, {"autoFloat", w.autoFloat},
		});
	}
	QJsonArray layouts;
	for (const auto& l: snapshot.layouts) {
		layouts.append(QJsonObject {
		    {"monitor", l.monitor}, {"desktop", l.desktop}, {"kind", l.kind}, {"tree", l.tree},
		});
	}
	return {{"version", 1}, {"windows", windows}, {"layouts", layouts}, {"options", snapshot.options}};
}

LayoutSnapshot decodeSnapshot(const QJsonObject& object) {
	LayoutSnapshot result;
	if (object.value("version").toInt() != 1) return result;
	result.options = object.value("options").toObject();
	auto windows = object.value("windows").toArray();
	auto layouts = object.value("layouts").toArray();
	if (windows.size() > MAX_WINDOWS || layouts.size() > MAX_WINDOWS) return result;
	for (const auto& value: windows) {
		auto w = value.toObject();
		WindowIdentity identity {
		    static_cast<quintptr>(integer(w.value("hwnd"))),
		    static_cast<quint32>(integer(w.value("pid"))), integer(w.value("started")),
		    w.value("app").toString(), w.value("title").toString(),
		};
		if (identity.hwnd == 0 || identity.pid == 0 || identity.started == 0) continue;
		result.windows.append({
		    identity, w.value("monitor").toString(), w.value("desktop").toString(),
		    qBound(0, w.value("override").toInt(), 2), w.value("floating").toBool(),
		    w.value("autoFloat").toBool(),
		});
	}
	for (const auto& value: layouts) {
		auto l = value.toObject();
		result.layouts.append({
		    l.value("monitor").toString(), l.value("desktop").toString(),
		    l.value("kind").toInt(-1), l.value("tree").toObject(),
		});
	}
	return result;
}

LayoutSnapshot readSnapshot(const QString& path) {
	QFile file(path);
	if (path.isEmpty() || !file.open(QIODevice::ReadOnly) || file.size() > MAX_BYTES) return {};
	return decodeSnapshot(QJsonDocument::fromJson(file.readAll()).object());
}

bool writeSnapshot(const QString& path, const LayoutSnapshot& snapshot) {
	if (path.isEmpty()) return true; // Fixtures can use memory only.
	if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
	QSaveFile file(path);
	// Do not allow QSaveFile's non-atomic direct-write fallback.
	file.setDirectWriteFallback(false);
	if (!file.open(QIODevice::WriteOnly)) return false;
	auto bytes = QJsonDocument(encodeSnapshot(snapshot)).toJson(QJsonDocument::Compact);
	return bytes.size() <= MAX_BYTES && file.write(bytes) == bytes.size() && file.commit();
}

QJsonObject LayoutEngine::saveState(const QHash<Id, Id>& ids) const {
	auto id = [&](Id value) { return number(ids.value(value)); };
	if (this->kind() == Kind::Dwindle) {
		const auto& layout = static_cast<const DwindleLayout&>(*this);
		std::function<QJsonObject(const DwindleLayout::Node*)> save = [&](const auto* node) {
			if (node == nullptr) return QJsonObject();
			if (node->isLeaf()) return QJsonObject {{"id", id(node->id)}};
			return QJsonObject {
			    {"first", save(node->first.get())}, {"second", save(node->second.get())},
			    {"top", node->splitTop}, {"pinned", node->splitPinned}, {"ratio", node->ratio},
			};
		};
		return {{"root", save(layout.root.get())}};
	}
	if (this->kind() == Kind::Master) {
		const auto& layout = static_cast<const MasterLayout&>(*this);
		QJsonArray entries;
		for (const auto& e: layout.entries) {
			entries.append(QJsonObject {{"id", id(e.id)}, {"weight", e.weight}});
		}
		return {{"entries", entries}, {"factor", layout.mfact}, {"configured", layout.configuredMfact}};
	}
	const auto& layout = static_cast<const ScrollingLayout&>(*this);
	QJsonArray columns;
	for (const auto& c: layout.columns) {
		QJsonArray windows;
		for (const auto& w: c.windows) {
			windows.append(QJsonObject {{"id", id(w.id)}, {"weight", w.weight}});
		}
		columns.append(QJsonObject {
		    {"windows", windows}, {"width", c.width}, {"restoreWidth", c.restoreWidth},
		    {"focused", id(c.focused)},
		});
	}
	return {{"columns", columns}, {"active", id(layout.activeId)}, {"offset", layout.offset}};
}

std::unique_ptr<LayoutEngine> LayoutEngine::restoreState(
    Kind kind, const QJsonObject& state, const QHash<Id, Id>& ids
) {
	auto result = create(kind);
	QSet<Id> used;
	auto id = [&](const QJsonValue& value) {
		auto mapped = ids.value(static_cast<Id>(integer(value)));
		if (mapped == 0 || used.contains(mapped)) return Id(0);
		used.insert(mapped);
		return mapped;
	};
	if (kind == Kind::Dwindle) {
		auto& layout = static_cast<DwindleLayout&>(*result);
		int nodes = 0;
		std::function<std::unique_ptr<DwindleLayout::Node>(const QJsonObject&, int)> load;
		load = [&](const QJsonObject& value, int depth) -> std::unique_ptr<DwindleLayout::Node> {
			if (value.isEmpty() || depth > 128 || ++nodes > MAX_WINDOWS * 2) return nullptr;
			auto node = std::make_unique<DwindleLayout::Node>();
			if (value.contains("id")) {
				node->id = id(value.value("id"));
				return node->id == 0 ? nullptr : std::move(node);
			}
			node->first = load(value.value("first").toObject(), depth + 1);
			node->second = load(value.value("second").toObject(), depth + 1);
			if (!node->first) return std::move(node->second);
			if (!node->second) return std::move(node->first);
			node->first->parent = node.get();
			node->second->parent = node.get();
			node->splitTop = value.value("top").toBool();
			node->splitPinned = value.value("pinned").toBool();
			node->ratio = bounded(value.value("ratio"), 0.5, 0.05, 0.95);
			return node;
		};
		layout.root = load(state.value("root").toObject(), 0);
	} else if (kind == Kind::Master) {
		auto& layout = static_cast<MasterLayout&>(*result);
		for (const auto& value: state.value("entries").toArray()) {
			if (used.size() >= MAX_WINDOWS) break;
			auto entry = value.toObject();
			auto mapped = id(entry.value("id"));
			if (mapped != 0) layout.entries.append({
			    mapped, bounded(entry.value("weight"), 1.0, 0.0, MAX_WINDOWS), {},
			});
		}
		layout.mfact = bounded(state.value("factor"), 0.55, 0.05, 0.95);
		layout.configuredMfact = bounded(state.value("configured"), -1.0, -1.0, 1.0);
	} else {
		auto& layout = static_cast<ScrollingLayout&>(*result);
		for (const auto& value: state.value("columns").toArray()) {
			if (used.size() >= MAX_WINDOWS) break;
			auto column = value.toObject();
			ScrollingLayout::Column c;
			c.width = bounded(column.value("width"), 0.5, 0.1, 1.0);
			c.restoreWidth = bounded(column.value("restoreWidth"), 0.0, 0.0, 1.0);
			for (const auto& w: column.value("windows").toArray()) {
				auto window = w.toObject();
				auto mapped = id(window.value("id"));
				if (mapped != 0) c.windows.append({
				    mapped, bounded(window.value("weight"), 1.0, 0.0, MAX_WINDOWS),
				});
			}
			if (c.windows.isEmpty()) continue;
			c.focused = ids.value(static_cast<Id>(integer(column.value("focused"))));
			bool found = false;
			for (const auto& w: c.windows) found |= w.id == c.focused;
			if (!found) c.focused = c.windows.first().id;
			layout.columns.append(c);
		}
		layout.activeId = ids.value(static_cast<Id>(integer(state.value("active"))));
		if (!used.contains(layout.activeId)) layout.activeId = used.isEmpty() ? 0 : layout.ids().first();
		layout.offset = qBound(-1000000, state.value("offset").toInt(), 1000000);
	}
	return result;
}
} // namespace qs::windows::tiling

#include "transition_trace.hpp"
#include "transition_trace_queue.hpp"

#include <memory>
#include <qdatetime.h>
#include <qfile.h>
#include <qjsondocument.h>
#include <qtimezone.h>

namespace qs::windows::transition_trace {
namespace {
struct Sink {
	std::unique_ptr<Queue> queue;
	explicit Sink(const QString& path) {
		LARGE_INTEGER frequency {};
		LARGE_INTEGER origin {};
		if (path.isEmpty() || !QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&origin)) return;
		const auto utcOrigin = QDateTime::currentMSecsSinceEpoch();
		const auto pid = static_cast<qint64>(GetCurrentProcessId());
		this->queue = std::make_unique<Queue>(1024, [path, frequency, origin, utcOrigin, pid,
		                                               file = std::shared_ptr<QFile>(), attempted = false](Record record) mutable {
			if (!attempted) {
				attempted = true;
				file = std::make_shared<QFile>(path);
				if (!file->open(QIODevice::WriteOnly | QIODevice::Append)) file.reset();
			}
			if (!file) return;
			auto& fields = record.fields;
			fields.insert("event", QString::fromLatin1(record.event));
			fields.insert("qpc", QString::number(record.ticks));
			fields.insert("qpcFrequency", QString::number(frequency.QuadPart));
			// UTC is calibrated once against QPC; interval calculations use QPC.
			const auto elapsed = static_cast<qint64>(
			    static_cast<double>(record.ticks - origin.QuadPart) * 1000.0 / frequency.QuadPart);
			fields.insert("utc", QDateTime::fromMSecsSinceEpoch(utcOrigin + elapsed, QTimeZone::utc()).toString(Qt::ISODateWithMs));
			fields.insert("pid", pid);
			file->write(QJsonDocument(fields).toJson(QJsonDocument::Compact) + '\n');
			file->flush();
		});
	}
};
// Initialize before the event hooks run, so the first producer does not
// construct a thread or initialize the clock calibration on a transition.
Sink instance(qEnvironmentVariable("QS_TRANSITION_TRACE")); // NOLINT
Sink& sink() { return instance; }
}

bool enabled() { return sink().queue != nullptr; }

void write(const char* event, QJsonObject fields) {
	auto& s = sink();
	if (s.queue) s.queue->push(event, std::move(fields));
}

QJsonObject rect(const QRect& r) {
	return {{"x", r.x()}, {"y", r.y()}, {"width", r.width()}, {"height", r.height()}};
}
QString handle(quintptr value) { return QString::number(value, 16); }
}

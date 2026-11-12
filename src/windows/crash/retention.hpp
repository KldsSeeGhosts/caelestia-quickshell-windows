#pragma once

#include <algorithm>

#include <qdatetime.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qregularexpression.h>
#include <qvector.h>

namespace qs::crash {

// Run during healthy startup, never from an exception handler. Only touch the
// three files written by our Windows handler. Unknown files, subdirectories and
// reparse points are not ours to delete. A failed removal is retried next start.
inline void pruneCrashReports(const QDir& root) {
	auto redirected = [](const QFileInfo& info) {
		return info.isSymbolicLink() || info.isJunction();
	};
	if (!root.exists() || redirected(QFileInfo(root.absolutePath()))) return;

	struct Report {
		QDir dir;
		QString dump;
		QDateTime modified;
	};
	QVector<Report> reports;
	const QRegularExpression instancePattern(QStringLiteral("^[0-9a-z]+$"));

	for (const auto& entry: root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
		if (redirected(entry) || !instancePattern.match(entry.fileName()).hasMatch()) continue;
		QDir dir(entry.absoluteFilePath());
		const QRegularExpression dumpPattern(
		    "^" + entry.fileName() + QStringLiteral("-[0-9]+\\.dmp$")
		);
		QString dump;
		QDateTime modified;
		QDateTime supportModified;
		bool owned = true;
		for (const auto& file: dir.entryInfoList(
		         QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot
		     ))
		{
			if (!file.isFile() || redirected(file)) {
				owned = false;
				break;
			}
			if (dumpPattern.match(file.fileName()).hasMatch() && dump.isEmpty()) {
				dump = file.fileName();
				modified = file.lastModified();
			} else if (file.fileName() == "report.txt" || file.fileName() == "log.qslog.log") {
				supportModified = std::max(supportModified, file.lastModified());
			} else {
				owned = false;
				break;
			}
		}
		if (owned && (!dump.isEmpty() || supportModified.isValid())) {
			reports.push_back({dir, dump, dump.isEmpty() ? supportModified : modified});
		}
	}

	std::sort(reports.begin(), reports.end(), [](const Report& a, const Report& b) {
		if (a.modified != b.modified) return a.modified > b.modified;
		return a.dir.path() > b.dir.path();
	});
	for (qsizetype i = 3; i < reports.size(); ++i) {
		const auto& report = reports.at(i);
		// Keep supporting files if the dump could not be removed.
		if (!report.dump.isEmpty() && !QFile::remove(report.dir.filePath(report.dump))) continue;
		QFile::remove(report.dir.filePath("report.txt"));
		QFile::remove(report.dir.filePath("log.qslog.log"));
		root.rmdir(report.dir.dirName());
	}
}

} // namespace qs::crash

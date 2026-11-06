#include "logscrub.hpp"

#include <algorithm>
#include <string>

#include <qbytearray.h>
#include <qdir.h>
#include <qlist.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtenvironmentvariables.h>
#include <qurl.h>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace qs::log {

namespace {

struct PathNeedle {
	QString needle;
	QString placeholder;
};

struct LogScrubber {
	QList<PathNeedle> paths;
	// Account name and profile folder names, redacted as whole words.
	QStringList names;
};

bool isWordCharacter(QChar character) {
	return character.isLetterOrNumber() || character == QLatin1Char('_');
}

QString withForwardSlashes(QString value) {
	value.replace(QLatin1Char('\\'), QLatin1Char('/'));
	return value;
}

QString withBackslashes(QString value) {
	value.replace(QLatin1Char('/'), QLatin1Char('\\'));
	return value;
}

// The long or 8.3 short alias of an existing path, or an empty string.
QString windowsPathAlias(const QString& path, bool shortForm) {
#ifdef Q_OS_WIN
	const auto native = path.toStdWString();
	const auto convert = shortForm ? GetShortPathNameW : GetLongPathNameW;
	const auto size = convert(native.c_str(), nullptr, 0);
	if (size == 0) return {};

	auto buffer = std::wstring(size, L'\0');
	const auto written = convert(native.c_str(), buffer.data(), size);
	if (written == 0 || written >= size) return {};

	buffer.resize(written);
	return QString::fromStdWString(buffer);
#else
	Q_UNUSED(path);
	Q_UNUSED(shortForm);
	return {};
#endif
}

void addNeedle(QList<PathNeedle>& needles, const QString& value, const QString& placeholder) {
	if (value.isEmpty()) return;

	auto needle = withForwardSlashes(value);
	if (needle == QStringLiteral("/")) return;

	for (const auto& existing: needles) {
		if (existing.needle.compare(needle, Qt::CaseInsensitive) == 0) return;
	}

	needles.append({needle, placeholder});
}

// A root in forward-slash form without trailing separators, so the boundary
// check sees the separator that follows it in a logged path. Bare drive and
// filesystem roots return empty: they identify nobody and would match every path.
QString trimmedRoot(const QString& path) {
	auto forward = withForwardSlashes(path);
	while (forward.endsWith(QLatin1Char('/'))) forward.chop(1);
	if (forward.size() == 2 && forward.at(1) == QLatin1Char(':')) return {};
	return forward;
}

// Adds every spelling of a path that can reach a log: as given, its long and
// 8.3 short aliases, and for each the percent-encoded forms (file: URLs) and
// the doubled-backslash form QDebug writes. Matching is case-insensitive.
void addPath(QList<PathNeedle>& needles, const QString& rawPath, const QString& placeholder) {
	const auto path = trimmedRoot(rawPath);
	if (path.isEmpty()) return;

	auto spellings = QStringList {path};
	for (const auto shortForm: {false, true}) {
		auto alias = windowsPathAlias(withBackslashes(path), shortForm);
		if (!alias.isEmpty()) spellings.append(alias);
	}

	for (const auto& spelling: spellings) {
		const auto forward = trimmedRoot(spelling);
		if (forward.isEmpty()) continue;
		addNeedle(needles, forward, placeholder);
		addNeedle(needles, QString(forward).replace(QLatin1Char('/'), QStringLiteral("//")), placeholder);
		addNeedle(needles, QString::fromLatin1(QUrl::toPercentEncoding(forward, "/:")), placeholder);
		addNeedle(needles, QString::fromLatin1(QUrl::toPercentEncoding(forward)), placeholder);
		addNeedle(needles, QString::fromLatin1(QUrl::toPercentEncoding(withBackslashes(forward))), placeholder);
	}
}

void addName(QStringList& names, const QString& rawName) {
	auto name = rawName.trimmed();
	if (name.isEmpty()) return;

	for (const auto& candidate:
	     {name, QString::fromLatin1(QUrl::toPercentEncoding(name))}) {
		if (!names.contains(candidate, Qt::CaseInsensitive)) names.append(candidate);
	}
}

// The profile folder's own name can differ from the account name.
void addLeafName(QStringList& names, const QString& path) {
	auto forward = withForwardSlashes(path);
	while (forward.endsWith(QLatin1Char('/'))) forward.chop(1);

	const auto slash = forward.lastIndexOf(QLatin1Char('/'));
	if (slash < 0) return; // A bare drive or relative name is not a profile folder.

	addName(names, forward.mid(slash + 1));
}

const LogScrubber& logScrubber() {
	static const auto instance = [] {
		auto value = LogScrubber();
		auto paths = QList<PathNeedle>();

		const auto profiles = QStringList {QDir::homePath(), qEnvironmentVariable("USERPROFILE")};

		for (const auto& profile: profiles) addPath(paths, profile, QStringLiteral("%USERPROFILE%"));
		addPath(paths, qEnvironmentVariable("HOME"), QStringLiteral("%USERPROFILE%"));
		addPath(paths, qEnvironmentVariable("LOCALAPPDATA"), QStringLiteral("%LOCALAPPDATA%"));
		addPath(paths, qEnvironmentVariable("APPDATA"), QStringLiteral("%APPDATA%"));
		addPath(paths, qEnvironmentVariable("TEMP"), QStringLiteral("%TEMP%"));
		addPath(paths, qEnvironmentVariable("TMP"), QStringLiteral("%TEMP%"));

		// Longest first, so LOCALAPPDATA/TEMP win over the containing profile.
		std::sort(paths.begin(), paths.end(), [](const PathNeedle& left, const PathNeedle& right) {
			return left.needle.size() > right.needle.size();
		});
		value.paths = paths;

		auto username = qEnvironmentVariable("USERNAME");
		if (username.isEmpty()) username = qEnvironmentVariable("USER");
		if (username.isEmpty()) username = qEnvironmentVariable("LOGNAME");
		addName(value.names, username);

		for (const auto& profile: profiles) {
			addLeafName(value.names, profile);
			addLeafName(value.names, windowsPathAlias(withBackslashes(profile), true));
		}

		return value;
	}();
	return instance;
}

} // namespace

void initLogScrubbing() { (void) logScrubber(); }

QString scrubLogText(const QString& text) {
	if (text.isEmpty()) return text;

	const auto& scrubber = qs::log::logScrubber();
	const auto mayContainPath = text.contains(QLatin1Char('/')) || text.contains(QLatin1Char('\\'))
	                         || text.contains(QLatin1Char('%'));
	const auto mayContainName = std::any_of(
	    scrubber.names.begin(),
	    scrubber.names.end(),
	    [&](const QString& name) { return text.contains(name, Qt::CaseInsensitive); }
	);
	if (!mayContainPath && !mayContainName) return text;

	auto redacted = text;
	auto normalized = withForwardSlashes(text);

	if (mayContainPath) {
		for (const auto& needle: scrubber.paths) {
			auto from = 0;
			while (true) {
				auto start = normalized.indexOf(needle.needle, from, Qt::CaseInsensitive);
				if (start < 0) break;

				auto end = start + needle.needle.size();
				if (end < normalized.size() && isWordCharacter(normalized.at(end))) {
					from = start + 1;
					continue;
				}

				redacted.replace(start, needle.needle.size(), needle.placeholder);
				normalized.replace(start, needle.needle.size(), needle.placeholder);
				from = start + needle.placeholder.size();
			}
		}
	}

	if (mayContainName) {
		for (const auto& name: scrubber.names) {
			auto from = 0;
			while (true) {
				auto start = normalized.indexOf(name, from, Qt::CaseInsensitive);
				if (start < 0) break;

				auto end = start + name.size();
				auto startsAtWord = start == 0 || !isWordCharacter(normalized.at(start - 1));
				auto endsAtWord = end >= normalized.size() || !isWordCharacter(normalized.at(end));
				// Skip %NAME% and <name> placeholders written by earlier replacements.
				auto surroundedBy = [&](QChar open, QChar close) {
					return start > 0 && normalized.at(start - 1) == open && end < normalized.size()
					    && normalized.at(end) == close;
				};
				auto insidePlaceholder = surroundedBy(QLatin1Char('%'), QLatin1Char('%'))
				                      || surroundedBy(QLatin1Char('<'), QLatin1Char('>'));
				if (!startsAtWord || !endsAtWord || insidePlaceholder) {
					from = start + 1;
					continue;
				}

				redacted.replace(start, name.size(), QStringLiteral("<user>"));
				normalized.replace(start, name.size(), QStringLiteral("<user>"));
				from = start + 6;
			}
		}
	}

	return redacted;
}

} // namespace qs::log

#include <qdir.h>
#include <qdiriterator.h>
#include <qfile.h>
#include <qregularexpression.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtest.h>

namespace {

// Keep this list in sync with the helpers in redact.hpp. A log argument wrapped
// in one of these calls is already redacted.
const QStringList SAFE_CALLS = {
    QStringLiteral("logId"),
    QStringLiteral("logPath"),
    QStringLiteral("qs::redact::logId"),
    QStringLiteral("qs::redact::logPath"),
    QStringLiteral("qs::windows::logId"),
    QStringLiteral("qs::windows::logPath"),
};

// Calls that answer a question about a value instead of publishing it. Logging
// `device.name.isEmpty()` publishes a bool, while logging `device.name`
// publishes the name, so only the latter is a leak.
const QStringList VALUE_PROBES = {
    QStringLiteral("compare"),
    QStringLiteral("contains"),
    QStringLiteral("count"),
    QStringLiteral("empty"),
    QStringLiteral("endsWith"),
    QStringLiteral("indexOf"),
    QStringLiteral("isEmpty"),
    QStringLiteral("isNull"),
    QStringLiteral("isValid"),
    QStringLiteral("isValidUtf8"),
    QStringLiteral("lastIndexOf"),
    QStringLiteral("length"),
    QStringLiteral("size"),
    QStringLiteral("startsWith"),
};

QString stripStrings(const QString& text) {
	auto out = QString();
	auto inString = false;
	auto escaped = false;

	for (const auto character: text) {
		if (escaped) {
			escaped = false;
			out += QLatin1Char(' ');
		} else if (inString) {
			if (character == QLatin1Char('\\')) escaped = true;
			else if (character == QLatin1Char('"')) {
				inString = false;
				out += character;
			} else {
				out += QLatin1Char(' ');
			}
		} else if (character == QLatin1Char('"')) {
			inString = true;
			out += character;
		} else {
			out += character;
		}
	}

	return out;
}

QString stripLineComments(const QString& text) {
	auto out = text;
	auto index = out.indexOf(QStringLiteral("//"));
	while (index >= 0) {
		auto end = out.indexOf(QLatin1Char('\n'), index);
		if (end < 0) end = out.size();
		out.remove(index, end - index);
		index = out.indexOf(QStringLiteral("//"));
	}
	return out;
}

QString stripSafeCalls(QString text) {
	for (const auto& name: SAFE_CALLS) {
		auto searchFrom = 0;
		while (true) {
			auto start = text.indexOf(name, searchFrom);
			if (start < 0) break;

			auto open = start + name.size();
			while (open < text.size() && text.at(open).isSpace()) open++;
			if (open >= text.size() || text.at(open) != QLatin1Char('(')) {
				searchFrom = open;
				continue;
			}

			auto depth = 0;
			auto end = -1;
			for (auto index = open; index < text.size(); index++) {
				if (text.at(index) == QLatin1Char('(')) depth++;
				else if (text.at(index) == QLatin1Char(')') && --depth == 0) {
					end = index;
					break;
				}
			}

			if (end < 0) break;
			text.replace(start, end - start + 1, QStringLiteral(" safe "));
			searchFrom = start + 6;
		}
	}

	return text;
}

bool isLogStart(const QString& line) {
	static const auto pattern = QRegularExpression(QStringLiteral(
	    R"(\bqC(?:Debug|Info|Warning|Critical)\b|\bq(?:Debug|Info|Warning|Critical)\s*\()"
	));
	return pattern.match(line).hasMatch();
}

// True when the matched identifier is only the receiver of a probe, so the
// statement logs a fact about the value rather than the value. Empty accessor
// parens are skipped so `item->bindableId().isNull()` is treated like
// `device.name.isEmpty()`.
bool isValueProbe(const QString& code, int from) {
	auto index = from;
	while (true) {
		while (index < code.size() && code.at(index).isSpace()) index++;

		if (index < code.size() && code.at(index) == QLatin1Char('(')) {
			auto close = index + 1;
			while (close < code.size() && code.at(close).isSpace()) close++;
			if (close >= code.size() || code.at(close) != QLatin1Char(')')) return false;
			index = close + 1;
			continue;
		}

		auto operatorLength = 0;
		if (code.mid(index, 2) == QStringLiteral("->")) operatorLength = 2;
		else if (index < code.size() && code.at(index) == QLatin1Char('.')) operatorLength = 1;
		else return false;

		index += operatorLength;
		while (index < code.size() && code.at(index).isSpace()) index++;

		auto nameStart = index;
		while (index < code.size()
		       && (code.at(index).isLetterOrNumber() || code.at(index) == QLatin1Char('_')))
		{
			index++;
		}
		const auto name = code.mid(nameStart, index - nameStart);

		while (index < code.size() && code.at(index).isSpace()) index++;
		return VALUE_PROBES.contains(name) && index < code.size()
		    && code.at(index) == QLatin1Char('(');
	}
}

// A value that belongs to the user, a device or an app. Bare names are matched
// where the value is always sensitive. Member names are matched only through a
// receiver that owns one, because `entry.name` is a user identifier while a
// method called `name()` on something else may not be.
const QRegularExpression& rawIdentifierPattern() {
	static const auto pattern = QRegularExpression(QStringLiteral(
	    R"((?:window|foreground|device|adapter|snapshot|entry|transport|toast|target)\s*(?:->|\.)\s*(?:address|adapterId|name|title|appId|ssid|bssid|aumid)\b)"
	    R"(|\b(?:aumid|bId|bssid|bindableId|desktopEntry|identity|imagePath|replyText|sourcePath|ssid)\b)"
	    R"(|\bbind\s*(?:->|\.)\s*argument\b)"
	    R"(|\b(?:item|trayItem|tray)\s*(?:->|\.)\s*(?:bId|bindableId|iconGuid|iconId|id|itemId|uid)\b)"
	    R"(|\bscreenName\s*\()"
	));
	return pattern;
}

struct ScanRoot {
	QString name;
	QString path;
};

// src/windows is the Windows backend; src/core and src/io also reach the log
// sinks on this platform, so a raw identifier there is the same leak. The
// plugin tree lives in the parent repository, not here.
const QList<ScanRoot>& scanRoots() {
	static const auto backend = QDir(QStringLiteral(QS_BACKEND_SRC));
	static const auto roots = QList<ScanRoot> {
	    {QStringLiteral("src/windows"), backend.filePath(QStringLiteral("src/windows"))},
	    {QStringLiteral("src/core"), backend.filePath(QStringLiteral("src/core"))},
	    {QStringLiteral("src/io"), backend.filePath(QStringLiteral("src/io"))},
	};
	return roots;
}

QStringList scanSource(const QString& source, const QString& relative) {
	static const auto macPattern = QRegularExpression(
	    QStringLiteral(R"((?i)(?<![0-9a-f])(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}(?![0-9a-f]))")
	);

	auto failures = QStringList();
	const auto lines = source.split(QLatin1Char('\n'));

	for (auto index = 0; index < lines.size(); index++) {
		const auto trimmed = lines.at(index).trimmed();
		if (trimmed.startsWith(QStringLiteral("//")) || trimmed.startsWith(QLatin1Char('*'))) continue;
		if (!isLogStart(lines.at(index))) continue;

		auto statement = QString();
		const auto statementLine = index + 1;
		auto cursor = index;
		while (cursor < lines.size() && cursor < index + 30) {
			statement += lines.at(cursor);
			statement += QLatin1Char('\n');
			if (lines.at(cursor).contains(QLatin1Char(';'))) break;
			cursor++;
		}
		index = cursor;

		const auto mac = macPattern.match(statement);
		if (mac.hasMatch()) {
			failures.append(
			    QStringLiteral("%1:%2: log statement contains a MAC-like literal: %3")
			        .arg(relative)
			        .arg(statementLine)
			        .arg(mac.captured(0))
			);
		}

		const auto code = stripSafeCalls(stripLineComments(stripStrings(statement)));
		auto matches = rawIdentifierPattern().globalMatch(code);
		while (matches.hasNext()) {
			const auto match = matches.next();
			if (isValueProbe(code, match.capturedEnd())) continue;

			failures.append(
			    QStringLiteral("%1:%2: raw identifier `%3` is streamed to a log")
			        .arg(relative)
			        .arg(statementLine)
			        .arg(match.captured(0).trimmed())
			);
			break;
		}
	}

	return failures;
}

} // namespace

class LogRedactionGuardTest: public QObject {
	Q_OBJECT

private slots:
	// Every log statement in the covered trees. This is the guard proper; the
	// fixture cases below pin its decisions so a future pattern edit cannot
	// silently reopen the holes this test exists to close.
	void loggedValuesAreRedacted() {
		auto failures = QStringList();
		auto scanned = 0;

		for (const auto& root: scanRoots()) {
			auto rootDir = QDir(root.path);
			QVERIFY2(
			    rootDir.exists(),
			    qPrintable(QStringLiteral("Source root missing: %1").arg(root.path))
			);

			auto iterator = QDirIterator(
			    root.path,
			    {QStringLiteral("*.cpp"), QStringLiteral("*.hpp")},
			    QDir::Files,
			    QDirIterator::Subdirectories
			);

			while (iterator.hasNext()) {
				const auto path = iterator.next();
				auto relative = rootDir.relativeFilePath(path).replace(QLatin1Char('\\'), QLatin1Char('/'));
				if (relative.startsWith(QStringLiteral("test/"))) continue;
				if (relative == QStringLiteral("redact.cpp")
				    || relative == QStringLiteral("redact.hpp"))
				{
					continue;
				}

				auto file = QFile(path);
				if (!file.open(QFile::ReadOnly | QFile::Text)) {
					failures.append(QStringLiteral("%1: cannot read").arg(relative));
					continue;
				}

				failures += scanSource(
				    QString::fromUtf8(file.readAll()),
				    root.name + QLatin1Char('/') + relative
				);
				scanned++;
			}
		}

		QVERIFY2(scanned > 0, "No log sinks were scanned");

		if (!failures.isEmpty()) {
			auto message = failures.mid(0, 20).join(QLatin1Char('\n'));
			if (failures.size() > 20) {
				message += QStringLiteral("\n... and %1 more").arg(failures.size() - 20);
			}
			QFAIL(
			    qPrintable(QStringLiteral(
			                   "Unredacted log identifiers (wrap them in qs::redact::logId()/logPath()):"
			                   "\n%1"
			    )
			                   .arg(message))
			);
		}
	}

	void rawIdentifiersAreRejected_data() {
		QTest::addColumn<QString>("snippet");
		QTest::addColumn<QString>("identifier");

		QTest::newRow("member name")
		    << QStringLiteral("qCDebug(lc) << device.name;")
		    << QStringLiteral("device.name");
		QTest::newRow("bare ssid")
		    << QStringLiteral("qCInfo(lc) << \"Registered network\" << ssid;")
		    << QStringLiteral("ssid");
		QTest::newRow("member ssid")
		    << QStringLiteral("qCDebug(lc) << network.ssid;")
		    << QStringLiteral("ssid");
		QTest::newRow("bare bssid")
		    << QStringLiteral("qCWarning(lc) << bssid;")
		    << QStringLiteral("bssid");
		QTest::newRow("bare identity")
		    << QStringLiteral("qCDebug(lc) << identity;")
		    << QStringLiteral("identity");
		QTest::newRow("bind argument")
		    << QStringLiteral("qCDebug(lc) << bind.argument;")
		    << QStringLiteral("bind.argument");
		QTest::newRow("bind pointer argument")
		    << QStringLiteral("qCWarning(lc) << bind->argument;")
		    << QStringLiteral("bind->argument");
		QTest::newRow("tray item id")
		    << QStringLiteral("qCDebug(lc) << item->bindableId().value();")
		    << QStringLiteral("item->bindableId");
		QTest::newRow("tray bound id")
		    << QStringLiteral("qCDebug(lc) << this->bId.value();")
		    << QStringLiteral("bId");
		QTest::newRow("tray icon id")
		    << QStringLiteral("qCDebug(lc) << item->iconId();")
		    << QStringLiteral("item->iconId");
		QTest::newRow("window title")
		    << QStringLiteral("qCDebug(lc) << window->title;")
		    << QStringLiteral("window->title");
		QTest::newRow("toast app id")
		    << QStringLiteral("qCDebug(lc) << toast.appId;")
		    << QStringLiteral("toast.appId");
		QTest::newRow("screen name")
		    << QStringLiteral("qCDebug(lc) << screenName(screen);")
		    << QStringLiteral("screenName(");
		QTest::newRow("multi-line statement")
		    << QStringLiteral("qCDebug(lc) << \"device\"\n    << device.name;")
		    << QStringLiteral("device.name");
		QTest::newRow("probe then raw value")
		    << QStringLiteral("qCDebug(lc) << device.name.isEmpty() << device.name;")
		    << QStringLiteral("device.name");
	}

	void rawIdentifiersAreRejected() {
		QFETCH(QString, snippet);
		QFETCH(QString, identifier);

		const auto failures = scanSource(snippet, QStringLiteral("snippet.cpp"));
		QCOMPARE(failures.size(), 1);
		QVERIFY2(
		    failures.first().contains(identifier),
		    qPrintable(QStringLiteral("`%1` did not report `%2`").arg(failures.first(), identifier))
		);
	}

	void valueProbesAndRedactedValuesAreAccepted_data() {
		QTest::addColumn<QString>("snippet");

		QTest::newRow("member probe")
		    << QStringLiteral("qCDebug(lc) << device.name.isEmpty();");
		QTest::newRow("negated member probe")
		    << QStringLiteral("qCDebug(lc) << \"device title\" << !device.name.isEmpty();");
		QTest::newRow("probe with argument")
		    << QStringLiteral("qCDebug(lc) << device.name.startsWith(\"Display\");");
		QTest::newRow("length probe")
		    << QStringLiteral("qCDebug(lc) << device.name.size();");
		QTest::newRow("bare ssid probe")
		    << QStringLiteral("qCDebug(lc) << ssid.isEmpty();");
		QTest::newRow("bind argument probe")
		    << QStringLiteral("qCDebug(lc) << bind.argument.isEmpty();");
		QTest::newRow("bindable accessor probe")
		    << QStringLiteral("qCDebug(lc) << item->bindableId().isNull();");
		QTest::newRow("redacted member")
		    << QStringLiteral("qCDebug(lc) << qs::windows::logId(device.name);");
		QTest::newRow("redacted core path")
		    << QStringLiteral("qCDebug(lc) << qs::redact::logPath(sourcePath);");
		QTest::newRow("redacted tray id")
		    << QStringLiteral("qCDebug(lc) << qs::windows::logId(item->bindableId().value());");
		QTest::newRow("ordinary member")
		    << QStringLiteral("qCDebug(lc) << window->geometry();");
		QTest::newRow("literal text")
		    << QStringLiteral("qCDebug(lc) << \"ssid\" << \"identity\";");
		QTest::newRow("commented statement")
		    << QStringLiteral("// qCDebug(lc) << ssid;\nqCDebug(lc) << \"ok\";");
		QTest::newRow("probe with redacted branch")
		    << QStringLiteral(
		           "qCDebug(lc) << (ssid.isEmpty() ? \"none\" : qs::windows::logId(ssid));"
		       );
	}

	void valueProbesAndRedactedValuesAreAccepted() {
		QFETCH(QString, snippet);

		QCOMPARE(scanSource(snippet, QStringLiteral("snippet.cpp")), QStringList());
	}

	// The guard only covers the trees listed in scanRoots(). A relocated or
	// renamed tree must fail loudly here instead of silently shrinking the scan.
	void scanRootsContainSources() {
		for (const auto& root: scanRoots()) {
			auto iterator = QDirIterator(
			    root.path,
			    {QStringLiteral("*.cpp"), QStringLiteral("*.hpp")},
			    QDir::Files,
			    QDirIterator::Subdirectories
			);
			QVERIFY2(
			    iterator.hasNext(),
			    qPrintable(QStringLiteral("No sources found under %1").arg(root.path))
			);
		}
	}
};

QTEST_GUILESS_MAIN(LogRedactionGuardTest)
#include "log_redaction_guard_test.moc"

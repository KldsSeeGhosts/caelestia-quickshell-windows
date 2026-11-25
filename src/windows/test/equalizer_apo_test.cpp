#include <cstring>
#include <vector>

#include <qdir.h>
#include <qfile.h>
#include <qprocess.h>
#include <qtemporarydir.h>
#include <qtest.h>
#include <qt_windows.h>

#include "../system/equalizer_apo.hpp"

using namespace qs::windows::sys;

namespace {

QByteArray wideBytes(const QString& text, bool terminate = false) {
	auto wide = text.toStdWString();
	auto chars = wide.size() + (terminate ? 1 : 0);
	QByteArray bytes(static_cast<qsizetype>(chars * sizeof(wchar_t)), '\0');
	std::memcpy(bytes.data(), wide.data(), wide.size() * sizeof(wchar_t));
	return bytes;
}

struct Entry {
	QString name;
	QString value;
	DWORD type = REG_SZ;
	bool terminate = false;
};

bool writeUtf8(const QString& path, const QString& text) {
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
	return file.write(text.toUtf8()) == text.toUtf8().size();
}

QString readUtf8(const QString& path) {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) return QString();
	return QString::fromUtf8(file.readAll());
}

class EqualizerApoTest: public QObject {
	Q_OBJECT

private slots:
	void registryStringDecodingUsesTheStoredByteCount() {
		const auto unterminated = wideBytes(QStringLiteral("EqualizerAPO"), false);
		QCOMPARE(
		    equalizer_apo_detail::decodeRegistryString(
		        reinterpret_cast<const BYTE*>(unterminated.constData()),
		        static_cast<DWORD>(unterminated.size())
		    ),
		    QStringLiteral("EqualizerAPO")
		);

		const auto terminated = wideBytes(QStringLiteral("EqualizerAPO"), true) + QByteArray(4, '\0');
		QCOMPARE(
		    equalizer_apo_detail::decodeRegistryString(
		        reinterpret_cast<const BYTE*>(terminated.constData()),
		        static_cast<DWORD>(terminated.size())
		    ),
		    QStringLiteral("EqualizerAPO")
		);
	}

	void clsidMatchingAcceptsCompositeNamesAndValues() {
		const auto clsid = QStringLiteral("{EC1CC9CE-FAED-4822-828A-82A81A6F018F}");
		QVERIFY(equalizer_apo_detail::registryTextContainsClsid(
		    QStringLiteral("{62DC5A94-A604-4FB3-B7DA-8BD2C93739D6},1,%1").arg(clsid),
		    clsid
		));
		QVERIFY(equalizer_apo_detail::registryTextContainsClsid(
		    QStringLiteral("{%1},2").arg(clsid),
		    clsid
		));
		QVERIFY(!equalizer_apo_detail::registryTextContainsClsid(
		    QStringLiteral("{62DC5A94-A604-4FB3-B7DA-8BD2C93739D6},1"),
		    clsid
		));
	}

	void registryScanResizesAfterMoreDataAndContinues() {
		const auto clsid = QStringLiteral("{EC1CC9CE-FAED-4822-828A-82A81A6F018F}");
		const auto largeNoise = QString(2200, QLatin1Char('x'));
		QList<Entry> entries {
		    {QStringLiteral("large"), largeNoise, REG_SZ, false},
		    {QStringLiteral("composite,1"), clsid, REG_EXPAND_SZ, false},
		};
		auto oversizedReads = 0;
		auto enumeratedAfterResize = false;

		equalizer_apo_detail::RegistryEnumValue enumerate =
		    [&](DWORD index,
		        wchar_t* name,
		        DWORD& nameLen,
		        DWORD& type,
		        BYTE* data,
		        DWORD& dataLen) -> LSTATUS {
			if (index >= static_cast<DWORD>(entries.size())) return ERROR_NO_MORE_ITEMS;
			const auto& entry = entries.at(static_cast<qsizetype>(index));
			auto wideName = entry.name.toStdWString();
			if (wideName.size() + 1 > nameLen) return ERROR_MORE_DATA;
			std::memcpy(name, wideName.data(), wideName.size() * sizeof(wchar_t));
			name[wideName.size()] = L'\0';
			nameLen = static_cast<DWORD>(wideName.size());

			auto bytes = wideBytes(entry.value, entry.terminate);
			type = entry.type;
			if (data == nullptr) {
				// Stale small metadata models a value that grows before the data read.
				dataLen = index == 0 ? DWORD(10) : static_cast<DWORD>(bytes.size());
				return ERROR_SUCCESS;
			}
			if (static_cast<size_t>(dataLen) < static_cast<size_t>(bytes.size())) {
				dataLen = static_cast<DWORD>(bytes.size());
				++oversizedReads;
				return ERROR_MORE_DATA;
			}
			std::memcpy(data, bytes.constData(), static_cast<size_t>(bytes.size()));
			dataLen = static_cast<DWORD>(bytes.size());
			if (index == 1) enumeratedAfterResize = true;
			return ERROR_SUCCESS;
		};

		auto matched = false;
		auto scanComplete = false;
		auto found = equalizer_apo_detail::scanRegistryStringValues(
		    enumerate,
		    [&](const QString&, const QString& value) {
			    matched = equalizer_apo_detail::registryTextContainsClsid(value, clsid);
			    return matched;
		    },
		    &scanComplete
		);

		QVERIFY(found);
		QVERIFY(matched);
		QVERIFY(scanComplete);
		QCOMPARE(oversizedReads, 1);
		QVERIFY(enumeratedAfterResize);
	}
	void registryScanChecksNamesForEveryTypeAndSkipsOversizedData() {
		const auto clsid = QStringLiteral("{EC1CC9CE-FAED-4822-828A-82A81A6F018F}");
		const auto oversizedNoise = QString(600000, QLatin1Char('x'));
		QList<Entry> entries {
		    {QStringLiteral("oversized"), oversizedNoise, REG_SZ, false},
		    {QStringLiteral("binary,%1").arg(clsid), QStringLiteral("noise"), REG_BINARY, false},
		    {QStringLiteral("later"), QStringLiteral("noise"), REG_SZ, false},
		};
		QStringList scannedNames;
		auto oversizedDataReads = 0;

		equalizer_apo_detail::RegistryEnumValue enumerate =
		    [&](DWORD index,
		        wchar_t* name,
		        DWORD& nameLen,
		        DWORD& type,
		        BYTE* data,
		        DWORD& dataLen) -> LSTATUS {
			if (index >= static_cast<DWORD>(entries.size())) return ERROR_NO_MORE_ITEMS;
			const auto& entry = entries.at(static_cast<qsizetype>(index));
			auto wideName = entry.name.toStdWString();
			std::memcpy(name, wideName.data(), wideName.size() * sizeof(wchar_t));
			name[wideName.size()] = L'\0';
			nameLen = static_cast<DWORD>(wideName.size());
			type = entry.type;
			auto bytes = wideBytes(entry.value, entry.terminate);
			if (data == nullptr) {
				dataLen = static_cast<DWORD>(bytes.size());
				return ERROR_SUCCESS;
			}
			++oversizedDataReads;
			if (static_cast<size_t>(dataLen) < static_cast<size_t>(bytes.size()))
				return ERROR_MORE_DATA;
			std::memcpy(data, bytes.constData(), static_cast<size_t>(bytes.size()));
			dataLen = static_cast<DWORD>(bytes.size());
			return ERROR_SUCCESS;
		};

		auto found = equalizer_apo_detail::scanRegistryStringValues(
		    enumerate,
		    [&](const QString& name, const QString&) {
			    scannedNames.append(name);
			    return equalizer_apo_detail::registryTextContainsClsid(name, clsid);
		    }
		);

		QVERIFY(found);
		QCOMPARE(scannedNames, QStringList({entries[0].name, entries[1].name}));
		// The oversized first value never caused a data read or ended the scan;
		// the binary second value still had its name inspected.
		QCOMPARE(oversizedDataReads, 0);
	}

	void elevatedHookScriptCreatesAndVerifiesTheIncludeFile() {
		const auto script = equalizer_apo_detail::elevatedHookScript(
		    QStringLiteral("C:\\EqualizerAPO\\config\\config.txt"),
		    QStringLiteral("C:\\EqualizerAPO\\config\\ii-equalizer.txt")
		);
		QVERIFY(script.contains(QStringLiteral("[IO.File]::WriteAllText($includePath")));
		QVERIFY(script.contains(QStringLiteral("Test-Path -LiteralPath $includePath -PathType Leaf")));
		QVERIFY(script.contains(QStringLiteral("exit 3")));
		QVERIFY(script.contains(QStringLiteral("[IO.File]::AppendAllText($configPath")));
	}

	void includeDirectiveParsingUsesBalancedQuotesAndConfigRelativePaths() {
		const auto configPath = QStringLiteral("C:/EqualizerAPO/config/config.txt");
		const auto includePath = QStringLiteral("C:/EqualizerAPO/config/ii-equalizer.txt");
		const auto newline = QChar(10);
		const auto doubleQuote = QChar(34);
		const auto singleQuote = QChar(39);
		const auto parentTarget = QStringLiteral("..") + QChar(92) + QStringLiteral("config")
		    + QChar(92) + QStringLiteral("ii-equalizer.txt");
		auto quoted = [](const QString& target, const QChar& quote) {
			return QString() + quote + target + quote;
		};
		auto active = [&configPath, &includePath](const QString& text) {
			return equalizer_apo_detail::hasActiveIncludeDirective(text, configPath, includePath);
		};

		QVERIFY(!active(QStringLiteral("# Include: ii-equalizer.txt") + newline));
		QVERIFY(!active(QStringLiteral("   # Include: ii-equalizer.txt") + newline));
		QVERIFY(!active(QStringLiteral("Include: other.txt # ii-equalizer.txt") + newline));
		QVERIFY(active(QStringLiteral("Include: ii-equalizer.txt") + newline));
		QVERIFY(active(QStringLiteral("  Include: ii-equalizer.txt") + QChar(13) + newline));
		QVERIFY(active(QStringLiteral("Include : ii-equalizer.txt") + newline));
		QVERIFY(active(QStringLiteral("Include: ./ii-equalizer.txt") + newline));
		QVERIFY(active(
		    QStringLiteral("Include: ") + quoted(QStringLiteral("./ii-equalizer.txt"), doubleQuote)
		    + newline
		));
		QVERIFY(active(
		    QStringLiteral("Include: ") + quoted(QStringLiteral("./ii-equalizer.txt"), singleQuote)
		    + newline
		));
		QVERIFY(active(QStringLiteral("Include: ") + parentTarget + newline));
		QVERIFY(active(QStringLiteral("Include: ") + quoted(parentTarget, doubleQuote) + newline));
		QVERIFY(active(QStringLiteral("Include: ") + includePath + newline));
		QVERIFY(!active(QStringLiteral("Include: ") + doubleQuote + QStringLiteral("ii-equalizer.txt") + newline));
		QVERIFY(!active(QStringLiteral("Include: ") + singleQuote + QStringLiteral("ii-equalizer.txt") + newline));
		QVERIFY(!active(QStringLiteral("Include: ../other/ii-equalizer.txt") + newline));

		const auto script = equalizer_apo_detail::elevatedHookScript(configPath, includePath);
		QVERIFY(script.contains(QStringLiteral("$entry.StartsWith('#')")));
		QVERIFY(script.contains(QStringLiteral("foreach ($rawLine in ($text -split")));
		QVERIFY(script.contains(QStringLiteral("[System.IO.Path]::GetFullPath")));
		QVERIFY(script.contains(QStringLiteral("$targetFullPath -ieq $includeFullPath")));
		QVERIFY(!script.contains(QStringLiteral("$includeName")));
	}

	void elevatedPowerShellParserMatchesCppOnDisk() {
		const auto doubleQuote = QChar(34);
		const auto parentTarget = QStringLiteral("..") + QChar(92) + QStringLiteral("config")
		    + QChar(92) + QStringLiteral("ii-equalizer.txt");
		struct Case {
			QString line;
			bool active;
		};
		const QList<Case> cases {
		    {QStringLiteral("Include: ./ii-equalizer.txt"), true},
		    {QStringLiteral("Include: ") + doubleQuote + QStringLiteral("./ii-equalizer.txt") + doubleQuote,
		        true},
		    {QStringLiteral("Include: ") + parentTarget, true},
		    {QStringLiteral("Include: ") + doubleQuote + QStringLiteral("ii-equalizer.txt"), false},
		};

		for (const auto& testCase: cases) {
			QTemporaryDir root;
			QVERIFY(root.isValid());
			QDir rootDir(root.path());
			QVERIFY(rootDir.mkpath(QStringLiteral("config")));
			const auto configDir = rootDir.filePath(QStringLiteral("config"));
			const auto configPath = QDir::toNativeSeparators(
			    QDir(configDir).filePath(QStringLiteral("config.txt"))
			);
			const auto includePath = QDir::toNativeSeparators(
			    QDir(configDir).filePath(QStringLiteral("ii-equalizer.txt"))
			);
			const auto initial = testCase.line + QChar(10);
			QVERIFY(writeUtf8(configPath, initial));
			QCOMPARE(
			    equalizer_apo_detail::hasActiveIncludeDirective(initial, configPath, includePath),
			    testCase.active
			);

			const auto scriptPath = root.filePath(QStringLiteral("hook.ps1"));
			QVERIFY(writeUtf8(
			    scriptPath,
			    equalizer_apo_detail::elevatedHookScript(configPath, includePath)
			));
			QProcess process;
			process.start(
			    QStringLiteral("powershell.exe"),
			    QStringList({
			        QStringLiteral("-NoLogo"),
			        QStringLiteral("-NoProfile"),
			        QStringLiteral("-NonInteractive"),
			        QStringLiteral("-ExecutionPolicy"),
			        QStringLiteral("Bypass"),
			        QStringLiteral("-File"),
			        scriptPath,
			    })
			);
			QVERIFY2(process.waitForStarted(5000), qPrintable(process.errorString()));
			QVERIFY2(process.waitForFinished(10000), qPrintable(process.errorString()));
			const auto standardError = QString::fromLocal8Bit(process.readAllStandardError());
			QVERIFY2(process.exitCode() == 0, qPrintable(standardError));

			const auto finalText = readUtf8(configPath);
			if (testCase.active) {
				QCOMPARE(finalText, initial);
			} else {
				QVERIFY(finalText.startsWith(initial));
				QCOMPARE(finalText.count(includePath), 1);
				QVERIFY(equalizer_apo_detail::hasActiveIncludeDirective(finalText, configPath, includePath));
			}
		}
	}

	void elevationCompletionNeverClaimsSuccessWithoutAProcessHandle() {
		auto unverified = equalizer_apo_detail::elevationCompletion(false, 0, 0);
		QVERIFY(!unverified.success);
		QCOMPARE(unverified.reason, QStringLiteral("completion-unverified"));

		auto success = equalizer_apo_detail::elevationCompletion(true, WAIT_OBJECT_0, 0);
		QVERIFY(success.success);
		QVERIFY(success.reason.isEmpty());

		auto failed = equalizer_apo_detail::elevationCompletion(true, WAIT_OBJECT_0, 2);
		QVERIFY(!failed.success);
		QCOMPARE(failed.reason, QStringLiteral("exit-2"));
	}

};

} // namespace

QTEST_GUILESS_MAIN(EqualizerApoTest)

#include "equalizer_apo_test.moc"

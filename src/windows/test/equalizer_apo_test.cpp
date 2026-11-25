#include <cstring>
#include <vector>

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

	void commentedIncludeDoesNotCountAsActive() {
		const auto includePath = QStringLiteral("C:/EqualizerAPO/config/ii-equalizer.txt");
		using equalizer_apo_detail::hasActiveIncludeDirective;
		QVERIFY(!hasActiveIncludeDirective(QStringLiteral("# Include: ii-equalizer.txt\n"), includePath));
		QVERIFY(!hasActiveIncludeDirective(QStringLiteral("   # Include: ii-equalizer.txt\n"), includePath));
		QVERIFY(!hasActiveIncludeDirective(QStringLiteral("Include: other.txt # ii-equalizer.txt\n"), includePath));
		QVERIFY(hasActiveIncludeDirective(QStringLiteral("Include: ii-equalizer.txt\n"), includePath));
		QVERIFY(hasActiveIncludeDirective(QStringLiteral("  Include: ii-equalizer.txt\r\n"), includePath));
		QVERIFY(hasActiveIncludeDirective(QStringLiteral("Include : ii-equalizer.txt\n"), includePath));
		QVERIFY(hasActiveIncludeDirective(QStringLiteral("Include: ./ii-equalizer.txt\n"), includePath));
		QVERIFY(hasActiveIncludeDirective(QStringLiteral("Include: %1\n").arg(includePath), includePath));

		const auto script = equalizer_apo_detail::elevatedHookScript(
		    QStringLiteral("C:/EqualizerAPO/config/config.txt"),
		    includePath
		);
		QVERIFY(script.contains(QStringLiteral("$entry.StartsWith('#')")));
		QVERIFY(script.contains(QStringLiteral("foreach ($rawLine in ($text -split")));
		QVERIFY(script.contains(QStringLiteral("$target -eq $includeName")));
		QVERIFY(!script.contains(QStringLiteral("[regex]::Escape($line)")));
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

#include "redact.hpp"

#include <qbytearray.h>
#include <qcryptographichash.h>
#include <qdir.h>
#include <qfileinfo.h>
#include <qrandom.h>
#include <qstring.h>
#include <qstringlist.h>

namespace qs::redact {

namespace {

const QByteArray& processSalt() {
	static const auto salt = [] {
		auto bytes = QByteArray(16, '\0');
		auto* random = QRandomGenerator::system();
		for (auto& byte: bytes) byte = static_cast<char>(random->bounded(256));
		return bytes;
	}();
	return salt;
}

QString hashed(const QString& value) {
	auto digest = QCryptographicHash::hash(processSalt() + value.toUtf8(), QCryptographicHash::Sha256);
	return QStringLiteral("redacted:") + QString::fromLatin1(digest.toHex().left(12));
}

QString extensionOf(const QString& path) {
	if (path.endsWith(QLatin1Char('/')) || path.endsWith(QLatin1Char('\\'))) return {};

	auto info = QFileInfo(QDir::fromNativeSeparators(path));
	if (info.isDir()) return {};

	static const auto fileTypes = QStringList {
	    QStringLiteral("png"),
	    QStringLiteral("jpg"),
	    QStringLiteral("jpeg"),
	    QStringLiteral("webp"),
	    QStringLiteral("gif"),
	    QStringLiteral("bmp"),
	    QStringLiteral("mp4"),
	    QStringLiteral("webm"),
	    QStringLiteral("mkv"),
	    QStringLiteral("json"),
	    QStringLiteral("qml"),
	    QStringLiteral("toml"),
	    QStringLiteral("ini"),
	    QStringLiteral("log"),
	    QStringLiteral("txt"),
	    QStringLiteral("exe"),
	    QStringLiteral("dll"),
	};

	auto extension = info.suffix().toLower();
	if (!fileTypes.contains(extension)) return {};
	return extension;
}

} // namespace

QString logId(const QString& value) {
	if (value.isEmpty()) return QStringLiteral("<empty>");
	return hashed(value);
}

QString logPath(const QString& path) {
	if (path.isEmpty()) return QStringLiteral("<empty>");

	auto extension = extensionOf(path);
	auto redacted = hashed(path);
	if (!extension.isEmpty()) redacted += QStringLiteral("(.") + extension + QLatin1Char(')');
	return redacted;
}

} // namespace qs::redact

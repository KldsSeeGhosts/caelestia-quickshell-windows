#include <qbuffer.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qimage.h>
#include <qregularexpression.h>
#include <qtemporarydir.h>
#include <qtest.h>
#include <qurl.h>
#include <limits>

#include "art_cache.hpp"

using qs::windows::services::mpris::cacheImageBytes;
using namespace qs::windows::services::mpris;

class MprisArtCacheTest: public QObject {
	Q_OBJECT;

	static QByteArray imageBytes() {
		QImage image(2, 2, QImage::Format_ARGB32);
		image.fill(Qt::red);
		QByteArray bytes;
		QBuffer buffer(&bytes);
		buffer.open(QIODevice::WriteOnly);
		image.save(&buffer, "PNG");
		return bytes;
	}

private slots:
	void encodedSizeLimits() {
		QVERIFY(!artworkSizeAllowed(0));
		QVERIFY(artworkSizeAllowed(1));
		QVERIFY(artworkSizeAllowed(MAX_ARTWORK_BYTES));
		QVERIFY(!artworkSizeAllowed(MAX_ARTWORK_BYTES + 1));
		// Values that would wrap to a small uint32_t must be rejected first.
		QVERIFY(!artworkSizeAllowed((uint64_t(1) << 32) + 1));
		QVERIFY(!artworkSizeAllowed(std::numeric_limits<uint64_t>::max()));
	}

	void decodedSizeLimits() {
		QVERIFY(!artworkDimensionsAllowed({}));
		QVERIFY(!artworkDimensionsAllowed({-1, 2}));
		QVERIFY(artworkDimensionsAllowed({1, 1}));
		QVERIFY(artworkDimensionsAllowed({MAX_ARTWORK_DIMENSION, MAX_ARTWORK_DIMENSION}));
		QVERIFY(!artworkDimensionsAllowed({MAX_ARTWORK_DIMENSION + 1, 1}));
		QVERIFY(!artworkDimensionsAllowed({1, MAX_ARTWORK_DIMENSION + 1}));
		QVERIFY(!artworkDimensionsAllowed({INT_MAX, INT_MAX}));
	}

	void oversizedImageDoesNotCreateCache() {
		QTemporaryDir temporary;
		QVERIFY(temporary.isValid());
		auto profile = temporary.filePath("cache");
		QByteArray bytes(MAX_ARTWORK_BYTES + 1, 'x');
		QVERIFY(cacheImageBytes(bytes, profile).url.isEmpty());
		QImage image(MAX_ARTWORK_DIMENSION + 1, 1, QImage::Format_ARGB32);
		image.fill(Qt::red);
		bytes.clear();
		QBuffer buffer(&bytes);
		QVERIFY(buffer.open(QIODevice::WriteOnly));
		QVERIFY(image.save(&buffer, "PNG"));
		QTest::ignoreMessage(QtWarningMsg, QRegularExpression("Could not decode a thumbnail.*"));
		QVERIFY(cacheImageBytes(bytes, profile).url.isEmpty());
		QVERIFY(!QFileInfo::exists(profile));
	}

	void writesOnlyToProvidedProfileCache() {
		QTemporaryDir temporary;
		QVERIFY(temporary.isValid());
		auto profile = temporary.filePath("profile/cache/mpris-art");
		auto art = cacheImageBytes(imageBytes(), profile);
		QVERIFY(!art.url.isEmpty());
		auto path = QUrl(art.url).toLocalFile();
		QCOMPARE(QFileInfo(path).absolutePath(), QDir(profile).absolutePath());
		QVERIFY(QFileInfo::exists(path));
		QVERIFY(!QImage(path).isNull());
		QCOMPARE(QFileInfo(path).baseName().toLatin1(), art.hash);
		auto again = cacheImageBytes(imageBytes(), profile);
		QCOMPARE(again.url, art.url);
		QCOMPARE(QDir(profile).entryList(QDir::Files).size(), 1);
	}

	void malformedImageDoesNotCreateCache() {
		QTemporaryDir temporary;
		QVERIFY(temporary.isValid());
		auto profile = temporary.filePath("profile/cache/mpris-art");
		QTest::ignoreMessage(QtWarningMsg, QRegularExpression("Could not decode a thumbnail.*"));
		QVERIFY(cacheImageBytes("invalid image", profile).url.isEmpty());
		QVERIFY(!QFileInfo::exists(profile));
		QVERIFY(cacheImageBytes(imageBytes(), {}).url.isEmpty());
	}

	void pruningCannotRemoveAnotherProfilesFiles() {
		QTemporaryDir temporary;
		QVERIFY(temporary.isValid());
		auto profile = temporary.filePath("profile/cache/mpris-art");
		QVERIFY(QDir().mkpath(profile));
		auto otherProfile = temporary.filePath("other-profile");
		QVERIFY(QDir().mkpath(otherProfile));
		QFile sentinel(QDir(otherProfile).filePath("keep.png"));
		QVERIFY(sentinel.open(QIODevice::WriteOnly));
		QCOMPARE(sentinel.write("preserve"), 8);
		sentinel.close();
		for (int i = 0; i < 55; ++i) {
			QFile file(QDir(profile).filePath(QString::number(i) + ".png"));
			QVERIFY(file.open(QIODevice::WriteOnly));
			file.write("old cache");
		}
		auto art = cacheImageBytes(imageBytes(), profile);
		QVERIFY(!art.url.isEmpty());
		QVERIFY(QFileInfo::exists(QUrl(art.url).toLocalFile()));
		QVERIFY(QDir(profile).entryList(QDir::Files).size() <= 50);
		QVERIFY(sentinel.open(QIODevice::ReadOnly));
		QCOMPARE(sentinel.readAll(), QByteArray("preserve"));
	}
};

QTEST_GUILESS_MAIN(MprisArtCacheTest)

#include "mpris_art_cache_test.moc"

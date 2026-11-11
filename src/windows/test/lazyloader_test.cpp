#include <qcoreapplication.h>
#include <qpointer.h>
#include <qqmlengine.h>
#include <qqmlincubator.h>
#include <qsignalspy.h>
#include <qtest.h>

#include "../../core/lazyloader.hpp"

class LazyLoaderTest: public QObject {
	Q_OBJECT;

private slots:
	void incubationFailureCanRetry_data() {
		QTest::addColumn<bool>("forceCompletion");
		QTest::newRow("asynchronous") << false;
		QTest::newRow("activation-forces-completion") << true;
	}

	void incubationFailureCanRetry() {
		QFETCH(bool, forceCompletion);
		QQmlEngine engine;
		QQmlIncubationController controller;
		engine.setIncubationController(&controller);
		QQmlComponent broken(&engine);
		broken.setData("import QtQml\nQtObject { required property int missing }", QUrl());
		QVERIFY2(broken.isReady(), qPrintable(broken.errorString()));
		QQmlComponent valid(&engine);
		valid.setData("import QtQml\nQtObject { objectName: \"recovered\" }", QUrl());
		QVERIFY2(valid.isReady(), qPrintable(valid.errorString()));

		LazyLoader loader;
		// Only the shell-generation integration is stubbed. Incubation uses
		// the real loader, QsQmlIncubator and Qt engine/controller.
		loader.reloadComplete = true;
		loader.setComponent(&broken);
		QSignalSpy loading(&loader, &LazyLoader::loadingChanged);
		QPointer<QsQmlIncubator> failed;
		connect(&loader, &LazyLoader::loadingChanged, &loader, [&] {
			if (loader.isLoading()) failed = loader.findChild<QsQmlIncubator*>();
		});
		loader.setLoading(true);
		QVERIFY(loader.isLoading());
		QVERIFY(failed);
		if (forceCompletion) loader.setActive(true);
		else controller.incubateFor(1000);

		QVERIFY(!loader.isLoading());
		QVERIFY(!loader.isActive());
		QCOMPARE(loader.item(), nullptr);
		QCOMPARE(loading.count(), 2);
		// Deleting inside statusChanged is unsafe. The failed incubator must
		// survive the callback and disappear at the deferred-delete boundary.
		QVERIFY(failed);
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		QVERIFY(!failed);

		loader.setComponent(&valid);
		if (forceCompletion) loader.setActive(true);
		else {
			loader.setLoading(true);
			QVERIFY(loader.isLoading());
			controller.incubateFor(1000);
		}
		QVERIFY(loader.isActive());
		QVERIFY(!loader.isLoading());
		QCOMPARE(loader.item()->objectName(), QString("recovered"));
		loader.setActive(false);
		QVERIFY(!loader.isActive());
		loader.setLoading(true);
		QVERIFY(loader.isLoading());
		controller.incubateFor(1000);
		QVERIFY(loader.isActive());
		QVERIFY(!loader.isLoading());
	}
};

QTEST_GUILESS_MAIN(LazyLoaderTest)

#include "lazyloader_test.moc"

#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQuickItem>
#include <QQuickStyle>
#include "RelayUpdater.h"

namespace {
const QString oldVersion="20261101T000000Z-1",newVersion="20261102T000000Z-2";
}
class RelayUpdaterTest : public QObject {
    Q_OBJECT
    static void fixture(RelayUpdater &updater,QTemporaryDir &dir,const QString &mode) {
        QFile script(dir.filePath("fixture.py"));QVERIFY(script.open(QIODevice::WriteOnly));script.write("# generated fixture\n");script.close();
        updater.m_enabled=true;updater.m_root=dir.path();updater.m_program=QCoreApplication::applicationFilePath();updater.m_script=script.fileName();updater.m_current=oldVersion;
        auto env=QProcessEnvironment::systemEnvironment();env.insert("RELAY_TEST_UPDATER_MODE",mode);updater.m_process.setProcessEnvironment(env);
        emit updater.changed();
    }
private slots:
    void manualCheckShowsAvailableVersionAndDoesNotRestart() {
        QTemporaryDir dir;RelayUpdater updater(false);fixture(updater,dir,"update");
        QVERIFY(updater.supported());QSignalSpy starts(&updater.m_process,&QProcess::started);
        updater.check();QVERIFY(updater.busy());updater.check();
        QTRY_VERIFY(!updater.busy());QCOMPARE(starts.size(),1);QVERIFY(updater.available());
        QCOMPARE(updater.currentVersion(),oldVersion);QCOMPARE(updater.availableVersion(),newVersion);QVERIFY(updater.error().isEmpty());QVERIFY(!updater.lastChecked().isEmpty());
    }
    void failedCheckIsVisibleAndCanBeRetried() {
        QTemporaryDir dir;RelayUpdater updater(false);fixture(updater,dir,"error");updater.check();
        QTRY_VERIFY(!updater.busy());QVERIFY(updater.status().contains("failed"));QVERIFY(updater.error().contains("Generated network failure"));QVERIFY(!updater.available());
        fixture(updater,dir,"current");updater.check();QTRY_VERIFY(!updater.busy());QVERIFY(updater.error().isEmpty());QVERIFY(updater.status().contains("latest published"));
    }
    void malformedProcessOutputIsNotReportedAsUpToDate() {
        QTemporaryDir dir;RelayUpdater updater(false);fixture(updater,dir,"invalid");updater.check();
        QTRY_VERIFY(!updater.busy());QVERIFY(!updater.error().isEmpty());QVERIFY(!updater.status().contains("latest published"));
    }
    void visibleCheckButtonWorksWhenNoUpdateIsKnown() {
        QTemporaryDir dir;RelayUpdater updater(false);fixture(updater,dir,"current");
        QQmlEngine engine;engine.rootContext()->setContextProperty("relayUpdater",&updater);
        QQmlComponent component(&engine,QUrl::fromLocalFile(QFINDTESTDATA("../qml/components/UpdatesSettings.qml")));
        QTRY_VERIFY(component.status()!=QQmlComponent::Loading);QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        QScopedPointer<QObject> pane(component.createWithInitialProperties({{"width",700},{"height",520}}));QVERIFY2(pane,qPrintable(component.errorString()));
        QQuickWindow window;window.resize(740,560);auto *item=qobject_cast<QQuickItem *>(pane.data());QVERIFY(item);item->setParentItem(window.contentItem());window.show();
        auto *button=pane->findChild<QQuickItem *>("relayUpdateButton");QVERIFY(button);QVERIFY(button->isVisible());QVERIFY(button->isEnabled());QCOMPARE(button->property("text").toString(),"Check for updates");
        QTest::qWait(50);auto at=button->mapToScene(QPointF(button->width()/2,button->height()/2));QTest::mouseClick(&window,Qt::LeftButton,Qt::NoModifier,at.toPoint());
        QTRY_VERIFY(!updater.lastChecked().isEmpty());QTRY_VERIFY(!updater.busy());QVERIFY(button->isVisible());QVERIFY(!updater.available());QVERIFY(updater.status().contains("latest published"));
        if(qEnvironmentVariableIsSet("RELAY_UPDATE_SCREENSHOT")){QTest::qWait(50);QVERIFY(window.grabWindow().save(qEnvironmentVariable("RELAY_UPDATE_SCREENSHOT")));}
        item->setParentItem(nullptr);
    }
    void installRequiresExplicitRestartConfirmation() {
        QTemporaryDir dir;RelayUpdater updater(false);fixture(updater,dir,"update");updater.check();QTRY_VERIFY(!updater.busy());QVERIFY(updater.available());
        QQmlEngine engine;engine.rootContext()->setContextProperty("relayUpdater",&updater);
        QQmlComponent component(&engine,QUrl::fromLocalFile(QFINDTESTDATA("../qml/components/UpdatesSettings.qml")));
        QTRY_VERIFY(component.status()!=QQmlComponent::Loading);QScopedPointer<QObject> pane(component.createWithInitialProperties({{"width",700},{"height",520}}));QVERIFY2(pane,qPrintable(component.errorString()));
        QQuickWindow window;window.resize(740,560);auto *item=qobject_cast<QQuickItem *>(pane.data());item->setParentItem(window.contentItem());window.show();
        auto *button=pane->findChild<QQuickItem *>("relayUpdateButton");QVERIFY(button);QCOMPARE(button->property("text").toString(),"Update and restart");
        QVERIFY(QMetaObject::invokeMethod(button,"clicked"));auto *dialog=pane->findChild<QObject *>("relayUpdateConfirmation");QVERIFY(dialog);QTRY_VERIFY(dialog->property("visible").toBool());QVERIFY(!updater.busy());
        QVERIFY(QMetaObject::invokeMethod(dialog,"reject"));QTRY_VERIFY(!dialog->property("visible").toBool());QVERIFY(!updater.busy());item->setParentItem(nullptr);
    }
};
int main(int argc,char **argv) {
    const auto mode=qgetenv("RELAY_TEST_UPDATER_MODE");
    if(!mode.isEmpty()) {
        if(mode=="invalid"){fputs("not a JSON update result\n",stdout);return 0;}
        QJsonObject result{{"current_version",oldVersion},{"available_version",mode=="update"?newVersion:QString()},{"runtime_path","generated fixture"},{"error",mode=="error"?"Generated network failure":""}};
        auto data=QJsonDocument(result).toJson(QJsonDocument::Compact);fwrite(data.constData(),1,size_t(data.size()),stdout);return mode=="error"?2:0;
    }
    QGuiApplication app(argc,argv);QQuickStyle::setStyle("Basic");RelayUpdaterTest test;return QTest::qExec(&test,argc,argv);
}
#include "relay_updater_test.moc"

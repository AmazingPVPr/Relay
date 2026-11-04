#include <QtTest>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickItem>
#include <QFileInfo>
#include "ChannelTools.h"
#include "DiscordClient.h"

class SearchUiTest : public QObject {
    Q_OBJECT
private slots:
    void keyboardCompletionAndSubmission() {
        DiscordClient client;
        ChannelTools tools(&client);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("channelTools", &tools);
        const auto directory = QFileInfo(QFINDTESTDATA("../qml/components/MessageSearchField.qml")).absolutePath();
        QQmlComponent component(&engine);
        component.setData(R"(
import QtQuick
import QtQuick.Controls
Item {
    width: 600; height: 500
    MessageSearchField { objectName: "search"; width: 400 }
})", QUrl::fromLocalFile(directory + "/generated-search-fixture.qml"));
        QScopedPointer<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        QQuickWindow window;
        auto *item = qobject_cast<QQuickItem *>(root.data());
        item->setParentItem(window.contentItem());
        window.resize(600, 500); window.show();
        auto *field = root->findChild<QQuickItem *>("search"); QVERIFY(field);
        field->forceActiveFocus();
        QSignalSpy accepted(field, SIGNAL(accepted()));
        QTest::keyClick(&window, Qt::Key_F); QTest::keyClick(&window, Qt::Key_R); QTest::keyClick(&window, Qt::Key_O);
        QTRY_COMPARE(field->property("suggestions").toList().size(), 1);
        QTest::keyClick(&window, Qt::Key_Tab);
        QCOMPARE(field->property("text").toString(), QString("from:"));
        QCOMPARE(accepted.size(), 0);
        QTest::keyClick(&window, Qt::Key_Down);
        QTest::keyClick(&window, Qt::Key_Return);
        QCOMPARE(field->property("text").toString(), QString("from:me "));
        QCOMPARE(accepted.size(), 0);
        QTest::keyClick(&window, Qt::Key_Escape);
        QTest::keyClick(&window, Qt::Key_Return);
        QCOMPARE(accepted.size(), 1);
        item->setParentItem(nullptr);
    }
};
QTEST_MAIN(SearchUiTest)
#include "search_ui_test.moc"

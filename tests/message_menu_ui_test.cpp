#include <QtTest>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QQuickWindow>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QImage>
#include <QPainter>
#include <QImageReader>
#include "AppSettings.h"
#include "ChatText.h"
#include "ChannelTools.h"
#include "DiscordClient.h"
#include "CrispImage.h"
#include "ImageEditor.h"
#include "SoftwareVideo.h"
#include "NativeMediaSource.h"

class MenuService : public QQmlPropertyMap {
    Q_OBJECT
public:
    MenuService() : QQmlPropertyMap(this, nullptr) {}
signals:
    void stateChanged();
    void componentError(QString messageId, QString error);
    void componentFinished(QString messageId);
};

// The production API's ownership, HTTP error and PATCH handling is covered in
// channel_tools_test. These generated UI fixtures control completion ordering
// without connecting to any account or sending a real message.
class MenuActionService : public QQmlPropertyMap {
    Q_OBJECT
public:
    QStringList call;
    MenuActionService() : QQmlPropertyMap(this, nullptr) {
        insert("actionBusy", false); insert("actionError", "");
    }
    Q_INVOKABLE void clearActionError() { insert("actionError", ""); emit changed(); }
    Q_INVOKABLE bool messageAction(QString action, QString channel, QString message, QString text, QString destination) {
        call = {action, channel, message, text, destination};
        insert("actionBusy", true); emit changed(); return true;
    }
    Q_INVOKABLE void setPinned(QString, QString, bool) {}
    void fail() { insert("actionBusy", false); insert("actionError", "Generated temporary failure"); emit changed(); }
    void succeed(QString action = "edit", QString id = "500") {
        insert("actionBusy", false); emit changed(); emit messageActionFinished(action, id);
    }
signals:
    void changed();
    void messageActionFinished(QString action, QString messageId);
};

class GeneratedDraftService : public QQmlPropertyMap {
    Q_OBJECT
public:
    int sends = 0;
    GeneratedDraftService() : QQmlPropertyMap(this, nullptr) {
        insert("attachments", QVariantList{}); insert("count", 0); insert("busy", false);
        insert("error", ""); insert("limitText", "Generated files only"); insert("cancelable", false);
        insert("status", ""); insert("progress", 0);
    }
    void setFiles(QVariantList files) { insert("attachments", files); insert("count", files.size()); emit changed(); }
    Q_INVOKABLE void clearDraft() { setFiles({}); }
    Q_INVOKABLE void removeAttachment(QString id) {
        auto files = value("attachments").toList();
        for (qsizetype i = files.size(); i-- > 0;) if (files[i].toMap().value("id").toString() == id) files.removeAt(i);
        setFiles(files);
    }
    Q_INVOKABLE void setSpoiler(QString, bool) {}
    Q_INVOKABLE void cancel() {}
    Q_INVOKABLE void clearError() {}
signals:
    void changed();
};

class MessageMenuUiTest : public QObject {
    Q_OBJECT
    QTemporaryDir settingsDirectory;
    std::unique_ptr<AppSettings> preferences;
    DiscordClient client;
    ChatText text;
    MenuService service;
    MenuActionService actions;
    GeneratedDraftService uploads;
    std::unique_ptr<QQmlEngine> engine;
    std::unique_ptr<QObject> root;
    QQuickWindow *window = nullptr;
    QQuickItem *message = nullptr;
    QStringList warnings;
    QQuickItem *visualItem(QQuickItem *parent, const QString &name) {
        if (parent->objectName() == name) return parent;
        for (auto *child : parent->childItems()) if (auto *found = visualItem(child, name)) return found;
        return nullptr;
    }
    QQuickItem *item(const char *name) {
        if (auto *found = root->findChild<QQuickItem *>(name)) return found;
        return visualItem(window->contentItem(), QString::fromLatin1(name));
    }
    void invoke(QObject *object, const char *method) { QVERIFY(QMetaObject::invokeMethod(object, method)); }
    void click(QQuickItem *item) {
        QVERIFY(item); QVERIFY(item->isVisible()); QVERIFY(item->isEnabled());
        QTest::qWait(50); // Let layouts polish before mapping the target to its actual window.
        QTest::mouseClick(item->window(), Qt::LeftButton, Qt::NoModifier, item->mapToScene(QPointF(item->width()/2, item->height()/2)).toPoint());
        QCoreApplication::processEvents();
    }
    void beginEdit() {
        auto *body = item("messageBody"); QVERIFY(body);
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, body->mapToScene(QPointF(45, 10)).toPoint());
        auto *menu = root->findChild<QObject *>("messageContextMenu"); QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());
        QTest::qWait(50); // Wait for the popup's position/implicit size polish.
        click(item("editMessageAction"));
        QTRY_VERIFY(message->property("editing").toBool());
        QTRY_VERIFY(item("inlineMessageEditText")->hasActiveFocus());
    }
private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        qmlRegisterType<ImageEditor>("Relay.Native",1,0,"ImageEditor");
        qmlRegisterType<CrispImage>("Relay.Native", 1, 0, "CrispImage");
        qmlRegisterType<SoftwareVideo>("Relay.Native", 1, 0, "SoftwareVideo");
        qmlRegisterType<NativeMediaSource>("Relay.Native", 1, 0, "NativeMediaSource");
    }
    void init() {
        warnings.clear(); actions.call.clear(); actions.insert("actionBusy", false); actions.insert("actionError", "");
        uploads.setFiles({});
        preferences = std::make_unique<AppSettings>(settingsDirectory.filePath("generated.ini"));
        service.insert("connected", true); service.insert("demoMode", false);
        service.insert("currentChannelId", "101"); service.insert("currentUserId", "42");
        service.insert("channels", QVariant::fromValue(client.channels()));
        service.insert("dms", QVariant::fromValue(client.dms()));
        engine = std::make_unique<QQmlEngine>();
        connect(engine.get(), &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors) warnings.append(error.toString());
        });
        engine->rootContext()->setContextProperty("preferences", preferences.get());
        engine->rootContext()->setContextProperty("discord", &service);
        engine->rootContext()->setContextProperty("channelTools", &actions);
        engine->rootContext()->setContextProperty("chatText", &text);
        engine->rootContext()->setContextProperty("uploads", &uploads);
        engine->rootContext()->setContextProperty("softwareRendering", true);
        const auto directory = QFileInfo(QFINDTESTDATA("../qml/components/MessageItem.qml")).absolutePath();
        QQmlComponent component(engine.get());
        component.setData(R"(
import QtQuick
import QtQuick.Controls
ApplicationWindow {
    visible: true; width: 760; height: 760; color: Qt.rgba(0.16, 0.16, 0.18, 1)
    MessageItem {
        objectName: "fixtureMessage"
        y: 40; width: parent.width
        message: ({recordId:"500", channelId:"101", guildId:"100", own:true, author:"Generated author", authorId:"42",
            content:"Generated message body for right-click testing", rawContent:"Generated message body for right-click testing",
            timestamp:"2026-10-28T12:00:00Z", reactions:[{emoji:"👍",label:"👍",count:2,me:true,customId:""}]})
    }
    AttachmentTray { objectName: "fixtureTray"; x: 20; y: 510; width: parent.width - 40 }
})", QUrl::fromLocalFile(directory + "/GeneratedMessageMenu.qml"));
        root.reset(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        window = qobject_cast<QQuickWindow *>(root.get()); QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        message = item("fixtureMessage"); QVERIFY(message);
    }
    void cleanup() {
        root.reset(); engine.reset(); preferences.reset();
        QCoreApplication::processEvents();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }
    void bodyRightClickAndOwnership() {
        auto *body = item("messageBody"); QVERIFY(body);
        auto *menu = root->findChild<QObject *>("messageContextMenu"); QVERIFY(menu);
        auto *deletion = root->findChild<QObject *>("deleteMessageAction"); QVERIFY(deletion);
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, body->mapToScene(QPointF(45, 10)).toPoint());
        QTRY_VERIFY(menu->property("visible").toBool());
        QVERIFY(deletion->property("visible").toBool());
        QVERIFY(menu->property("height").toReal() <= window->height());
        QVERIFY(window->grabWindow().save("/workspace/work/message-menu-generated.png"));
        invoke(menu, "close");
        auto row = message->property("message").toMap(); row.insert("own", false); message->setProperty("message", row);
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, body->mapToScene(QPointF(45, 10)).toPoint());
        QTRY_VERIFY(menu->property("visible").toBool());
        QVERIFY(!deletion->property("visible").toBool());
        QVERIFY(!item("editMessageAction")->isVisible());
        invoke(menu, "close");
        invoke(message, "beginEdit");
        QVERIFY(!message->property("editing").toBool());
    }
    void embedsAndFieldsUseTheOriginalMessageReferenceContext() {
        auto row=message->property("message").toMap();
        row.insert("mentionChannels",QVariantList{QVariantMap{{"id","101"},{"name","announcements"},{"known",true},{"accessible",true}}});
        row.insert("embeds",QVariantList{QVariantMap{{"kind","link"},{"title","Discord announcements"},
            {"description","Use <#101> and <id:customize>."},
            {"fields",QVariantList{QVariantMap{{"name","See <#101>"},{"value","Choose <id:customize>"}}}}}});
        message->setProperty("message",row);
        QTRY_VERIFY(item("embedDescription"));
        QVERIFY(item("embedDescription")->property("text").toString().contains("#announcements"));
        QVERIFY(item("embedDescription")->property("text").toString().contains("relay-guild:100:customize"));
        QTRY_VERIFY(item("embedFieldName"));QTRY_VERIFY(item("embedFieldValue"));
        QVERIFY(item("embedFieldName")->property("text").toString().contains("relay-channel:101"));
        QVERIFY(item("embedFieldValue")->property("text").toString().contains("relay-guild:100:customize"));
        row.insert("guildId","200");message->setProperty("message",row);
        QTRY_VERIFY(item("embedDescription")->property("text").toString().contains("relay-guild:200:customize"));
        QVERIFY(!item("embedDescription")->property("text").toString().contains("relay-guild:100:"));
        QVERIFY(window->grabWindow().save("/workspace/work/message-reference-embed-generated.png"));
    }
    void inlineEditKeyboardFailureRetryAndSuccess() {
        const auto original = message->property("message").toMap().value("rawContent").toString();
        beginEdit();
        auto *editor = item("inlineMessageEditText"); QVERIFY(editor);
        QCOMPARE(editor->property("text").toString(), original);
        QVERIFY(!item("messageBody")->isVisible());
        QVERIFY(!root->findChild<QObject *>("messageActionDialog")->property("visible").toBool());
        QVERIFY(editor->mapToScene(QPointF()).y() > message->y());
        QVERIFY(window->grabWindow().save("/workspace/work/message-inline-edit-generated.png"));
        QTest::keyClick(window, Qt::Key_Return, Qt::ShiftModifier);
        QVERIFY(editor->property("text").toString().contains('\n'));
        QVERIFY(actions.call.isEmpty());
        editor->setProperty("text", "Revised **message**\nwith a second line");
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(actions.call, QStringList({"edit", "101", "500", "Revised **message**\nwith a second line", ""}));
        QVERIFY(message->property("editing").toBool());
        QVERIFY(!editor->isEnabled());
        actions.fail();
        QTRY_VERIFY(editor->isEnabled());
        QVERIFY(message->property("editing").toBool());
        QCOMPARE(editor->property("text").toString(), "Revised **message**\nwith a second line");
        QCOMPARE(message->property("editError").toString(), "Generated temporary failure");
        click(item("inlineMessageSave"));
        actions.succeed("edit", "501"); // Unrelated completion cannot close this editor.
        QVERIFY(message->property("editing").toBool());
        actions.succeed();
        QTRY_VERIFY(!message->property("editing").toBool());
        QVERIFY(item("messageBody")->isVisible());
    }
    void cancelNoopValidationAndRecycledMessage() {
        beginEdit();
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!message->property("editing").toBool());
        QVERIFY(actions.call.isEmpty());
        beginEdit();
        QTest::keyClick(window, Qt::Key_Return); // Unchanged text closes without an API request.
        QTRY_VERIFY(!message->property("editing").toBool());
        QVERIFY(actions.call.isEmpty());
        beginEdit();
        auto *editor = item("inlineMessageEditText");
        editor->setProperty("text", " ");
        QVERIFY(!item("inlineMessageSave")->isEnabled());
        QTest::keyClick(window, Qt::Key_Return); QVERIFY(actions.call.isEmpty());
        editor->setProperty("text", QString(2001, 'x'));
        QVERIFY(!item("inlineMessageSave")->isEnabled());
        editor->setProperty("text", "unsaved original message");
        auto row = message->property("message").toMap();
        row.insert("recordId", "501"); row.insert("content", "different message"); row.insert("rawContent", "different message");
        message->setProperty("message", row);
        QTRY_VERIFY(!message->property("editing").toBool());
        QCOMPARE(editor->property("text").toString(), "");
        beginEdit(); QCOMPARE(editor->property("text").toString(), "different message");
        editor->setProperty("text", "Draft survives reconnect");
        service.insert("connected", false); emit service.stateChanged();
        QVERIFY(message->property("editing").toBool());
        QCOMPARE(editor->property("text").toString(), "Draft survives reconnect");
        QVERIFY(!item("inlineMessageSave")->isEnabled());
        service.insert("connected", true); emit service.stateChanged();
        QVERIFY(item("inlineMessageSave")->isEnabled());
        service.insert("currentChannelId", "202"); emit service.stateChanged();
        QTRY_VERIFY(!message->property("editing").toBool());
        QVERIFY(actions.call.isEmpty());
    }
    void draftImageOpensLocallyBeforeSendAndClosesOnRemoval_data() {
        QTest::addColumn<QSize>("dimensions");
        QTest::addColumn<bool>("animated");
        QTest::addColumn<bool>("spoiler");
        QTest::newRow("landscape") << QSize(640,360) << false << false;
        QTest::newRow("portrait-spoiler") << QSize(900,1600) << false << true;
        QTest::newRow("large-wide") << QSize(4096,1024) << false << false;
        QTest::newRow("animated-first-frame") << QSize(16,8) << true << false;
    }
    void draftImageOpensLocallyBeforeSendAndClosesOnRemoval() {
        QFETCH(QSize, dimensions); QFETCH(bool, animated); QFETCH(bool, spoiler);
        QTemporaryDir files;
        const auto path = files.filePath(animated ? "generated image.gif" : "generated image.png");
        if (animated) {
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
            // Two generated 16x8 red/blue frames, also used by attachment_upload_test.
            const auto bytes = QByteArray::fromHex("47494638396110000800810000ff000000000000000000000021ff0b4e45545343415045322e30030100000021f90400040000002c00000000100008000008150001081c48b0a0c18308132a5cc8b0a1c3870803020021f90401040001002c0000000010000800810000ff00000000000000000008150001081c48b0a0c18308132a5cc8b0a1c387080302003b");
            QCOMPARE(file.write(bytes), bytes.size()); file.close();
            QImageReader reader(path); QVERIFY(reader.supportsAnimation()); QCOMPARE(reader.imageCount(), 2);
        } else {
            QImage generated(dimensions, QImage::Format_RGB32); generated.fill(QColor("#5079af"));
            QPainter painter(&generated);
            painter.fillRect(0,0,dimensions.width()/2,dimensions.height()/2,QColor("#68c7b8"));
            painter.fillRect(dimensions.width()/2,dimensions.height()/2,dimensions.width()/2,dimensions.height()/2,QColor("#e0a061"));
            painter.end(); QVERIFY(generated.save(path));
        }
        const auto url = QUrl::fromLocalFile(path).toString();
        uploads.setFiles({QVariantMap{{"id","1"},{"name",QFileInfo(path).fileName()},{"mime",animated ? "image/gif" : "image/png"},{"previewUrl",url},{"size",QFileInfo(path).size()},{"spoiler",spoiler}}});
        QTRY_VERIFY(item("attachmentPreview_1"));
        click(item("attachmentPreview_1"));
        auto *preview = root->findChild<QObject *>("draftImagePreview"); QVERIFY(preview);
        QTRY_VERIFY(preview->property("visible").toBool());
        auto *image = item("draftPreviewImage"); QVERIFY(image);
        QTRY_COMPARE(image->property("status").toInt(), 1);
        QCOMPARE(image->property("source").toUrl(), QUrl::fromLocalFile(path));
        QCOMPARE(uploads.value("count").toInt(), 1);
        QVERIFY(actions.call.isEmpty());
        QVERIFY(image->property("paintedWidth").toReal() <= image->width() + 1);
        QVERIFY(image->property("paintedHeight").toReal() <= image->height() + 1);
        QVERIFY(qAbs(image->property("paintedWidth").toReal() / image->property("paintedHeight").toReal()
                     - double(dimensions.width()) / dimensions.height()) < .01);
        // No AnimatedImage/QMovie is created; the bounded first frame is stable.
        QCOMPARE(image->metaObject()->indexOfProperty("playing"), -1);
        QVERIFY(window->grabWindow().save(QString("/workspace/work/draft-image-preview-%1.png").arg(QTest::currentDataTag())));

        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!preview->property("visible").toBool());
        QCOMPARE(uploads.value("count").toInt(), 1);
        click(item("attachmentPreview_1"));
        uploads.clearDraft();
        QTRY_VERIFY(!preview->property("visible").toBool());
        QTRY_VERIFY(image->property("source").toUrl().isEmpty());
    }
    void nonImagesAndUntrustedPreviewUrlsCannotOpen() {
        const auto bad = QVariantMap{{"id","2"},{"name","not-an-image.zip"},{"mime","application/zip"},{"previewUrl","file:///tmp/non-image.zip"},{"size",123}};
        uploads.setFiles({bad});
        QTRY_VERIFY(item("attachmentPreview_2"));
        QVERIFY(!item("attachmentPreview_2")->isEnabled());
        auto *tray = item("fixtureTray");
        QVERIFY(QMetaObject::invokeMethod(tray, "openPreview", Q_ARG(QVariant, QVariant(bad))));
        QVERIFY(!root->findChild<QObject *>("draftImagePreview")->property("visible").toBool());
        auto network = bad; network["id"]="3"; network["name"]="remote.png"; network["mime"]="image/png"; network["previewUrl"]="https://example.invalid/private.png";
        uploads.setFiles({network});
        QTRY_VERIFY(item("attachmentPreview_3"));
        QVERIFY(!item("attachmentPreview_3")->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(tray, "openPreview", Q_ARG(QVariant, QVariant(network))));
        QVERIFY(!root->findChild<QObject *>("draftImagePreview")->property("visible").toBool());
    }
};
QTEST_MAIN(MessageMenuUiTest)
#include "message_menu_ui_test.moc"

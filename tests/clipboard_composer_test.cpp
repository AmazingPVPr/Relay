#include <QtTest>
#include <QClipboard>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeySequence>
#include <QMimeData>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <memory>

// This executable forces Qt's isolated offscreen platform before constructing
// QGuiApplication. Clipboard contents are generated here and can never reach
// the desktop clipboard. The upload mock never reads files or sends messages.
class GeneratedClipboardUpload final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy MEMBER busy NOTIFY stateChanged)
public:
    bool busy = false;
    bool consumes = true;
    int pastes = 0;
    Q_INVOKABLE bool pasteFromClipboard() { ++pastes; return consumes; }
signals:
    void stateChanged();
};

class ClipboardComposerTest final : public QObject {
    Q_OBJECT
    std::unique_ptr<QQmlEngine> engine;
    std::unique_ptr<QObject> root;
    GeneratedClipboardUpload uploads;
    QQuickWindow *window = nullptr;
    QQuickItem *editor = nullptr;
    QQuickItem *other = nullptr;
    QStringList warnings;

    bool evaluate(const QString &expression, QObject *scope = nullptr) {
        QQmlExpression expressionObject(engine->rootContext(), scope ? scope : editor, expression);
        expressionObject.evaluate();
        if (expressionObject.hasError()) { qWarning() << expressionObject.error(); return false; }
        return true;
    }
    void focusEditor() {
        window->requestActivate(); editor->forceActiveFocus();
        QTRY_VERIFY(editor->hasActiveFocus());
    }
    void generatedText(const QString &text, const QString &html = {}) {
        auto *mime = new QMimeData;
        mime->setText(text);
        if (!html.isEmpty()) mime->setHtml(html);
        QGuiApplication::clipboard()->setMimeData(mime);
    }
    void pasteKey(Qt::Key key = Qt::Key_V, Qt::KeyboardModifiers modifiers = Qt::ControlModifier) {
        QTest::keyClick(window, key, modifiers); QCoreApplication::processEvents();
    }
    QObject *openContextMenu() {
        focusEditor();
        QTest::qWait(25);
        const auto position = editor->mapToScene(QPointF(40, 20)).toPoint();
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, position);
        QCoreApplication::processEvents();
        return root->findChild<QObject *>("composerEditingMenu");
    }
    void clickPasteAction() {
        auto *action = root->findChild<QQuickItem *>("composerPasteAction");
        QVERIFY(action); QVERIFY(action->isVisible()); QVERIFY(action->isEnabled());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          action->mapToScene(QPointF(action->width() / 2, action->height() / 2)).toPoint());
        QCoreApplication::processEvents();
    }
private slots:
    void initTestCase() { QCOMPARE(QGuiApplication::platformName(), "offscreen"); }
    void init() {
        warnings.clear(); uploads.busy = false; uploads.consumes = true; uploads.pastes = 0;
        generatedText("generated unused clipboard text");
        engine = std::make_unique<QQmlEngine>();
        connect(engine.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors) warnings.append(error.toString());
        });
        engine->rootContext()->setContextProperty("fixtureUploads", &uploads);
        const auto directory = QFileInfo(QFINDTESTDATA("../qml/components/AttachmentComposer.qml")).absolutePath();
        QQmlComponent component(engine.get());
        const auto fixture = QString(R"(
import QtQuick
import QtQuick.Controls
import "%1"
ApplicationWindow {
    width: 540; height: 300; visible: true
    property int sends: 0
    AttachmentComposer {
        objectName: "fixtureComposer"; x: 20; y: 20; width: 500; height: 150
        attachmentService: fixtureUploads; text: "hello old world"
        onEditingKeyPressed: event => {
            if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !(event.modifiers & Qt.ShiftModifier)) {
                sends++; event.accepted = true
            }
        }
    }
    TextField { objectName: "fixtureOtherInput"; x: 20; y: 210; width: 500; text: "search " }
}
)").arg(QUrl::fromLocalFile(directory).toString());
        component.setData(fixture.toUtf8(), QUrl::fromLocalFile(directory + "/generated-composer-fixture.qml"));
        root.reset(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        window = qobject_cast<QQuickWindow *>(root.get()); QVERIFY(window);
        editor = root->findChild<QQuickItem *>("fixtureComposer"); QVERIFY(editor);
        other = root->findChild<QQuickItem *>("fixtureOtherInput"); QVERIFY(other);
        focusEditor();
    }
    void cleanup() {
        root.reset(); engine.reset(); QCoreApplication::processEvents();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }
    void attachmentPastePreservesDraftAndSelection() {
        QVERIFY(evaluate("select(6, 9)"));
        const int cursor = editor->property("cursorPosition").toInt();
        const bool undo = editor->property("canUndo").toBool();
        pasteKey();
        QCOMPARE(uploads.pastes, 1);
        QCOMPARE(editor->property("text").toString(), "hello old world");
        QCOMPARE(editor->property("selectedText").toString(), "old");
        QCOMPARE(editor->property("cursorPosition").toInt(), cursor);
        QCOMPARE(editor->property("canUndo").toBool(), undo);
        QCOMPARE(root->property("sends").toInt(), 0);
    }
    void textPasteKeepsNativeSelectionCaretUndoAndRedo() {
        uploads.consumes = false;
        generatedText("**bold**\nnew line");
        QVERIFY(evaluate("select(6, 9)")); pasteKey();
        QCOMPARE(uploads.pastes, 1);
        QCOMPARE(editor->property("text").toString(), "hello **bold**\nnew line world");
        QCOMPARE(editor->property("cursorPosition").toInt(), 6 + QString("**bold**\nnew line").size());
        QVERIFY(evaluate("undo()")); QCOMPARE(editor->property("text").toString(), "hello old world");
        QVERIFY(evaluate("redo()")); QCOMPARE(editor->property("text").toString(), "hello **bold**\nnew line world");
        QCOMPARE(root->property("sends").toInt(), 0);
    }
    void richClipboardPastesPlainText() {
        uploads.consumes = false;
        generatedText("literal <tag> and **markup**", "<b>formatted</b>");
        QVERIFY(evaluate("selectAll()")); pasteKey();
        QCOMPARE(editor->property("text").toString(), "literal <tag> and **markup**");
        QCOMPARE(editor->property("textFormat").toInt(), 0);
    }
    void shiftInsertUsesStandardPaste() {
        const auto alternate = QKeySequence(Qt::SHIFT | Qt::Key_Insert);
        if (!QKeySequence::keyBindings(QKeySequence::Paste).contains(alternate))
            QSKIP("This Qt platform does not bind Shift+Insert to the standard Paste action.");
        pasteKey(Qt::Key_Insert, Qt::ShiftModifier);
        QCOMPARE(uploads.pastes, 1); QCOMPARE(editor->property("text").toString(), "hello old world");
    }
    void busyAttachmentDoesNotSpillFileUris() {
        uploads.busy = true;
        generatedText("file:///generated/private-name.zip"); pasteKey();
        QCOMPARE(uploads.pastes, 1); QCOMPARE(editor->property("text").toString(), "hello old world");
    }
    void busyUploadStillAllowsTextEditing() {
        uploads.busy = true; uploads.consumes = false; generatedText("while uploading");
        QVERIFY(evaluate("selectAll()")); pasteKey();
        QCOMPARE(uploads.pastes, 1); QCOMPARE(editor->property("text").toString(), "while uploading");
    }
    void otherInputKeepsNativePaste() {
        generatedText("some message text"); other->forceActiveFocus();
        QVERIFY(evaluate("cursorPosition = text.length", other)); pasteKey();
        QCOMPARE(uploads.pastes, 0); QCOMPARE(other->property("text").toString(), "search some message text");
        QCOMPARE(editor->property("text").toString(), "hello old world");
    }
    void readOnlyDoesNotReadClipboard() {
        editor->setProperty("readOnly", true); pasteKey();
        QCOMPARE(uploads.pastes, 0); QCOMPARE(editor->property("text").toString(), "hello old world");
    }
    void disabledComposerDoesNotReadClipboard() {
        editor->setEnabled(false); pasteKey();
        QCOMPARE(uploads.pastes, 0); QCOMPARE(editor->property("text").toString(), "hello old world");
    }
    void ordinaryTypingAndSendKeyRemainNative() {
        QVERIFY(evaluate("selectAll()")); for (const char letter : QByteArray("draft")) QTest::keyClick(window, letter);
        QTest::keyClick(window, Qt::Key_Return, Qt::ShiftModifier);
        for (const char letter : QByteArray("line")) QTest::keyClick(window, letter);
        QCOMPARE(editor->property("text").toString(), "draft\nline");
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(root->property("sends").toInt(), 1); QCOMPARE(uploads.pastes, 0);
        QCOMPARE(editor->property("text").toString(), "draft\nline");
    }
    void contextPasteStagesAttachmentEvenWithoutNativeTextPaste() {
        QGuiApplication::clipboard()->clear();
        QVERIFY(!editor->property("canPaste").toBool());
        auto *menu = openContextMenu(); QVERIFY(menu); QVERIFY(menu->property("visible").toBool());
        clickPasteAction(); QCOMPARE(uploads.pastes, 1);
        QCOMPARE(editor->property("text").toString(), "hello old world"); QCOMPARE(root->property("sends").toInt(), 0);
    }
    void contextPasteUsesNativeTextEditing() {
        uploads.consumes = false; generatedText("context text");
        QVERIFY(evaluate("select(6, 9)"));
        auto *menu = openContextMenu(); QVERIFY(menu); QVERIFY(menu->property("visible").toBool());
        clickPasteAction(); QCOMPARE(uploads.pastes, 1);
        QCOMPARE(editor->property("text").toString(), "hello context text world");
        QVERIFY(evaluate("undo()")); QCOMPARE(editor->property("text").toString(), "hello old world");
    }
    void keyboardContextMenu_data() {
        QTest::addColumn<int>("key");
        QTest::addColumn<int>("modifiers");
        QTest::newRow("Menu") << int(Qt::Key_Menu) << int(Qt::NoModifier);
        QTest::newRow("Shift-F10") << int(Qt::Key_F10) << int(Qt::ShiftModifier);
    }
    void keyboardContextMenu() {
        QFETCH(int, key); QFETCH(int, modifiers);
        QTest::keyClick(window, Qt::Key(key), Qt::KeyboardModifiers(modifiers));
        QCoreApplication::processEvents();
        auto *menu = root->findChild<QObject *>("composerEditingMenu");
        QVERIFY(menu); QVERIFY(menu->property("visible").toBool());
        QCOMPARE(uploads.pastes, 0);
        clickPasteAction();
        QCOMPARE(uploads.pastes, 1);
        QCOMPARE(editor->property("text").toString(), "hello old world");
        QCOMPARE(root->property("sends").toInt(), 0);
    }
};

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    QGuiApplication application(argc, argv);
    ClipboardComposerTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "clipboard_composer_test.moc"

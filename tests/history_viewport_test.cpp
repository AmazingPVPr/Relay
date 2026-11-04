#include "AppSettings.h"
#include "RecordModel.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQmlComponent>
#include <QQuickView>
#include <QQuickItem>
#include <QWheelEvent>
#include <memory>

class HistoryFixtureBackend final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString currentChannelId MEMBER channel NOTIFY stateChanged)
    Q_PROPERTY(bool connected MEMBER connected NOTIFY stateChanged)
    Q_PROPERTY(bool demoMode MEMBER demoMode CONSTANT)
    Q_PROPERTY(bool loadingMessages MEMBER loading NOTIFY stateChanged)
    Q_PROPERTY(bool hasMoreMessages MEMBER hasOlder NOTIFY stateChanged)
    Q_PROPERTY(bool hasNewerMessages MEMBER hasNewer NOTIFY stateChanged)
    Q_PROPERTY(QString error MEMBER error NOTIFY stateChanged)
public:
    RecordModel records;
    QString channel = "200", error;
    bool connected = true, demoMode = false, loading = false, hasOlder = true, hasNewer = false;
    bool duplicateNext = false, failNext = false;
    int olderCalls = 0, newerCalls = 0, completed = 0, delay = 40, pageSize = 50, generation = 0;
    QVariantMap row(int id) const {
        QString content = QString("Generated history %1").arg(id);
        for (int line = 0; line < id % 5; ++line) content += "\nVariable-height fixture line";
        return {{"recordId", QString::number(id)}, {"channelId", channel}, {"content", content}};
    }
    void populate(int first, int count) {
        QVector<QVariantMap> values;
        for (int id = first; id < first + count; ++id) values.append(row(id));
        records.replace(values);
    }
    Q_INVOKABLE void loadOlder() { load(true); }
    Q_INVOKABLE void loadNewer() { load(false); }
    void load(bool older) {
        if (loading) return;
        if (older) ++olderCalls; else ++newerCalls;
        loading = true; emit stateChanged();
        const auto requestGeneration = generation;
        QTimer::singleShot(delay, this, [this, older, requestGeneration] {
            if (requestGeneration != generation) return;
            if (failNext) { failNext = false; error = "Synthetic history failure"; }
            else if (duplicateNext) duplicateNext = false;
            else {
                auto retained = records.rows(); QVector<QVariantMap> desired;
                if (older) {
                    const auto first = retained.first().value("recordId").toInt();
                    for (int id = first - pageSize; id < first; ++id) desired.append(row(id));
                    desired.append(retained);
                    if (desired.size() > records.maximumRows()) { desired.resize(records.maximumRows()); hasNewer = true; }
                } else {
                    desired = retained; const auto last = retained.last().value("recordId").toInt();
                    for (int id = last + 1; id <= last + pageSize; ++id) desired.append(row(id));
                    if (desired.size() > records.maximumRows()) desired.remove(0, desired.size() - records.maximumRows());
                }
                records.reconcile(desired);
            }
            loading = false; ++completed; emit stateChanged();
        });
    }
signals:
    void stateChanged();
};

class HistoryViewportTest final : public QObject {
    Q_OBJECT
    QTemporaryDir settings;
    std::unique_ptr<AppSettings> prefs;
    std::unique_ptr<HistoryFixtureBackend> backend;
    std::unique_ptr<QQuickView> window;
    QObject *view = nullptr, *history = nullptr, *wheel = nullptr;
    void load(int rows = 50, int height = 300, bool following = false) {
        backend->populate(5000, rows);
        window = std::make_unique<QQuickView>();
        window->setResizeMode(QQuickView::SizeRootObjectToView);
        window->rootContext()->setContextProperty("preferences", prefs.get());
        window->rootContext()->setContextProperty("fixtureRecords", &backend->records);
        window->rootContext()->setContextProperty("fixtureBackend", backend.get());
        const auto file = QFINDTESTDATA("../qml/components/HistoryViewport.qml"); QVERIFY(!file.isEmpty());
        QQmlComponent component(window->engine());
        QByteArray body = R"(import QtQuick
Item {
    id: root; width: 400; height: HEIGHT
    property bool following: FOLLOWING
    property bool suspended: false
    property var geometryGrowth: ({})
    ListView {
        id: list; objectName: "viewport"; anchors.fill: parent
        model: fixtureRecords; topMargin: 10; bottomMargin: 8; cacheBuffer: 500
        boundsBehavior: Flickable.StopAtBounds; boundsMovement: Flickable.StopAtBounds
        header: Item { width: list.width; height: 8 }
        onCountChanged: if (root.following) Qt.callLater(list.positionViewAtEnd)
        delegate: Rectangle {
            required property var model
            property var message: model
            width: list.width; height: 36 + String(model.content).split("\n").length * 20 + Number(root.geometryGrowth[model.recordId] || 0)
            onHeightChanged: if (pager && model) pager.delegateGeometryChanged(String(model.recordId))
            color: Number(model.recordId) % 2 ? "#202020" : "#252525"
            Text { anchors.left: parent.left; anchors.leftMargin: 12; anchors.verticalCenter: parent.verticalCenter; text: model.content; color: "white" }
        }
        ScrollBoost { id: boost; objectName: "wheel"; flickable: list }
    }
    HistoryViewport {
        id: pager; objectName: "history"
        view: list; records: fixtureRecords; backend: fixtureBackend; wheel: boost
        followTail: root.following; suspended: root.suspended
        onTailRequested: follow => root.following = follow
        onOlderRequested: fixtureBackend.loadOlder()
        onNewerRequested: fixtureBackend.loadNewer()
    }
    function goTop() { list.positionViewAtBeginning(); list.forceLayout(); list.contentY = list.originY - list.topMargin; list.forceLayout() }
    function goBottom() { list.positionViewAtEnd(); list.forceLayout(); list.contentY = Math.max(list.originY - list.topMargin, list.originY + list.contentHeight + list.bottomMargin - list.height); list.forceLayout() }
    function goIndex(index, offset) { list.positionViewAtIndex(index, ListView.Beginning); list.contentY += offset; list.forceLayout() }
})";
        body.replace("FOLLOWING", following ? "true" : "false");
        body.replace("HEIGHT", QByteArray::number(height));
        component.setData(body, QUrl::fromLocalFile(QFileInfo(file).absolutePath() + "/HistoryViewportHarness.qml"));
        auto *root = component.create(); QVERIFY2(root, qPrintable(component.errorString()));
        window->setContent({}, &component, root); window->resize(400, height); window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get())); QTest::qWait(25);
        view = root->findChild<QObject *>("viewport"); history = root->findChild<QObject *>("history"); wheel = root->findChild<QObject *>("wheel");
        QVERIFY(view); QVERIFY(history); QVERIFY(wheel);
    }
    QVariantMap snapshot() {
        QQmlExpression expression(qmlContext(window->rootObject()), window->rootObject(), R"((function(){
            for(let y=1;y<list.height;y+=8){const index=list.indexAt(5,list.contentY+y);if(index>=0){const item=list.itemAtIndex(index);if(item)return {id:String(fixtureRecords.get(index).recordId),offset:item.y-list.contentY};}}
            return {id:'',offset:0};
        })())");
        const auto value = expression.evaluate();
        if (expression.hasError()) qFatal("%s", qPrintable(expression.error().toString()));
        return value.toMap();
    }
    void sameAnchor(const QVariantMap &before) {
        QQmlExpression expression(qmlContext(window->rootObject()), window->rootObject(),
            QString("(function(){const index=fixtureRecords.indexOf('%1');const item=list.itemAtIndex(index);return {exists:!!item,offset:item?item.y-list.contentY:0};})()").arg(before.value("id").toString()));
        const auto after = expression.evaluate().toMap(); QVERIFY(after.value("exists").toBool());
        QVERIFY2(qAbs(after.value("offset").toDouble() - before.value("offset").toDouble()) < 2.0,
                 qPrintable(QString("Anchor %1 offset moved from %2 to %3").arg(before.value("id").toString()).arg(before.value("offset").toDouble()).arg(after.value("offset").toDouble())));
    }
    void top() { QVERIFY(QMetaObject::invokeMethod(window->rootObject(), "goTop")); QTest::qWait(10); }
    void bottom() { QVERIFY(QMetaObject::invokeMethod(window->rootObject(), "goBottom")); QTest::qWait(10); }
    void go(int index, qreal offset = 0) { QVERIFY(QMetaObject::invokeMethod(window->rootObject(), "goIndex", Q_ARG(QVariant, index), Q_ARG(QVariant, offset))); QTest::qWait(10); }
    void intent(qreal delta) { QVERIFY(QMetaObject::invokeMethod(history, "userScroll", Q_ARG(QVariant, delta))); }
    void mouseWheel(int angle) {
        const QPointF position{150, 150};
        QWheelEvent event(position, window->mapToGlobal(position.toPoint()), {}, {0, angle}, Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(window.get(), &event); QCoreApplication::processEvents();
    }
private slots:
    void init() {
        prefs = std::make_unique<AppSettings>(settings.path() + "/prefs.ini"); prefs->setReducedMotion(true); prefs->setScrollMultiplier(1);
        backend = std::make_unique<HistoryFixtureBackend>();
    }
    void cleanup() { window.reset(); backend.reset(); prefs.reset(); view = history = wheel = nullptr; }
    void cachedRowGeometryGrowthDoesNotMoveTheReadersMessageOrArmPagination() {
        load(500); go(12, 17); const auto before = snapshot();
        const auto above = backend->records.get(10).value("recordId").toString();
        window->rootObject()->setProperty("geometryGrowth", QVariantMap{{above, 180}});
        QTest::qWait(120); sameAnchor(before); QCOMPARE(backend->olderCalls, 0);
        for (int height = 200; height <= 340; height += 20) {
            window->rootObject()->setProperty("geometryGrowth", QVariantMap{{above, height}});
            QTest::qWait(15); sameAnchor(before);
        }
        QCOMPARE(backend->olderCalls, 0);
    }
    void clippedMessageReflowKeepsTheNextVisibleMessageFixed() {
        load(500); go(12, 70);
        const auto nextId = backend->records.get(13).value("recordId").toString();
        QQmlExpression position(qmlContext(window->rootObject()), window->rootObject(),
            "list.itemAtIndex(13).y-list.contentY");
        const QVariantMap next{{"id", nextId}, {"offset", position.evaluate()}};
        QVERIFY(next.value("offset").toDouble() > 0);
        window->rootObject()->setProperty("geometryGrowth", QVariantMap{{backend->records.get(12).value("recordId").toString(), 160}});
        QTest::qWait(120); sameAnchor(next); QCOMPARE(backend->olderCalls, 0);
    }
    void sameCountMessageEditsAboveTheReaderKeepThePixelAnchor() {
        load(500); go(12, 17); const auto before = snapshot();
        const auto above = backend->records.get(10).value("recordId").toString();
        backend->records.patch(above, {{"content", QString("A late edited line\n").repeated(30)}});
        QTest::qWait(120); sameAnchor(before); QCOMPARE(backend->records.count(), 500); QCOMPARE(backend->olderCalls, 0);
    }
    void exactTopPrependPreservesPixelAnchorAndDoesNotPageWithoutMoreInput() {
        load(); top(); const auto before = snapshot(); QCOMPARE(before.value("id").toString(), "5000");
        mouseWheel(120); QTRY_COMPARE_WITH_TIMEOUT(backend->completed, 1, 1500); QTest::qWait(180);
        sameAnchor(before); QCOMPARE(backend->olderCalls, 1); QCOMPARE(backend->records.count(), 100);
        QVERIFY(!view->property("atYBeginning").toBool());
        top(); QTest::qWait(200); QCOMPARE(backend->olderCalls, 1);
        mouseWheel(120); QTRY_COMPARE_WITH_TIMEOUT(backend->completed, 2, 1500); QTest::qWait(150); QCOMPARE(backend->olderCalls, 2);
    }
    void anchorIsCapturedWhenPageAppliesAfterReaderMovesDuringFetch() {
        backend->delay = 180; load(); top(); intent(-160); QTRY_COMPARE_WITH_TIMEOUT(backend->olderCalls, 1, 500);
        go(12, 17); const auto moved = snapshot(); QVERIFY(moved.value("id").toString() != "5000");
        QTRY_COMPARE_WITH_TIMEOUT(backend->completed, 1, 1000); QTest::qWait(100); sameAnchor(moved);
    }
    void boundedPrependAndAppendKeepTheSamePixelAnchor() {
        load(1000); go(1, 9); const auto beforeOlder = snapshot(); intent(-160);
        QTRY_COMPARE_WITH_TIMEOUT(backend->completed, 1, 1500); QTest::qWait(100); sameAnchor(beforeOlder);
        QCOMPARE(backend->records.count(), 1000); QVERIFY(backend->hasNewer);
        bottom(); const auto beforeNewer = snapshot(); intent(160);
        QTRY_COMPARE_WITH_TIMEOUT(backend->completed, 2, 1500); QTest::qWait(100); sameAnchor(beforeNewer);
        QCOMPARE(backend->newerCalls, 1); QCOMPARE(backend->records.count(), 1000);
    }
    void geometryBackendStateAndProgrammaticPositionsDoNotArmPaging() {
        load(); top(); emit backend->stateChanged(); window->resize(470, 340); QTest::qWait(220);
        QCOMPARE(backend->olderCalls, 0); QCOMPARE(backend->newerCalls, 0);
        backend->hasNewer = true; bottom(); emit backend->stateChanged(); QTest::qWait(150);
        QCOMPARE(backend->olderCalls, 0); QCOMPARE(backend->newerCalls, 0);
    }
    void duplicatePageAndFailureCannotCreateAutomaticRetryLoops() {
        load(); top(); backend->duplicateNext = true; intent(-160);
        QTRY_COMPARE_WITH_TIMEOUT(backend->completed, 1, 1000); QTest::qWait(220); QCOMPARE(backend->olderCalls, 1);
        backend->failNext = true; intent(-160); QTRY_COMPARE_WITH_TIMEOUT(backend->completed, 2, 1000);
        QTest::qWait(220); QCOMPARE(backend->olderCalls, 2); QVERIFY(!backend->error.isEmpty());
        backend->error.clear(); emit backend->stateChanged(); QTest::qWait(150); QCOMPARE(backend->olderCalls, 2);
    }
    void shortInitialTailHasAFiniteAutofillBudget() {
        backend->pageSize = 1; load(1, 900, true);
        QTRY_COMPARE_WITH_TIMEOUT(backend->completed, 4, 2000); QTest::qWait(250);
        QCOMPARE(backend->olderCalls, 4); QCOMPARE(backend->records.count(), 5);
    }
    void oppositeDirectionInputDoesNotFetchTheWrongEdge() {
        backend->hasNewer = true; load(); top(); intent(160); QTest::qWait(180); QCOMPARE(backend->olderCalls, 0);
        bottom(); intent(-160); QTest::qWait(180); QCOMPARE(backend->newerCalls, 0);
    }
    void reachingTheNewestEdgeAfterDownwardInputRestoresFollowing() {
        backend->hasNewer = true; backend->duplicateNext = true; load(); bottom(); QVERIFY(view->property("atYEnd").toBool()); intent(160);
        QTRY_COMPARE_WITH_TIMEOUT(backend->completed, 1, 1000);
        QVERIFY(!window->rootObject()->property("following").toBool());
        backend->hasNewer = false; emit backend->stateChanged();
        QTRY_VERIFY_WITH_TIMEOUT(window->rootObject()->property("following").toBool(), 500);
    }
    void upwardWheelLeavesFollowingBeforeTheFirstAnimationFrame() {
        prefs->setReducedMotion(false); load(); bottom();
        window->rootObject()->setProperty("following", true); QVERIFY(view->property("atYEnd").toBool());
        mouseWheel(120);
        QVERIFY(!window->rootObject()->property("following").toBool());
        QTest::qWait(10); QVERIFY(!window->rootObject()->property("following").toBool());
        QTest::qWait(150); QVERIFY(!window->rootObject()->property("following").toBool());
    }
    void prependDuringSmoothWheelGlideRebasesItsRemainingMotion() {
        prefs->setReducedMotion(false); backend->delay = 5; load(); go(3, 0);
        QVariantMap atMutation; qreal remaining = 0;
        connect(&backend->records, &QAbstractItemModel::rowsAboutToBeInserted, this, [&] {
            atMutation = snapshot(); remaining = history->property("remainingMotion").toReal();
        });
        mouseWheel(120); QTRY_COMPARE_WITH_TIMEOUT(backend->completed, 1, 1500); QTest::qWait(220);
        QVERIFY(!atMutation.value("id").toString().isEmpty()); QVERIFY(remaining > 0.1 || remaining < -0.1);
        QQmlExpression expression(qmlContext(window->rootObject()), window->rootObject(),
            QString("(function(){const index=fixtureRecords.indexOf('%1');const item=list.itemAtIndex(index);return item?item.y-list.contentY:NaN;})()").arg(atMutation.value("id").toString()));
        const auto finalOffset = expression.evaluate().toReal();
        QVERIFY2(qIsFinite(finalOffset) && qAbs(finalOffset - (atMutation.value("offset").toDouble() - remaining)) < 3.0,
                 qPrintable(QString("Remaining glide %1 moved anchor %2 -> %3").arg(remaining).arg(atMutation.value("offset").toDouble()).arg(finalOffset)));
        QVERIFY(!wheel->property("animating").toBool()); QCOMPARE(backend->olderCalls, 1);
    }
    void changingConversationDropsQueuedAnchorAndPagingIntent() {
        load(); top(); intent(-160); QTRY_COMPARE_WITH_TIMEOUT(backend->olderCalls, 1, 500);
        ++backend->generation; backend->loading = false; backend->channel = "201"; emit backend->stateChanged();
        backend->populate(9000, 50); top(); const auto nextChannel = snapshot(); QTest::qWait(250);
        sameAnchor(nextChannel); QCOMPARE(backend->olderCalls, 1); QVERIFY(!history->property("anchorPending").toBool());
    }
};
QTEST_MAIN(HistoryViewportTest)
#include "history_viewport_test.moc"

#include "RecordModel.h"
#include <QtTest>
#include <QElapsedTimer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <memory>

// Generated identities and an in-memory receiver only. This fixture never
// constructs DiscordClient, accesses an account, or sends a network ACK.
class ReadTrackerBackend final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString currentChannelId MEMBER channel NOTIFY stateChanged)
    Q_PROPERTY(QString currentUserId MEMBER account NOTIFY stateChanged)
    Q_PROPERTY(bool connected MEMBER connected NOTIFY stateChanged)
    Q_PROPERTY(bool demoMode MEMBER demo NOTIFY stateChanged)
    Q_PROPERTY(bool loadingMessages MEMBER loading NOTIFY stateChanged)
    Q_PROPERTY(bool hasNewerMessages MEMBER gap NOTIFY stateChanged)
public:
    struct Call { QString account, channel, message; qint64 at; };
    QString channel="200", account="100";
    bool connected=true, demo=false, loading=false, gap=false, accept=true;
    QList<Call> calls;
    QElapsedTimer elapsed;
    ReadTrackerBackend() { elapsed.start(); }
    Q_INVOKABLE bool acknowledgeVisibleMessages(const QString &channelId,const QString &messageId) {
        calls.append({account,channelId,messageId,elapsed.elapsed()});return accept;
    }
signals:
    void stateChanged();
};

class ReadReceiptTrackerTest final : public QObject {
    Q_OBJECT
    std::unique_ptr<ReadTrackerBackend> backend;
    std::unique_ptr<RecordModel> records;
    std::unique_ptr<QQuickView> window;
    QQuickItem *view=nullptr;
    QObject *tracker=nullptr;
    QStringList warnings;
    QVariantMap row(const QString &id,bool pending=false) const {
        return {{"recordId",id},{"channelId",backend->channel},{"pending",pending},{"content","Generated message"}};
    }
    void populate(int first=1000,int count=50) {
        QVector<QVariantMap> values;
        for(int i=0;i<count;++i)values.append(row(QString::number(first+i)));
        records->replace(values);
    }
    void rootProperty(const char *name,const QVariant &value) { QVERIFY(window->rootObject()->setProperty(name,value)); }
    void bottom() { QVERIFY(QMetaObject::invokeMethod(window->rootObject(),"goBottom"));QCoreApplication::processEvents();QVERIFY(view->property("atYEnd").toBool()); }
    void top() { QVERIFY(QMetaObject::invokeMethod(window->rootObject(),"goTop"));QCoreApplication::processEvents();QVERIFY(!view->property("atYEnd").toBool()); }
    void active(bool value=true) { rootProperty("trackingActive",value); }
    void changed() { emit backend->stateChanged(); }
private slots:
    void init() {
        warnings.clear();backend=std::make_unique<ReadTrackerBackend>();records=std::make_unique<RecordModel>();populate();
        window=std::make_unique<QQuickView>();window->setResizeMode(QQuickView::SizeRootObjectToView);
        connect(window->engine(),&QQmlEngine::warnings,this,[this](const QList<QQmlError> &errors){for(const auto &error:errors)warnings.append(error.toString());});
        window->rootContext()->setContextProperty("fixtureBackend",backend.get());
        window->rootContext()->setContextProperty("fixtureRecords",records.get());
        const auto source=QFINDTESTDATA("../qml/components/ReadReceiptTracker.qml");QVERIFY(!source.isEmpty());
        QQmlComponent component(window->engine());
        component.setData(R"(import QtQuick
Item {
    id: root; width:360; height:240
    property bool trackingActive:false
    property bool following:true
    property bool listVisible:true
    property real listWidth:360
    property real listHeight:240
    ListView {
        id:list; objectName:"messages"; visible:root.listVisible
        width:root.listWidth; height:root.listHeight; model:fixtureRecords
        boundsBehavior:Flickable.StopAtBounds; boundsMovement:Flickable.StopAtBounds
        delegate:Item { width:list.width; height:32 }
    }
    ReadReceiptTracker { objectName:"tracker"; view:list; records:fixtureRecords; backend:fixtureBackend; active:root.trackingActive; following:root.following }
    function goBottom(){list.forceLayout();list.positionViewAtEnd();list.forceLayout()}
    function goTop(){list.forceLayout();list.positionViewAtBeginning();list.forceLayout()}
})",QUrl::fromLocalFile(QFileInfo(source).absolutePath()+"/ReadTrackerHarness.qml"));
        auto *root=component.create();QVERIFY2(root,qPrintable(component.errorString()));
        window->setContent({},&component,root);window->resize(360,240);window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        view=root->findChild<QQuickItem*>("messages");tracker=root->findChild<QObject*>("tracker");QVERIFY(view);QVERIFY(tracker);
        bottom();QTest::qWait(30);QVERIFY(backend->calls.isEmpty());
    }
    void cleanup() {
        window.reset();records.reset();backend.reset();view=nullptr;tracker=nullptr;
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join('\n')));
    }
    void actualTailRequiresDwellAndIdleDoesNotPoll() {
        const auto started=backend->elapsed.elapsed();
        active();QTest::qWait(90);QVERIFY(backend->calls.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(backend->calls.size(),1,350);
        QCOMPARE(backend->calls.first().account,"100");QCOMPARE(backend->calls.first().channel,"200");QCOMPARE(backend->calls.first().message,"1049");
        QVERIFY(backend->calls.first().at-started>=120);QTest::qWait(450);QCOMPARE(backend->calls.size(),1);
    }
    void everyEligibilityGateSuppressesAcknowledgement_data() {
        QTest::addColumn<QString>("gate");
        for(const auto &gate:{"background","notFollowing","aboveTail","hidden","zeroWidth","zeroHeight","disconnected","demo","loading","historyGap","noChannel","noAccount","empty"})
            QTest::newRow(gate)<<QString::fromLatin1(gate);
    }
    void everyEligibilityGateSuppressesAcknowledgement() {
        QFETCH(QString,gate);
        if(gate=="notFollowing")rootProperty("following",false);
        else if(gate=="aboveTail")top();
        else if(gate=="hidden")rootProperty("listVisible",false);
        else if(gate=="zeroWidth")rootProperty("listWidth",0);
        else if(gate=="zeroHeight")rootProperty("listHeight",0);
        else if(gate=="disconnected")backend->connected=false;
        else if(gate=="demo")backend->demo=true;
        else if(gate=="loading")backend->loading=true;
        else if(gate=="historyGap")backend->gap=true;
        else if(gate=="noChannel")backend->channel.clear();
        else if(gate=="noAccount")backend->account.clear();
        else if(gate=="empty")records->clear();
        changed();if(gate!="background")active();
        QTest::qWait(230);QVERIFY(backend->calls.isEmpty());
    }
    void scrollingAwayOrCoveringChatCancelsPendingDwell_data() {
        QTest::addColumn<bool>("cover");QTest::newRow("scroll-away")<<false;QTest::newRow("background-or-overlay")<<true;
    }
    void scrollingAwayOrCoveringChatCancelsPendingDwell() {
        QFETCH(bool,cover);active();QTest::qWait(80);
        if(cover)active(false);else top();
        QTest::qWait(180);QVERIFY(backend->calls.isEmpty());
        if(cover)active();else bottom();
        QTest::qWait(80);QVERIFY(backend->calls.isEmpty());QTRY_COMPARE_WITH_TIMEOUT(backend->calls.size(),1,300);
    }
    void burstAtTailCoalescesLatestLoadedRecord() {
        active();QTest::qWait(55);records->upsert(row("1050"));bottom();
        QTest::qWait(35);records->upsert(row("1051"));bottom();
        QTest::qWait(20);QVERIFY(backend->calls.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(backend->calls.size(),1,350);QCOMPARE(backend->calls.first().message,"1051");
        QTest::qWait(230);QCOMPARE(backend->calls.size(),1);
    }
    void channelAndAccountReplacementGetANewDwell_data() {
        QTest::addColumn<bool>("accountChange");QTest::newRow("channel")<<false;QTest::newRow("same-channel-new-account")<<true;
    }
    void channelAndAccountReplacementGetANewDwell() {
        QFETCH(bool,accountChange);active();QTest::qWait(90);
        if(accountChange)backend->account="101";else backend->channel="201";changed();
        QTest::qWait(85);QVERIFY(backend->calls.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(backend->calls.size(),1,300);
        QCOMPARE(backend->calls.first().account,accountChange?"101":"100");QCOMPARE(backend->calls.first().channel,accountChange?"200":"201");
    }
    void replacingTheModelContentsRestartsTheDwell() {
        active();QTest::qWait(90);populate(2000);bottom();
        QTest::qWait(85);QVERIFY(backend->calls.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(backend->calls.size(),1,300);QCOMPARE(backend->calls.first().message,"2049");
    }
    void pendingOnlyAndNonNumericRowsNeverBecomeReadWatermarks() {
        top();QTest::qWait(30);
        records->replace({row("local-1",true),row("pending:2",true),row("0",true)});bottom();active();
        QTest::qWait(230);QVERIFY(backend->calls.isEmpty());
        records->replace({row("1007"),row("local-1",true),row("pending:2",true)});bottom();
        QTRY_COMPARE_WITH_TIMEOUT(backend->calls.size(),1,350);QCOMPARE(backend->calls.first().message,"1007");
    }
    void backendRejectionDoesNotStartAnEndlessRetryLoop() {
        backend->accept=false;active();QTRY_COMPARE_WITH_TIMEOUT(backend->calls.size(),1,350);
        QTest::qWait(450);QCOMPARE(backend->calls.size(),1);
        active(false);backend->accept=true;active();QTRY_COMPARE_WITH_TIMEOUT(backend->calls.size(),2,350);
    }
    void destructionCancelsTheOutstandingTimer() {
        active();QTest::qWait(80);window.reset();view=nullptr;tracker=nullptr;
        QTest::qWait(180);QVERIFY(backend->calls.isEmpty());
    }
};
QTEST_MAIN(ReadReceiptTrackerTest)
#include "read_receipt_tracker_test.moc"

#include "AppSettings.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickView>
#include <QQuickItem>
#include <QWheelEvent>
#include <QPointingDevice>
#include <memory>
#include <limits>

class ScrollTest final : public QObject {
    Q_OBJECT
    QTemporaryDir settings;
    std::unique_ptr<AppSettings> prefs;
    std::unique_ptr<QQuickView> window;
    QPointingDevice touchpad{"Synthetic touchpad",2,QInputDevice::DeviceType::TouchPad,QPointingDevice::PointerType::Finger,QInputDevice::Capability::Position|QInputDevice::Capability::Scroll|QInputDevice::Capability::PixelScroll,10,0};
    QObject *viewport=nullptr;
    void load(const QByteArray &body) {
        window=std::make_unique<QQuickView>();
        window->rootContext()->setContextProperty("preferences",prefs.get());
        const QString file=QFINDTESTDATA("../qml/components/ScrollBoost.qml");
        QVERIFY(!file.isEmpty());
        QQmlComponent component(window->engine());
        component.setData("import QtQuick\nimport QtQuick.Controls\nItem { width: 400; height: 300\n"+body+"\n}",QUrl::fromLocalFile(QFileInfo(file).absolutePath()+"/ScrollHarness.qml"));
        auto *root=component.create();
        QVERIFY2(root,qPrintable(component.errorString()));
        window->setContent({},&component,root);window->resize(400,300);window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        QTest::qWait(20);
        viewport=root->findChild<QObject*>("viewport");QVERIFY(viewport);
    }
    void wheel(QPoint angle,QPoint pixels={},const QPointingDevice *device=QPointingDevice::primaryPointingDevice(),QPointF position={150,150},Qt::ScrollPhase phase=Qt::NoScrollPhase) {
        QWheelEvent event(position,window->mapToGlobal(position.toPoint()),pixels,angle,Qt::NoButton,Qt::NoModifier,phase,false,Qt::MouseEventNotSynthesized,device);
        QCoreApplication::sendEvent(window.get(),&event);QCoreApplication::processEvents();
    }
    qreal position() {return viewport->property("contentY").toReal();}
    static QQuickItem *visualChild(QQuickItem *parent,const QString &name) {
        if(parent->objectName()==name)return parent;
        for(auto *child:parent->childItems())if(auto *result=visualChild(child,name))return result;
        return nullptr;
    }
    static QByteArray listBody() {
        return R"(ListView { objectName: "viewport"; id: list; anchors.fill: parent; model: 100; delegate: Rectangle { width: list.width; height: 40; color: index % 2 ? "black" : "gray"; Button { anchors.centerIn: parent; text: "Click" } } boundsBehavior: Flickable.StopAtBounds; boundsMovement: Flickable.StopAtBounds; ScrollBoost { objectName: "wheel"; flickable: list } })";
    }
    qreal lowerBound() {return viewport->property("originY").toReal()-viewport->property("topMargin").toReal();}
    qreal upperBound() {return qMax(lowerBound(),viewport->property("originY").toReal()+viewport->property("contentHeight").toReal()+viewport->property("bottomMargin").toReal()-viewport->property("height").toReal());}
private slots:
    void init() { prefs=std::make_unique<AppSettings>(settings.path()+"/prefs.ini");prefs->setScrollMultiplier(3);prefs->setReducedMotion(true); }
    void cleanup() {window.reset();prefs.reset();viewport=nullptr;}
    void wheelGlidesBrieflyAndAccumulatesItsFullDistance(){
        prefs->setReducedMotion(false);prefs->setScrollMultiplier(1);load(listBody());
        wheel({0,-120},{0,-15},&touchpad);QVERIFY(position()<160.);
        QTest::qWait(35);QVERIFY(position()>0.);QVERIFY(position()<160.);
        wheel({0,-120},{0,-15},&touchpad);
        qreal previous=position();for(int i=0;i<8;++i){QTest::qWait(20);QVERIFY(position()>=previous);previous=position();}
        QTRY_COMPARE_WITH_TIMEOUT(position(),320.,400);
    }
    void smoothWheelReachesBothBoundsWithoutAnImmediateJump() {
        prefs->setReducedMotion(false);prefs->setScrollMultiplier(1);load(listBody());
        viewport->setProperty("contentY",lowerBound()+100.);wheel({0,120});
        QVERIFY(position()>lowerBound());
        QTRY_COMPARE_WITH_TIMEOUT(position(),lowerBound(),300);
        viewport->setProperty("contentY",upperBound()-100.);wheel({0,-120});
        QVERIFY(position()<upperBound());
        QTRY_COMPARE_WITH_TIMEOUT(position(),upperBound(),300);
        // Accumulating the next detent at an edge must keep the visible glide.
        viewport->setProperty("contentY",600.);
        for(int i=0;i<4;++i){
            const auto before=position();
            QWheelEvent event(QPointF(150,150),window->mapToGlobal(QPoint(150,150)),{},QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
            QCoreApplication::sendEvent(window.get(),&event);
            QVERIFY(qAbs(position()-before)<.5);QTest::qWait(20);
        }
        QTRY_COMPARE_WITH_TIMEOUT(position(),lowerBound(),300);
    }
    void repeatedAnchorRebasesKeepTheOriginalGlideCompletionTime() {
        prefs->setReducedMotion(false);prefs->setScrollMultiplier(1);load(listBody());
        auto *boost=window->rootObject()->findChild<QObject*>("wheel");QVERIFY(boost);
        viewport->setProperty("contentY",1000.);wheel({0,-120});
        for(int i=0;i<3;++i){
            QTest::qWait(25);QVariant remaining;
            QVERIFY(QMetaObject::invokeMethod(boost,"pauseForAnchor",Q_RETURN_ARG(QVariant,remaining)));
            viewport->setProperty("contentY",position()+500.);
            QVERIFY(QMetaObject::invokeMethod(boost,"resumeAfterAnchor",Q_ARG(QVariant,remaining)));
        }
        // Three layout corrections must not each add another full 110 ms tail.
        QTest::qWait(70);QVERIFY(!boost->property("animating").toBool());
        QVERIFY(qAbs(position()-2660.)<.01);
    }
    void reversingCancelsOldMomentumAndExternalAnchorsWin(){
        prefs->setReducedMotion(false);prefs->setScrollMultiplier(1);load(listBody());
        viewport->setProperty("contentY",1000.);wheel({0,-120});QTest::qWait(35);
        const auto before=position();wheel({0,120});qreal previous=position();
        for(int i=0;i<8;++i){QTest::qWait(20);QVERIFY(position()<=previous);previous=position();}
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(position()-(before-160.))<.01,400);
        wheel({0,-120});QTest::qWait(25);viewport->setProperty("contentY",600.);QTest::qWait(180);QCOMPARE(position(),600.);
        wheel({0,-120});QTest::qWait(25);const auto touchStart=position();wheel({0,-120},{0,-15},&touchpad,{150,150},Qt::ScrollUpdate);
        const auto precise=position();QCOMPARE(precise,touchStart+15.);QTest::qWait(160);QCOMPARE(position(),precise);
    }
    void mouseDriverPixelDeltaCannotBypassSpeed() {
        load(listBody());QVERIFY(viewport);
        wheel({0,-120});QCOMPARE(position(),480.);
        viewport->setProperty("contentY",0);wheel({0,-120},{0,-15});QCOMPARE(position(),480.);
        viewport->setProperty("contentY",0);wheel({},{0,-15});QCOMPARE(position(),480.);
        viewport->setProperty("contentY",2000);wheel({0,-120},{0,-15});QCOMPARE(position(),2480.);
        wheel({0,120},{0,15});QCOMPARE(position(),2000.);
    }
    void preferenceAndHighResolutionMouseStepsApplyImmediately() {
        load(listBody());QVERIFY(viewport);
        wheel({0,-30});QCOMPARE(position(),120.);
        prefs->setScrollMultiplier(8);wheel({0,-120});QCOMPARE(position(),1400.);
        prefs->setScrollMultiplier(.5);wheel({0,-120});QCOMPARE(position(),1480.);
        wheel({},{0,-20});QCOMPARE(position(),1586.6666666666667);
    }
    void touchpadPixelsAndMomentumStayNatural() {
        load(listBody());QVERIFY(viewport);prefs->setScrollMultiplier(8);
        wheel({0,-120},{0,-15},&touchpad,{150,150},Qt::ScrollUpdate);QCOMPARE(position(),15.);
        wheel({0,-60},{0,-7},&touchpad,{150,150},Qt::ScrollMomentum);QCOMPARE(position(),22.);
        wheel({0,120},{0,12},&touchpad,{150,150},Qt::ScrollUpdate);QCOMPARE(position(),10.);
        wheel({},{},&touchpad,{150,150},Qt::ScrollEnd);QCOMPARE(position(),10.);
        wheel({-120,0},{-20,0},&touchpad);QCOMPARE(position(),10.);
    }
    void waylandMouseReportedAsTouchpadUsesWheelSpeed() {
        // Captured from the user's physical mouse on Wayland: Qt labels it
        // TouchPad, with no gesture phase and 15 synthesized pixels per detent.
        load(listBody());QVERIFY(viewport);
        wheel({0,-120},{0,-15},&touchpad);QCOMPARE(position(),480.);
        QTest::qWait(120);QCOMPARE(position(),480.);
        wheel({0,-120},{0,-15},&touchpad);QCOMPARE(position(),960.);
        wheel({0,120},{0,15},&touchpad);QCOMPARE(position(),480.);
        prefs->setScrollMultiplier(4);
        wheel({0,-240},{0,-30},&touchpad);QCOMPARE(position(),1760.);
        wheel({0,120},{},&touchpad);QCOMPARE(position(),1120.);
        // Unphased, genuinely precise touchpad movement keeps its pixel delta.
        wheel({0,-60},{0,-7},&touchpad);QCOMPARE(position(),1127.);
        wheel({0,-120},{0,-40},&touchpad);QCOMPARE(position(),1167.);
    }
    void defaultSpeedUsesAModerateStepWithThePhysicalMouseSignature() {
        AppSettings fresh(settings.path()+"/default-speed.ini");
        QCOMPARE(fresh.scrollMultiplier(),1.);
        prefs->setScrollMultiplier(fresh.scrollMultiplier());
        load(listBody());QVERIFY(viewport);
        wheel({0,-120},{0,-15},&touchpad);QCOMPARE(position(),160.);
        wheel({0,120},{0,15},&touchpad);QCOMPARE(position(),0.);
    }
    void selectableMessageTextDoesNotFallBackToSlowNativeScrolling() {
        load(R"(ListView {objectName:"viewport";id:list;anchors.fill:parent;model:100;delegate:TextEdit {width:list.width;height:60;textFormat:TextEdit.RichText;text:"<a href='https://example.invalid'>Selectable chat link</a>";readOnly:true;selectByMouse:true} boundsBehavior:Flickable.StopAtBounds;boundsMovement:Flickable.StopAtBounds;ScrollBoost {flickable:list} })");
        QVERIFY(viewport);
        wheel({0,-120},{0,-15},QPointingDevice::primaryPointingDevice(),{100,20});QCOMPARE(position(),480.);
        wheel({},{0,-15},QPointingDevice::primaryPointingDevice(),{100,20});QCOMPARE(position(),960.);
    }
    void bothBoundsClampWithoutDelayedMovement() {
        load(listBody());QVERIFY(viewport);
        viewport->setProperty("contentY",upperBound()-20);wheel({0,-120});QCOMPARE(position(),upperBound());
        wheel({0,-1200});QCOMPARE(position(),upperBound());QTest::qWait(120);QCOMPARE(position(),upperBound());
        viewport->setProperty("contentY",lowerBound()+20);wheel({0,120});QCOMPARE(position(),lowerBound());
        wheel({0,1200});QCOMPARE(position(),lowerBound());QTest::qWait(120);QCOMPARE(position(),lowerBound());
    }
    void scrollViewControlsUseSameStepAndRemainClickable() {
        load(R"(HardScrollView { anchors.fill: parent; id: view; Component.onCompleted: contentItem.objectName="viewport"; Column { width: parent.width; Repeater {model:100; Button {objectName:index===0?"firstButton":"";width:300;height:40;text:"Fixture"}}} })");
        QVERIFY(viewport);
        auto *button=visualChild(window->contentItem(),"firstButton");QVERIFY(button);QSignalSpy clicked(button,SIGNAL(clicked()));
        QTest::mouseClick(window.get(),Qt::LeftButton,Qt::NoModifier,{150,20});QCOMPARE(clicked.count(),1);
        wheel({0,-120},{0,-15},QPointingDevice::primaryPointingDevice(),{150,20});QCOMPARE(position(),480.);
        QCOMPARE(clicked.count(),1);
    }
    void mediaGridUsesSameWheelSpeed() {
        load(R"(GridView { id:grid;objectName:"viewport";anchors.fill:parent;model:400;cellWidth:80;cellHeight:80;delegate:Rectangle {width:78;height:78;color:"gray"} boundsBehavior:Flickable.StopAtBounds;boundsMovement:Flickable.StopAtBounds;ScrollBoost {flickable:grid} })");
        QVERIFY(viewport);wheel({0,-120},{0,-15});QCOMPARE(position(),480.);
        viewport->setProperty("contentY",upperBound()-10);wheel({0,-120});QCOMPARE(position(),upperBound());
    }
    void contextMenuUsesSameWheelSpeed() {
        load(R"(BoundedMenu {id:menu;x:0;y:0;height:280;Component.onCompleted:{contentItem.objectName="viewport";open()} Repeater {model:50;MenuItem {text:"Item "+index}} })");
        QVERIFY(viewport);wheel({0,-120},{0,-15},QPointingDevice::primaryPointingDevice(),{100,150});QCOMPARE(position(),480.);
        viewport->setProperty("contentY",upperBound()-10);wheel({0,-120},{},QPointingDevice::primaryPointingDevice(),{100,150});QCOMPARE(position(),upperBound());
    }
    void speedPreferencePersistsAndStaysBounded() {
        prefs->setScrollMultiplier(4.25);AppSettings restored(settings.path()+"/prefs.ini");QCOMPARE(restored.scrollMultiplier(),4.25);
        prefs->setScrollMultiplier(99);QCOMPARE(prefs->scrollMultiplier(),16.);prefs->setScrollMultiplier(.01);QCOMPARE(prefs->scrollMultiplier(),.5);
        prefs->setScrollMultiplier(std::numeric_limits<double>::quiet_NaN());QCOMPARE(prefs->scrollMultiplier(),.5);
    }
};
QTEST_MAIN(ScrollTest)
#include "scroll_test.moc"

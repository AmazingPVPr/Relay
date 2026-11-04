#include "MessageReferences.h"
#include "DiscordClient.h"
#include "ChatText.h"
#include "AppSettings.h"
#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTextDocument>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QFileInfo>

class ReferenceHttp final:public QObject {
public:
    struct Response { int status=200;QJsonValue value=QJsonObject{};int delay=0; };
    QTcpServer server;QList<QByteArray> requests;QQueue<Response> responses;
    ReferenceHttp(){
        connect(&server,&QTcpServer::newConnection,this,[this]{
            while(auto *socket=server.nextPendingConnection()){
                connect(socket,&QTcpSocket::readyRead,socket,[this,socket]{
                    auto bytes=socket->property("bytes").toByteArray()+socket->readAll();socket->setProperty("bytes",bytes);
                    const int end=bytes.indexOf("\r\n\r\n");if(end<0 || socket->property("handled").toBool())return;
                    int length=0;for(const auto &line:bytes.left(end).split('\n'))if(line.toLower().startsWith("content-length:"))length=line.mid(15).trimmed().toInt();
                    if(bytes.size()<end+4+length)return;socket->setProperty("handled",true);requests.append(bytes);
                    const auto response=responses.isEmpty()?Response{}:responses.dequeue();
                    const auto body=response.value.isArray()?QJsonDocument(response.value.toArray()).toJson(QJsonDocument::Compact):QJsonDocument(response.value.toObject()).toJson(QJsonDocument::Compact);
                    QTimer::singleShot(response.delay,socket,[socket,response,body]{
                        socket->write("HTTP/1.1 "+QByteArray::number(response.status)+" Result\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);socket->disconnectFromHost();
                    });
                });
                connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            }
        });
    }
    QString base()const{return "http://127.0.0.1:"+QString::number(server.serverPort());}
    QJsonObject body(int index)const{const auto bytes=requests.at(index);return QJsonDocument::fromJson(bytes.mid(bytes.indexOf("\r\n\r\n")+4)).object();}
};
class MessageReferencesTest:public QObject {
    Q_OBJECT
    static QJsonObject channel(QString id,QString name,int type=0){
        return {{"id",id},{"guild_id","900"},{"type",type},{"name",name},{"position",id.toInt()}};
    }
    static void setup(DiscordClient &client,ReferenceHttp &http){
        client.m_apiBase=http.base();client.m_token="generated-reference-fixture-token";client.m_currentUserId="42";client.m_connected=true;
        client.m_currentGuildId="900";client.m_currentGuildName="Generated community";
        client.m_guildObjects.insert("900",{{"id","900"},{"name","Generated community"},{"owner_id","7"},
            {"roles",QJsonArray{QJsonObject{{"id","900"},{"permissions","1024"}}}},{"member",QJsonObject{{"roles",QJsonArray{}}}}});
        auto hidden=channel("199","Private name");
        hidden.insert("permission_overwrites",QJsonArray{QJsonObject{{"id","900"},{"type",0},{"allow","0"},{"deny","1024"}}});
        client.cacheChannels("900",{channel("101","addon-announcements",5),channel("102","mod-suggestions",15),channel("103","Voice",2),hidden});
        client.rebuildChannels(false);
    }
    static QJsonObject onboarding(bool responses=true){
        QJsonObject object{{"guild_id","900"},{"enabled",true},{"prompts",QJsonArray{
            QJsonObject{{"id","500"},{"title","Are you interested in contributing to the pack?"},{"single_select",true},{"required",true},
                {"options",QJsonArray{QJsonObject{{"id","501"},{"title","Yes"},{"description","See the contributor channels"}},QJsonObject{{"id","502"},{"title","No"}}}}},
            QJsonObject{{"id","600"},{"title","What do you play?"},{"single_select",false},{"required",false},
                {"options",QJsonArray{QJsonObject{{"id","601"},{"title","GregTech"}},QJsonObject{{"id","602"},{"title","Other packs"}}}}}
        }}};
        if(responses)object.insert("responses",QJsonArray{"502","601","999"});
        return object;
    }
    static QQuickItem *visual(QQuickItem *parent,const QString &name){
        if(parent->objectName()==name)return parent;
        for(auto *child:parent->childItems())if(auto *found=visual(child,name))return found;
        return nullptr;
    }
    static QPoint linkPoint(QQuickItem *item,const QString &expected){
        for(qreal y=4;y<item->height();y+=5)for(qreal x=3;x<item->width();x+=5){
            QString link;QMetaObject::invokeMethod(item,"linkAt",Q_RETURN_ARG(QString,link),Q_ARG(qreal,x),Q_ARG(qreal,y));
            if(link==expected)return item->mapToScene(QPointF(x,y)).toPoint();
        }
        return {-1,-1};
    }
private slots:
    void formatsChannelAndNavigationMentionsWithSafeContextAndCache() {
        ChatText text;QVariantMap metadata{{"guildId","900"},{"mentionChannels",QVariantList{
            QVariantMap{{"id","101"},{"name","announcements & <news>"},{"known",true},{"accessible",true}},
            QVariantMap{{"id","199"},{"name","Do not reveal"},{"known",true},{"accessible",false}}}}};
        const auto html=text.render("See <#101> and <#199>, then <id:customize> or <id:browse>.",metadata);
        QTextDocument doc;doc.setHtml(html);
        QCOMPARE(doc.toPlainText(),QString("See #announcements & <news> and #Unknown channel, then Channels & Roles or Browse Channels."));
        QVERIFY(html.contains("relay-channel:101"));QVERIFY(!html.contains("relay-channel:199"));QVERIFY(!html.contains("Do not reveal"));
        QVERIFY(html.contains("relay-guild:900:customize"));QVERIFY(!html.contains("<news>"));
        metadata.insert("guildId","901");QVERIFY(text.render("<id:customize>",metadata).contains("relay-guild:901:customize"));
        metadata.insert("mentionChannels",QVariantList{QVariantMap{{"id","101"},{"name","new-name"},{"accessible",true}}});
        QVERIFY(text.render("<#101>",metadata).contains("#new-name"));
        QVERIFY(!text.render("<id:customize>",{{"guildId","900\"><img"}}).contains("relay-guild:"));
        const auto tick=QString(QChar(96));
        const auto literal=text.render(tick+"<#101> <id:customize>"+tick+"\n"+tick.repeated(3)+"\n<#101> <id:customize>\n"+tick.repeated(3)+"\n\\<#101> \\<id:customize>",metadata);
        QVERIFY(!literal.contains("<a "));QTextDocument code;code.setHtml(literal);QVERIFY(code.toPlainText().contains("<#101> <id:customize>"));
        QVERIFY(!text.render("[<#101>](https://example.com)",metadata).contains("relay-channel:"));
    }
    void cachedReferencesRespectPermissionsAndUpdateWithoutRequests() {
        ReferenceHttp http;QVERIFY(http.server.listen(QHostAddress::LocalHost,0));DiscordClient client;setup(client,http);MessageReferences refs(&client);
        auto info=refs.resolve("<#101> <#199> <#777>","900").value("mentionChannels").toList();
        QCOMPARE(info.size(),3);QCOMPARE(info[0].toMap().value("name").toString(),QString("addon-announcements"));
        QVERIFY(!info[1].toMap().contains("name"));QVERIFY(!info[1].toMap().value("accessible").toBool());
        QSignalSpy revisions(&refs,&MessageReferences::referencesChanged),opened(&refs,&MessageReferences::channelRequested);
        client.dispatch("CHANNEL_UPDATE",channel("101","renamed"));
        QVERIFY(revisions.size()>0);QCOMPARE(refs.resolve("<#101>","900").value("mentionChannels").toList()[0].toMap().value("name").toString(),QString("renamed"));
        QVERIFY(refs.activate("relay-channel:102"));QCOMPARE(opened.size(),1);QCOMPARE(opened.first().first().toString(),QString("102"));
        QVERIFY(!refs.activate("relay-channel:199"));QCOMPARE(opened.size(),1);
        QVERIFY(!refs.activate("relay-channel:1/../../token"));QVERIFY(!refs.activate("relay-guild:900:unknown"));
        QCOMPARE(http.requests.size(),0);
    }
    void unknownChannelResolvesOnClickAndStaleNavigationCannotJump() {
        ReferenceHttp http;QVERIFY(http.server.listen(QHostAddress::LocalHost,0));DiscordClient client;setup(client,http);MessageReferences refs(&client);QSignalSpy opened(&refs,&MessageReferences::channelRequested);
        http.responses.enqueue({200,channel("777","uncached-channel"),0});
        QVERIFY(refs.activate("relay-channel:777"));QTRY_COMPARE(opened.size(),1);
        QVERIFY(http.requests.first().startsWith("GET /channels/777 "));QCOMPARE(opened.first().first().toString(),QString("777"));
        QCOMPARE(refs.resolve("<#777>","900").value("mentionChannels").toList()[0].toMap().value("name").toString(),QString("uncached-channel"));
        http.responses.enqueue({200,channel("778","late-channel"),100});QVERIFY(refs.activate("relay-channel:778"));
        QTRY_COMPARE(http.requests.size(),2);++client.m_selectionGeneration;QTest::qWait(140);QCOMPARE(opened.size(),1);
        http.responses.enqueue({403,QJsonObject{{"message","Missing Access"}},0});QVERIFY(refs.activate("relay-channel:779"));QTRY_VERIFY(!refs.error().isEmpty());QCOMPARE(opened.size(),1);
    }
    void nativeQuestionsSaveOnlyAfterExplicitActionAndPreserveOtherAnswers() {
        ReferenceHttp http;QVERIFY(http.server.listen(QHostAddress::LocalHost,0));DiscordClient client;setup(client,http);MessageReferences refs(&client);
        http.responses.enqueue({200,onboarding(),0});QVERIFY(refs.activate("relay-guild:900:customize"));QTRY_VERIFY(!refs.loading());
        QCOMPARE(refs.prompts().size(),2);QVERIFY(!refs.dirty());QCOMPARE(http.requests.size(),1);
        QVERIFY(refs.choose("500","501",true));QVERIFY(refs.dirty());QVERIFY(!refs.choose("500","601",true));
        QVERIFY(refs.choose("600","602",true));QCOMPARE(http.requests.size(),1);
        http.responses.enqueue({403,QJsonObject{{"message","Generated save rejection"}},0});QVERIFY(refs.save());QTRY_VERIFY(!refs.saving());
        QVERIFY(refs.dirty());QVERIFY(!refs.error().isEmpty());QCOMPARE(http.requests.size(),2);
        QVERIFY(http.requests[1].startsWith("PUT /guilds/900/onboarding-responses "));
        const auto submitted=http.body(1).value("onboarding_responses").toArray();
        QVERIFY(submitted.contains("501"));QVERIFY(!submitted.contains("502"));QVERIFY(submitted.contains("601"));QVERIFY(submitted.contains("602"));QVERIFY(submitted.contains("999"));
        QCOMPARE(http.body(1).value("onboarding_prompts_seen").toObject().keys(),QStringList({"500","600"}));
        http.responses.enqueue({200,QJsonObject{},0});http.responses.enqueue({200,QJsonObject{{"user",QJsonObject{{"id","42"}}},{"roles",QJsonArray{}}},0});
        http.responses.enqueue({200,QJsonArray{channel("101","addon-announcements",5),channel("102","mod-suggestions",15)},0});
        QVERIFY(refs.save());QTRY_VERIFY(!refs.saving());QVERIFY(!refs.dirty());QVERIFY(!refs.status().isEmpty());QTRY_COMPARE(http.requests.size(),5);
        QVERIFY(refs.choose("500","501",false));QVERIFY(!refs.save());QVERIFY(refs.error().contains("Choose an option"));QCOMPARE(http.requests.size(),5);
        const auto draft=refs.prompts();client.m_connected=false;
        QVERIFY(!refs.choose("600","602",false));QCOMPARE(refs.prompts(),draft);QVERIFY(!refs.save());QVERIFY(refs.error().contains("Reconnect"));QCOMPARE(http.requests.size(),5);
    }
    void incompleteChoicesAndAccountSwitchCannotOverwriteExistingAnswers() {
        ReferenceHttp http;QVERIFY(http.server.listen(QHostAddress::LocalHost,0));DiscordClient client;setup(client,http);MessageReferences refs(&client);
        http.responses.enqueue({200,onboarding(false),0});QVERIFY(refs.openGuild("900"));QTRY_VERIFY(!refs.loading());QVERIFY(!refs.error().isEmpty());
        QVERIFY(!refs.choose("500","501",true));QVERIFY(!refs.save());QCOMPARE(http.requests.size(),1);
        http.responses.enqueue({200,onboarding(),100});refs.refresh();QTRY_COMPARE(http.requests.size(),2);
        ++client.m_generation;client.m_currentUserId="84";emit client.stateChanged();QTest::qWait(140);
        QVERIFY(refs.guildId().isEmpty());QVERIFY(refs.prompts().isEmpty());QVERIFY(!refs.save());
    }
    void queuedSaveCannotRunAfterAccountChange() {
        ReferenceHttp http;QVERIFY(http.server.listen(QHostAddress::LocalHost,0));DiscordClient client;setup(client,http);MessageReferences refs(&client);
        http.responses.enqueue({200,onboarding(),0});QVERIFY(refs.openGuild("900"));QTRY_VERIFY(!refs.loading());
        QVERIFY(refs.choose("500","501",true));client.m_globalReset=QDateTime::currentMSecsSinceEpoch()+250;
        QVERIFY(refs.save());++client.m_generation;client.m_currentUserId="84";emit client.stateChanged();
        QTest::qWait(320);QCOMPARE(http.requests.size(),1);QVERIFY(refs.guildId().isEmpty());QVERIFY(!refs.saving());
    }
    void nativePanelAndRealPointerLinksUseTheMessageGuild() {
        ReferenceHttp http;QVERIFY(http.server.listen(QHostAddress::LocalHost,0));DiscordClient client;setup(client,http);MessageReferences refs(&client);
        auto other=client.m_guildObjects.value("900");other.insert("id","901");other.insert("name","Another server");client.m_guildObjects.insert("901",other);client.m_currentGuildId="901";
        QTemporaryDir dir;AppSettings preferences(dir.path()+"/preferences.ini");ChatText text;QQmlEngine engine;QStringList warnings;
        connect(&engine,&QQmlEngine::warnings,&engine,[&](const QList<QQmlError> &errors){for(const auto &error:errors)warnings.append(error.toString());});
        engine.rootContext()->setContextProperty("preferences",&preferences);engine.rootContext()->setContextProperty("chatText",&text);
        engine.rootContext()->setContextProperty("discord",&client);engine.rootContext()->setContextProperty("messageReferences",&refs);
        const QString code=QStringLiteral(R"qml(import QtQuick
import QtQuick.Controls
import "%1/qml/components" as Components
ApplicationWindow {
 visible:true; width:760; height:760
 Components.LinkText { objectName:"referenceBody"; width:700; x:20; y:20; font.pixelSize:16; rawText:"See <#101> and <#102>. Go to <id:customize> to choose your interests."; metadata:({guildId:"900"}) }
 Components.ServerChoicesDialog { id:panel; objectName:"serverChoices"; service:messageReferences }
 Connections { target:messageReferences; function onPanelRequested(){panel.open()} }
})qml").arg(QUrl::fromLocalFile(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()+"/..").toString());
        QQmlComponent component(&engine);component.setData(code.toUtf8(),QUrl::fromLocalFile(dir.path()+"/fixture.qml"));
        QVERIFY2(component.isReady(),qPrintable(component.errorString()));std::unique_ptr<QObject> object(component.create());QVERIFY2(object,qPrintable(component.errorString()));
        auto *window=qobject_cast<QQuickWindow*>(object.get());QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *body=object->findChild<QQuickItem*>("referenceBody");QVERIFY(body);
        QSignalSpy channels(&refs,&MessageReferences::channelRequested);
        const auto point=linkPoint(body,"relay-channel:102");QVERIFY(point.x()>=0);QTest::mouseClick(window,Qt::LeftButton,{},point);QCOMPARE(channels.size(),1);
        http.responses.enqueue({200,onboarding(),0});
        const auto nav=linkPoint(body,"relay-guild:900:customize");QVERIFY(nav.x()>=0);QTest::mouseClick(window,Qt::LeftButton,{},nav);QTRY_VERIFY(!refs.loading());QCOMPARE(refs.guildId(),QString("900"));
        auto *panel=object->findChild<QObject*>("serverChoices");QVERIFY(panel);QTRY_VERIFY(panel->property("visible").toBool());
        QDir().mkpath("/workspace/work/message-references");QTest::qWait(100);window->grabWindow().save("/workspace/work/message-references/server-choices-generated.png");
        QQuickItem *option=nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((option=visual(window->contentItem(),"serverChoiceOption_501"))!=nullptr,1500);
        QVERIFY2(option->isVisible(),qPrintable(warnings.join('\n')));
        QTest::mouseClick(window,Qt::LeftButton,{},option->mapToScene(QPointF(10,option->height()/2)).toPoint());QVERIFY(refs.dirty());
        auto *save=visual(window->contentItem(),"serverChoicesSave");QVERIFY(save);QVERIFY(save->isEnabled());
        http.responses.enqueue({403,QJsonObject{{"message","Generated save failure"}},0});
        QTest::mouseClick(window,Qt::LeftButton,{},save->mapToScene(QPointF(save->width()/2,save->height()/2)).toPoint());
        QTRY_COMPARE(http.requests.size(),2);QTRY_VERIFY(!refs.saving());QVERIFY(refs.dirty());QVERIFY(!refs.error().isEmpty());
        QVERIFY(http.requests[1].startsWith("PUT /guilds/900/onboarding-responses "));
        QVERIFY(http.body(1).value("onboarding_responses").toArray().contains("501"));
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join('\n')));
        QDir().mkpath("/workspace/work/message-references");QVERIFY(window->grabWindow().save("/workspace/work/message-references/server-choices-generated.png"));
        QVERIFY(refs.openGuild("900","browse"));
        QQuickItem *destination=nullptr;QTRY_VERIFY((destination=visual(window->contentItem(),"serverChoiceChannel_102"))!=nullptr);
        QTest::mouseClick(window,Qt::LeftButton,{},destination->mapToScene(QPointF(destination->width()/2,destination->height()/2)).toPoint());
        QTRY_COMPARE(channels.size(),2);QCOMPARE(channels.last().first().toString(),QString("102"));
        QTRY_VERIFY(!panel->property("visible").toBool());QTRY_VERIFY(refs.guildId().isEmpty());
        QVERIFY(!refs.activate("relay-channel:199"));QTRY_VERIFY(panel->property("visible").toBool());
        auto *questions=visual(window->contentItem(),"serverChoicesQuestions");QVERIFY(questions);QVERIFY(!questions->isVisible());
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join('\n')));
    }
};
QTEST_MAIN(MessageReferencesTest)
#include "message_references_test.moc"

#include "VoiceRoster.h"
#include <QtTest>
class VoiceRosterTest final:public QObject{
 Q_OBJECT
 static QVariantMap member(const QString &id,const QString &channel,bool speaking=false){return {{"recordId",id},{"channelId",channel},{"name","Generated "+id},{"speaking",speaking}};}
private slots:
 void speakingUpdatesKeepTheSameChannelModelAndOnlyUpdateTheChangedMember(){
  RecordModel source;source.replace({member("1","100"),member("2","100"),member("3","200")});VoiceRoster roster(&source);auto *first=roster.forChannel("100"),*other=roster.forChannel("200");QCOMPARE(first->count(),2);
  QSignalSpy reset(first,&QAbstractItemModel::modelReset),insert(first,&QAbstractItemModel::rowsInserted),remove(first,&QAbstractItemModel::rowsRemoved),updated(first,&QAbstractItemModel::dataChanged),otherChanged(other,&QAbstractItemModel::dataChanged);const auto revision=roster.revision();
  source.patch("2",{{"speaking",true}});QCOMPARE(roster.forChannel("100"),first);QCOMPARE(updated.count(),1);QCOMPARE(updated.first().first().value<QModelIndex>().row(),1);QVERIFY(first->byId("2").value("speaking").toBool());QCOMPARE(reset.count(),0);QCOMPARE(insert.count(),0);QCOMPARE(remove.count(),0);QCOMPARE(otherChanged.count(),0);QCOMPARE(roster.revision(),revision);
 }
 void viewerChangesExposeANotifyRoleAndReachExistingDelegates(){
  RecordModel source;source.replace({member("1","100")});VoiceRoster roster(&source);auto *channel=roster.forChannel("100");
  const auto role=source.roleNames().key("watchingYourStream",-1);QVERIFY(role>Qt::UserRole);
  QCOMPARE(source.data(source.index(0),role).toBool(),false);
  QSignalSpy changed(channel,&QAbstractItemModel::dataChanged);
  source.patch("1",{{"watchingYourStream",true}});
  QCOMPARE(changed.count(),1);QVERIFY(changed.first().at(2).value<QList<int>>().contains(role));
  QVERIFY(channel->data(channel->index(0),role).toBool());
  source.patch("1",{{"watchingYourStream",false}});QVERIFY(!channel->data(channel->index(0),role).toBool());
 }
 void structuralBatchesKeepRetainedMembersAndCoalesceGrouping(){
  RecordModel source;source.replace({member("1","100")});VoiceRoster roster(&source);auto *channel=roster.forChannel("100");const auto revision=roster.revision();QSignalSpy changed(&roster,&VoiceRoster::changed),reset(channel,&QAbstractItemModel::modelReset);
  for(int i=2;i<=100;++i)source.upsert(member(QString::number(i),"100"));QTRY_COMPARE(channel->count(),100);QCOMPARE(changed.count(),1);QCOMPARE(roster.revision(),revision+1);QCOMPARE(roster.forChannel("100"),channel);QCOMPARE(reset.count(),0);
 }
 void movingAndLeavingOnlyChangesTheActualRosters(){
  RecordModel source;source.replace({member("1","100"),member("2","100"),member("3","200")});VoiceRoster roster(&source);auto *a=roster.forChannel("100"),*b=roster.forChannel("200");
  source.patch("2",{{"channelId","200"}});QTRY_COMPARE(a->count(),1);QCOMPARE(b->count(),2);QCOMPARE(roster.forChannel("100"),a);QCOMPARE(roster.forChannel("200"),b);
  source.remove("3");QTRY_COMPARE(b->count(),1);source.clear();QTRY_COMPARE(roster.members("100").size(),0);QCOMPARE(roster.members("200").size(),0);QCOMPARE(roster.forChannel("missing")->count(),0);
 }
 void sourceDestructionClearsPrivateRosterData(){
  auto *source=new RecordModel;source->replace({member("1","100")});VoiceRoster roster(source);QCOMPARE(roster.members("100").size(),1);delete source;QTRY_VERIFY(roster.members("100").isEmpty());
 }
};
QTEST_GUILESS_MAIN(VoiceRosterTest)
#include "voice_roster_test.moc"

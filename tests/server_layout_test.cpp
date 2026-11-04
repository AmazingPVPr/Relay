#include "ServerLayout.h"
#include "RecordModel.h"
#include <QAbstractItemModelTester>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPersistentModelIndex>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {
QVariantMap guild(const QString &id) { return {{"recordId", id}, {"name", "Generated guild " + id}, {"iconUrl", "https://fixture.invalid/" + id}, {"unread", 0}}; }
void populate(RecordModel &model) { model.replace({guild("101"), guild("202"), guild("303"), guild("404")}); }
QStringList ids(const ServerLayout &model) { QStringList result; for (int i = 0; i < model.count(); ++i) result.append(model.get(i).value("id").toString()); return result; }
QString storageKey(const QString &account) { return "serverLayout/v1/" + QString::fromLatin1(QCryptographicHash::hash(account.toUtf8(), QCryptographicHash::Sha256).toHex()) + "/layout"; }
QByteArray stored(const QString &file, const QString &account) { QSettings settings(file, QSettings::IniFormat); return settings.value(storageKey(account)).toByteArray(); }
void setStored(const QString &file, const QString &account, const QByteArray &json) { QSettings settings(file, QSettings::IniFormat); settings.setValue(storageKey(account), json); settings.sync(); }
int rowFor(const ServerLayout &model, const QString &id) { for (int row = 0; row < model.count(); ++row) if (model.get(row).value("id").toString() == id) return row; return -1; }
}

class ServerLayoutTest final : public QObject {
    Q_OBJECT
private slots:
    void confirmedMembershipRemovalForgetsSavedFolderButSnapshotsDoNot() {
        QTemporaryDir directory;const auto file=directory.filePath("layout.ini");RecordModel guilds;populate(guilds);
        ServerLayout layout(&guilds,file);layout.setAccountKey("one");
        QVERIFY(layout.drop("202","101","into"));const auto folder=layout.folderForGuild("101");
        const auto saved=stored(file,"one");QVERIFY(!layout.forgetGuild("101"));QCOMPARE(stored(file,"one"),saved);
        guilds.remove("101");QCOMPARE(stored(file,"one"),saved);QCOMPARE(layout.folderForGuild("101"),folder);
        QVERIFY(layout.forgetGuild("101"));QVERIFY(layout.folderForGuild("101").isEmpty());
        const auto after=stored(file,"one");QVERIFY(!after.contains("\"101\""));QVERIFY(after.contains("\"202\""));
        QVERIFY(!layout.forgetGuild("101"));QVERIFY(!layout.forgetGuild("../101"));
        guilds.upsert(guild("101"));ServerLayout restored(&guilds,file);restored.setAccountKey("one");
        QVERIFY(restored.folderForGuild("101").isEmpty());QCOMPARE(restored.folderForGuild("202"),folder);
        guilds.remove("202");QVERIFY(restored.forgetGuild("202"));QVERIFY(rowFor(restored,folder)<0);
        const auto noFolder=stored(file,"one");QVERIFY(!noFolder.contains(folder.toUtf8()));
    }
    void rootReorderingPersistsOnlyUserMutations() {
        QTemporaryDir directory; const auto file = directory.filePath("layout.ini"); RecordModel guilds; populate(guilds);
        ServerLayout layout(&guilds, file); QAbstractItemModelTester tester(&layout, QAbstractItemModelTester::FailureReportingMode::QtTest);
        layout.setAccountKey("account-one"); QCOMPARE(ids(layout), QStringList({"101", "202", "303", "404"})); QVERIFY(stored(file, "account-one").isEmpty());
        QCOMPARE(layout.roleNames().value(Qt::UserRole + 1), QByteArray("entry")); QCOMPARE(layout.data(layout.index(0), Qt::UserRole + 1).toMap(), layout.get(0));
        QVERIFY(layout.get(-1).isEmpty()); QVERIFY(layout.get(4).isEmpty()); QCOMPARE(layout.rowCount(layout.index(0)), 0);
        QVERIFY(layout.drop("404", "101", "before")); QCOMPARE(ids(layout), QStringList({"404", "101", "202", "303"}));
        QVERIFY(layout.drop("101", "303", "after")); QCOMPARE(ids(layout), QStringList({"404", "202", "303", "101"}));
        QVERIFY(layout.drop("404", {}, "before")); QCOMPARE(ids(layout), QStringList({"202", "303", "101", "404"}));
        const auto saved = stored(file, "account-one"); QVERIFY(!saved.isEmpty()); QVERIFY(!saved.contains("Generated guild")); QVERIFY(!saved.contains("fixture.invalid")); QVERIFY(!saved.contains("iconUrl"));
        guilds.patch("202", {{"unread", 3}}); guilds.upsert(guild("505")); QCOMPARE(ids(layout), QStringList({"202", "303", "101", "404", "505"})); QCOMPARE(stored(file, "account-one"), saved);
        ServerLayout restored(&guilds, file); restored.setAccountKey("account-one"); QCOMPARE(ids(restored), ids(layout));
    }
    void createsExpandsRenamesAndUngroupsFolders() {
        QTemporaryDir directory; const auto file = directory.filePath("layout.ini"); RecordModel guilds; populate(guilds); ServerLayout layout(&guilds, file); layout.setAccountKey("one");
        QVERIFY(layout.drop("202", "101", "into")); const auto folder = layout.folderForGuild("101"); QVERIFY(folder.startsWith("folder:")); QCOMPARE(layout.folderForGuild("202"), folder);
        QCOMPARE(ids(layout), QStringList({folder, "101", "202", "303", "404"})); const auto entry = layout.get(0);
        QCOMPARE(entry.value("kind").toString(), QString("folder")); QCOMPARE(entry.value("folderId").toString(), folder); QCOMPARE(entry.value("children").toList().size(), 2);
        QCOMPARE(entry.value("children").toList().first().toMap(), guilds.byId("101")); QCOMPARE(layout.get(1).value("guild").toMap(), guilds.byId("101"));
        QVERIFY(layout.toggleFolder(folder)); QCOMPARE(ids(layout), QStringList({folder, "303", "404"})); QVERIFY(!layout.get(0).value("expanded").toBool());
        QVERIFY(layout.updateFolder(folder, "  Games\n\t ", "#AB12EF")); QCOMPARE(layout.get(0).value("name").toString(), QString("Games")); QCOMPARE(layout.get(0).value("color").toString(), QString("#ab12ef"));
        QVERIFY(!layout.updateFolder(folder, "Should not apply", "red; bad")); QCOMPARE(layout.get(0).value("name").toString(), QString("Games"));
        ServerLayout restored(&guilds, file); restored.setAccountKey("one"); QCOMPARE(restored.get(0), layout.get(0));
        QVERIFY(restored.toggleFolder(folder)); QVERIFY(restored.ungroupFolder(folder)); QCOMPARE(ids(restored), QStringList({"101", "202", "303", "404"})); QVERIFY(restored.folderForGuild("101").isEmpty());
        QVERIFY(!restored.toggleFolder(folder)); QVERIFY(!restored.ungroupFolder(folder));
    }
    void moveIntoOutOfAndWithinFoldersWithoutNesting() {
        QTemporaryDir directory; RecordModel guilds; populate(guilds); ServerLayout layout(&guilds, directory.filePath("layout.ini")); layout.setAccountKey("one");
        const auto first = layout.createFolder("101"); QVERIFY(!first.isEmpty()); QVERIFY(layout.createFolder("101").isEmpty());
        QVERIFY(layout.drop("202", first, "into")); QVERIFY(layout.drop("303", "101", "after")); QCOMPARE(ids(layout), QStringList({first, "101", "303", "202", "404"}));
        QVERIFY(layout.drop("202", "101", "before")); QCOMPARE(ids(layout), QStringList({first, "202", "101", "303", "404"}));
        QVERIFY(layout.drop("404", "101", "into")); QCOMPARE(layout.folderForGuild("404"), first); QCOMPARE(ids(layout), QStringList({first, "202", "101", "404", "303"}));
        QVERIFY(layout.drop("202", {}, "after")); QVERIFY(layout.folderForGuild("202").isEmpty()); QCOMPARE(ids(layout), QStringList({first, "101", "404", "303", "202"}));
        const auto second = layout.createFolder("202"); QVERIFY(!second.isEmpty()); QVERIFY(!layout.drop(first, second, "into"));
        QVERIFY(!layout.drop(first, "101", "before")); QVERIFY(layout.drop(second, "101", "before")); QCOMPARE(ids(layout).first(), second);
        QVERIFY(layout.drop("101", second, "into")); QCOMPARE(layout.folderForGuild("101"), second);
        QVERIFY(layout.drop("404", {}, "after")); QVERIFY(layout.drop("303", {}, "after")); QVERIFY(rowFor(layout, first) < 0); // Last child left: remove the empty folder.
        QVERIFY(rowFor(layout, second) >= 0); QVERIFY(layout.ungroupFolder(second)); QCOMPARE(ids(layout), QStringList({"202", "101", "404", "303"}));
    }
    void singletonFoldersRemainUntilTheirLastGuildMovesOut() {
        QTemporaryDir directory; const auto file = directory.filePath("layout.ini"); RecordModel guilds; populate(guilds); ServerLayout layout(&guilds, file); layout.setAccountKey("one");
        const auto folder = layout.createFolder("303"); QVERIFY(!folder.isEmpty()); QCOMPARE(layout.get(rowFor(layout, folder)).value("children").toList().size(), 1);
        QVERIFY(layout.toggleFolder(folder)); ServerLayout restored(&guilds, file); restored.setAccountKey("one"); QVERIFY(rowFor(restored, folder) >= 0); QCOMPARE(restored.folderForGuild("303"), folder);
        QVERIFY(restored.drop("303", folder, "after")); QCOMPARE(ids(restored), QStringList({"101", "202", "303", "404"})); QVERIFY(rowFor(restored, folder) < 0);
    }
    void sourceStatusUpdatesAreIncrementalAndDoNotResetPersistentRows() {
        QTemporaryDir directory; RecordModel guilds; populate(guilds); ServerLayout layout(&guilds, directory.filePath("layout.ini")); layout.setAccountKey("one");
        QVERIFY(layout.drop("202", "101", "into")); const auto folder = layout.folderForGuild("101"); QPersistentModelIndex child(layout.index(rowFor(layout, "101"))), sibling(layout.index(rowFor(layout, "404")));
        QSignalSpy reset(&layout, &QAbstractItemModel::modelReset), changed(&layout, &QAbstractItemModel::dataChanged), inserted(&layout, &QAbstractItemModel::rowsInserted), removed(&layout, &QAbstractItemModel::rowsRemoved);
        guilds.patch("101", {{"unread", 7}, {"muted", true}}); QCOMPARE(reset.count(), 0); QVERIFY(changed.count() >= 2); QCOMPARE(child.data(Qt::UserRole + 1).toMap().value("guild").toMap().value("unread").toInt(), 7);
        QCOMPARE(layout.get(rowFor(layout, folder)).value("children").toList().first().toMap().value("unread").toInt(), 7);
        guilds.remove("202"); QCOMPARE(reset.count(), 0); QCOMPARE(removed.count(), 1); QVERIFY(child.isValid()); QVERIFY(sibling.isValid());
        guilds.upsert(guild("505")); QCOMPARE(reset.count(), 0); QVERIFY(inserted.count() > 0); QVERIFY(child.isValid()); QVERIFY(sibling.isValid());
        guilds.move("404", 0); QCOMPARE(ids(layout).last(), QString("505")); QCOMPARE(reset.count(), 0);
    }
    void unavailableGuildsAndFoldersReturnToTheirSavedPositions() {
        QTemporaryDir directory; const auto file = directory.filePath("layout.ini"); RecordModel guilds; populate(guilds); ServerLayout layout(&guilds, file); layout.setAccountKey("one");
        QVERIFY(layout.drop("202", "101", "into")); const auto folder = layout.folderForGuild("101"); QVERIFY(layout.drop("404", folder, "before")); const auto expected = ids(layout); const auto saved = stored(file, "one");
        guilds.replace({guild("303")}); QCOMPARE(ids(layout), QStringList({"303"})); QCOMPARE(stored(file, "one"), saved);
        QVERIFY(layout.updateFolder(folder, "Games", "#123456")); // A user edit preserves unavailable folder members.
        ServerLayout restored(&guilds, file); restored.setAccountKey("one"); QCOMPARE(ids(restored), QStringList({"303"}));
        populate(guilds); QCOMPARE(ids(restored), expected); QCOMPARE(layout.folderForGuild("101"), folder);
    }
    void accountLayoutsRemainIsolatedAndSourceSnapshotsNeverSave() {
        QTemporaryDir directory; const auto file = directory.filePath("layout.ini"); RecordModel guilds; populate(guilds); ServerLayout layout(&guilds, file); layout.setAccountKey("account/a");
        QVERIFY(layout.drop("404", "101", "before")); const auto first = stored(file, "account/a");
        guilds.clear(); layout.setAccountKey("account/b"); populate(guilds); QCOMPARE(ids(layout), QStringList({"101", "202", "303", "404"})); QVERIFY(stored(file, "account/b").isEmpty());
        const auto folder = layout.createFolder("202"); QVERIFY(!folder.isEmpty()); const auto second = stored(file, "account/b");
        guilds.clear(); layout.setAccountKey("account/a"); guilds.upsert(guild("101")); guilds.upsert(guild("303")); QCOMPARE(stored(file, "account/a"), first); populate(guilds); QCOMPARE(ids(layout), QStringList({"404", "101", "202", "303"}));
        layout.setAccountKey("account/b"); QCOMPARE(layout.folderForGuild("202"), folder); QCOMPARE(stored(file, "account/b"), second);
        layout.setAccountKey({}); layout.drop("404", "101", "before"); QCOMPARE(stored(file, "account/a"), first); QCOMPARE(stored(file, "account/b"), second);
    }
    void malformedAndOversizedPersistenceIsBoundedAndSanitized() {
        QTemporaryDir directory; const auto file = directory.filePath("layout.ini"); RecordModel guilds; populate(guilds);
        for (const auto &raw : QList<QByteArray>{"not JSON", "[]", "{\"version\":2,\"entries\":[]}", QByteArray(600 * 1024, 'x')}) {
            setStored(file, "one", raw); ServerLayout layout(&guilds, file); layout.setAccountKey("one"); QCOMPARE(ids(layout), QStringList({"101", "202", "303", "404"})); QCOMPARE(stored(file, "one"), raw);
        }
        const QString folder = "folder:00000000-0000-4000-8000-000000000001";
        QJsonArray entries{QJsonObject{{"id", "202"}}, QJsonObject{{"id", folder}, {"name", QString(150, 'N') + "\n"}, {"color", "url(https://bad.invalid/)"},
            {"children", QJsonArray{"101", "202", "101", "invalid", QJsonObject{{"id", "303"}}, "303"}}}, QJsonObject{{"id", folder}, {"children", QJsonArray{"404"}}}, QJsonObject{{"id", "folder:not-a-uuid"}, {"children", QJsonArray{"404"}}}, QJsonObject{{"id", "999999999999999999999999"}}};
        setStored(file, "one", QJsonDocument(QJsonObject{{"version", 1}, {"entries", entries}}).toJson(QJsonDocument::Compact));
        ServerLayout layout(&guilds, file); layout.setAccountKey("one"); QAbstractItemModelTester tester(&layout, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QCOMPARE(ids(layout), QStringList({"202", folder, "101", "303", "404"})); QCOMPARE(layout.get(1).value("name").toString().size(), 80); QCOMPARE(layout.get(1).value("color").toString(), QString("#5865f2"));
        QVERIFY(!layout.drop("101", "not-existing", "before")); QVERIFY(!layout.drop("101", "202", "bogus")); QVERIFY(!layout.drop("missing", "202", "into")); QVERIFY(!layout.drop(folder, {}, "into"));
    }
    void sourceDestructionAndNoOpDropsAreSafe() {
        QTemporaryDir directory; const auto file = directory.filePath("layout.ini"); auto guilds = std::make_unique<RecordModel>(); populate(*guilds); ServerLayout layout(guilds.get(), file); layout.setAccountKey("one");
        QVERIFY(!layout.drop("101", "101", "into")); QVERIFY(!layout.drop("101", "202", "before")); QVERIFY(!layout.drop("404", {}, "after")); QVERIFY(stored(file, "one").isEmpty());
        guilds.reset(); QCOMPARE(layout.count(), 0); QVERIFY(!layout.drop("101", {}, "after"));
    }
};
QTEST_GUILESS_MAIN(ServerLayoutTest)
#include "server_layout_test.moc"

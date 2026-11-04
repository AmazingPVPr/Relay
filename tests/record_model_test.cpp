#include "RecordModel.h"

#include <QAbstractItemModelTester>
#include <QPersistentModelIndex>
#include <QRandomGenerator>
#include <QSet>
#include <QSignalSpy>
#include <QTest>

namespace {
// These are the public delegate roles, independent of the model's storage.
const QStringList &roleKeys() {
    static const QStringList value = {
        "recordId", "name", "iconUrl", "initials", "kind", "guildId", "parentId",
        "topic", "unread", "muted", "authorId", "author", "avatarUrl", "content",
        "timestamp", "edited", "own", "attachments", "pending", "failed", "channelId",
        "deafened", "speaking", "embeds", "stickers", "replyAuthor", "replyContent",
        "replyId", "replyMissing", "forwarded", "systemMessage", "messageType", "paused",
        "streaming", "streamKey", "status", "roleName", "roleColor", "bot", "voiceActive",
        "rawContent", "mentionUsers", "mentionRoles", "mentionEveryone", "currentUserMentioned",
        "activity", "inVoice", "primaryGuild", "desktopStatus", "mobileStatus", "webStatus",
        "blocked", "conversationMuted", "rolePosition", "voiceStartedAt", "components",
        "channelName", "pinnedAt", "pinned", "locked", "bitrate", "userLimit", "recipientId",
        "verifiedBot", "hoistedRoleId", "hoistedRoleName", "hoistedRolePosition", "camera",
        "replyChannelId", "replyDeleted", "forumTags", "replyCount", "archived", "hidden", "watchingYourStream"
    };
    return value;
}

QVariant expectedDefault(const QString &key) {
    static const QSet<QString> lists = {
        "attachments", "embeds", "stickers", "mentionUsers", "mentionRoles", "components", "forumTags"
    };
    static const QSet<QString> numbers = {
        "unread", "messageType", "replyCount", "rolePosition", "hoistedRolePosition",
        "voiceStartedAt", "bitrate", "userLimit"
    };
    static const QSet<QString> flags = {
        "muted", "edited", "own", "pending", "failed", "hidden", "archived", "deafened",
        "speaking", "replyMissing", "replyDeleted", "forwarded", "systemMessage", "paused",
        "streaming", "watchingYourStream", "camera", "bot", "verifiedBot", "voiceActive", "mentionEveryone",
        "currentUserMentioned", "inVoice", "blocked", "conversationMuted", "pinned", "locked"
    };
    if (lists.contains(key)) return QVariantList{};
    if (key == "primaryGuild") return QVariantMap{};
    if (numbers.contains(key)) return 0;
    if (flags.contains(key)) return false;
    return QString{};
}

QVariantMap completed(QVariantMap row) {
    for (const auto &key : roleKeys())
        if (!row.contains(key)) row.insert(key, expectedDefault(key));
    return row;
}

QVariantMap row(const QString &id, const QString &name = {}) {
    return {{"recordId", id}, {"name", name}};
}

int role(const QString &key) {
    return Qt::UserRole + 1 + roleKeys().indexOf(key);
}

QStringList ids(const RecordModel &model) {
    QStringList result;
    for (int i = 0; i < model.count(); ++i) result.append(model.get(i).value("recordId").toString());
    return result;
}

QSet<int> changedRoles(const QSignalSpy &spy, int signalIndex = 0) {
    const auto values = qvariant_cast<QList<int>>(spy.at(signalIndex).at(2));
    return QSet<int>(values.cbegin(), values.cend());
}

void checkAccessors(const RecordModel &model, int at, const QVariantMap &source) {
    const auto expected = completed(source);
    const auto actual = model.get(at);
    QCOMPARE(actual.keys(), expected.keys());
    QCOMPARE(model.byId(source.value("recordId").toString()), actual);
    QCOMPARE(model.indexOf(source.value("recordId").toString()), at);
    QCOMPARE(model.recordIdAt(at), source.value("recordId").toString());
    for (const auto &key : roleKeys()) {
        const auto fromGet = actual.value(key);
        const auto fromRole = model.data(model.index(at), role(key));
        QVERIFY2(actual.contains(key), qPrintable(key));
        QVERIFY2(fromGet.metaType() == expected.value(key).metaType(), qPrintable(key));
        QVERIFY2(fromGet == expected.value(key), qPrintable(key));
        QVERIFY2(fromRole.metaType() == fromGet.metaType(), qPrintable(key));
        QVERIFY2(fromRole == fromGet, qPrintable(key));
    }
    for (auto it = source.cbegin(); it != source.cend(); ++it) {
        QVERIFY2(actual.contains(it.key()), qPrintable(it.key()));
        QVERIFY2(actual.value(it.key()).metaType() == it.value().metaType(), qPrintable(it.key()));
        QVERIFY2(actual.value(it.key()) == it.value(), qPrintable(it.key()));
    }
}
}

class RecordModelTest final : public QObject {
    Q_OBJECT
private slots:
    void roleInventoryAndTypedDefaults() {
        RecordModel model;
        QAbstractItemModelTester invariant(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        const auto roles = model.roleNames();
        QCOMPARE(roles.size(), roleKeys().size());
        for (int i = 0; i < roleKeys().size(); ++i)
            QCOMPARE(roles.value(Qt::UserRole + 1 + i), roleKeys().at(i).toUtf8());

        // The main insertion paths must expose the same complete record.
        model.upsert({{"recordId", "one"}});
        model.replace({{{"recordId", "two"}}});
        model.reconcile({{{"recordId", "two"}}, {{"recordId", "three"}}});
        QCOMPARE(model.count(), 2);
        checkAccessors(model, 0, {{"recordId", "two"}});
        checkAccessors(model, 1, {{"recordId", "three"}});
        QCOMPARE(model.rowCount(model.index(0)), 0);
        QVERIFY(!model.data({}, role("name")).isValid());
        QVERIFY(!model.data(model.index(0), Qt::DisplayRole).isValid());
        QVERIFY(!model.data(model.index(0), Qt::UserRole + 1 + roleKeys().size()).isValid());
        QVERIFY(!model.data(model.index(20), role("name")).isValid());
        QVERIFY(model.get(-1).isEmpty());
        QVERIFY(model.get(2).isEmpty());
        QVERIFY(model.byId("missing").isEmpty());
        QCOMPARE(model.indexOf("missing"), -1);
        QVERIFY(model.recordIdAt(-1).isEmpty());
        QVERIFY(model.recordIdAt(2).isEmpty());
    }

    void populatedRolesAndRoundTripDoNotNotify() {
        RecordModel model;
        QVariantMap populated;
        for (const auto &key : roleKeys()) {
            const auto type = expectedDefault(key).metaType().id();
            if (type == QMetaType::Bool) populated.insert(key, true);
            else if (type == QMetaType::Int) populated.insert(key, 42);
            else if (type == QMetaType::QVariantList)
                populated.insert(key, QVariantList{QVariantMap{{"id", "generated"}, {"count", 3}}});
            else if (type == QMetaType::QVariantMap)
                populated.insert(key, QVariantMap{{"identity_guild_id", "generated-guild"}, {"tag", "QA"}});
            else populated.insert(key, "sample-" + key);
        }
        populated.insert("futureField", QVariantMap{{"state", "retained"}});
        model.upsert(populated);
        checkAccessors(model, 0, populated);
        const auto originalIndex = QPersistentModelIndex(model.index(0));
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
        QSignalSpy moved(&model, &QAbstractItemModel::rowsMoved);
        QSignalSpy count(&model, &RecordModel::countChanged);
        model.upsert(model.get(0));
        model.upsert(populated);
        model.reconcile({model.get(0)});
        model.patch(populated.value("recordId").toString(), model.get(0));
        QCOMPARE(changed.count(), 0);
        QCOMPARE(reset.count(), 0);
        QCOMPARE(inserted.count(), 0);
        QCOMPARE(removed.count(), 0);
        QCOMPARE(moved.count(), 0);
        QCOMPARE(count.count(), 0);
        QVERIFY(originalIndex.isValid());
        QCOMPARE(originalIndex.data(role("recordId")).toString(), populated.value("recordId").toString());
    }

    void sparseAndCompleteDefaultRecordsAreEquivalent() {
        RecordModel model;
        model.upsert({{"recordId", "one"}});
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        const auto full = model.get(0);
        model.upsert(full);
        model.upsert({{"recordId", "one"}});
        model.reconcile({full});
        model.reconcile({{{"recordId", "one"}}});
        model.patch("one", full);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(reset.count(), 0);
        checkAccessors(model, 0, {{"recordId", "one"}});
    }

    void explicitlySuppliedTypesAreNotDefaultCoerced() {
        RecordModel model;
        // QVariant equality permits cross-type matches. Accessors must still
        // preserve a caller's supplied type instead of substituting a default.
        const QVariantMap supplied = {
            {"recordId", "types"}, {"unread", QVariant::fromValue<qint64>(0)},
            {"voiceStartedAt", QVariant::fromValue<qint64>(9223372036854770000LL)},
            {"muted", 0}, {"name", QVariant()}, {"attachments", QStringList{}},
            {"primaryGuild", QVariantHash{}}, {"futureNull", QVariant()}
        };
        model.upsert(supplied);
        checkAccessors(model, 0, supplied);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        model.upsert(model.get(0));
        QCOMPARE(changed.count(), 0);
        model.replace({supplied});
        checkAccessors(model, 0, supplied);
        model.reconcile({supplied, row("other")});
        checkAccessors(model, 0, supplied);
    }

    void upsertClearsOmittedFieldsButPatchRetainsThem() {
        RecordModel model;
        const QVariantMap initial = {
            {"recordId", "message"}, {"name", "before"}, {"content", "old content"},
            {"muted", true}, {"unread", 7}, {"attachments", QVariantList{"file"}},
            {"primaryGuild", QVariantMap{{"tag", "QA"}}}, {"unknown", "old-extra"}
        };
        model.upsert(initial);
        const auto retained = QPersistentModelIndex(model.index(0));
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
        QSignalSpy count(&model, &RecordModel::countChanged);

        model.patch("message", {{"recordId", "cannot-reidentify"}, {"name", "patched"}});
        auto patched = initial;
        patched.insert("name", "patched");
        checkAccessors(model, 0, patched);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(changedRoles(changed), QSet<int>{role("name")});
        QCOMPARE(model.indexOf("cannot-reidentify"), -1);
        changed.clear();

        model.upsert(row("message", "replacement"));
        checkAccessors(model, 0, row("message", "replacement"));
        QVERIFY(!model.get(0).contains("unknown"));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(changedRoles(changed), (QSet<int>{role("name"), role("content"), role("muted"),
            role("unread"), role("attachments"), role("primaryGuild")}));
        QCOMPARE(reset.count(), 0);
        QCOMPARE(inserted.count(), 0);
        QCOMPARE(removed.count(), 0);
        QCOMPARE(count.count(), 0);
        QVERIFY(retained.isValid());
        QCOMPARE(retained.data(role("recordId")).toString(), QString("message"));

        // A sparse server snapshot is also a replacement, rather than a patch.
        model.upsert(initial);
        changed.clear();
        model.reconcile({{{"recordId", "message"}}});
        checkAccessors(model, 0, {{"recordId", "message"}});
        QVERIFY(!model.get(0).contains("unknown"));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(changedRoles(changed), (QSet<int>{role("name"), role("content"), role("muted"),
            role("unread"), role("attachments"), role("primaryGuild")}));
        QVERIFY(retained.isValid());
        QCOMPARE(reset.count(), 0);
        QCOMPARE(inserted.count(), 0);
        QCOMPARE(removed.count(), 0);
    }

    void patchingDefaultsNotifiesExactlyChangedRoles() {
        RecordModel model;
        model.upsert({{"recordId", "one"}, {"muted", true}, {"unread", 3}, {"name", "named"}});
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        model.patch("one", {{"muted", false}, {"unread", 0}, {"name", QString{}}, {"attachments", QVariantList{}}});
        QCOMPARE(changed.count(), 1);
        QCOMPARE(changedRoles(changed), (QSet<int>{role("muted"), role("unread"), role("name")}));
        QCOMPARE(qvariant_cast<QModelIndex>(changed.at(0).at(0)), model.index(0));
        QCOMPARE(qvariant_cast<QModelIndex>(changed.at(0).at(1)), model.index(0));
        checkAccessors(model, 0, {{"recordId", "one"}});
        changed.clear();
        model.patch("one", {{"muted", false}, {"unread", 0}, {"name", QString{}}, {"attachments", QVariantList{}}});
        model.patch("one", {});
        model.patch("missing", {{"name", "unused"}});
        QCOMPARE(changed.count(), 0);
    }

    void unknownExtrasSurviveCopiesAndCanBeCleared() {
        RecordModel model;
        const auto original = QVariantMap{{"recordId", "one"}, {"newFlag", false},
            {"newCount", 0}, {"newList", QVariantList{}}, {"newMap", QVariantMap{{"nested", 8}}},
            {"newNull", QVariant()}};
        model.upsert(original);
        checkAccessors(model, 0, original);
        auto fetched = model.get(0);
        fetched.insert("name", "caller copy");
        fetched.remove("newMap");
        auto storedCopy = model.rows();
        storedCopy[0].insert("name", "another caller copy");
        checkAccessors(model, 0, original);

        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        model.patch("one", {{"newCount", 9}});
        QCOMPARE(model.byId("one").value("newCount").toInt(), 9);
        QCOMPARE(changed.count(), 1);
        // Unknown fields have no role ID; an empty role list means all roles.
        QVERIFY(changedRoles(changed).isEmpty());
        changed.clear();
        model.upsert({{"recordId", "one"}});
        QCOMPARE(changed.count(), 1);
        QVERIFY(changedRoles(changed).isEmpty());
        for (auto it = original.cbegin(); it != original.cend(); ++it)
            if (it.key() != "recordId") QVERIFY(!model.get(0).contains(it.key()));
    }

    void reconcilePreservesRetainedIdentityAndInvalidatesOnlyRemovedRows() {
        RecordModel model;
        QAbstractItemModelTester invariant(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.replace({row("a"), row("b"), row("c"), row("d"), row("e")});
        QPersistentModelIndex a(model.index(0)), b(model.index(1)), c(model.index(2)), d(model.index(3)), e(model.index(4));
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
        QSignalSpy moved(&model, &QAbstractItemModel::rowsMoved);
        QSignalSpy count(&model, &RecordModel::countChanged);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        model.reconcile({row("x"), row("d", "updated"), row("b"), row("y"), row("a")});
        QCOMPARE(ids(model), (QStringList{"x", "d", "b", "y", "a"}));
        QCOMPARE(reset.count(), 0);
        QCOMPARE(count.count(), 0);
        QVERIFY(removed.count() > 0);
        QVERIFY(moved.count() > 0);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(changedRoles(changed), QSet<int>{role("name")});
        QVERIFY(a.isValid()); QVERIFY(b.isValid()); QVERIFY(d.isValid());
        QVERIFY(!c.isValid()); QVERIFY(!e.isValid());
        QCOMPARE(a.row(), 4); QCOMPARE(b.row(), 2); QCOMPARE(d.row(), 1);
        QCOMPARE(a.data(role("recordId")).toString(), QString("a"));
        QCOMPARE(b.data(role("recordId")).toString(), QString("b"));
        QCOMPARE(d.data(role("recordId")).toString(), QString("d"));
        QCOMPARE(d.data(role("name")).toString(), QString("updated"));

        model.reconcile({row("b"), row("a"), row("d", "updated")});
        QCOMPARE(count.count(), 1);
        QVERIFY(a.isValid()); QVERIFY(b.isValid()); QVERIFY(d.isValid());
        QCOMPARE(a.row(), 1); QCOMPARE(b.row(), 0); QCOMPARE(d.row(), 2);
        QCOMPARE(reset.count(), 0);
        QCOMPARE(changed.count(), 1);
        model.reconcile({});
        QVERIFY(!a.isValid()); QVERIFY(!b.isValid()); QVERIFY(!d.isValid());
        QCOMPARE(model.count(), 0);
        QCOMPARE(count.count(), 2);
        QCOMPARE(reset.count(), 0);
    }

    void movesDoNotResetOrRewriteDelegates() {
        RecordModel model;
        QAbstractItemModelTester invariant(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.replace({row("a"), row("b"), row("c"), row("d")});
        QList<QPersistentModelIndex> retained;
        for (int i = 0; i < model.count(); ++i) retained.append(model.index(i));
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        QSignalSpy moved(&model, &QAbstractItemModel::rowsMoved);
        QSignalSpy count(&model, &RecordModel::countChanged);
        model.move("a", 3);
        QCOMPARE(ids(model), (QStringList{"b", "c", "d", "a"}));
        model.move("d", 0);
        QCOMPARE(ids(model), (QStringList{"d", "b", "c", "a"}));
        model.move("c", 1);
        QCOMPARE(ids(model), (QStringList{"d", "c", "b", "a"}));
        model.move("missing", 0); model.move("a", -1); model.move("a", 4); model.move("a", 3);
        QCOMPARE(moved.count(), 3);
        QCOMPARE(reset.count(), 0); QCOMPARE(inserted.count(), 0); QCOMPARE(removed.count(), 0);
        QCOMPARE(changed.count(), 0); QCOMPARE(count.count(), 0);
        for (int i = 0; i < retained.size(); ++i) {
            QVERIFY(retained.at(i).isValid());
            QCOMPARE(retained.at(i).data(role("recordId")).toString(), QString(QChar('a' + i)));
            QCOMPARE(model.indexOf(QString(QChar('a' + i))), retained.at(i).row());
        }
    }

    void duplicatesBoundsAndCapacityHaveConsistentIdentity() {
        RecordModel model(nullptr, 3);
        QAbstractItemModelTester invariant(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.replace({{}, row("a", "first"), row("b"), row("a", "last"), row("c"), row("ignored"), row("b", "final")});
        QCOMPARE(ids(model), (QStringList{"a", "b", "c"}));
        QCOMPARE(model.byId("a").value("name").toString(), QString("last"));
        QCOMPARE(model.byId("b").value("name").toString(), QString("final"));
        QPersistentModelIndex evicted(model.index(0)), retained(model.index(1));
        QSignalSpy count(&model, &RecordModel::countChanged);
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        model.upsert(row("new"));
        QCOMPARE(ids(model), (QStringList{"b", "c", "new"}));
        QVERIFY(!evicted.isValid()); QVERIFY(retained.isValid());
        QCOMPARE(retained.row(), 0);
        QCOMPARE(retained.data(role("recordId")).toString(), QString("b"));
        QCOMPARE(count.count(), 0); QCOMPARE(reset.count(), 0);
        QCOMPARE(removed.count(), 1); QCOMPARE(inserted.count(), 1);
        model.reconcile({row("new"), row("b", "first"), row("z"), row("excluded"), row("b", "last")});
        QCOMPARE(ids(model), (QStringList{"new", "b", "z"}));
        QCOMPARE(model.byId("b").value("name").toString(), QString("last"));
        QVERIFY(retained.isValid()); QCOMPARE(retained.row(), 1);
        QCOMPARE(reset.count(), 0); QCOMPARE(count.count(), 0);

        model.upsert({}); model.upsert({{"recordId", ""}}); model.remove("missing");
        QCOMPARE(ids(model), (QStringList{"new", "b", "z"}));
        model.remove("b");
        QVERIFY(!retained.isValid()); QCOMPARE(count.count(), 1);
        model.clear(); QCOMPARE(count.count(), 2); QCOMPARE(reset.count(), 1);
        model.clear(); QCOMPARE(count.count(), 2); QCOMPARE(reset.count(), 1);
        RecordModel minimum(nullptr, 0);
        QCOMPARE(minimum.maximumRows(), 1);
        minimum.upsert(row("a")); minimum.upsert(row("b"));
        QCOMPARE(ids(minimum), QStringList{"b"});
    }

    void deterministicReconcilesPreserveAllRetainedIndices() {
        RecordModel model(nullptr, 30);
        QAbstractItemModelTester invariant(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QRandomGenerator generated(0x51a7U);
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        // Repeated permutations and partial snapshots exercise overlapping
        // inserts/removals/moves without a reset destroying retained delegates.
        for (int cycle = 0; cycle < 80; ++cycle) {
            QHash<QString, QPersistentModelIndex> previous;
            for (int i = 0; i < model.count(); ++i)
                previous.insert(model.get(i).value("recordId").toString(), model.index(i));
            QStringList desiredIds;
            const int wanted = 1 + generated.bounded(24);
            while (desiredIds.size() < wanted) {
                const auto id = QString::number(1000 + generated.bounded(30));
                if (!desiredIds.contains(id)) desiredIds.append(id);
            }
            QVector<QVariantMap> desired;
            for (const auto &id : desiredIds) desired.append(row(id, "cycle-" + QString::number(cycle)));
            model.reconcile(desired);
            QCOMPARE(ids(model), desiredIds);
            QCOMPARE(model.count(), wanted);
            QCOMPARE(reset.count(), 0);
            for (int i = 0; i < desired.size(); ++i) checkAccessors(model, i, desired.at(i));
            for (auto it = previous.cbegin(); it != previous.cend(); ++it) {
                if (desiredIds.contains(it.key())) {
                    QVERIFY(it.value().isValid());
                    QCOMPARE(it.value().data(role("recordId")).toString(), it.key());
                    QCOMPARE(it.value().row(), desiredIds.indexOf(it.key()));
                } else QVERIFY(!it.value().isValid());
            }
        }
    }
};

QTEST_GUILESS_MAIN(RecordModelTest)
#include "record_model_test.moc"

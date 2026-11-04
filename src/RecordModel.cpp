#include "RecordModel.h"
#include <algorithm>

namespace {
const QList<QByteArray> &names() {
    static const QList<QByteArray> value = {
        "recordId", "name", "iconUrl", "initials", "kind", "guildId", "parentId",
        "topic", "unread", "muted", "authorId", "author", "avatarUrl", "content",
        "timestamp", "edited", "own", "attachments", "pending", "failed",
        "channelId", "deafened", "speaking", "embeds", "stickers", "replyAuthor",
        "replyContent", "replyId", "replyMissing", "forwarded", "systemMessage", "messageType", "paused", "streaming", "streamKey",
        "status", "roleName", "roleColor", "bot", "voiceActive", "rawContent", "mentionUsers",
        "mentionRoles", "mentionEveryone", "currentUserMentioned", "activity", "inVoice", "primaryGuild", "desktopStatus", "mobileStatus", "webStatus", "blocked", "conversationMuted", "rolePosition", "voiceStartedAt", "components", "channelName", "pinnedAt", "pinned", "locked", "bitrate", "userLimit", "recipientId", "verifiedBot", "hoistedRoleId", "hoistedRoleName", "hoistedRolePosition", "camera", "replyChannelId", "replyDeleted", "forumTags", "replyCount", "archived", "hidden", "watchingYourStream"
    };
    return value;
}
const QList<QString> &keys() {
    static const auto value=[] { QList<QString> result;for(const auto &name:names())result.append(QString::fromUtf8(name));return result; }();
    return value;
}
const QList<QVariant> &defaults() {
    static const auto value=[] {
        QList<QVariant> result;
        for(const auto &name:names()) {
            if (name == "attachments" || name == "embeds" || name == "stickers" || name == "mentionUsers" ||
                name == "mentionRoles" || name == "components" || name == "forumTags") result.append(QVariant(QVariantList{}));
            else if(name == "primaryGuild") result.append(QVariantMap{});
            else if(name == "unread" || name == "messageType" || name == "replyCount" || name == "rolePosition" ||
                    name == "hoistedRolePosition" || name == "voiceStartedAt" || name == "bitrate" || name == "userLimit") result.append(0);
            else if(name == "muted" || name == "edited" || name == "own" || name == "pending" || name == "failed" ||
                    name == "hidden" || name == "archived" || name == "deafened" || name == "speaking" || name == "replyMissing" ||
                    name == "replyDeleted" || name == "forwarded" || name == "systemMessage" || name == "paused" ||
                    name == "watchingYourStream" || name == "streaming" || name == "camera" || name == "bot" || name == "verifiedBot" || name == "voiceActive" ||
                    name == "mentionEveryone" || name == "currentUserMentioned" || name == "inVoice" || name == "blocked" ||
                    name == "conversationMuted" || name == "pinned" || name == "locked") result.append(false);
            else result.append(QString{});
        }
        return result;
    }();
    return value;
}
const QHash<QString,QVariant> &defaultMap() {
    static const auto value=[] { QHash<QString,QVariant> result;for(int i=0;i<keys().size();++i)result.insert(keys()[i],defaults()[i]);return result; }();
    return value;
}
}

RecordModel::RecordModel(QObject *parent, int maximumRows)
    : QAbstractListModel(parent), m_maximumRows(std::max(1, maximumRows)) {}

int RecordModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : count();
}

QVariant RecordModel::data(const QModelIndex &index, int role) const {
    const int key = role - Qt::UserRole - 1;
    if (!index.isValid() || index.column() != 0 || index.row() < 0 ||
        index.row() >= count() || key < 0 || key >= names().size()) return {};
    return m_rows.at(index.row()).value(keys().at(key),defaults().at(key));
}

QHash<int, QByteArray> RecordModel::roleNames() const {
    static const auto roles=[] { QHash<int,QByteArray> result;for(int i=0;i<names().size();++i)result.insert(Qt::UserRole+i+1,names()[i]);return result; }();
    return roles;
}

QVariantMap RecordModel::complete(QVariantMap row) {
    for(int i=0;i<keys().size();++i)if(!row.contains(keys()[i]))row.insert(keys()[i],defaults()[i]);
    return row;
}
QVariantMap RecordModel::compact(QVariantMap row) {
    // Avoid allocating dozens of unrelated/default roles for every message,
    // member, channel and server. The public role accessors synthesize them.
    QStringList redundant;
    for(auto it=row.cbegin();it!=row.cend();++it) {
        const auto standard=defaultMap().constFind(it.key());
        if(standard!=defaultMap().cend() && it.value().metaType()==standard.value().metaType() && it.value()==standard.value())redundant.append(it.key());
    }
    for(const auto &key:redundant)row.remove(key);
    return row;
}

QVariantMap RecordModel::get(int row) const {
    return row >= 0 && row < count() ? complete(m_rows.at(row)) : QVariantMap{};
}
QString RecordModel::recordIdAt(int row)const{return row>=0 && row<count() ? m_rows.at(row).value(QStringLiteral("recordId")).toString() : QString{};}

int RecordModel::indexOf(const QString &id) const { return m_indices.value(id, -1); }
QVariantMap RecordModel::byId(const QString &id) const { return get(indexOf(id)); }

void RecordModel::reindex() {
    m_indices.clear();
    for (int i = 0; i < count(); ++i) m_indices.insert(m_rows.at(i).value("recordId").toString(), i);
}

void RecordModel::replace(QVector<QVariantMap> rows) {
    QVector<QVariantMap> unique;
    QHash<QString, int> seen;
    unique.reserve(std::min(static_cast<int>(rows.size()), m_maximumRows));
    for (auto &row : rows) {
        const auto id = row.value("recordId").toString();
        if (id.isEmpty()) continue;
        if (seen.contains(id)) unique[seen.value(id)] = compact(std::move(row));
        else if (unique.size() < m_maximumRows) {
            seen.insert(id, static_cast<int>(unique.size()));
            unique.append(compact(std::move(row)));
        }
    }
    const auto oldCount = count();
    beginResetModel();
    m_rows = std::move(unique);
    reindex();
    endResetModel();
    if (oldCount != count()) emit countChanged();
}

void RecordModel::upsert(QVariantMap row) {
    const auto id = row.value("recordId").toString();
    if (id.isEmpty()) return;
    if (const int existing = indexOf(id); existing >= 0) {
        const auto updated = compact(std::move(row));
        if (m_rows.at(existing) == updated) return;
        QList<int> roles;
        for (int i = 0; i < names().size(); ++i)
            if (m_rows.at(existing).value(keys()[i],defaults()[i]) != updated.value(keys()[i],defaults()[i]))
                roles.append(Qt::UserRole + i + 1);
        m_rows[existing] = updated;
        emit dataChanged(index(existing), index(existing), roles);
        return;
    }
    const int oldCount = count();
    if (count() == m_maximumRows) {
        beginRemoveRows({}, 0, 0);
        m_rows.removeFirst();
        reindex();
        endRemoveRows();
    }
    const int at = count();
    beginInsertRows({}, at, at);
    m_rows.append(compact(std::move(row)));
    m_indices.insert(id, at);
    endInsertRows();
    if (oldCount != count()) emit countChanged();
}

void RecordModel::reconcile(QVector<QVariantMap> rows) {
    QVector<QVariantMap> desired;
    QHash<QString, int> ids;
    for (auto &row : rows) {
        const auto id = row.value("recordId").toString();
        if (id.isEmpty()) continue;
        if (ids.contains(id)) desired[ids.value(id)] = compact(std::move(row));
        else if (desired.size() < m_maximumRows) { ids.insert(id, desired.size()); desired.append(compact(std::move(row))); }
    }
    const int oldCount = count();
    // Keep the delegates and persistent indices for retained records alive.
    for (int last = count() - 1; last >= 0;) {
        if (ids.contains(m_rows.at(last).value("recordId").toString())) { --last; continue; }
        int first = last;
        while (first > 0 && !ids.contains(m_rows.at(first - 1).value("recordId").toString())) --first;
        beginRemoveRows({}, first, last); m_rows.remove(first, last - first + 1); reindex(); endRemoveRows(); last = first - 1;
    }
    for (int at = 0; at < desired.size();) {
        const auto id = desired.at(at).value("recordId").toString();
        const int existing = indexOf(id);
        if (existing < 0) {
            int end = at + 1;
            while (end < desired.size() && indexOf(desired.at(end).value("recordId").toString()) < 0) ++end;
            beginInsertRows({}, at, end - 1);
            for (int i = at; i < end; ++i) m_rows.insert(i, std::move(desired[i]));
            reindex(); endInsertRows(); at = end;
        } else {
            if (existing != at) move(id, at);
            if (m_rows.at(at) != desired.at(at)) upsert(std::move(desired[at]));
            ++at;
        }
    }
    if (oldCount != count()) emit countChanged();
}

void RecordModel::patch(const QString &id, const QVariantMap &changes) {
    const int at = indexOf(id);
    if (at < 0) return;
    auto row = m_rows.at(at);
    for (auto it = changes.cbegin(); it != changes.cend(); ++it)
        if (it.key() != "recordId") row.insert(it.key(), it.value());
    upsert(std::move(row));
}

void RecordModel::remove(const QString &id) {
    const int at = indexOf(id);
    if (at < 0) return;
    beginRemoveRows({}, at, at);
    m_rows.removeAt(at);
    reindex();
    endRemoveRows();
    emit countChanged();
}

void RecordModel::move(const QString &id, int destination) {
    const int source = indexOf(id);
    if (source < 0 || destination < 0 || destination >= count() || source == destination) return;
    if (!beginMoveRows({}, source, source, {}, destination > source ? destination + 1 : destination)) return;
    m_rows.move(source, destination);
    reindex();
    endMoveRows();
}

void RecordModel::clear() { if (!m_rows.isEmpty()) replace({}); }

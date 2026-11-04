#include "ServerLayout.h"
#include "RecordModel.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <QUuid>
#include <algorithm>

namespace {
constexpr int EntryRole = Qt::UserRole + 1;
constexpr int MaximumGuilds = 1000, MaximumFolders = 1000, MaximumJsonBytes = 512 * 1024;
const QString defaultName = QStringLiteral("Server folder"), defaultColor = QStringLiteral("#5865f2");
bool guildId(const QString &id) {
    static const QRegularExpression pattern(QStringLiteral("^[1-9][0-9]{0,21}$"));
    bool valid = false; id.toULongLong(&valid); return valid && pattern.match(id).hasMatch();
}
QString folderId(const QString &id) {
    if (!id.startsWith("folder:") || id.size() != 43) return {};
    const QUuid uuid(id.mid(7)); return uuid.isNull() ? QString{} : "folder:" + uuid.toString(QUuid::WithoutBraces);
}
QString folderName(QString name) {
    for (int i = name.size() - 1; i >= 0; --i) if (name.at(i).isNull() || name.at(i).category() == QChar::Other_Control) name.remove(i, 1);
    name = name.trimmed().left(80); return name.isEmpty() ? defaultName : name;
}
bool folderColor(const QString &color) {
    static const QRegularExpression pattern(QStringLiteral("^#[0-9a-fA-F]{6}$")); return pattern.match(color).hasMatch();
}
QString newFolderId() { return "folder:" + QUuid::createUuid().toString(QUuid::WithoutBraces); }
}

ServerLayout::ServerLayout(RecordModel *guilds, const QString &settingsFile, QObject *parent)
    : QAbstractListModel(parent), m_guilds(guilds),
      m_settings(settingsFile.isEmpty() ? std::make_unique<QSettings>() : std::make_unique<QSettings>(settingsFile, QSettings::IniFormat)) {
    if (guilds) {
        connect(guilds, &QAbstractItemModel::dataChanged, this, &ServerLayout::sourceChanged);
        connect(guilds, &QAbstractItemModel::rowsInserted, this, &ServerLayout::sourceChanged);
        connect(guilds, &QAbstractItemModel::rowsRemoved, this, &ServerLayout::sourceChanged);
        connect(guilds, &QAbstractItemModel::rowsMoved, this, &ServerLayout::sourceChanged);
        connect(guilds, &QAbstractItemModel::modelReset, this, &ServerLayout::sourceChanged);
        connect(guilds, &QAbstractItemModel::layoutChanged, this, &ServerLayout::sourceChanged);
        connect(guilds, &QObject::destroyed, this, [this] { m_guilds = nullptr; sourceChanged(); });
    }
    sourceChanged();
}
ServerLayout::~ServerLayout() = default;
int ServerLayout::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : count(); }
QVariant ServerLayout::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.model() != this || index.column() != 0 || index.row() < 0 || index.row() >= count() || role != EntryRole) return {};
    return get(index.row());
}
QHash<int, QByteArray> ServerLayout::roleNames() const { return {{EntryRole, "entry"}}; }
QVariantMap ServerLayout::get(int row) const {
    if (row < 0 || row >= count()) return {};
    auto entry = m_rows.at(row);
    // Keep cached snapshots compact, but retain the complete public guild-map
    // contract for delegates and callers of get().
    if (m_guilds) {
        const auto id = entry.value("guildId").toString();
        if (!id.isEmpty()) entry.insert("guild", m_guilds->byId(id));
        auto children = entry.value("children").toList();
        for (auto &child : children) child = m_guilds->byId(child.toMap().value("recordId").toString());
        entry.insert("children", children);
    }
    return entry;
}
QString ServerLayout::settingsKey() const {
    return "serverLayout/v1/" + QString::fromLatin1(QCryptographicHash::hash(m_accountKey.toUtf8(), QCryptographicHash::Sha256).toHex()) + "/layout";
}
void ServerLayout::setAccountKey(const QString &key) {
    if (key.size() > 128 || key.contains(QChar(0)) || key == m_accountKey) return;
    m_accountKey = key; load(); appendNewGuilds(); refresh(true); emit accountKeyChanged();
}
ServerLayout::Location ServerLayout::locate(const QList<Node> &nodes, const QString &id) {
    for (int root = 0; root < nodes.size(); ++root) {
        if (nodes[root].id == id) return {root, -1};
        const int child = nodes[root].folder ? nodes[root].children.indexOf(id) : -1;
        if (child >= 0) return {root, child};
    }
    return {};
}
void ServerLayout::sourceChanged() {
    m_available.clear(); m_sourceOrder.clear();
    if (m_guilds) for (const auto &row : m_guilds->rows()) {
        const auto id = row.value("recordId").toString();
        if (!guildId(id) || m_available.contains(id) || m_available.size() >= MaximumGuilds) continue;
        m_available.insert(id, row); m_sourceOrder.append(id);
    }
    appendNewGuilds(); refresh();
}
void ServerLayout::appendNewGuilds() {
    QSet<QString> known;
    for (const auto &node : m_nodes) {
        if (node.folder) for (const auto &id : node.children) known.insert(id);
        else known.insert(node.id);
    }
    for (const auto &id : m_sourceOrder) if (!known.contains(id) && known.size() < MaximumGuilds) {
        Node node; node.id = id; m_nodes.append(node); known.insert(id);
    }
}
void ServerLayout::refresh(bool reset) {
    QVector<QVariantMap> desired;
    auto guildRow = [this](const QString &id, const QString &parent, const QString &color = QString{}) {
        const auto guild = m_available.value(id);
        return QVariantMap{{"id", id}, {"kind", "guild"}, {"folderId", parent}, {"name", guild.value("name")},
            {"color", color}, {"expanded", false}, {"guildId", id}, {"guild", guild}, {"children", QVariantList{}}};
    };
    for (const auto &node : m_nodes) {
        if (!node.folder) { if (m_available.contains(node.id)) desired.append(guildRow(node.id, {})); continue; }
        QVariantList children;
        for (const auto &id : node.children) if (m_available.contains(id)) children.append(m_available.value(id));
        if (children.isEmpty()) continue;
        desired.append({{"id", node.id}, {"kind", "folder"}, {"folderId", node.id}, {"name", node.name}, {"color", node.color},
            {"expanded", node.expanded}, {"guildId", QString{}}, {"guild", QVariantMap{}}, {"children", children}});
        if (node.expanded) for (const auto &id : node.children) if (m_available.contains(id)) desired.append(guildRow(id, node.id, node.color));
    }
    const int previousCount = count();
    if (reset) { beginResetModel(); m_rows = std::move(desired); endResetModel(); }
    else {
        QSet<QString> wanted; for (const auto &row : desired) wanted.insert(row.value("id").toString());
        for (int at = count() - 1; at >= 0; --at) if (!wanted.contains(m_rows[at].value("id").toString())) {
            beginRemoveRows({}, at, at); m_rows.removeAt(at); endRemoveRows();
        }
        for (int at = 0; at < desired.size(); ++at) {
            const auto id = desired[at].value("id").toString(); int existing = -1;
            for (int i = at; i < count(); ++i) if (m_rows[i].value("id").toString() == id) { existing = i; break; }
            if (existing < 0) { beginInsertRows({}, at, at); m_rows.insert(at, desired[at]); endInsertRows(); }
            else {
                if (existing != at) { beginMoveRows({}, existing, existing, {}, at); m_rows.move(existing, at); endMoveRows(); }
                if (m_rows[at] != desired[at]) { m_rows[at] = desired[at]; emit dataChanged(index(at), index(at), {EntryRole}); }
            }
        }
    }
    if (count() != previousCount) emit countChanged();
}
void ServerLayout::load() {
    m_nodes.clear(); if (m_accountKey.isEmpty()) return;
    const auto raw = m_settings->value(settingsKey()).toByteArray(); if (raw.size() > MaximumJsonBytes) return;
    const auto document = QJsonDocument::fromJson(raw); if (!document.isObject()) return;
    const auto root = document.object(); if (root.value("version").toInt() != 1 || !root.value("entries").isArray()) return;
    QSet<QString> seenGuilds, seenFolders; int folders = 0;
    for (const auto &value : root.value("entries").toArray()) {
        if (seenGuilds.size() >= MaximumGuilds) break;
        const auto object = value.toObject(); const auto id = object.value("id").toString();
        if (guildId(id)) {
            if (seenGuilds.contains(id)) continue;
            Node node; node.id = id; m_nodes.append(node); seenGuilds.insert(id); continue;
        }
        const auto folder = folderId(id);
        if (folder.isEmpty() || seenFolders.contains(folder) || folders >= MaximumFolders || !object.value("children").isArray()) continue;
        Node node; node.id = folder; node.folder = true; node.name = folderName(object.value("name").toString());
        const auto color = object.value("color").toString(); node.color = folderColor(color) ? color.toLower() : defaultColor;
        node.expanded = object.value("expanded").toBool(true);
        for (const auto &child : object.value("children").toArray()) {
            const auto childId = child.toString(); if (!guildId(childId) || seenGuilds.contains(childId) || seenGuilds.size() >= MaximumGuilds) continue;
            node.children.append(childId); seenGuilds.insert(childId);
        }
        if (node.children.isEmpty()) continue;
        seenFolders.insert(folder); ++folders; m_nodes.append(node);
    }
}
void ServerLayout::save() {
    if (m_accountKey.isEmpty()) return;
    QJsonArray entries;
    for (const auto &node : m_nodes) {
        QJsonObject object{{"id", node.id}};
        if (node.folder) object = {{"id", node.id}, {"name", node.name}, {"color", node.color}, {"expanded", node.expanded}, {"children", QJsonArray::fromStringList(node.children)}};
        entries.append(object);
    }
    const auto raw = QJsonDocument(QJsonObject{{"version", 1}, {"entries", entries}}).toJson(QJsonDocument::Compact);
    if (raw.size() <= MaximumJsonBytes) { m_settings->setValue(settingsKey(), raw); m_settings->sync(); }
}
bool ServerLayout::commit(const QList<Node> &before) {
    m_nodes.removeIf([](const Node &node) { return node.folder && node.children.isEmpty(); });
    if (m_nodes == before) return false;
    refresh(true); save(); return true;
}
QString ServerLayout::folderForGuild(const QString &id) const {
    const auto location = locate(m_nodes, id); return location.child >= 0 ? m_nodes[location.root].id : QString{};
}
bool ServerLayout::forgetGuild(const QString &id) {
    // Called only after an authoritative membership removal. A transient
    // model reset or unavailable server must preserve the saved organization.
    if (!guildId(id) || m_available.contains(id)) return false;
    const auto location = locate(m_nodes, id);
    if (location.root < 0) return false;
    const auto before = m_nodes;
    if (location.child >= 0) m_nodes[location.root].children.removeAt(location.child);
    else m_nodes.removeAt(location.root);
    return commit(before);
}
QString ServerLayout::createFolder(const QString &id) {
    const auto location = locate(m_nodes, id);
    if (!m_available.contains(id) || location.root < 0 || location.child >= 0 || m_nodes[location.root].folder) return {};
    const auto before = m_nodes; Node folder; folder.id = newFolderId(); folder.folder = true;
    folder.name = defaultName; folder.color = defaultColor; folder.children = {id};
    m_nodes[location.root] = folder; commit(before); return folder.id;
}
bool ServerLayout::toggleFolder(const QString &id) {
    const auto location = locate(m_nodes, id); if (location.root < 0 || location.child >= 0 || !m_nodes[location.root].folder) return false;
    const auto before = m_nodes; m_nodes[location.root].expanded = !m_nodes[location.root].expanded; return commit(before);
}
bool ServerLayout::updateFolder(const QString &id, const QString &name, const QString &color) {
    const auto location = locate(m_nodes, id); if (location.root < 0 || location.child >= 0 || !m_nodes[location.root].folder || !folderColor(color)) return false;
    const auto before = m_nodes; m_nodes[location.root].name = folderName(name); m_nodes[location.root].color = color.toLower(); return commit(before);
}
bool ServerLayout::ungroupFolder(const QString &id) {
    const auto location = locate(m_nodes, id); if (location.root < 0 || location.child >= 0 || !m_nodes[location.root].folder) return false;
    const auto before = m_nodes; const auto children = m_nodes.takeAt(location.root).children;
    for (int i = 0; i < children.size(); ++i) { Node node; node.id = children[i]; m_nodes.insert(location.root + i, node); }
    return commit(before);
}
bool ServerLayout::drop(const QString &sourceId, const QString &targetId, const QString &placement) {
    if (placement != "before" && placement != "after" && placement != "into") return false;
    const auto source = locate(m_nodes, sourceId); if (source.root < 0 || sourceId == targetId) return false;
    const bool sourceFolder = source.child < 0 && m_nodes[source.root].folder;
    if ((!sourceFolder && !m_available.contains(sourceId)) || (sourceFolder && placement == "into") || (targetId.isEmpty() && placement == "into")) return false;
    auto target = locate(m_nodes, targetId); if (!targetId.isEmpty() && target.root < 0) return false;
    QString destinationId = targetId;
    if (sourceFolder && target.child >= 0) destinationId = m_nodes[target.root].id;
    if (sourceId == destinationId) return false;
    const auto before = m_nodes;
    Node moving;
    if (sourceFolder || source.child < 0) moving = m_nodes.takeAt(source.root);
    else { moving.id = sourceId; m_nodes[source.root].children.removeAt(source.child); }
    target = locate(m_nodes, destinationId);
    if (destinationId.isEmpty()) m_nodes.append(moving);
    else if (placement == "into") {
        auto &node = m_nodes[target.root];
        if (node.folder) node.children.insert(target.child < 0 ? node.children.size() : target.child + 1, sourceId);
        else {
            Node folder; folder.id = newFolderId(); folder.folder = true; folder.name = defaultName; folder.color = defaultColor;
            folder.children = {destinationId, sourceId}; node = folder;
        }
    } else if (target.child >= 0) m_nodes[target.root].children.insert(target.child + (placement == "after" ? 1 : 0), sourceId);
    else m_nodes.insert(target.root + (placement == "after" ? 1 : 0), moving);
    return commit(before);
}

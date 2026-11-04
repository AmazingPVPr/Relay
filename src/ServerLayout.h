#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QPointer>
#include <QStringList>
#include <QVariantMap>
#include <memory>

class RecordModel;
class QSettings;

// Local organization only. Remote guild records remain owned by DiscordClient.
class ServerLayout final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString accountKey READ accountKey WRITE setAccountKey NOTIFY accountKeyChanged)
public:
    explicit ServerLayout(RecordModel *guilds, const QString &settingsFile = {}, QObject *parent = nullptr);
    ~ServerLayout() override;
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return int(m_rows.size()); }
    QString accountKey() const { return m_accountKey; }
    void setAccountKey(const QString &key);
    bool forgetGuild(const QString &guildId);
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE bool drop(const QString &sourceId, const QString &targetId, const QString &placement);
    Q_INVOKABLE bool toggleFolder(const QString &id);
    Q_INVOKABLE bool updateFolder(const QString &id, const QString &name, const QString &color);
    Q_INVOKABLE bool ungroupFolder(const QString &id);
    Q_INVOKABLE QString createFolder(const QString &guildId);
    Q_INVOKABLE QString folderForGuild(const QString &guildId) const;
signals:
    void countChanged();
    void accountKeyChanged();
private:
    struct Node {
        QString id, name, color;
        bool folder = false, expanded = true;
        QStringList children;
        bool operator==(const Node &) const = default;
    };
    struct Location { int root = -1, child = -1; };
    static Location locate(const QList<Node> &nodes, const QString &id);
    void sourceChanged();
    void appendNewGuilds();
    void refresh(bool reset = false);
    bool commit(const QList<Node> &before);
    void load();
    void save();
    QString settingsKey() const;
    QPointer<RecordModel> m_guilds;
    std::unique_ptr<QSettings> m_settings;
    QString m_accountKey;
    QList<Node> m_nodes;
    QHash<QString, QVariantMap> m_available;
    QStringList m_sourceOrder;
    QVector<QVariantMap> m_rows;
};

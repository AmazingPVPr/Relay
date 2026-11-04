#pragma once
#include "RecordModel.h"
#include <QJsonObject>
#include <QPointer>
#include <QQueue>
#include <limits>
class DiscordClient;

// Explicit forum browsing/posting through the account's existing REST queue.
class ForumTools final : public QObject {
    Q_OBJECT
    Q_PROPERTY(RecordModel* posts READ posts CONSTANT)
    Q_PROPERTY(QString channelId READ channelId NOTIFY changed)
    Q_PROPERTY(QString channelName READ channelName NOTIFY changed)
    Q_PROPERTY(QVariantList tags READ tags NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(bool posting READ posting NOTIFY changed)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString postError READ postError NOTIFY changed)
    Q_PROPERTY(bool canPost READ canPost NOTIFY changed)
    Q_PROPERTY(bool tagRequired READ tagRequired NOTIFY changed)
    Q_PROPERTY(QString postRestriction READ postRestriction NOTIFY changed)
    Q_PROPERTY(QString filterTag READ filterTag WRITE setFilterTag NOTIFY changed)
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY changed)
    Q_PROPERTY(QString sortOrder READ sortOrder WRITE setSortOrder NOTIFY changed)
    Q_PROPERTY(bool currentThread READ currentThread NOTIFY changed)
    Q_PROPERTY(bool canReply READ canReply NOTIFY changed)
public:
    explicit ForumTools(DiscordClient *client,QObject *parent=nullptr);
    RecordModel *posts(){return &m_posts;}
    QString channelId()const{return m_channel;}QString channelName()const{return m_forum.value("name").toString();}
    QVariantList tags()const;bool loading()const{return m_loading;}bool posting()const{return m_posting;}bool hasMore()const{return m_more;}
    QString error()const{return m_error;}QString postError()const{return m_postError;}
    bool canPost()const;bool tagRequired()const{return m_forum.value("flags").toInt()&16;}
    QString postRestriction()const;QString filterTag()const{return m_filter;}QString searchText()const{return m_search;}QString sortOrder()const{return m_sort;}
    void setFilterTag(const QString &id);void setSearchText(const QString &text);void setSortOrder(const QString &order);
    bool currentThread()const;bool canReply()const;
    Q_INVOKABLE bool selectForum(const QString &id);
    Q_INVOKABLE void refresh();Q_INVOKABLE void loadMore();
    Q_INVOKABLE bool openPost(const QString &id);
    Q_INVOKABLE void returnToForum();
    Q_INVOKABLE bool createPost(const QString &title,const QString &content,const QVariantList &tags={});
    Q_INVOKABLE void clearPostError(){m_postError.clear();emit changed();}
signals:
    void changed();void postCreated(const QString &id);
private:
    friend class ForumToolsTest;
    bool ready()const;bool authenticated()const;void reset();void syncSelection();
    void handleEvent(const QString &event,const QJsonObject &data);
    QJsonObject knownChannel(const QString &id)const;quint64 permissions()const;
    void fetchArchived(quint64 operation);void acceptThreads(const QJsonArray &threads,quint64 snapshot=std::numeric_limits<quint64>::max());void rebuild();
    void markRevision(const QString &id);
    bool validThread(const QJsonObject &thread)const;void cacheThread(const QJsonObject &thread);
    QPointer<DiscordClient> m_client;RecordModel m_posts{this,500};
    QString m_guild,m_channel,m_cursor,m_error,m_postError,m_filter,m_search,m_sort="activity";
    QJsonObject m_forum;QHash<QString,QJsonObject> m_threads;
    QHash<QString,quint64> m_revisions;QSet<QString> m_deleted;QQueue<QString> m_deletedOrder;quint64 m_revision=0;
    quint64 m_session=0,m_operation=0,m_write=0,m_selection=0;
    bool m_loading=false,m_posting=false,m_more=false,m_navigating=false;
};

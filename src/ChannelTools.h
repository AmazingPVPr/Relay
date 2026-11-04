#pragma once

#include "RecordModel.h"
#include <QElapsedTimer>
#include <QJsonObject>
#include <QPointer>

class DiscordClient;

// Reuses the account client's authenticated, bounded request queue. No account
// secret or independently configurable endpoint is exposed to the UI.
class ChannelTools final : public QObject {
    Q_OBJECT
    Q_PROPERTY(RecordModel* pins READ pins CONSTANT)
    Q_PROPERTY(RecordModel* searchResults READ searchResults CONSTANT)
    Q_PROPERTY(bool pinsBusy READ pinsBusy NOTIFY changed)
    Q_PROPERTY(bool searchBusy READ searchBusy NOTIFY changed)
    Q_PROPERTY(bool actionBusy READ actionBusy NOTIFY changed)
    Q_PROPERTY(bool jumpBusy READ jumpBusy NOTIFY changed)
    Q_PROPERTY(bool pinsHasMore READ pinsHasMore NOTIFY changed)
    Q_PROPERTY(bool searchHasMore READ searchHasMore NOTIFY changed)
    Q_PROPERTY(QString pinsError READ pinsError NOTIFY changed)
    Q_PROPERTY(QString searchError READ searchError NOTIFY changed)
    Q_PROPERTY(QString actionError READ actionError NOTIFY changed)
    Q_PROPERTY(QString searchStatus READ searchStatus NOTIFY changed)
    Q_PROPERTY(QString pinsChannelId READ pinsChannelId NOTIFY changed)
    Q_PROPERTY(QString searchQuery READ searchQuery NOTIFY changed)
    Q_PROPERTY(QString searchScope READ searchScope NOTIFY changed)
    Q_PROPERTY(int totalResults READ totalResults NOTIFY changed)
public:
    explicit ChannelTools(DiscordClient *client, QObject *parent = nullptr);
    RecordModel *pins() { return &m_pins; }
    RecordModel *searchResults() { return &m_search; }
    bool pinsBusy() const { return m_pinsBusy; }
    bool searchBusy() const { return m_searchBusy; }
    bool actionBusy() const { return m_actionBusy; }
    bool jumpBusy() const { return m_jumpBusy; }
    bool pinsHasMore() const { return m_pinsMore; }
    bool searchHasMore() const { return m_searchMore; }
    QString pinsError() const { return m_pinsError; }
    QString searchError() const { return m_searchError; }
    QString actionError() const { return m_actionError; }
    QString searchStatus() const { return m_searchStatus; }
    QString pinsChannelId() const { return m_pinsChannel; }
    QString searchQuery() const { return m_query; }
    QString searchScope() const { return m_searchScope; }
    int totalResults() const { return m_totalResults; }
    Q_INVOKABLE bool readPins(const QString &channelId);
    Q_INVOKABLE void loadMorePins();
    Q_INVOKABLE void requestSearchSuggestions(const QString &text, int cursor);
    Q_INVOKABLE QVariantList searchSuggestions(const QString &text, int cursor) const;
    Q_INVOKABLE bool searchMessages(const QString &query);
    Q_INVOKABLE void loadMoreSearch();
    Q_INVOKABLE void clearSearch();
    Q_INVOKABLE bool messageAction(const QString &action, const QString &channelId, const QString &messageId,
                                   const QString &text = {}, const QString &targetChannel = {});
    Q_INVOKABLE bool setPinned(const QString &channelId, const QString &messageId, bool pinned);
    Q_INVOKABLE bool openMessage(const QString &channelId, const QString &messageId);
    Q_INVOKABLE void clearActionError();
signals:
    void changed();
    void searchSuggestionsChanged();
    void messageOpened(const QString &channelId, const QString &messageId, int rowIndex);
    void messageActionFinished(const QString &action, const QString &messageId);
    void pinnedChanged(const QString &channelId, const QString &messageId, bool pinned);
private:
    friend class ChannelToolsTest;
    bool ready() const;
    QString resolveSearchTarget(const QString &value, bool channel, QString *error) const;
    bool parseSearch();
    QString m_searchParameters, m_suggestionKey;
    quint64 m_suggestionOperation = 0;
    QElapsedTimer m_suggestionTime;
    void reset();
    void fetchPins(quint64 operation);
    void fetchSearch(quint64 operation, int indexingAttempts = 0);
    void navigateAndFetch(const QString &channelId, const QString &messageId, quint64 operation);
    QJsonObject knownChannel(const QString &id) const;
    QVariantMap resultRow(QJsonObject message, const QString &guildId = {}) const;
    QPointer<DiscordClient> m_client;
    RecordModel m_pins{this, 500}, m_search{this, 500};
    QHash<QString, QJsonObject> m_resultChannels;
    quint64 m_session = 0, m_pinsOperation = 0, m_searchOperation = 0, m_actionOperation = 0, m_jumpOperation = 0;
    quint64 m_lastSelection = 0;
    quint64 m_jumpHistorySelection = 0;
    bool m_navigating = false;
    bool m_pinsBusy = false, m_searchBusy = false, m_actionBusy = false, m_jumpBusy = false;
    bool m_pinsMore = false, m_searchMore = false;
    QString m_pinsError, m_searchError, m_actionError, m_searchStatus;
    QString m_pinsChannel, m_pinCursor, m_query, m_searchGuild, m_searchChannel, m_searchScope;
    int m_totalResults = 0, m_searchOffset = 0;
    QElapsedTimer m_indexingTime;
};

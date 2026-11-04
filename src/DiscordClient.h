#pragma once

#include "RecordModel.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QNetworkAccessManager>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QWebSocket>
#include <functional>
#include <memory>

class QNetworkReply;

class DiscordClient final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool showHiddenChannels READ showHiddenChannels WRITE setShowHiddenChannels NOTIFY stateChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
    Q_PROPERTY(bool connecting READ connecting NOTIFY stateChanged)
    Q_PROPERTY(bool demoMode READ demoMode NOTIFY stateChanged)
    Q_PROPERTY(bool loadingMessages READ loadingMessages NOTIFY stateChanged)
    Q_PROPERTY(bool hasMoreMessages READ hasMoreMessages NOTIFY stateChanged)
    Q_PROPERTY(bool hasNewerMessages READ hasNewerMessages NOTIFY stateChanged)
    Q_PROPERTY(bool sendingMessage READ sendingMessage NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(QString currentUserName READ currentUserName NOTIFY stateChanged)
    Q_PROPERTY(QString currentUserAvatarUrl READ currentUserAvatarUrl NOTIFY stateChanged)
    Q_PROPERTY(QString currentUserId READ currentUserId NOTIFY stateChanged)
    Q_PROPERTY(QString currentPresenceStatus READ currentPresenceStatus NOTIFY stateChanged)
    Q_PROPERTY(QString currentGuildId READ currentGuildId NOTIFY stateChanged)
    Q_PROPERTY(QString currentGuildName READ currentGuildName NOTIFY stateChanged)
    Q_PROPERTY(QString currentChannelId READ currentChannelId NOTIFY stateChanged)
    Q_PROPERTY(QString currentChannelName READ currentChannelName NOTIFY stateChanged)
    Q_PROPERTY(QString currentChannelTopic READ currentChannelTopic NOTIFY stateChanged)
    Q_PROPERTY(QString currentChannelKind READ currentChannelKind NOTIFY stateChanged)
    Q_PROPERTY(RecordModel* guilds READ guilds CONSTANT)
    Q_PROPERTY(RecordModel* channels READ channels CONSTANT)
    Q_PROPERTY(RecordModel* messages READ messages CONSTANT)
    Q_PROPERTY(RecordModel* dms READ dms CONSTANT)
    Q_PROPERTY(RecordModel* unreadDms READ unreadDms CONSTANT)
    Q_PROPERTY(RecordModel* voiceMembers READ voiceMembers CONSTANT)
    Q_PROPERTY(RecordModel* streams READ streams CONSTANT)
    Q_PROPERTY(RecordModel* members READ members CONSTANT)
    Q_PROPERTY(bool membersLoading READ membersLoading NOTIFY stateChanged)
    Q_PROPERTY(bool membersPartial READ membersPartial NOTIFY stateChanged)
    Q_PROPERTY(QString membersStatus READ membersStatus NOTIFY stateChanged)
    Q_PROPERTY(int memberCount READ memberCount NOTIFY stateChanged)
    Q_PROPERTY(bool inviteBusy READ inviteBusy NOTIFY inviteChanged)
    Q_PROPERTY(QString inviteUrl READ inviteUrl NOTIFY inviteChanged)
    Q_PROPERTY(QString inviteError READ inviteError NOTIFY inviteChanged)
    Q_PROPERTY(QVariantList friends READ friends NOTIFY socialChanged)
    Q_PROPERTY(QVariantList pendingFriends READ pendingFriends NOTIFY socialChanged)
    Q_PROPERTY(QVariantList messageRequests READ messageRequests NOTIFY socialChanged)
    Q_PROPERTY(QVariantList spamRequests READ spamRequests NOTIFY socialChanged)
    Q_PROPERTY(QVariantMap selectedMessageRequest READ selectedMessageRequest NOTIFY socialChanged)
    Q_PROPERTY(QVariantList requestPreviewMessages READ requestPreviewMessages NOTIFY socialChanged)
    Q_PROPERTY(bool socialBusy READ socialBusy NOTIFY socialChanged)
    Q_PROPERTY(bool requestPreviewLoading READ requestPreviewLoading NOTIFY socialChanged)
    Q_PROPERTY(QString socialError READ socialError NOTIFY socialChanged)
    Q_PROPERTY(QString socialStatus READ socialStatus NOTIFY socialChanged)
    Q_PROPERTY(bool directCallStarting READ directCallStarting NOTIFY socialChanged)
    Q_PROPERTY(QString readReceiptError READ readReceiptError NOTIFY stateChanged)
public:
    explicit DiscordClient(QObject *parent = nullptr);
    ~DiscordClient() override;
    bool connected() const { return m_connected; }
    bool showHiddenChannels() const { return m_showHiddenChannels; }
    void setShowHiddenChannels(bool enabled);
    bool connecting() const { return m_connecting; }
    bool demoMode() const { return m_demoMode; }
    bool loadingMessages() const { return m_loadingMessages; }
    bool hasMoreMessages() const { return m_hasMoreMessages; }
    bool hasNewerMessages() const { return m_hasNewerMessages; }
    bool sendingMessage() const { return m_sendingMessage; }
    QString status() const { return m_status; }
    QString error() const { return m_error; }
    QString currentUserName() const { return m_currentUserName; }
    QString currentUserAvatarUrl() const { return avatarUrl(m_users.value(m_currentUserId)); }
    QString currentUserId() const { return m_currentUserId; }
    QString currentPresenceStatus() const { return m_currentPresenceStatus; }
    QString currentGuildId() const { return m_currentGuildId; }
    QString currentGuildName() const { return m_currentGuildName; }
    QString currentChannelId() const { return m_currentChannelId; }
    QString currentChannelName() const { return m_currentChannelName; }
    QString currentChannelTopic() const { return m_currentChannelTopic; }
    QString currentChannelKind() const { return m_currentChannelKind; }
    RecordModel *guilds() { return &m_guilds; }
    RecordModel *channels() { return &m_channels; }
    RecordModel *messages() { return &m_messages; }
    RecordModel *dms() { return &m_dms; }
    RecordModel *unreadDms() { return &m_unreadDms; }
    RecordModel *voiceMembers() { return &m_voiceMembers; }
    RecordModel *streams() { return &m_streams; }
    RecordModel *members() { return &m_members; }
    bool membersLoading() const { return m_membersLoading; }
    bool membersPartial() const { return m_membersPartial; }
    QString membersStatus() const { return m_membersStatus; }
    int memberCount() const { return m_memberCount; }
    bool inviteBusy() const { return m_inviteBusy; }
    QString inviteUrl() const { return m_inviteUrl; }
    QString inviteError() const { return m_inviteError; }
    QVariantList friends() const { return m_friends; }
    QVariantList pendingFriends() const { return m_pendingFriends; }
    QVariantList messageRequests() const { return m_messageRequests; }
    QVariantList spamRequests() const { return m_spamRequests; }
    QVariantMap selectedMessageRequest() const { return m_selectedMessageRequest; }
    QVariantList requestPreviewMessages() const { return m_requestPreviewMessages; }
    bool socialBusy() const { return m_socialBusy; }
    bool requestPreviewLoading() const { return m_requestPreviewLoading; }
    QString socialError() const { return m_socialError; }
    QString socialStatus() const { return m_socialStatus; }
    bool directCallStarting() const { return m_directCallStarting; }
    QString readReceiptError() const { return m_readReceiptError; }
    QString currentVoiceSessionId() const { return m_voiceSessionId; }
    bool watchStream(const QString &key);
    void stopWatchingStream(const QString &key = {});
    Q_INVOKABLE bool startBroadcast(const QString &guildId, const QString &channelId);
    Q_INVOKABLE void stopBroadcast();
    void setVoiceGuild(const QString &guildId);
    Q_INVOKABLE void setReadingActive(bool active);
    Q_INVOKABLE bool acknowledgeVisibleMessages(const QString &channelId, const QString &messageId);
    Q_INVOKABLE void clearReadReceiptError();
    void setSpeaking(const QString &userId, bool speaking);

    Q_INVOKABLE void login(const QString &token);
    Q_INVOKABLE void logout();
    Q_INVOKABLE void loadDemo();
    Q_INVOKABLE void selectGuild(const QString &id);
    Q_INVOKABLE void selectChannel(const QString &id);
    Q_INVOKABLE void activateChannel(const QString &id);
    Q_INVOKABLE void openVoiceChat(const QString &channelId);
    Q_INVOKABLE bool createVoiceInvite(const QString &channelId);
    Q_INVOKABLE bool copyInvite();
    Q_INVOKABLE void loadOlderMessages();
    Q_INVOKABLE void loadNewerMessages();
    Q_INVOKABLE void setFollowingMessages(bool follow);
    Q_INVOKABLE void jumpToPresent();
    Q_INVOKABLE bool jumpToMessage(const QString &channelId, const QString &messageId);
    Q_INVOKABLE bool sendMessage(const QString &content);
    Q_INVOKABLE void openDm(const QString &userId);
    Q_INVOKABLE void openDirectMessage(const QString &userId);
    Q_INVOKABLE bool startDirectCall(const QString &userId);
    Q_INVOKABLE bool isDirectCall(const QString &allocationId) const { return privateVoiceAllocation(allocationId); }
    void onDirectCallConnected(const QString &channelId);
    Q_INVOKABLE void selectHome();
    Q_INVOKABLE void refreshSocial();
    Q_INVOKABLE void clearSocialFeedback();
    Q_INVOKABLE bool sendFriendRequest(const QString &username);
    Q_INVOKABLE bool acceptFriendRequest(const QString &userId);
    Q_INVOKABLE bool dismissFriendRequest(const QString &userId);
    Q_INVOKABLE bool removeFriend(const QString &userId);
    Q_INVOKABLE QVariantMap relationshipInfo(const QString &userId) const;
    Q_INVOKABLE bool addFriend(const QString &userId);
    Q_INVOKABLE bool setFriendNickname(const QString &userId, const QString &nickname);
    Q_INVOKABLE bool setUserIgnored(const QString &userId, bool ignored);
    Q_INVOKABLE QString userNote(const QString &userId) const { return m_userNotes.value(userId); }
    Q_INVOKABLE bool fetchUserNote(const QString &userId);
    Q_INVOKABLE bool setUserNote(const QString &userId, const QString &note);
    Q_INVOKABLE bool setPresenceStatus(const QString &status);
    Q_INVOKABLE bool previewMessageRequest(const QString &channelId);
    Q_INVOKABLE bool acceptMessageRequest(const QString &channelId);
    Q_INVOKABLE bool dismissMessageRequest(const QString &channelId);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void clearError();
    Q_INVOKABLE void openAttachment(const QString &url);
    Q_INVOKABLE bool openLink(const QString &url);
    Q_INVOKABLE QVariantList mentionCandidates(const QString &query) const;
    Q_INVOKABLE QVariantMap memberInfo(const QString &userId) const;
    Q_INVOKABLE QVariantMap memberInfoForGuild(const QString &userId, const QString &guildId) const;
    Q_INVOKABLE void fetchProfile(const QString &userId);
    Q_INVOKABLE void fetchProfileForGuild(const QString &userId, const QString &guildId);
    Q_INVOKABLE bool executeComponent(const QString &messageId, const QString &customId, int componentType, const QVariantList &values = {});
    Q_INVOKABLE QVariantMap guildInfo(const QString &guildId) const;
    Q_INVOKABLE QVariantMap guildHoverInfo(const QString &guildId) const;
    Q_INVOKABLE bool navigateGuild(const QString &guildId);
    Q_INVOKABLE bool sendVoiceState(const QString &guildId, const QString &channelId,
                                    bool mute, bool deaf);
signals:
    void stateChanged();
    void guildRemoved(const QString &guildId);
    void forumEventReceived(const QString &event,const QJsonObject &data);
    void modelsReady();
    void channelReferencesChanged();
    void relaySetupObserved(const QJsonObject &message);
    void messageSent(const QString &content);
    void messageSendFailed(const QString &content, const QString &reason);
    void messageJumpReady(const QString &channelId, const QString &messageId);
    void messageJumpFailed(const QString &channelId, const QString &messageId, const QString &reason);
    void voiceStateReceived(const QJsonObject &state);
    void voiceServerReceived(const QJsonObject &server);
    void voiceStateRequestRejected(const QString &guildId,const QString &channelId,const QString &reason);
    void streamCreated(const QJsonObject &event);
    void streamRestarting(const QString &streamKey);
    void streamServerReceived(const QJsonObject &event);
    void streamDeleted(const QJsonObject &event);
    void broadcastCreated(const QJsonObject &event);
    void broadcastServerReceived(const QJsonObject &event);
    void broadcastDeleted(const QJsonObject &event);
    void notificationRequested(const QString &channelId, const QString &title,
                               const QString &body, bool direct, bool mention);
    void activitySoundRequested(const QString &kind);
    void soundboardEffect(const QJsonObject &effect);
    void authenticationRejected();
    void inviteChanged();
    void socialChanged();
    void userNoteLoaded(const QString &userId, const QString &note);
    void directCallRequested(const QString &channelId, const QString &channelName);
    void directCallEnded(const QString &channelId);
    void inviteReady(const QString &url);
    void profileUpdated(const QString &userId);
    void componentError(const QString &messageId, const QString &reason);
    void componentFinished(const QString &messageId, const QString &customId);
private:
    friend class RelayDiscordBridge;
    friend class RelayDiscordBridgeTest;
    friend class AddonIdentityBroker;
    friend class AddonIdentityTest;
    friend class BackendTests;
    friend class HiddenChannelsTest;
    friend class MessageRenderTest;
    friend class MemberBackendTest;
    friend class ForumTools;
    friend class ForumToolsTest;
    friend class ChannelTools;
    friend class ChannelToolsTest;
    friend class MemberActions;
    friend class MemberActionsTest;
    friend class GuildManagement;
    friend class GuildManagementTest;
    friend class AccountSettingsService;
    friend class AccountSettingsServiceTest;
    friend class Soundboard;
    friend class SoundboardTest;
    friend class RoleColorTest;
    friend class VideoMessageTest;
    friend class ReplyJumpTest;
    friend class SocialBackendTest;
    friend class ContextActions;
    friend class ContextActionsTest;
    friend class DirectCallTest;
    friend class ReadReceiptTest;
    friend class MessageReferences;
    friend class MessageReferencesTest;
    friend class AttachmentUpload;
    friend class AttachmentUploadTest;
    using ReplyHandler = std::function<void(const QJsonValue &, const QString &)>;
    struct RestRequest {
        QByteArray method;
        QString path;
        QJsonObject body;
        ReplyHandler callback;
        quint64 generation = 0;
        qint64 dueAt = 0;
        int attempts = 0;
        QByteArray auditReason;
        int expectedStatus = 0;
        std::function<bool()> preflight;
    };
    void request(const QByteArray &method, const QString &path, const QJsonObject &body,
                 ReplyHandler callback, const QString &auditReason = {}, int expectedStatus = 0,
                 std::function<bool()> preflight = {});
    void pumpRequests();
    void startRequest(const std::shared_ptr<RestRequest> &job);
    QString routeKey(const RestRequest &job) const;
    QString describeError(int httpStatus, const QJsonObject &body, bool networkError) const;
    void setError(const QString &error);
    void fetchGuilds(const QString &after = {});
    void fetchDms();
    void acceptGuild(QJsonObject guild);
    void acceptDm(const QJsonObject &channel);
    void recordDmActivity(const QJsonObject &message);
    void rebuildUnreadDms();
    void acceptDmReadState(const QJsonObject &state);
    void applyReadWatermark(const QString &channelId, const QString &messageId);
    bool readReceiptAllowed(const QString &channelId, const QString &messageId, bool requireTail) const;
    bool readReceiptAccessible(const QString &channelId) const;
    void flushReadReceipt();
    void clearPendingReadReceipt();
    void noteLatestChannelMessage(const QString &channelId, const QString &messageId, const QString &guildId = {});
    void retireDeletedReadTarget(const QString &channelId, const QString &messageId);
    QString readChannelGuild(const QString &channelId) const;
    void selectGuildInternal(const QString &id, bool chooseDefault);
    void syncDm(const QString &id);
    static bool newerSnowflake(const QString &a, const QString &b);
    void acceptVoiceState(const QJsonObject &state, const QString &guildId = {});
    void rebuildVoiceMembers();
    void rebuildStreams();
    static bool validStreamKey(const QString &key);
    void rememberMember(const QJsonObject &member);
    void cacheMember(const QString &guildId, const QJsonObject &member);
    void acceptPresence(const QString &guildId, const QJsonObject &presence);
    void acceptProfile(const QString &userId, const QString &guildId, const QJsonObject &profile);
    QVariantMap userIndicators(const QString &userId) const;
    QVariantMap memberPresentation(const QString &guildId, const QJsonObject &user, const QJsonObject &member) const;
    bool conversationMuted(const QString &channelId) const;
    void acceptRelationship(const QJsonObject &relationship, bool removed = false);
    void rebuildSocial();
    QVariantMap socialUserRow(const QString &userId) const;
    static bool pendingMessageRequest(const QJsonObject &channel);
    QString dmRecipientId(const QJsonObject &channel) const;
    bool socialReady();
    bool changeFriendRelationship(const QString &userId, int expectedType, bool accept);
    bool changeMessageRequest(const QString &channelId, bool accept);
    void clearRequestPreview();
    bool privateVoiceAllocation(const QString &id) const;
    void prepareDirectCall(const QString &channelId, const QString &userId, quint64 operation);
    void beginDirectCall(const QString &channelId, const QString &userId, quint64 operation);
    QJsonObject wireVoiceState(const QJsonObject &state) const;
    void requestChannelInfo();
    void acceptChannelInfo(const QString &guildId, const QJsonObject &channel);
    void rebuildMembers();
    void subscribeMembers();
    void acceptMemberList(const QJsonObject &data);
    QString memberListId(const QString &channelId) const;
    QJsonObject memberSubscription() const;
    void updateGuildActivity(const QString &guildId);
    void acceptNotificationSettings(const QJsonObject &settings);
    void notifyMessage(const QJsonObject &message);
    static QUrl safeExternalUrl(const QString &raw);
    static QJsonObject allowedMentions(const QString &content);
    void cacheChannels(const QString &guildId, const QJsonArray &channels, bool retainThreads = false);
    QJsonObject cachedChannel(const QString &guildId, const QString &channelId) const;
    void rebuildChannels(bool chooseDefault);
    void fetchMessages(bool older, bool newer = false);
    void mergeMessages(const QJsonArray &messages, bool older = false);
    void acceptMessage(QJsonObject message, bool partial = false);
    void pruneMessageCache();
    void loadDemoMessages();
    void clearSelection();
    void openGateway();
    void onGatewayText(const QString &text);
    void dispatch(const QString &event, QJsonObject data);
    bool sendGateway(int opcode, const QJsonValue &data);
    void flushPendingVoiceState();
    bool rejectVoiceState(const QString &guildId,const QString &channelId,const QString &reason);
    void cancelPendingVoiceState(const QString &replacementGuild,const QString &replacementChannel);
    void heartbeat();
    void onDisconnected();
    void reconnect(bool freshSession, int minimumDelay = 0);
    void identifyOrResume();
    void updateUser(const QJsonObject &user);
    bool channelVisible(const QJsonObject &channel, const QString &guildId) const;
    static bool validId(const QString &id);
    static QString displayName(const QJsonObject &user);
    static QString initials(const QString &name);
    static QString cleanText(QString text, int limit);
    static QString safeCdnUrl(const QString &url);
    static QString avatarUrl(const QJsonObject &user);
    QVariantMap primaryGuild(const QJsonObject &user) const;
    static QString channelKind(int type);
    QVariantMap guildRow(const QJsonObject &guild) const;
    QVariantMap channelRow(const QJsonObject &channel, const QString &guildId = {}) const;
    QVariantMap messageRow(const QJsonObject &message) const;
    static QVariantList normalizedComponents(const QJsonObject &message, bool readOnly = false);

    QNetworkAccessManager m_network;
    QWebSocket m_socket;
    QTimer m_heartbeat;
    QTimer m_reconnect;
    QTimer m_requestPump;
    QTimer m_gatewayDeadline;
    QQueue<std::shared_ptr<RestRequest>> m_queue;
    QSet<QNetworkReply *> m_replies;
    QHash<QString, qint64> m_rateReset;
    qint64 m_globalReset = 0;
    RecordModel m_guilds{this, 1000};
    RecordModel m_channels{this, 2000};
    bool m_showHiddenChannels = false;
    RecordModel m_messages{this, 1000};
    RecordModel m_dms{this, 1000};
    RecordModel m_unreadDms{this, 1000};
    QTimer m_unreadDmMuteTimer;
    int m_unreadDmBatchDepth = 0;
    bool m_unreadDmsDirty = false;
    RecordModel m_voiceMembers{this, 1000};
    RecordModel m_streams{this, 1000};
    RecordModel m_members{this, 200};
    QHash<QString, QHash<QString, QJsonObject>> m_guildMembers;
    QHash<QString, QHash<QString, QString>> m_presences;
    QHash<QString, QJsonObject> m_userPresence;
    QSet<QString> m_blockedUsers;
    QHash<QString, QJsonObject> m_relationships;
    QHash<QString, QString> m_userNotes;
    QHash<QString, quint64> m_userNoteVersions;
    QSet<QString> m_pinnedDms, m_favoriteDms;
    QSet<QString> m_hideMutedGuilds;
    QHash<QString, quint64> m_relationshipVersions, m_dmSocialVersions;
    quint64 m_socialVersion = 0, m_requestPreviewGeneration = 0;
    QVariantList m_friends, m_pendingFriends, m_messageRequests, m_spamRequests;
    QVariantMap m_selectedMessageRequest;
    QVariantList m_requestPreviewMessages;
    bool m_socialBusy = false, m_requestPreviewLoading = false;
    int m_socialBatchDepth = 0;
    bool m_socialDirty = false;
    QString m_socialError, m_socialStatus;
    QSet<QString> m_privateVoiceAllocations, m_privateCalls, m_outgoingRinging;
    QQueue<QString> m_privateLeaveAllocations;
    QString m_directCallChannel, m_directCallRecipient;
    quint64 m_directCallOperation = 0;
    bool m_directCallStarting = false;
    QHash<QString, QVariantMap> m_profiles;
    QHash<QString, qint64> m_profileFetchedAt, m_channelInfoRequestedAt;
    QSet<QString> m_profilesLoading, m_componentsPending;
    QMap<int, QJsonObject> m_memberSlots;
    QTimer m_memberDeadline;
    QString m_subscribedGuild, m_subscribedChannel, m_expectedMemberList;
    bool m_memberListReceived = false;
    bool m_membersLoading = false, m_membersPartial = true;
    int m_memberCount = 0;
    QString m_membersStatus = QStringLiteral("Choose a server to view members");
    QHash<QString, QJsonObject> m_notificationSettings;
    QSet<QString> m_seenLiveMessages;
    QQueue<QString> m_seenLiveOrder;
    qint64 m_notificationStartMs = 0;
    bool m_inviteBusy = false;
    QString m_inviteUrl, m_inviteError;
    QHash<QString, QJsonObject> m_streamObjects;
    void resumeWatchingStream();
    QString m_voiceSessionId, m_watchingStream, m_desiredStream, m_broadcastStream;
    QJsonObject m_pendingVoiceState;
    QString m_pendingVoiceAccount;
    quint64 m_pendingVoiceGeneration = 0;
    QHash<QString, QHash<QString, QJsonObject>> m_voiceStates;
    QSet<QString> m_voiceSnapshots;
    QString m_voiceGuildId;
    QHash<QString, QJsonObject> m_guildObjects;
    QHash<QString, quint64> m_guildMembershipVersions;
    QSet<QString> m_removedGuilds;
    quint64 m_nextGuildMembershipVersion = 0;
    QHash<QString, QJsonArray> m_channelCache;
    QStringList m_channelCacheOrder;
    QHash<QString, QJsonObject> m_dmObjects;
    QHash<QString, QString> m_dmReadMessageIds;
    QHash<QString, QString> m_confirmedReadMessageIds, m_latestChannelMessageIds, m_localReadMessageIds;
    QHash<QString, QString> m_deletedMessageChannels, m_deletedLatestReadBoundaries;
    QHash<QString, QString> m_readChannelGuilds;
    QTimer m_readReceiptTimer;
    struct ObservedReadReceipt { QString messageId, accountId; quint64 generation = 0; int failures = 0; qint64 retryAfter = 0; };
    QHash<QString, ObservedReadReceipt> m_pendingReadReceipts;
    QQueue<QString> m_readReceiptOrder;
    QHash<QString, qint64> m_blockedReadReceipts;
    QString m_readReceiptError, m_readReceiptErrorChannel;
    quint64 m_readReceiptOperation = 0;
    qint64 m_readReceiptLastSentAt = 0;
    bool m_readReceiptInFlight = false;
    QHash<QString, QJsonObject> m_messageObjects;
    QHash<QString, QJsonObject> m_pendingMessageUpdates;
    QSet<QString> m_deletedMessages;
    QHash<QString, QJsonObject> m_users;
    QHash<QString, int> m_unread;
    QByteArray m_token;
    // Private seam used only by the local HTTP fixture tests; never configurable from QML.
    QString m_apiBase = QStringLiteral("https://discord.com/api/v10");
    QString m_sessionId;
    QString m_gatewayUrl = QStringLiteral("wss://gateway.discord.gg");
    QString m_resumeUrl;
    qint64 m_sequence = -1;
    int m_reconnectAttempts = 0;
    int m_heartbeatInterval = 41250;
    bool m_waitingHeartbeat = false;
    quint64 m_generation = 0;
    quint64 m_selectionGeneration = 0;
    quint64 m_guildGeneration = 0;
    bool m_connected = false;
    bool m_connecting = false;
    bool m_demoMode = false;
    bool m_loadingMessages = false;
    bool m_hasMoreMessages = false;
    bool m_hasNewerMessages = false;
    bool m_followingMessages = true;
    bool m_readingActive = true;
    QHash<QString,QJsonObject> m_deferredLiveMessages;
    bool m_sendingMessage = false;
    bool m_closing = false;
    QString m_status = QStringLiteral("Not connected");
    QString m_error;
    QString m_authSessionIdHash;
    QString m_currentUserName;
    QString m_currentUserId;
    QString m_currentPresenceStatus = QStringLiteral("online");
    QString m_currentGuildId;
    QString m_currentGuildName = QStringLiteral("Direct messages");
    QString m_currentChannelId;
    QString m_currentChannelName;
    QString m_currentChannelTopic;
    QString m_currentChannelKind;
};

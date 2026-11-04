#include "RelayUpdater.h"
#include "BuildVersion.h"
#include "DiagnosticLog.h"
#include "RelayPeers.h"
#include "RelayStreams.h"
#include "RelayDiscordBridge.h"
#include "addons/AddonIdentityBroker.h"
#include <QTemporaryDir>
#include <QElapsedTimer>
#include "DiscordClient.h"
#include "DiscordAuth.h"
#include "VoiceEngine.h"
#include "CameraEngine.h"
#include "ForumTools.h"
#include "StreamEngine.h"
#include "SoftwareVideo.h"
#include "AppSettings.h"
#include "ServerLayout.h"
#include "AccountSettingsService.h"
#include "CrispImage.h"
#include "ChatText.h"
#include "MessageReferences.h"
#include "GifSearch.h"
#include "VideoResolver.h"
#include "MediaLogging.h"
#include "TranslationService.h"
#include "ChannelTools.h"
#include "MemberActions.h"
#include "GuildManagement.h"
#include "NotificationManager.h"
#include "SoundEffects.h"
#include "Soundboard.h"
#include "CredentialStore.h"
#include "CredentialStatus.h"
#include "AccountSwitcher.h"
#include "ContextActions.h"
#include "AttachmentUpload.h"
#include "ImageEditor.h"
#include "VoiceRoster.h"
#include "NativeMediaSource.h"
#include "BroadcastEngine.h"
#include "WindowLifecycleProbe.h"
#include "addons/AddonManager.h"
#include "addons/AddonBroadcastBridge.h"
#include "control/ControlServer.h"
#include "control/ShortcutSettings.h"
#include "control/GlobalShortcuts.h"
#include <QGuiApplication>
#include <QIcon>
#ifdef Q_OS_WIN
#include <QApplication>
#endif
#include <QSysInfo>
#include <QMetaProperty>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QCommandLineParser>
#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSaveFile>
#include <QJsonDocument>
#include <QDateTime>
#include <QTextStream>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QFile>
#include <QSGRendererInterface>

int main(int argc, char** argv) {
    if (argc == 3 && QByteArray(argv[1]) == "--relay-mcp") {
        QCoreApplication app(argc, argv);
        return RelayPeers::runMcp(QString::fromLocal8Bit(argv[2]));
    }
    const QStringList commandActions = ControlServer::supportedActions();
    QString controlAction;
    for (int i = 1; i < argc; ++i) {
        const auto flag = QString::fromLocal8Bit(argv[i]);
        if (flag.startsWith("--") && commandActions.contains(flag.mid(2))) {
            if (!controlAction.isEmpty()) { QTextStream(stderr) << "Choose one control action.\n"; return 2; }
            controlAction = flag.mid(2);
        }
    }
    if (!controlAction.isEmpty()) {
        QCoreApplication app(argc, argv);
        const auto result = ControlServer::sendCommand(controlAction);
        QTextStream(stdout) << QJsonDocument::fromVariant(result).toJson(QJsonDocument::Compact) << '\n';
        return result.value("ok").toBool() ? 0 : 1;
    }
    MediaLogging::install();
#ifdef Q_OS_WIN
    QApplication app(argc, argv);
#else
    QGuiApplication app(argc, argv);
#endif
    app.setApplicationName("Relay");
    app.setWindowIcon(QIcon(":/relay/icons/relay.svg"));
    app.setApplicationDisplayName("Relay");
    app.setOrganizationName("Relay");
    app.setApplicationVersion("0.1.0");
    app.setProperty("relayBuildId", QString::fromLatin1(RELAY_BUILD_RELEASE_ID));
    app.setDesktopFileName(qEnvironmentVariableIsSet("FLATPAK_ID")
        ? qEnvironmentVariable("FLATPAK_ID") : QStringLiteral("relay"));
    QQuickStyle::setStyle("Basic");

    QCommandLineParser options;
    options.setApplicationDescription("Relay — a native Discord client.");
    options.addHelpOption(); options.addVersionOption();
    options.addOption({"relay-mcp", "Run the Relay-only local MCP stdio bridge.", "endpoint"});
    options.addOption({"software-rendering", "Use software rendering for this launch without changing saved settings."});
    options.addOption({"account-window", "Open a saved account in an independent window.", "account-id"});
    options.addOption({"demo", "Open a clearly labeled offline sample workspace."});
    options.addOption({"sign-in", "Open the phone QR sign-in flow."});
    options.addOption({"diagnostic-status", "Write connection states and counters without account details or content.", "path"});
    options.addOption({"screenshot", "Save an application-only screenshot and exit.", "path"});
    options.addOption({"quit-after", "Exit after this many milliseconds (smoke testing).", "milliseconds"});
    options.addOption({"studio", "Start the local OBS input receiver (no broadcast)."});
    options.addOption({"panel", "Open a settings panel for UI testing (share, studio, audio, addons, appearance or shortcuts).", "name"});
    for (const auto &action : commandActions) options.addOption({action, "Send " + action + " to the running Relay app."});
    options.addOption({"share-source", "Select an add-on source in an isolated demo share panel.", "addon-id"});
    options.addOption({"share-input-mode", "Select an input-mode enum in an isolated demo editor (never connects input).", "mode"});
    options.addOption({"window-lifecycle", "Run a generated demo window-closing probe.", "scenario"});
    options.addOption({"window-lifecycle-report", "Write the generated demo window probe report.", "path"});
    options.addOption({"font-scale", "Set font scale for an isolated UI test.", "scale"});
    options.addOption({"window-size", "Set the initial window size for UI testing, such as 960x640.", "size"});
    options.process(app);
    if (options.isSet("screenshot") || options.isSet("quit-after")) QStandardPaths::setTestModeEnabled(true);
    const bool testRun = options.isSet("screenshot") || options.isSet("quit-after");
    app.setProperty("relayPreviewMode", testRun || options.isSet("demo"));
    const QString preferenceFile = testRun ? QDir::tempPath() + "/relay-preview-" + QString::number(app.applicationPid()) + ".ini" : QString{};
    const QString windowAccount = options.value("account-window");
    if (options.isSet("account-window") && !QRegularExpression("^[0-9]{1,22}$").match(windowAccount).hasMatch()) {
        QTextStream(stderr) << "Invalid account ID.\n";return 2;
    }
    const QString sharedConfig = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (!windowAccount.isEmpty()) {
        // Only an account ID crosses the process boundary, never a credential.
        app.setApplicationName("Relay-account-" + windowAccount);

    }
    AppSettings preferences(preferenceFile);
    std::unique_ptr<QTemporaryDir> peerPreviewDirectory;
    if (testRun || options.isSet("demo")) peerPreviewDirectory = std::make_unique<QTemporaryDir>();
    RelayPeers relayPeers(peerPreviewDirectory ? peerPreviewDirectory->path() + "/profile" : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/relay-peers");
    if (testRun && options.isSet("font-scale")) preferences.setFontScale(options.value("font-scale").toDouble());
    if (options.isSet("software-rendering") || !preferences.hardwareAcceleration()) QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    const auto appDirectory = QCoreApplication::applicationDirPath();
    AddonManager addons({appDirectory + "/../lib/relay/addons", appDirectory + "/addons"},
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/addons", preferenceFile);
    ShortcutSettings shortcutSettings(preferenceFile);
    ControlServer controls;
    GlobalShortcuts globalShortcuts;
    QObject::connect(&globalShortcuts, &GlobalShortcuts::actionRequested, &controls, &ControlServer::execute);

    DiagnosticLog diagnosticHistory(testRun, &app);
    DiscordClient discord;
    RelayDiscordBridge relayConnections(&discord, &relayPeers);
    VoiceRoster voiceRoster(discord.voiceMembers());
    ServerLayout serverLayout(discord.guilds(), preferenceFile);
    QObject::connect(&discord, &DiscordClient::guildRemoved, &serverLayout, &ServerLayout::forgetGuild);
    QObject::connect(&discord, &DiscordClient::stateChanged, &serverLayout, [&] {
        app.setProperty("relayPreviewMode", testRun || discord.demoMode());
        serverLayout.setAccountKey(discord.demoMode() ? QStringLiteral("demo") : discord.currentUserId());
    });
    const auto applyChannelVisibility = [&] { discord.setShowHiddenChannels(addons.enabled("org.relay.hiddenchannels")); };
    QObject::connect(&addons, &AddonManager::itemsChanged, &discord, applyChannelVisibility);
    applyChannelVisibility();
    DiscordAuth auth;
    CredentialStore credentials(testRun ? preferenceFile : QString{}, windowAccount.isEmpty() ? QStringLiteral("discord-session") : "discord-window-session-" + windowAccount, &app);
    CredentialStore savedLogin(testRun ? preferenceFile + ".relogin" : QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/relogin-preferences.ini", windowAccount.isEmpty() ? QStringLiteral("discord-relogin") : "discord-window-relogin-" + windowAccount, &app);
    bool automaticAttempted = false;
    QElapsedTimer automaticCooldown;
    QString lastAuthenticatedAccount = testRun ? QString{} : QSettings().value("signIn/lastAccount").toString();
    CredentialStatus credentialStatus(&credentials);
    AccountSwitcher accounts(testRun ? preferenceFile + ".accounts/accounts.json" : sharedConfig + "/accounts.json");
    accounts.setWindowHandler([&](const QString &id) {
        if (testRun || discord.demoMode()) return false;
        return QProcess::startDetached(QCoreApplication::applicationFilePath(), {"--account-window", id});
    });
    ContextActions contextActions(&discord,preferenceFile);
    AttachmentUpload uploads(&discord);
    AttachmentUpload imageUploads(&discord);
    AccountSettingsService accountSettings(&discord);
    const auto synchronizeAccountWrite = [&] { accounts.setSessionBusy(accountSettings.mutating()); };
    QObject::connect(&accountSettings, &AccountSettingsService::changed, &accounts, synchronizeAccountWrite);
    synchronizeAccountWrite();
    QObject::connect(&accountSettings, &AccountSettingsService::credentialRotated, &credentials, [&](const QString &token) {
        if(testRun)return;
        if(credentials.remembered())credentials.save(token);
        accounts.rememberCurrentCredential(token);
    });
    ChatText chatText;
    MessageReferences messageReferences(&discord);
    TranslationService translation;
    ChannelTools channelTools(&discord);
    ForumTools forumTools(&discord);
    GifSearch gifSearch(preferenceFile);
    VideoResolver videoResolver;
    QString videoAccount;
    QObject::connect(&discord, &DiscordClient::stateChanged, &videoResolver, [&] {
        if (videoAccount != discord.currentUserId()) {
            videoResolver.cancelAll();
            chatText.clearCache();
            videoAccount = discord.currentUserId();
        }
    });
    NotificationManager notifications;
    const auto applyNotificationDuration = [&] { notifications.setMinimumDuration(preferences.notificationDuration()); };
    QObject::connect(&preferences, &AppSettings::changed, &notifications, applyNotificationDuration);
    applyNotificationDuration();
    SoundEffects soundEffects(&preferences);
    VoiceEngine voice;
    CameraEngine camera(&voice);
    MemberActions memberActions(&discord, &voice, preferenceFile);
    GuildManagement guildManagement(&discord);
    Soundboard soundboard(&discord,&voice,&guildManagement,&preferences);
    QObject::connect(&addons, &AddonManager::serviceCreated, &discord, [&](const QString &id, QObject *service) {
        const auto authorization = addons.identityAuthorization(id);
        if (authorization.isEmpty() || service->metaObject()->indexOfProperty("relayIdentity") < 0) return;
        auto *broker = new AddonIdentityBroker(&discord, authorization.value("clientId").toString(), QUrl(authorization.value("redirectUri").toString()), service);
        service->setProperty("relayIdentity", QVariant::fromValue<QObject *>(broker));
    });
    QObject::connect(&addons,&AddonManager::serviceCreated,&voice,[&](const QString &id,QObject *service){
        if(!addons.voiceAudioAllowed(id))return;
        const auto update=[&,weak=QPointer<QObject>(service)]{if(weak){weak->setProperty("voiceAvailable",voice.canPlaySoundboard());weak->setProperty("outputDeviceId",voice.outputDeviceId());}};
        QObject::connect(&voice,&VoiceEngine::changed,service,update);
        QObject::connect(&voice,&VoiceEngine::devicesChanged,service,update);
        QObject::connect(service,SIGNAL(voiceClipRequested(QByteArray)),&voice,SLOT(playSoundboardPcm(QByteArray)));
        QObject::connect(service,SIGNAL(stopVoiceClip()),&voice,SLOT(stopSoundboard()));
        update();
    });
    StreamEngine stream;
    QObject::connect(&stream,&StreamEngine::watchRequested,&soundEffects,[&]{if(!testRun)soundEffects.play("watch-start");});
    QObject::connect(&stream,&StreamEngine::stopRequested,&soundEffects,[&]{if(!testRun)soundEffects.play("watch-stop");});
    QObject::connect(&camera,&CameraEngine::watchRequested,&soundEffects,[&]{if(!testRun)soundEffects.play("watch-start");});
    QObject::connect(&camera,&CameraEngine::stopRequested,&soundEffects,[&]{if(!testRun)soundEffects.play("watch-stop");});
    BroadcastEngine broadcast;
    RelayStreams relayStreams(&relayPeers, &broadcast);
    diagnosticHistory.watch(&accountSettings, "Account settings", SIGNAL(changed()));
    diagnosticHistory.watch(&accounts, "Accounts", SIGNAL(changed()));
    diagnosticHistory.watch(&uploads, "Uploads", SIGNAL(changed()));
    diagnosticHistory.watch(&contextActions, "Actions", SIGNAL(changed()));
    diagnosticHistory.watch(&forumTools, "Forums", SIGNAL(changed()));
    diagnosticHistory.watch(&gifSearch, "GIF search", SIGNAL(changed()));
    diagnosticHistory.watch(&guildManagement, "Server settings", SIGNAL(changed()));
    diagnosticHistory.watch(&memberActions, "Member actions", SIGNAL(changed()));
    diagnosticHistory.watch(&messageReferences, "Messages", SIGNAL(changed()));
    diagnosticHistory.watch(&notifications, "Notifications", SIGNAL(changed()));
    diagnosticHistory.watch(&soundboard, "Soundboard", SIGNAL(changed()));
    diagnosticHistory.watch(&translation, "Translation", SIGNAL(changed()));
    diagnosticHistory.watch(&discord, "Discord", SIGNAL(stateChanged()));
    diagnosticHistory.watch(&auth, "Sign-in", SIGNAL(changed()));
    diagnosticHistory.watch(&voice, "Voice", SIGNAL(changed()));
    diagnosticHistory.watch(&camera, "Camera", SIGNAL(changed()));
    diagnosticHistory.watch(&stream, "Stream playback", SIGNAL(changed()));
    diagnosticHistory.watch(&broadcast, "Broadcast", SIGNAL(changed()));
    diagnosticHistory.watch(&soundEffects, "Sounds", SIGNAL(changed()));
    QObject::connect(&addons, &AddonManager::serviceCreated, &diagnosticHistory, [&](const QString &id, QObject *service) {
        if (id != "org.relay.obs") return;
        diagnosticHistory.watch(service, "OBS input", SIGNAL(changed()));
        QObject::connect(service, SIGNAL(diagnostic(QString)), &diagnosticHistory, SLOT(obsEvent(QString)));
    });
    AddonBroadcastBridge addonBroadcast(&addons, &broadcast);
    const auto updateAddonVoice = [&] { addonBroadcast.setVoiceContext(voice.connected() && !discord.isDirectCall(voice.currentGuildId()), voice.currentGuildId(), voice.channelId(), voice.channelName()); };
    QObject::connect(&voice, &VoiceEngine::changed, &addonBroadcast, updateAddonVoice);
    updateAddonVoice();
    QObject::connect(&broadcast, &BroadcastEngine::startRequested, &discord, [&](const QString &guild, const QString &channel) {
        broadcast.setIdentity(discord.currentUserId(), discord.currentVoiceSessionId());
        if (!voice.connected() || voice.currentGuildId()!=guild || voice.channelId()!=channel || !discord.startBroadcast(guild,channel)) broadcast.stop();
    });
    QObject::connect(&broadcast, &BroadcastEngine::stopRequested, &discord, &DiscordClient::stopBroadcast);
    QObject::connect(&discord, &DiscordClient::broadcastCreated, &broadcast, &BroadcastEngine::onStreamCreate);
    QObject::connect(&discord, &DiscordClient::broadcastServerReceived, &broadcast, &BroadcastEngine::onStreamServer);
    QObject::connect(&discord, &DiscordClient::broadcastDeleted, &broadcast, &BroadcastEngine::onStreamDelete);
    QObject::connect(&voice, &VoiceEngine::micTestStarting, &broadcast, [&] { broadcast.setAudioSuppressed(true); });
    QObject::connect(&voice, &VoiceEngine::changed, &broadcast, [&] {
        broadcast.setAudioSuppressed(voice.micTesting());
        if (!voice.connected() && broadcast.active()) broadcast.stop();
    });
    const auto applyAudio = [&] {
        voice.configureAudio({preferences.noiseSuppression(), preferences.echoCancellation(), preferences.automaticGainControl(),
            preferences.inputVolume(), preferences.outputVolume(), preferences.inputSensitivityDb(),
            preferences.voiceLimiterEnabled(), preferences.voiceLimiterCeiling()});
    };
    QObject::connect(&preferences, &AppSettings::changed, &voice, applyAudio);
    applyAudio();
    voice.setInputDeviceId(preferences.inputDeviceId());
    voice.setOutputDeviceId(preferences.outputDeviceId());
    stream.setOutputDeviceId(preferences.outputDeviceId());
    relayStreams.setOutputDeviceId(preferences.outputDeviceId());
    QObject::connect(&voice, &VoiceEngine::devicesChanged, &preferences, [&] {
        preferences.setInputDeviceId(voice.inputDeviceId()); preferences.setOutputDeviceId(voice.outputDeviceId());
    });
    controls.setStatusProvider([&] { return QVariantMap{{"voiceConnected", voice.connected()}, {"muted", voice.muted()},
        {"deafened", voice.deafened()}, {"discordConnected", discord.connected()},
        {"soundEffectsEnabled",preferences.activitySoundsEnabled()},{"soundEffectsVolume",preferences.soundEffectsVolume()},
        {"soundEffectsError",!soundEffects.error().isEmpty()},{"soundEffectsCounters",soundEffects.diagnosticSummary()},
        {"voiceHistory",voice.diagnosticHistory().toVariantMap()}}; });
    // On-demand IPC has the same content-free numeric schema as the optional
    // status file, and is available to ordinary launcher-started instances.
    controls.setDiagnosticsProvider([&] { return QVariantMap{
        {"voiceConnected", voice.connected()}, {"muted", voice.muted()}, {"deafened", voice.deafened()},
        {"discordConnected", discord.connected()}, {"micTesting", voice.micTesting()},
        {"broadcastActive", broadcast.active()}, {"streamActive", stream.active()}, {"cameraActive", camera.active()},
        {"voiceGatewayPingMs", voice.latencyMs()}, {"voiceGatewayAveragePingMs", voice.averageLatencyMs()},
        {"broadcast", broadcast.publishingDiagnostics()}, {"voicePlayback", voice.playbackDiagnostics()},
        {"streamPlayback", stream.playbackDiagnostics()}}; });
    QObject::connect(&controls, &ControlServer::actionRequested, &voice, [&](const QString &action) {
        if (action == "mute") voice.setMuted(true);
        else if (action == "unmute") voice.setMuted(false);
        else if (action == "toggle-mute") voice.setMuted(!voice.muted());
        else if (action == "deafen") voice.setDeafened(true);
        else if (action == "undeafen") voice.setDeafened(false);
        else if (action == "toggle-deafen") voice.setDeafened(!voice.deafened());
        else if (action == "disconnect-voice") voice.leave();
    });
    if (!testRun && windowAccount.isEmpty()) controls.start();
    QObject::connect(&stream, &StreamEngine::watchRequested, &discord, [&](const QString &key) {
        stream.setIdentity(discord.currentUserId(), discord.currentVoiceSessionId());
        if (!voice.connected() || !discord.watchStream(key)) stream.stop();
    });
    QObject::connect(&stream, &StreamEngine::stopRequested, &discord, &DiscordClient::stopWatchingStream);
    QObject::connect(&discord, &DiscordClient::streamCreated, &stream, &StreamEngine::onStreamCreate);
    QObject::connect(&discord, &DiscordClient::streamRestarting, &stream, &StreamEngine::onStreamRestart);
    QObject::connect(&discord, &DiscordClient::streamServerReceived, &stream, &StreamEngine::onStreamServer);
    QObject::connect(&discord, &DiscordClient::streamDeleted, &stream, &StreamEngine::onStreamDelete);
    QObject::connect(&voice, &VoiceEngine::changed, &stream, [&] {
        stream.setDeafened(voice.deafened());
        relayStreams.setDeafened(voice.deafened());
        if (!voice.connected() && !stream.streamKey().isEmpty()) stream.stop();
    });
    QObject::connect(&voice, &VoiceEngine::devicesChanged, &stream, [&] { stream.setOutputDeviceId(voice.outputDeviceId()); });
    QObject::connect(&voice, &VoiceEngine::devicesChanged, &relayStreams, [&] { relayStreams.setOutputDeviceId(voice.outputDeviceId()); });
    const auto closeAccountSession = [&] {
        relayStreams.stopWatching();
        savedLogin.cancelRestore(); credentials.cancelRestore(); stream.stop(); camera.stop(); broadcast.stop(); voice.leave();
        auth.cancel(); notifications.clear(); discord.logout();
    };
    accounts.setSessionHandler([&](const QString &token) {
        closeAccountSession();
        if(!testRun)credentials.save(token);
        discord.login(token);
    });
    QObject::connect(&auth, &DiscordAuth::authenticated, &accounts, &AccountSwitcher::captureAuthenticated);
    QObject::connect(&credentials, &CredentialStore::restored, &accounts, &AccountSwitcher::captureAuthenticated);
    const auto tryAutomaticLogin = [&] {
        if (testRun || !windowAccount.isEmpty() || automaticAttempted || auth.active() || !QSettings().value("signIn/automaticLogin", false).toBool()) return false;
        const auto owner = QSettings().value("signIn/automaticAccount").toString();
        if (!owner.isEmpty() && !lastAuthenticatedAccount.isEmpty() && owner != lastAuthenticatedAccount) return false;
        if (automaticCooldown.isValid() && automaticCooldown.elapsed() < 60000) return false;
        automaticAttempted = true; automaticCooldown.start();
        savedLogin.restore();
        return true;
    };
    QObject::connect(&auth, &DiscordAuth::rememberLoginChanged, &savedLogin, [&] {
        if (!auth.rememberLogin()) { savedLogin.cancelRestore(); savedLogin.clear(); }
        else automaticAttempted = false;
    });
    QObject::connect(&savedLogin, &CredentialStore::restored, &auth, [&](const QString &encoded) { auth.signInSavedCredential(encoded); });
    QObject::connect(&auth, &DiscordAuth::rememberedLoginReady, &savedLogin, [&](const QString &encoded) {
        if (!testRun && QSettings().value("signIn/automaticLogin", false).toBool()) savedLogin.save(encoded);
    });
    QObject::connect(&savedLogin, &CredentialStore::restoreFinished, &auth, [&](bool found) {
        if (!found && !testRun && options.isSet("sign-in") && !auth.active()) auth.start();
    });
    QObject::connect(&credentials, &CredentialStore::restoreFinished, &auth, [&](bool found) {
        if (!found && !tryAutomaticLogin() && options.isSet("sign-in") && !auth.active()) auth.start();
    });
    QObject::connect(&auth, &DiscordAuth::changed, &credentials, [&] { if(auth.active()) credentials.cancelRestore(); });
    QObject::connect(&auth, &DiscordAuth::changed, &savedLogin, [&] { if (auth.active()) savedLogin.cancelRestore(); });
    QObject::connect(&credentials, &CredentialStore::cleared, &notifications, &NotificationManager::clear);
    QObject::connect(&discord, &DiscordClient::authenticationRejected, &credentials, [&] { if(!testRun) credentials.clear(); });
    QObject::connect(&discord, &DiscordClient::authenticationRejected, &accounts, &AccountSwitcher::rejectAuthentication);
    QObject::connect(&discord, &DiscordClient::authenticationRejected, &auth, [&] { tryAutomaticLogin(); });
    QObject::connect(&discord, &DiscordClient::stateChanged, &accounts, [&] {
        accounts.updateCurrentAccount(discord.currentUserId(),discord.currentUserName(),discord.currentUserAvatarUrl());
        if (!discord.currentUserId().isEmpty() && !discord.demoMode()) {
            lastAuthenticatedAccount = discord.currentUserId();
            if (!testRun) QSettings().setValue("signIn/lastAccount", lastAuthenticatedAccount);
            if (automaticAttempted && QSettings().value("signIn/automaticAccount").toString().isEmpty())
                QSettings().setValue("signIn/automaticAccount", lastAuthenticatedAccount);
            if (discord.connected()) automaticAttempted = false;
        }
    });
    QObject::connect(&accounts, &AccountSwitcher::addAccountRequested, &auth, [&] {
        if(testRun || discord.demoMode())return;
        QSettings().setValue("signIn/automaticLogin", false); savedLogin.cancelRestore();
        closeAccountSession(); accounts.resetSession(); auth.start();
    });
    QObject::connect(&accounts, &AccountSwitcher::signOutRequested, &auth, [&] {
        QSettings().setValue("signIn/automaticLogin", false); savedLogin.cancelRestore(); savedLogin.clear();
        closeAccountSession(); if(!testRun)credentials.clear();
    });
    QObject::connect(&voice, &VoiceEngine::voiceStateRequested, &discord, [&](const QString &guild,const QString &channel,bool mute,bool deaf) {
        // Backend notifications are queued. An account change or a newer join
        // can supersede one before it reaches the main gateway.
        if (!voice.matchesAccount(discord.currentUserId())) return;
        if (!channel.isEmpty() && (voice.currentGuildId() != guild || voice.channelId() != channel)) return;
        discord.sendVoiceState(guild,channel,mute,deaf);
    });
    QObject::connect(&discord, &DiscordClient::voiceStateRequestRejected, &voice, &VoiceEngine::onVoiceRequestFailed);
    QObject::connect(&discord, &DiscordClient::voiceStateReceived, &voice, &VoiceEngine::onVoiceState);
    QObject::connect(&discord, &DiscordClient::voiceServerReceived, &voice, &VoiceEngine::onVoiceServer);
    QObject::connect(&voice, &VoiceEngine::speakingChanged, &discord, &DiscordClient::setSpeaking);
    QObject::connect(&voice, &VoiceEngine::changed, &discord, [&] {
        discord.setVoiceGuild(voice.currentGuildId());
        if(voice.connected())discord.onDirectCallConnected(voice.currentGuildId());
    });
    QObject::connect(&discord, &DiscordClient::directCallRequested, &voice, [&](const QString &channelId,const QString &name) {
        voice.join(channelId,channelId,name,discord.currentUserId());
    });
    QObject::connect(&discord, &DiscordClient::directCallEnded, &voice, [&](const QString &channelId) {
        if(voice.currentGuildId()==channelId && voice.channelId()==channelId)voice.leave();
    });
    QObject::connect(&discord, &DiscordClient::guildRemoved, &voice, [&](const QString &guildId) {
        if (voice.currentGuildId() == guildId) voice.leave();
    });
    QObject::connect(&discord, &DiscordClient::stateChanged, &voice, [&] {
        voice.onAccountState(discord.currentUserId());
        if (!voice.channelName().isEmpty() && !discord.connected() && !discord.connecting()) voice.leave();
    });
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&] {
        videoResolver.cancelAll();
        stream.stop();
        broadcast.stop();
        notifications.clear();
        voice.leave();
        globalShortcuts.disable();
        addons.shutdown();
        auth.cancel();
        discord.logout();
    });

    qmlRegisterType<ImageEditor>("Relay.Native", 1, 0, "ImageEditor");
    qmlRegisterType<CrispImage>("Relay.Native", 1, 0, "CrispImage");
    qmlRegisterType<NativeMediaSource>("Relay.Native", 1, 0, "NativeMediaSource");
    qmlRegisterType<SoftwareVideo>("Relay.Native", 1, 0, "SoftwareVideo");
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("softwareRendering", QQuickWindow::graphicsApi() == QSGRendererInterface::Software || qEnvironmentVariable("QT_QUICK_BACKEND") == "software");
    RelayUpdater relayUpdater(!discord.demoMode());
    engine.rootContext()->setContextProperty("relayUpdater", &relayUpdater);
    engine.rootContext()->setContextProperty("diagnosticLog", &diagnosticHistory);
    engine.rootContext()->setContextProperty("discord", &discord);
    engine.rootContext()->setContextProperty("relayPeers", &relayPeers);
    engine.rootContext()->setContextProperty("relayStreams", &relayStreams);
    engine.rootContext()->setContextProperty("relayConnections", &relayConnections);
    engine.rootContext()->setContextProperty("voiceRoster", &voiceRoster);
    engine.rootContext()->setContextProperty("serverLayout", &serverLayout);
    engine.rootContext()->setContextProperty("accountSettings", &accountSettings);
    engine.rootContext()->setContextProperty("chatText", &chatText);
    engine.rootContext()->setContextProperty("messageReferences", &messageReferences);
    engine.rootContext()->setContextProperty("gifSearch", &gifSearch);
    engine.rootContext()->setContextProperty("videoResolver", &videoResolver);
    engine.rootContext()->setContextProperty("translation", &translation);
    engine.rootContext()->setContextProperty("channelTools", &channelTools);
    engine.rootContext()->setContextProperty("forumTools", &forumTools);
    engine.rootContext()->setContextProperty("memberActions", &memberActions);
    engine.rootContext()->setContextProperty("guildManagement", &guildManagement);
    engine.rootContext()->setContextProperty("notifications", &notifications);
    engine.rootContext()->setContextProperty("soundEffects", &soundEffects);
    engine.rootContext()->setContextProperty("soundboard", &soundboard);
    engine.rootContext()->setContextProperty("auth", &auth);
    engine.rootContext()->setContextProperty("credentials", &credentialStatus);
    engine.rootContext()->setContextProperty("accounts", &accounts);
    engine.rootContext()->setContextProperty("contextActions", &contextActions);
    engine.rootContext()->setContextProperty("uploads", &uploads);
    engine.rootContext()->setContextProperty("imageUploads", &imageUploads);
    engine.rootContext()->setContextProperty("broadcast", &broadcast);
    engine.rootContext()->setContextProperty("voice", &voice);
    engine.rootContext()->setContextProperty("camera", &camera);
    engine.rootContext()->setContextProperty("stream", &stream);
    engine.rootContext()->setContextProperty("addons", &addons);
    engine.rootContext()->setContextProperty("addonBroadcast", &addonBroadcast);
    engine.rootContext()->setContextProperty("preferences", &preferences);
    engine.rootContext()->setContextProperty("shortcutSettings", &shortcutSettings);
    engine.rootContext()->setContextProperty("controls", &controls);
    engine.rootContext()->setContextProperty("globalShortcuts", &globalShortcuts);
    engine.addImportPath("qrc:/");
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.load(QUrl("qrc:/Relay/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 1;
    QObject::connect(&relayConnections, &RelayDiscordBridge::invitationReceived, &notifications,
        [&](const QString &channel, const QString &name) {
            if (testRun || !preferences.notificationsEnabled() || !preferences.notifyDirectMessages()) return;
            notifications.deliver(channel, "Relay chat invitation", preferences.notificationPreviews() ? name + " wants to connect with you on Relay." : QStringLiteral("New Relay chat invitation"), preferences.notificationSound());
        });
    QObject::connect(&discord, &DiscordClient::notificationRequested, &notifications,
        [&](const QString &channel, const QString &title, const QString &body, bool direct, bool mention) {
            if (testRun || !preferences.notificationsEnabled()) return;
            if (!preferences.notifyWhenFocused() && app.applicationState() == Qt::ApplicationActive) return;
            if (!(direct ? preferences.notifyDirectMessages() : mention ? preferences.notifyMentions() : preferences.notifyAllMessages())) return;
            notifications.deliver(channel, title, preferences.notificationPreviews() ? body : QStringLiteral("New message"), preferences.notificationSound());
        });
    QObject::connect(&notifications, &NotificationManager::notificationDisplayed, &soundEffects, [&](bool sound) { if(sound && !testRun) soundEffects.play("message"); });
    QObject::connect(&notifications, &NotificationManager::openChannelRequested, &discord, [&](const QString &channel) {
        discord.activateChannel(channel);
        if (auto window = qobject_cast<QQuickWindow*>(engine.rootObjects().first()); window && !window->property("applicationClosing").toBool()) {
            for (const auto &invitation : relayConnections.invitations()) if (invitation.toMap().value("channel").toString() == channel) {
                window->setProperty("relayMode", true); window->setProperty("homePage", QString{}); break;
            }
            window->show(); window->raise(); window->requestActivate();
        }
    });
    QObject::connect(&discord, &DiscordClient::activitySoundRequested, &soundEffects, [&](const QString &kind) {
        // Gateway activity must never trigger local microphone/headphone cues.
        static const QStringList remoteCues{"join","leave","user-join","user-leave","stream-start","stream-stop"};
        if(!testRun && remoteCues.contains(kind)) soundEffects.play(kind);
    });
    bool lastSharing = broadcast.sharing();
    QObject::connect(&broadcast, &BroadcastEngine::changed, &soundEffects, [&] {
        if(!testRun && broadcast.sharing()!=lastSharing) soundEffects.play(broadcast.sharing()?"stream-start":"stream-stop");
        lastSharing=broadcast.sharing();
    });
    QObject::connect(&voice, &VoiceEngine::localAudioCueRequested, &soundEffects, [&](const QString &kind) {
        if(!testRun) soundEffects.play(kind);
    });
    bool lastVoiceConnected = voice.connected();
    QObject::connect(&voice, &VoiceEngine::changed, &soundEffects, [&] {
        if(!testRun && voice.connected()!=lastVoiceConnected) soundEffects.play(voice.connected()?"join":"leave");
        lastVoiceConnected=voice.connected();
    });
    if (options.isSet("demo")) discord.loadDemo();
    else if (!testRun && !windowAccount.isEmpty()) {
        if (!accounts.switchAccount(windowAccount)) auth.start();
    }
    else if (!testRun) credentials.restore();
    else if (options.isSet("sign-in")) auth.start();
    QTimer diagnostics;
    if (options.isSet("diagnostic-status")) {
        const auto path = QFileInfo(options.value("diagnostic-status")).absoluteFilePath();
        const auto writeStatus = [&, path] {
            // This deliberately excludes names, IDs, QR contents, tokens, messages,
            // device names, and server-provided error bodies.
            const auto *diagnosticWindow=qobject_cast<QQuickWindow*>(engine.rootObjects().first());
            const auto graphicsApi=diagnosticWindow?diagnosticWindow->rendererInterface()->graphicsApi():QSGRendererInterface::Unknown;
            const QJsonObject state{{"qtVersion",QString::fromLatin1(qVersion())},{"platform",QSysInfo::kernelType()},{"architecture",QSysInfo::buildCpuArchitecture()},{"graphicsApi",int(graphicsApi)},{"hardwareAccelerated",graphicsApi!=QSGRendererInterface::Software && graphicsApi!=QSGRendererInterface::Unknown},{"updated", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                {"authActive", auth.active()}, {"qrReady", auth.qrReady()},
                {"authFailed", !auth.error().isEmpty()}, {"authenticated", !discord.currentUserId().isEmpty() && !discord.demoMode()},
                {"connected", discord.connected()}, {"connecting", discord.connecting()},
                {"discordError", !discord.error().isEmpty()}, {"serverCount", discord.guilds()->count()},
                {"channelCount", discord.channels()->count()}, {"messageCount", discord.messages()->count()},
                {"voiceMembers", discord.voiceMembers()->count()}, {"voiceConnected", voice.connected()},
                {"voiceStage", voice.stage()}, {"voiceEncrypted", voice.encryptedReady()}, {"micTesting", voice.micTesting()},
                {"voiceGatewayPingMs", voice.latencyMs()}, {"voiceGatewayAveragePingMs", voice.averageLatencyMs()},
                {"streamCount", discord.streams()->count()}, {"broadcastActive",broadcast.active()}, {"broadcastSharing",broadcast.sharing()}, {"broadcastAudio",broadcast.audioActive()}, {"broadcastAudioSuppressed",broadcast.audioSuppressed()}, {"broadcastFrames",double(broadcast.framesSent())}, {"broadcastError",!broadcast.error().isEmpty()}, {"broadcastCounters",broadcast.diagnosticSummary()}, {"loginRemembered",credentials.remembered()}, {"credentialBusy",credentials.busy()}, {"credentialError",!credentials.error().isEmpty()}, {"voiceSessionReady", !discord.currentVoiceSessionId().isEmpty()},
                {"streamActive", stream.active()}, {"streamConnected", stream.connected()}, {"streamReceiving", stream.receiving()},
                {"streamFrameCount", double(stream.frameCount())}, {"streamWidth",stream.videoWidth()}, {"streamHeight",stream.videoHeight()}, {"streamCodec",stream.codec()},
                {"streamError", !stream.error().isEmpty()}, {"streamCounters", stream.diagnosticSummary()}, {"streamPlayback",QJsonObject::fromVariantMap(stream.playbackDiagnostics())},
                {"cameraActive",camera.active()},{"cameraReceiving",camera.receiving()},{"cameraFrames",double(camera.frameCount())},{"cameraError",!camera.error().isEmpty()},{"cameraWidth",camera.videoWidth()},{"cameraHeight",camera.videoHeight()},
                {"soundEffectsError",!soundEffects.error().isEmpty()},{"soundEffectsCounters",soundEffects.diagnosticSummary()},
                {"voiceError", !voice.error().isEmpty()}, {"voiceCounters", voice.diagnosticSummary()}, {"voicePlayback",QJsonObject::fromVariantMap(voice.playbackDiagnostics())}, {"voiceHistory",voice.diagnosticHistory()}};
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly)) return;
            file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            file.write(QJsonDocument(state).toJson(QJsonDocument::Compact));
            file.commit();
        };
        QObject::connect(&diagnostics, &QTimer::timeout, &app, writeStatus);
        diagnostics.start(1000);
        writeStatus();
    }
    if (options.isSet("window-size")) {
        const auto parts = options.value("window-size").split('x');
        if (parts.size() == 2) {
            if (auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first()))
                window->resize(qMax(960, parts[0].toInt()), qMax(640, parts[1].toInt()));
        }
    }
    if (options.isSet("panel")) {
        const auto panel = options.value("panel");
        if (panel == "custom-soundboard" && testRun) { addons.setEnabled("org.relay.soundboard",true); QMetaObject::invokeMethod(engine.rootObjects().first(),"openAddon",Q_ARG(QVariant,QVariant("org.relay.soundboard"))); }
        if (panel == "share") QMetaObject::invokeMethod(engine.rootObjects().first(), "showScreenShare");
        if (panel == "studio") QMetaObject::invokeMethod(engine.rootObjects().first(), "showStudio");
        if (panel == "audio") QMetaObject::invokeMethod(engine.rootObjects().first(), "showAudio");
        if (panel == "addons") QMetaObject::invokeMethod(engine.rootObjects().first(), "showAddons");
        if (panel == "appearance") QMetaObject::invokeMethod(engine.rootObjects().first(), "showAppearance");
        if (QStringList{"account","messaging","notifications","privacy","accessibility","language","system","logs","billing","nitro","subscriptions","boosts","gifts","apps","connections","family","sessions"}.contains(panel))
            QMetaObject::invokeMethod(engine.rootObjects().first(), "showSettings", Q_ARG(QVariant, QVariant(panel)));
        if (panel == "shortcuts") QMetaObject::invokeMethod(engine.rootObjects().first(), "showShortcuts");
    }
    if (testRun && options.isSet("demo") && options.isSet("share-source")) {
        const auto id = options.value("share-source");
        QTimer::singleShot(0, &app, [&, id] {
            if (options.isSet("share-input-mode")) {
                bool valid = false;
                const int mode = options.value("share-input-mode").toInt(&valid);
                auto *source = addons.openBroadcastSource(id).value("service").value<QObject *>();
                const int index = source ? source->metaObject()->indexOfProperty("inputMode") : -1;
                const auto property = index >= 0 ? source->metaObject()->property(index) : QMetaProperty{};
                bool declared = false;
                if (property.isEnumType()) for (int i = 0; i < property.enumerator().keyCount(); ++i)
                    declared |= property.enumerator().value(i) == mode;
                if (!valid || mode < 0 || mode > 32 || !property.isWritable() || !declared || !property.write(source, mode)) {
                    qWarning("The isolated source input mode is invalid."); return;
                }
            }
            if (!QMetaObject::invokeMethod(engine.rootObjects().first(), "selectScreenShareSource", Q_ARG(QVariant, QVariant(id))))
                qWarning("The isolated share-source editor could not be opened.");
        });
    }
    if (testRun && options.isSet("demo") && options.isSet("window-lifecycle"))
        runWindowLifecycleProbe(&engine, options.value("window-lifecycle"), options.value("window-lifecycle-report"));
    if (options.isSet("studio")) {
        if (auto service = addons.service("org.relay.obs")) QMetaObject::invokeMethod(service, "start");
    }
    if (options.isSet("screenshot")) {
        const QString path = QFileInfo(options.value("screenshot")).absoluteFilePath();
        QTimer::singleShot(1400, &app, [&, path] {
            auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
            if (options.isSet("panel")) for (auto *candidate : QGuiApplication::topLevelWindows()) {
                if (candidate->isVisible() && candidate->objectName() == "settingsWindow") {
                    if (auto *settingsWindow = qobject_cast<QQuickWindow *>(candidate)) window = settingsWindow;
                }
            }
            const bool saved = window && window->grabWindow().save(path);
            if (!saved) qWarning("Application screenshot could not be saved.");
            app.exit(saved ? 0 : 2);
        });
    }
    if (options.isSet("quit-after")) {
        bool valid = false;
        const int milliseconds = options.value("quit-after").toInt(&valid);
        if (valid && milliseconds > 0) QTimer::singleShot(milliseconds, &app, &QCoreApplication::quit);
    }
    return app.exec();
}

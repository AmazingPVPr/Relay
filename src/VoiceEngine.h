#pragma once
#include <QObject>
#include "AudioProcessor.h"
#include <QJsonObject>
#include <QVariantList>
#include <memory>
#include <QThread>
#include <optional>
#include <type_traits>
class QUrl;
class QIODevice;

class VoiceEngine : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool connected READ connected NOTIFY changed)
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY changed)
    Q_PROPERTY(bool deafened READ deafened WRITE setDeafened NOTIFY changed)
    Q_PROPERTY(QString channelName READ channelName NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString stage READ stage NOTIFY diagnosticsChanged)
    Q_PROPERTY(QString diagnosticSummary READ diagnosticSummary NOTIFY diagnosticsChanged)
    Q_PROPERTY(bool encryptedReady READ encryptedReady NOTIFY changed)
    Q_PROPERTY(QString channelId READ channelId NOTIFY changed)
    Q_PROPERTY(QString currentGuildId READ currentGuildId NOTIFY changed)
    Q_PROPERTY(QVariantList inputDevices READ inputDevices NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList outputDevices READ outputDevices NOTIFY devicesChanged)
    Q_PROPERTY(QString inputDeviceId READ inputDeviceId WRITE setInputDeviceId NOTIFY devicesChanged)
    Q_PROPERTY(QString outputDeviceId READ outputDeviceId WRITE setOutputDeviceId NOTIFY devicesChanged)
    Q_PROPERTY(double inputLevel READ inputLevel NOTIFY inputLevelChanged)
    Q_PROPERTY(bool micTesting READ micTesting NOTIFY changed)
    Q_PROPERTY(double latencyMs READ latencyMs NOTIFY statsChanged)
    Q_PROPERTY(double averageLatencyMs READ averageLatencyMs NOTIFY statsChanged)
    Q_PROPERTY(QVariantList latencyHistory READ latencyHistory NOTIFY statsChanged)
    Q_PROPERTY(QString voiceServerLabel READ voiceServerLabel NOTIFY statsChanged)
public:
    explicit VoiceEngine(QObject *parent=nullptr);
    ~VoiceEngine() override;
    QString status() const; bool connected() const; bool available() const;
    bool muted() const; bool deafened() const; QString channelName() const; QString error() const;
    QVariantList inputDevices() const; QVariantList outputDevices() const;
    QJsonObject diagnosticHistory() const;
    QVariantMap playbackDiagnostics() const;
    bool matchesAccount(const QString &userId) const;
    void onAccountState(const QString &userId);
    QString stage() const; QString diagnosticSummary() const; bool encryptedReady() const;
    QString channelId() const; QString currentGuildId() const;
    QString inputDeviceId() const; QString outputDeviceId() const; double inputLevel() const; bool micTesting() const;
    double latencyMs() const;double averageLatencyMs() const;QVariantList latencyHistory() const;QString voiceServerLabel() const;
    Q_INVOKABLE void join(const QString &guildId,const QString &channelId,const QString &channelName,const QString &userId);
    Q_INVOKABLE void leave();
    Q_INVOKABLE void setMuted(bool muted);
    Q_INVOKABLE void setDeafened(bool deafened);
    Q_INVOKABLE void setInputDeviceId(const QString &id);
    Q_INVOKABLE void setOutputDeviceId(const QString &id);
    Q_INVOKABLE bool userMuted(const QString &userId) const;
    bool canPlaySoundboard() const;
    Q_INVOKABLE double userVolume(const QString &userId) const;
    Q_INVOKABLE double userSoundboardVolume(const QString &userId) const;
    Q_INVOKABLE bool setUserMuted(const QString &userId, bool muted);
    Q_INVOKABLE bool setUserVolume(const QString &userId, double volume);
    Q_INVOKABLE bool setUserSoundboardVolume(const QString &userId, double volume);
    void setSoundboardVolume(double volume);
    quint64 soundboardPlaybackGeneration() const;
    bool playReceivedSoundboardPcm(const QString &userId, const QByteArray &pcm, double sourceVolume, quint64 playbackGeneration);
    void stopReceivedSoundboards();
    void clearUserAudioSettings();
    void configureAudio(const AudioProcessor::Options &options);
    bool watchCamera(const QString &userId);
    void stopCamera();
    void requestCameraKeyframe();
    QString cameraUserId() const;
    Q_INVOKABLE void startMicTest();
    Q_INVOKABLE void stopMicTest();
public slots:
    bool playSoundboardPcm(const QByteArray &pcm);
    void stopSoundboard();
    void onVoiceState(const QJsonObject &state);
    void onVoiceServer(const QJsonObject &server);
    void onVoiceRequestFailed(const QString &guild,const QString &channel,const QString &reason);
signals:
    void changed(); void devicesChanged(); void inputLevelChanged(); void diagnosticsChanged();
    void statsChanged();
    void micTestStarting();
    // Explicit local button/shortcut actions, captured before queued GUI delivery.
    void localAudioCueRequested(QString kind);
    void userAudioChanged(const QString &userId);
    void speakingChanged(QString userId,bool speaking);
    void voiceStateRequested(QString guildId,QString channelId,bool mute,bool deaf);
    void cameraChanged();
    void cameraDiscontinuity();
    void cameraFrameReceived(QByteArray frame,quint32 timestamp,QString codec,int rotation,bool mirrored);
private:
    // Only the local integration test friend can select this transport. Never a QML API.
    friend class VoiceGatewayTest;
    friend class VoiceMicTest;
    friend class VoiceMemberTest;
    friend class SoundboardTest;
    bool configureLocalFixture(const QUrl &url);
    void queueFixtureAudio(const QByteArray &pcm);
    void expireFixtureDeadline();
    void requestFixtureHeartbeat();
    void failFixtureAudio(const QString &reason);
    QByteArray takeFixtureAudio();
    QVariantMap fixtureCounters() const;
    void queueFixturePeerAudio(const QString &userId, quint32 ssrc, quint16 sequence, const QByteArray &opus);
    void processFixturePlayback();
    void processFixturePlaybackAt(qint64 elapsedMs);
    void setFixtureAudioOutput(QIODevice *output,int bufferBytes);
    template<class Signal,class... Arguments> void notifyGui(Signal signal,Arguments... arguments) {
        QMetaObject::invokeMethod(this,[this,signal,... values=std::move(arguments)]{
            (this->*signal)(values...);
        },Qt::QueuedConnection);
    }
    // Every backend read and action has the same owner as capture, sockets,
    // Opus, DAVE and AEC. The facade remains on the GUI thread for QML.
    // Blocking calls cannot form a cycle: backend callbacks never wait on GUI.
    template<class F> auto onRuntime(F &&function) const -> std::invoke_result_t<F> {
        using Result=std::invoke_result_t<F>;
        if(QThread::currentThread()==m_runtimeThread.get())return function();
        if constexpr(std::is_void_v<Result>){
            if(!QMetaObject::invokeMethod(m_runtimeContext,[&function]{function();},Qt::BlockingQueuedConnection))qFatal("Voice runtime invocation failed");
        }else{
            std::optional<Result> result;
            if(!QMetaObject::invokeMethod(m_runtimeContext,[&]{result.emplace(function());},Qt::BlockingQueuedConnection))qFatal("Voice runtime invocation failed");
            return std::move(*result);
        }
    }
    std::unique_ptr<QThread> m_runtimeThread;
    QObject *m_runtimeContext=nullptr;
    struct Private; std::unique_ptr<Private> d;
};

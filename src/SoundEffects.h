#pragma once
#include <QObject>
#include <QHash>
#include <QPointer>
#include <QElapsedTimer>
#include <QMediaDevices>
#include <QAudioDevice>
class QAudioSink;
class QBuffer;
class QIODevice;
class AppSettings;
class SoundEffects final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    explicit SoundEffects(AppSettings *settings, QObject *parent = nullptr);
    ~SoundEffects() override;
    Q_INVOKABLE void play(const QString &name);
    Q_INVOKABLE void preview(const QString &name);
    QString error() const {return m_error;}
    QString diagnosticSummary() const;
signals:
    void changed();
    void diagnosticsChanged();
    void effectTriggered(const QString &name);
private:
    friend class SoundEffectsTest;
    struct Playback {
        QPointer<QAudioSink> sink;
        QPointer<QBuffer> source;
        quint64 generation = 0;
    };
    bool allowed(const QString &name) const;
    void configureOutput();
    QByteArray sample(const QString &name);
    void stopCue(const QString &name);
    void stopAll();
    void finishCue(const QString &name, quint64 generation, bool failed = false);
    void setError(const QString &error);
    void cancelOpposite(const QString &name);
    AppSettings *m_settings;
    QHash<QString, QByteArray> m_samples;
    QHash<QString, Playback> m_playbacks;
    QHash<QString, qint64> m_lastPlayed;
    QElapsedTimer m_clock;
    QMediaDevices m_devices;
    QAudioDevice m_output;
    QString m_outputId,m_error;
    double m_volume = 0;
    quint64 m_generation=0,m_requested=0,m_started=0,m_completed=0,m_canceled=0,m_failures=0,m_blocked=0,m_pcmBytes=0;
    // Only tests replace the output endpoint; samples and lifecycle stay real.
    bool m_previewRequest=false;
    bool m_fixtureMode=false;
    QPointer<QIODevice> m_fixtureOutput;
};

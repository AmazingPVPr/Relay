#pragma once
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QUrl>
#include <memory>

class AddonManager final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)
    Q_PROPERTY(QVariantList broadcastSources READ broadcastSources NOTIFY itemsChanged)
    Q_PROPERTY(QString activeId READ activeId NOTIFY activeChanged)
    Q_PROPERTY(QString activeTitle READ activeTitle NOTIFY activeChanged)
    Q_PROPERTY(QUrl activeSettingsUrl READ activeSettingsUrl NOTIFY activeChanged)
    Q_PROPERTY(QObject* activeService READ activeService NOTIFY activeChanged)
    Q_PROPERTY(QString localDirectory READ localDirectory CONSTANT)
public:
    explicit AddonManager(const QStringList &bundledDirectories = {}, const QString &localDirectory = {},
                          const QString &settingsFile = {}, QObject *parent = nullptr);
    ~AddonManager() override;
    QVariantList items() const;
    QVariantList broadcastSources() const;
    Q_INVOKABLE QVariantMap openBroadcastSource(const QString &id);
    QString activeId() const;
    QString activeTitle() const;
    QUrl activeSettingsUrl() const;
    QObject *activeService() const;
    QString localDirectory() const;
    Q_INVOKABLE bool enabled(const QString &id) const;
    QVariantMap identityAuthorization(const QString &id) const;
    bool voiceAudioAllowed(const QString &id) const;
    bool broadcastSourceAllowed(const QString &id) const;
    Q_INVOKABLE bool setEnabled(const QString &id, bool enabled);
    Q_INVOKABLE bool openSettings(const QString &id);
    Q_INVOKABLE void closeSettings();
    Q_INVOKABLE void rescan();
    Q_INVOKABLE bool openLocalDirectory();
    // Starts no capture by itself. Services are created only when requested.
    Q_INVOKABLE QObject *service(const QString &id);
    void shutdown();
signals:
    void itemsChanged();
    void serviceCreated(const QString &id,QObject *service);
    void activeChanged();
private:
    struct Private;
    std::unique_ptr<Private> d;
};

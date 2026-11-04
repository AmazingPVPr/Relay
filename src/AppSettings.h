#pragma once
#include <QObject>
#include <QSettings>
#include <memory>

class AppSettings final : public QObject {
    friend class Soundboard;
    Q_OBJECT
    Q_PROPERTY(bool hardwareAcceleration READ hardwareAcceleration WRITE setHardwareAcceleration NOTIFY changed)
    Q_PROPERTY(bool graphicsRestartRequired READ graphicsRestartRequired NOTIFY changed)
    Q_PROPERTY(double fontScale READ fontScale WRITE setFontScale NOTIFY changed)
    Q_PROPERTY(bool compactMessages READ compactMessages WRITE setCompactMessages NOTIFY changed)
    Q_PROPERTY(bool reducedMotion READ reducedMotion WRITE setReducedMotion NOTIFY changed)
    Q_PROPERTY(QString inputDeviceId READ inputDeviceId WRITE setInputDeviceId NOTIFY changed)
    Q_PROPERTY(QString outputDeviceId READ outputDeviceId WRITE setOutputDeviceId NOTIFY changed)
    Q_PROPERTY(double inputVolume READ inputVolume WRITE setInputVolume NOTIFY changed)
    Q_PROPERTY(double outputVolume READ outputVolume WRITE setOutputVolume NOTIFY changed)
    Q_PROPERTY(bool voiceLimiterEnabled READ voiceLimiterEnabled WRITE setVoiceLimiterEnabled NOTIFY changed)
    Q_PROPERTY(double voiceLimiterCeiling READ voiceLimiterCeiling WRITE setVoiceLimiterCeiling NOTIFY changed)
    Q_PROPERTY(bool noiseSuppression READ noiseSuppression WRITE setNoiseSuppression NOTIFY changed)
    Q_PROPERTY(bool echoCancellation READ echoCancellation WRITE setEchoCancellation NOTIFY changed)
    Q_PROPERTY(bool automaticGainControl READ automaticGainControl WRITE setAutomaticGainControl NOTIFY changed)
    Q_PROPERTY(double inputSensitivityDb READ inputSensitivityDb WRITE setInputSensitivityDb NOTIFY changed)
    Q_PROPERTY(double channelSidebarWidth READ channelSidebarWidth WRITE setChannelSidebarWidth NOTIFY changed)
    Q_PROPERTY(double memberSidebarWidth READ memberSidebarWidth WRITE setMemberSidebarWidth NOTIFY changed)
    Q_PROPERTY(bool memberListVisible READ memberListVisible WRITE setMemberListVisible NOTIFY changed)
    Q_PROPERTY(double serverRailWidth READ serverRailWidth WRITE setServerRailWidth NOTIFY changed)
    Q_PROPERTY(double scrollMultiplier READ scrollMultiplier WRITE setScrollMultiplier NOTIFY changed)
    Q_PROPERTY(bool streamCropEnabled READ streamCropEnabled WRITE setStreamCropEnabled NOTIFY changed)
    Q_PROPERTY(double streamCropWidth READ streamCropWidth WRITE setStreamCropWidth NOTIFY changed)
    Q_PROPERTY(double streamCropHeight READ streamCropHeight WRITE setStreamCropHeight NOTIFY changed)
    Q_PROPERTY(bool notificationsEnabled READ notificationsEnabled WRITE setNotificationsEnabled NOTIFY changed)
    Q_PROPERTY(bool notifyDirectMessages READ notifyDirectMessages WRITE setNotifyDirectMessages NOTIFY changed)
    Q_PROPERTY(bool notifyMentions READ notifyMentions WRITE setNotifyMentions NOTIFY changed)
    Q_PROPERTY(bool notifyAllMessages READ notifyAllMessages WRITE setNotifyAllMessages NOTIFY changed)
    Q_PROPERTY(bool notificationSound READ notificationSound WRITE setNotificationSound NOTIFY changed)
    Q_PROPERTY(bool notificationPreviews READ notificationPreviews WRITE setNotificationPreviews NOTIFY changed)
    Q_PROPERTY(bool notifyWhenFocused READ notifyWhenFocused WRITE setNotifyWhenFocused NOTIFY changed)
    Q_PROPERTY(QString streamCropPreset READ streamCropPreset WRITE setStreamCropPreset NOTIFY changed)
    Q_PROPERTY(bool activitySoundsEnabled READ activitySoundsEnabled WRITE setActivitySoundsEnabled NOTIFY changed)
    Q_PROPERTY(bool voiceActivitySounds READ voiceActivitySounds WRITE setVoiceActivitySounds NOTIFY changed)
    Q_PROPERTY(bool streamActivitySounds READ streamActivitySounds WRITE setStreamActivitySounds NOTIFY changed)
    Q_PROPERTY(double soundboardVolume READ soundboardVolume WRITE setSoundboardVolume NOTIFY changed)
    Q_PROPERTY(double soundEffectsVolume READ soundEffectsVolume WRITE setSoundEffectsVolume NOTIFY changed)
    Q_PROPERTY(double chatFontSize READ chatFontSize WRITE setChatFontSize NOTIFY changed)
    Q_PROPERTY(bool underlineLinks READ underlineLinks WRITE setUnderlineLinks NOTIFY changed)
    Q_PROPERTY(double messageGroupGap READ messageGroupGap WRITE setMessageGroupGap NOTIFY changed)
    Q_PROPERTY(QString roleColorsMode READ roleColorsMode WRITE setRoleColorsMode NOTIFY changed)
    Q_PROPERTY(QString uiDensity READ uiDensity WRITE setUiDensity NOTIFY changed)
    Q_PROPERTY(bool highContrast READ highContrast WRITE setHighContrast NOTIFY changed)
    Q_PROPERTY(bool animateImages READ animateImages WRITE setAnimateImages NOTIFY changed)
    Q_PROPERTY(int notificationDuration READ notificationDuration WRITE setNotificationDuration NOTIFY changed)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY changed)
    Q_PROPERTY(QString timeFormat READ timeFormat WRITE setTimeFormat NOTIFY changed)
public:
    double chatFontSize() const; void setChatFontSize(double value);
    bool underlineLinks() const; void setUnderlineLinks(bool value);
    double messageGroupGap() const; void setMessageGroupGap(double value);
    QString roleColorsMode() const; void setRoleColorsMode(const QString &value);
    QString uiDensity() const; void setUiDensity(const QString &value);
    bool highContrast() const; void setHighContrast(bool value);
    bool animateImages() const; void setAnimateImages(bool value);
    int notificationDuration() const; void setNotificationDuration(int value);
    QString language() const; void setLanguage(const QString &value);
    QString timeFormat() const; void setTimeFormat(const QString &value);
    Q_INVOKABLE QString formatMessageTime(const QString &timestamp) const;
    Q_INVOKABLE QString formatDateTime(const QString &timestamp) const;
    Q_INVOKABLE bool soundEnabled(const QString &cue) const;
    Q_INVOKABLE void setSoundEnabled(const QString &cue, bool enabled);
    double soundboardVolume() const; void setSoundboardVolume(double value);
    double soundEffectsVolume() const; void setSoundEffectsVolume(double value);
    bool streamActivitySounds() const; void setStreamActivitySounds(bool value);
    bool voiceActivitySounds() const; void setVoiceActivitySounds(bool value);
    bool activitySoundsEnabled() const; void setActivitySoundsEnabled(bool value);
    double channelSidebarWidth() const; void setChannelSidebarWidth(double value);
    double memberSidebarWidth() const; void setMemberSidebarWidth(double value);
    bool memberListVisible() const; void setMemberListVisible(bool value);
    double serverRailWidth() const; void setServerRailWidth(double value);
    double scrollMultiplier() const; void setScrollMultiplier(double value);
    bool streamCropEnabled() const; void setStreamCropEnabled(bool value);
    double streamCropWidth() const; void setStreamCropWidth(double value);
    double streamCropHeight() const; void setStreamCropHeight(double value);
    bool notificationsEnabled() const; void setNotificationsEnabled(bool value);
    bool notifyDirectMessages() const; void setNotifyDirectMessages(bool value);
    bool notifyMentions() const; void setNotifyMentions(bool value);
    bool notifyAllMessages() const; void setNotifyAllMessages(bool value);
    bool notificationSound() const; void setNotificationSound(bool value);
    bool notificationPreviews() const; void setNotificationPreviews(bool value);
    bool notifyWhenFocused() const; void setNotifyWhenFocused(bool value);
    QString streamCropPreset() const; void setStreamCropPreset(const QString &value);
    Q_INVOKABLE bool setStreamCropRatio(double width, double height);
    explicit AppSettings(const QString &settingsFile = {}, QObject *parent = nullptr);
    bool hardwareAcceleration() const; bool graphicsRestartRequired() const;
    double fontScale() const; bool compactMessages() const; bool reducedMotion() const;
    QString inputDeviceId() const; QString outputDeviceId() const;
    void setInputDeviceId(const QString &); void setOutputDeviceId(const QString &);
    double inputVolume() const; double outputVolume() const;
    bool voiceLimiterEnabled() const; void setVoiceLimiterEnabled(bool);
    double voiceLimiterCeiling() const; void setVoiceLimiterCeiling(double);
    bool noiseSuppression() const; bool echoCancellation() const; bool automaticGainControl() const;
    double inputSensitivityDb() const;
    void setHardwareAcceleration(bool); void setFontScale(double); void setCompactMessages(bool); void setReducedMotion(bool);
    void setInputVolume(double); void setOutputVolume(double);
    void setNoiseSuppression(bool); void setEchoCancellation(bool); void setAutomaticGainControl(bool); void setInputSensitivityDb(double);
signals:
    void changed();
private:
    void put(const QString &key, const QVariant &value);
    double number(const QString &key, double fallback, double minimum, double maximum) const;
    std::unique_ptr<QSettings> m_settings;
    bool m_initialHardware = true;
};

#include "AppSettings.h"
#include <QDateTime>
#include <QLocale>
#include <algorithm>
#include <cmath>
namespace {
const QStringList &soundCues() {
    static const QStringList cues{"mute","unmute","deafen","undeafen","join","leave","user-join","user-leave","stream-start","stream-stop","watch-start","watch-stop","message"};
    return cues;
}
QString allowedSetting(const QSettings &settings, const QString &key, const QStringList &allowed, const QString &fallback) {
    const auto value = settings.value(key, fallback).toString(); return allowed.contains(value) ? value : fallback;
}
QDateTime parsedTime(const QString &value) {
    auto result = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!result.isValid()) result = QDateTime::fromString(value, Qt::ISODate);
    return result.toLocalTime();
}
QString formattedTime(const QDateTime &date, const QLocale &locale, const QString &format, bool seconds = false) {
    if (format == "12") return locale.toString(date.time(), seconds ? "h:mm:ss AP" : "h:mm AP");
    if (format == "24") return locale.toString(date.time(), seconds ? "HH:mm:ss" : "HH:mm");
    return locale.toString(date.time(), seconds ? QLocale::LongFormat : QLocale::ShortFormat);
}
}
AppSettings::AppSettings(const QString &path, QObject *parent) : QObject(parent) {
    m_settings = path.isEmpty() ? std::make_unique<QSettings>() : std::make_unique<QSettings>(path, QSettings::IniFormat);
    m_initialHardware = hardwareAcceleration();
}
void AppSettings::put(const QString &key, const QVariant &value) {
    if (m_settings->value(key) == value) return;
    m_settings->setValue(key, value); m_settings->sync(); emit changed();
}
double AppSettings::number(const QString &key, double fallback, double low, double high) const {
    bool ok = false; const double value = m_settings->value(key, fallback).toDouble(&ok);
    return ok && std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}
bool AppSettings::hardwareAcceleration() const { return m_settings->value("appearance/hardwareAcceleration", true).toBool(); }
bool AppSettings::graphicsRestartRequired() const { return hardwareAcceleration() != m_initialHardware; }
double AppSettings::fontScale() const { return number("appearance/fontScale", 1, .8, 1.4); }
bool AppSettings::compactMessages() const { return m_settings->value("appearance/compactMessages", false).toBool(); }
bool AppSettings::reducedMotion() const { return m_settings->value("appearance/reducedMotion", false).toBool(); }
double AppSettings::inputVolume() const { return number("audio/inputVolume", 1, 0, 2); }
double AppSettings::outputVolume() const { return number("audio/outputVolume", 1, 0, 2); }
bool AppSettings::voiceLimiterEnabled() const { return m_settings->value("audio/voiceLimiterEnabled", false).toBool(); }
void AppSettings::setVoiceLimiterEnabled(bool v) { put("audio/voiceLimiterEnabled", v); }
double AppSettings::voiceLimiterCeiling() const { return number("audio/voiceLimiterCeiling", .5, .1, 1); }
void AppSettings::setVoiceLimiterCeiling(double v) { if (std::isfinite(v)) put("audio/voiceLimiterCeiling", std::clamp(v, .1, 1.)); }
bool AppSettings::noiseSuppression() const { return m_settings->value("audio/noiseSuppression", false).toBool(); }
bool AppSettings::echoCancellation() const { return m_settings->value("audio/echoCancellation", false).toBool(); }
bool AppSettings::automaticGainControl() const { return m_settings->value("audio/automaticGainControl", false).toBool(); }
double AppSettings::inputSensitivityDb() const { return number("audio/inputSensitivityDb", -48, -70, -10); }
void AppSettings::setHardwareAcceleration(bool v) { put("appearance/hardwareAcceleration", v); }
void AppSettings::setFontScale(double v) { if(std::isfinite(v)) put("appearance/fontScale", std::clamp(v,.8,1.4)); }
void AppSettings::setCompactMessages(bool v) { put("appearance/compactMessages", v); }
void AppSettings::setReducedMotion(bool v) { put("appearance/reducedMotion", v); }
void AppSettings::setInputVolume(double v) { if(std::isfinite(v)) put("audio/inputVolume", std::clamp(v,0.,2.)); }
void AppSettings::setOutputVolume(double v) { if(std::isfinite(v)) put("audio/outputVolume", std::clamp(v,0.,2.)); }
void AppSettings::setNoiseSuppression(bool v) { put("audio/noiseSuppression", v); }
void AppSettings::setEchoCancellation(bool v) { put("audio/echoCancellation", v); }
void AppSettings::setAutomaticGainControl(bool v) { put("audio/automaticGainControl", v); }
void AppSettings::setInputSensitivityDb(double v) { if(std::isfinite(v)) put("audio/inputSensitivityDb", std::clamp(v,-70.,-10.)); }

QString AppSettings::inputDeviceId() const { return m_settings->value("audio/inputDeviceId").toString().left(2048); }
QString AppSettings::outputDeviceId() const { return m_settings->value("audio/outputDeviceId").toString().left(2048); }
void AppSettings::setInputDeviceId(const QString &id) { if(id.size()<=2048) put("audio/inputDeviceId",id); }
void AppSettings::setOutputDeviceId(const QString &id) { if(id.size()<=2048) put("audio/outputDeviceId",id); }

double AppSettings::channelSidebarWidth() const { return number("layout/channelSidebarWidth", 240, 180, 520); }
void AppSettings::setChannelSidebarWidth(double value) { if (std::isfinite(value)) put("layout/channelSidebarWidth", std::clamp(value, double(180), double(520))); }

double AppSettings::memberSidebarWidth() const { return number("layout/memberSidebarWidth", 240, 180, 480); }
void AppSettings::setMemberSidebarWidth(double value) { if (std::isfinite(value)) put("layout/memberSidebarWidth", std::clamp(value, double(180), double(480))); }

bool AppSettings::memberListVisible() const { return m_settings->value("layout/memberListVisible", true).toBool(); }
void AppSettings::setMemberListVisible(bool value) { put("layout/memberListVisible", value); }

double AppSettings::serverRailWidth() const { return number("layout/serverRailWidth", 72, 56, 104); }
void AppSettings::setServerRailWidth(double value) { if (std::isfinite(value)) put("layout/serverRailWidth", std::clamp(value, double(56), double(104))); }

double AppSettings::scrollMultiplier() const { return number("appearance/scrollMultiplier", 1, 0.5, 16); }
void AppSettings::setScrollMultiplier(double value) { if (std::isfinite(value)) put("appearance/scrollMultiplier", std::clamp(value, double(0.5), double(16))); }

bool AppSettings::streamCropEnabled() const { return m_settings->value("stream/cropEnabled", false).toBool(); }
void AppSettings::setStreamCropEnabled(bool value) { put("stream/cropEnabled", value); }

double AppSettings::streamCropWidth() const { return number("stream/cropWidth", 16, 0.1, 10000); }
void AppSettings::setStreamCropWidth(double value) { if (std::isfinite(value)) put("stream/cropWidth", std::clamp(value, double(0.1), double(10000))); }

double AppSettings::streamCropHeight() const { return number("stream/cropHeight", 9, 0.1, 10000); }
void AppSettings::setStreamCropHeight(double value) { if (std::isfinite(value)) put("stream/cropHeight", std::clamp(value, double(0.1), double(10000))); }

bool AppSettings::notificationsEnabled() const { return m_settings->value("notifications/enabled", true).toBool(); }
void AppSettings::setNotificationsEnabled(bool value) { put("notifications/enabled", value); }

bool AppSettings::notifyDirectMessages() const { return m_settings->value("notifications/directMessages", true).toBool(); }
void AppSettings::setNotifyDirectMessages(bool value) { put("notifications/directMessages", value); }

bool AppSettings::notifyMentions() const { return m_settings->value("notifications/mentions", true).toBool(); }
void AppSettings::setNotifyMentions(bool value) { put("notifications/mentions", value); }

bool AppSettings::notifyAllMessages() const { return m_settings->value("notifications/allMessages", false).toBool(); }
void AppSettings::setNotifyAllMessages(bool value) { put("notifications/allMessages", value); }

bool AppSettings::notificationSound() const { return m_settings->value("notifications/sound", true).toBool(); }
void AppSettings::setNotificationSound(bool value) { put("notifications/sound", value); }

bool AppSettings::notificationPreviews() const { return m_settings->value("notifications/previews", true).toBool(); }
void AppSettings::setNotificationPreviews(bool value) { put("notifications/previews", value); }

bool AppSettings::notifyWhenFocused() const { return m_settings->value("notifications/whenFocused", false).toBool(); }
void AppSettings::setNotifyWhenFocused(bool value) { put("notifications/whenFocused", value); }

QString AppSettings::streamCropPreset() const {
    const QString value = m_settings->value("stream/cropPreset", "16:9").toString();
    const QStringList allowed{"16:9", "21:9", "32:9", "16:10", "4:3", "1:1", "custom"};
    return allowed.contains(value) ? value : QStringLiteral("16:9");
}
void AppSettings::setStreamCropPreset(const QString &value) {
    const QStringList allowed{"16:9", "21:9", "32:9", "16:10", "4:3", "1:1", "custom"};
    if (allowed.contains(value)) put("stream/cropPreset", value);
}
bool AppSettings::setStreamCropRatio(double width, double height) {
    if (!std::isfinite(width) || !std::isfinite(height) || width < .1 || height < .1 || width > 10000 || height > 10000 || width / height < .1 || width / height > 10) return false;
    if (streamCropWidth() == width && streamCropHeight() == height) return true;
    m_settings->setValue("stream/cropWidth", width);
    m_settings->setValue("stream/cropHeight", height);
    m_settings->sync(); emit changed(); return true;
}

bool AppSettings::activitySoundsEnabled() const { return m_settings->value("sounds/activitySoundsEnabled", true).toBool(); }
void AppSettings::setActivitySoundsEnabled(bool value) { put("sounds/activitySoundsEnabled", value); }

bool AppSettings::voiceActivitySounds() const { return m_settings->value("sounds/voiceActivitySounds", true).toBool(); }
void AppSettings::setVoiceActivitySounds(bool value) { put("sounds/voiceActivitySounds", value); }

bool AppSettings::streamActivitySounds() const { return m_settings->value("sounds/streamActivitySounds", true).toBool(); }
void AppSettings::setStreamActivitySounds(bool value) { put("sounds/streamActivitySounds", value); }

double AppSettings::soundEffectsVolume() const { return number("sounds/volume", .5, 0, 1); }
void AppSettings::setSoundEffectsVolume(double value) { if(std::isfinite(value)) put("sounds/volume", std::clamp(value, 0., 1.)); }

double AppSettings::soundboardVolume() const {return number("sounds/soundboardVolume",.5,0,1);}
void AppSettings::setSoundboardVolume(double value){if(std::isfinite(value))put("sounds/soundboardVolume",std::clamp(value,0.,1.));}

double AppSettings::chatFontSize() const { return number("accessibility/chatFontSize", 14, 12, 24); }
void AppSettings::setChatFontSize(double value) { if (std::isfinite(value)) put("accessibility/chatFontSize", std::clamp(value, 12., 24.)); }
bool AppSettings::underlineLinks() const { return m_settings->value("accessibility/underlineLinks", true).toBool(); }
void AppSettings::setUnderlineLinks(bool value) { put("accessibility/underlineLinks", value); }
double AppSettings::messageGroupGap() const { return number("accessibility/messageGroupGap", 16, 0, 24); }
void AppSettings::setMessageGroupGap(double value) { if (std::isfinite(value)) put("accessibility/messageGroupGap", std::clamp(value, 0., 24.)); }
QString AppSettings::roleColorsMode() const { return allowedSetting(*m_settings, "accessibility/roleColorsMode", {"names", "dots", "none"}, "names"); }
void AppSettings::setRoleColorsMode(const QString &value) { if (QStringList{"names", "dots", "none"}.contains(value)) put("accessibility/roleColorsMode", value); }
QString AppSettings::uiDensity() const { return allowedSetting(*m_settings, "accessibility/uiDensity", {"compact", "default", "spacious"}, "default"); }
void AppSettings::setUiDensity(const QString &value) { if (QStringList{"compact", "default", "spacious"}.contains(value)) put("accessibility/uiDensity", value); }
bool AppSettings::highContrast() const { return m_settings->value("accessibility/highContrast", false).toBool(); }
void AppSettings::setHighContrast(bool value) { put("accessibility/highContrast", value); }
bool AppSettings::animateImages() const { return m_settings->value("accessibility/animateImages", true).toBool(); }
void AppSettings::setAnimateImages(bool value) { put("accessibility/animateImages", value); }
int AppSettings::notificationDuration() const { return int(number("accessibility/notificationDuration", 5, 3, 20)); }
void AppSettings::setNotificationDuration(int value) { put("accessibility/notificationDuration", std::clamp(value, 3, 20)); }
QString AppSettings::language() const { return allowedSetting(*m_settings, "language/locale", {"en_US", "en_GB"}, "en_US"); }
void AppSettings::setLanguage(const QString &value) { if (QStringList{"en_US", "en_GB"}.contains(value)) put("language/locale", value); }
QString AppSettings::timeFormat() const { return allowedSetting(*m_settings, "language/timeFormat", {"auto", "12", "24"}, "auto"); }
void AppSettings::setTimeFormat(const QString &value) { if (QStringList{"auto", "12", "24"}.contains(value)) put("language/timeFormat", value); }
QString AppSettings::formatMessageTime(const QString &timestamp) const {
    const auto date = parsedTime(timestamp); if (!date.isValid()) return {};
    const QLocale locale(language()); const auto today = QDate::currentDate();
    const auto day = date.date() == today ? QStringLiteral("Today") : date.date() == today.addDays(-1) ? QStringLiteral("Yesterday")
        : locale.toString(date.date(), language() == "en_GB" ? "d MMM yyyy" : "MMM d, yyyy");
    return day + " at " + formattedTime(date, locale, timeFormat());
}
QString AppSettings::formatDateTime(const QString &timestamp) const {
    const auto date = parsedTime(timestamp); if (!date.isValid()) return {};
    const QLocale locale(language()); return locale.toString(date.date(), QLocale::LongFormat) + " at " + formattedTime(date, locale, timeFormat(), true);
}
bool AppSettings::soundEnabled(const QString &cue) const { return soundCues().contains(cue) && m_settings->value("sounds/cues/" + cue, true).toBool(); }
void AppSettings::setSoundEnabled(const QString &cue, bool enabled) { if (soundCues().contains(cue)) put("sounds/cues/" + cue, enabled); }

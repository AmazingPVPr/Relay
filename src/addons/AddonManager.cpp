#include "AddonManager.h"
#include "AddonInterface.h"
#include "WindowAddonInterface.h"
#include "BroadcastSourceInterface.h"
#include "ShareSourceInterface.h"
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonObject>
#include <QJsonArray>
#include <QLibrary>
#include <QPluginLoader>
#include <QPointer>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QSettings>
#include <QThread>
#include <map>

struct AddonManager::Private {
    struct Addon {
        QString id, name, description, version, path, status;
        bool enabled = false, available = true, bundled = false, voiceAudio = false, broadcastSource = false, shareSource = false;
        std::unique_ptr<QPluginLoader> loader;
        RelayAddonInterface *plugin = nullptr;
        QPointer<QObject> service;
        QUrl settingsUrl;
    };
    AddonManager *q;
    QStringList bundledDirectories;
    QString localDirectory, active;
    std::unique_ptr<QSettings> settings;
    std::map<QString, std::unique_ptr<Addon>> entries;
    bool shuttingDown = false;
    explicit Private(AddonManager *owner) : q(owner) {}
    Addon *get(const QString &id) const {
        auto it = entries.find(id);
        return it == entries.end() ? nullptr : it->second.get();
    }
    void retire(Addon &addon, bool deferred) {
        if (active == addon.id) { active.clear(); emit q->activeChanged(); }
        if (!addon.service) return;
        QObject *object = addon.service;
        addon.service.clear();
        addon.plugin->stopService(object);
        if (deferred) object->deleteLater(); else delete object;
    }
    void discover(const QString &directory, bool bundled) {
        if (directory.isEmpty()) return;
        const QDir dir(directory);
        for (const auto &info : dir.entryInfoList(QDir::Files | QDir::Readable, QDir::Name)) {
            if (!QLibrary::isLibrary(info.fileName())) continue;
            const QString path = info.canonicalFilePath();
            if (path.isEmpty()) continue;
            auto loader = std::make_unique<QPluginLoader>(path);
            loader->setLoadHints(QLibrary::PreventUnloadHint);
            // Reading Qt's metadata does not load or execute the native module.
            const auto root = loader->metaData();
            if (root.value("IID").toString() != QLatin1String(RelayAddonInterface_iid)) continue;
            const auto metadata = root.value("MetaData").toObject();
            const auto id = metadata.value("id").toString();
            static const QRegularExpression validId("^[a-z][a-z0-9]*(?:[.-][a-z0-9]+)+$");
            if (id.size() > 100 || !validId.match(id).hasMatch() || metadata.value("apiVersion").toInt() != 1 || entries.contains(id)) continue;
            const auto name = metadata.value("name").toString();
            if (name.isEmpty() || name.size() > 100) continue;
            auto addon = std::make_unique<Addon>();
            addon->id = id; addon->name = name;
            addon->description = metadata.value("description").toString().left(1000);
            addon->version = metadata.value("version").toString().left(50);
            addon->path = path; addon->bundled = bundled;
            addon->voiceAudio=metadata.value("capabilities").toArray().contains("voice-pcm");
            addon->broadcastSource=metadata.value("capabilities").toArray().contains("broadcast-source");
            addon->shareSource=metadata.value("capabilities").toArray().contains("share-source");
            addon->enabled = settings->value("addons/" + id + "/enabled", bundled && metadata.value("defaultEnabled").toBool()).toBool();
            addon->status = addon->enabled ? "Ready to open" : "Disabled";
            addon->loader = std::move(loader);
            entries.emplace(id, std::move(addon));
        }
    }
};

AddonManager::AddonManager(const QStringList &bundledDirectories, const QString &localDirectory,
                           const QString &settingsFile, QObject *parent)
    : QObject(parent), d(std::make_unique<Private>(this)) {
    d->bundledDirectories = bundledDirectories;
    d->localDirectory = localDirectory.isEmpty() ? QString() : QFileInfo(localDirectory).absoluteFilePath();
    d->settings = settingsFile.isEmpty() ? std::make_unique<QSettings>()
        : std::make_unique<QSettings>(settingsFile, QSettings::IniFormat);
    rescan();
}
AddonManager::~AddonManager() { shutdown(); }
QVariantList AddonManager::items() const {
    QVariantList result;
    for (const auto &[_, addon] : d->entries) {
        result.append(QVariantMap{{"id", addon->id}, {"name", addon->name}, {"description", addon->description},
            {"version", addon->version}, {"enabled", addon->enabled}, {"available", addon->available},
            {"status", addon->status}, {"bundled", addon->bundled}});
    }
    return result;
}
QVariantList AddonManager::broadcastSources() const {
    QVariantList result;
    for (const auto &[_, addon] : d->entries) {
        // Discovery reads metadata only. Selecting a source creates its service.
        if (!addon->enabled || !addon->broadcastSource || !addon->shareSource) continue;
        result.append(QVariantMap{{"id", addon->id}, {"name", addon->name},
            {"available", addon->available}, {"status", addon->status}});
    }
    return result;
}
QVariantMap AddonManager::openBroadcastSource(const QString &id) {
    const auto a = d->get(id);
    if (!a || !a->enabled || !a->broadcastSource || !a->shareSource) return {};
    auto *object = service(id);
    if (!object) return {{"error", a->status}};
    const auto contribution = qobject_cast<RelayShareSourceInterface *>(a->loader->instance());
    const auto url = contribution ? contribution->shareSourceUrl() : QUrl{};
    const QString prefix = "/relay/addons/" + id + "/";
    if (!qobject_cast<RelayBroadcastSourceInterface *>(object) || url.scheme() != "qrc"
        || !url.host().isEmpty() || !url.path().startsWith(prefix) || url.path().contains("..")
        || url.hasQuery() || url.hasFragment() || !QFile::exists(":" + url.path()))
        return {{"error", "The add-on does not provide a valid share-source editor."}};
    return {{"id", id}, {"name", a->name}, {"service", QVariant::fromValue(object)}, {"editorUrl", url}};
}
QString AddonManager::activeId() const { return d->active; }
QString AddonManager::activeTitle() const { const auto a = d->get(d->active); return a ? a->name : QString(); }
QUrl AddonManager::activeSettingsUrl() const { const auto a = d->get(d->active); return a ? a->settingsUrl : QUrl(); }
QObject *AddonManager::activeService() const { const auto a = d->get(d->active); return a ? a->service.data() : nullptr; }
QString AddonManager::localDirectory() const { return d->localDirectory; }
bool AddonManager::enabled(const QString &id) const { const auto a = d->get(id); return a && a->enabled; }
bool AddonManager::voiceAudioAllowed(const QString &id) const {const auto a=d->get(id);return a && a->enabled && a->voiceAudio;}
bool AddonManager::broadcastSourceAllowed(const QString &id) const {const auto a=d->get(id);return a && a->enabled && a->broadcastSource;}
bool AddonManager::setEnabled(const QString &id, bool enabled) {
    auto a = d->get(id);
    if (!a || d->shuttingDown) return false;
    if (a->enabled == enabled) return true;
    if (!enabled) d->retire(*a, true);
    a->enabled = enabled;
    a->status = enabled ? "Ready to open" : "Disabled";
    // A failed dependency load can be retried after a deliberate off/on toggle.
    if (enabled) a->available = true;
    d->settings->setValue("addons/" + id + "/enabled", enabled);
    d->settings->sync();
    emit itemsChanged();
    return true;
}
QObject *AddonManager::service(const QString &id) {
    auto a = d->get(id);
    if (!a || !a->enabled || !a->available || d->shuttingDown) return nullptr;
    if (a->service) return a->service;
    auto fail = [&](const QString &reason) -> QObject * {
        a->available = false; a->status = reason; emit itemsChanged(); return nullptr;
    };
    QObject *instance = a->loader->instance();
    if (!instance) return fail("Could not load this add-on: " + a->loader->errorString());
    a->plugin = qobject_cast<RelayAddonInterface *>(instance);
    if (!a->plugin || a->plugin->id() != id) return fail("The add-on does not match its native interface metadata.");
    const auto url = a->plugin->settingsUrl();
    const QString prefix = "/relay/addons/" + id + "/";
    const bool windowOnly = url.isEmpty() && qobject_cast<RelayWindowAddonInterface *>(instance)
        && a->loader->metaData().value("MetaData").toObject().value("capabilities").toArray().contains("native-window");
    if (!windowOnly && (url.scheme() != "qrc" || !url.host().isEmpty() || !url.path().startsWith(prefix) || url.path().contains("..") || url.hasQuery() || url.hasFragment()))
        return fail("The add-on settings must be bundled in its own Qt resource.");
    if (!windowOnly && !QFile::exists(":" + url.path())) return fail("The add-on settings resource is missing.");
    auto object = a->plugin->createService(this);
    if (!object || object == instance || object == this) return fail("The add-on could not create its service.");
    if (object->thread() != thread()) { object->deleteLater(); return fail("The add-on service must belong to the application thread."); }
    object->setParent(this);
    QQmlEngine::setObjectOwnership(object, QQmlEngine::CppOwnership);
    a->service = object; a->settingsUrl = url; a->status = "Enabled";
    emit itemsChanged();
    emit serviceCreated(id,object);
    return object;
}
bool AddonManager::openSettings(const QString &id) {
    auto *object = service(id);
    if (!object) return false;
    auto *addon = d->get(id);
    if (addon->loader->metaData().value("MetaData").toObject().value("capabilities").toArray().contains("native-window")) {
        auto *windows = qobject_cast<RelayWindowAddonInterface *>(addon->loader->instance());
        return windows && windows->openWindow(object);
    }
    d->active = id; emit activeChanged(); return true;
}
void AddonManager::closeSettings() {
    if (d->active.isEmpty()) return;
    d->active.clear(); emit activeChanged();
}
void AddonManager::rescan() {
    if (d->shuttingDown) return;
    for (const auto &directory : d->bundledDirectories) d->discover(directory, true);
    d->discover(d->localDirectory, false);
    emit itemsChanged();
}
bool AddonManager::openLocalDirectory() {
    if (d->localDirectory.isEmpty() || !QDir().mkpath(d->localDirectory)) return false;
    return QDesktopServices::openUrl(QUrl::fromLocalFile(d->localDirectory));
}
void AddonManager::shutdown() {
    if (d->shuttingDown) return;
    d->shuttingDown = true;
    for (auto &[_, addon] : d->entries) d->retire(*addon, false);
    // PreventUnloadHint keeps QML component code/resources resident until exit.
    // Inactive QObject services are destroyed; no capture continues after disable.
}

QVariantMap AddonManager::identityAuthorization(const QString &id) const {
    auto addon = d->get(id);
    if (!addon || !addon->enabled || !addon->available) return {};
    const auto metadata = addon->loader->metaData().value("MetaData").toObject();
    if (!metadata.value("capabilities").toArray().contains("discord-identity")) return {};
    return metadata.value("identityAuthorization").toObject().toVariantMap();
}

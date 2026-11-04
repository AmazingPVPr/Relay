#include "GlobalShortcuts.h"

#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QKeySequence>
#include <QRegularExpression>
#include <QUuid>

// Primary protocol: https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.GlobalShortcuts.html
// Trigger grammar: https://specifications.freedesktop.org/shortcuts/latest/
namespace {
const QString DesktopPath = "/org/freedesktop/portal/desktop";
const QString GlobalInterface = "org.freedesktop.portal.GlobalShortcuts";
const QString RequestInterface = "org.freedesktop.portal.Request";
const QString SessionInterface = "org.freedesktop.portal.Session";
const QMap<QString, QString> Labels{{"toggle-mute", "Toggle Relay microphone"},
    {"toggle-deafen", "Toggle Relay deafen"}, {"disconnect-voice", "Disconnect Relay voice"}};
QString randomToken() { return "relay_" + QUuid::createUuid().toString(QUuid::Id128); }
PortalShortcutList decodeShortcuts(const QVariant &value) {
    if (value.metaType() == QMetaType::fromType<QDBusArgument>())
        return qdbus_cast<PortalShortcutList>(value.value<QDBusArgument>());
    return value.value<PortalShortcutList>();
}
}

QDBusArgument &operator<<(QDBusArgument &argument, const PortalShortcut &shortcut) {
    argument.beginStructure();
    argument << shortcut.id << shortcut.properties;
    argument.endStructure();
    return argument;
}
const QDBusArgument &operator>>(const QDBusArgument &argument, PortalShortcut &shortcut) {
    argument.beginStructure();
    argument >> shortcut.id >> shortcut.properties;
    argument.endStructure();
    return argument;
}

GlobalShortcuts::GlobalShortcuts(QObject *parent)
    : GlobalShortcuts(QDBusConnection::sessionBus(), "org.freedesktop.portal.Desktop", parent) {}

GlobalShortcuts::GlobalShortcuts(const QDBusConnection &bus, const QString &service, QObject *parent)
    : QObject(parent), m_bus(bus), m_service(service),
      m_serviceWatcher(service, bus, QDBusServiceWatcher::WatchForUnregistration, this) {
    qDBusRegisterMetaType<PortalShortcut>();
    qDBusRegisterMetaType<PortalShortcutList>();
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] { fail("The desktop shortcut request timed out. You can try again."); });
    connect(&m_serviceWatcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] {
        if (m_active || m_busy) fail("The desktop shortcut service disconnected. Enable system shortcuts again.");
    });
    m_bus.connect(m_service, DesktopPath, GlobalInterface, "Activated", this,
        SLOT(onActivated(QDBusObjectPath,QString,qulonglong,QVariantMap)));
    m_bus.connect(m_service, DesktopPath, GlobalInterface, "Deactivated", this,
        SLOT(onDeactivated(QDBusObjectPath,QString,qulonglong,QVariantMap)));
    m_bus.connect(m_service, DesktopPath, GlobalInterface, "ShortcutsChanged", this,
        SLOT(onShortcutsChanged(QDBusObjectPath,PortalShortcutList)));
}

GlobalShortcuts::~GlobalShortcuts() { release(true); }

QString GlobalShortcuts::senderPart() const {
    auto sender = m_bus.baseService();
    if (sender.startsWith(':')) sender.remove(0, 1);
    return sender.replace('.', '_');
}

bool GlobalShortcuts::validPortalPath(const QString &path, const QString &kind) const {
    const auto prefix = DesktopPath + "/" + kind + "/" + senderPart() + "/";
    static const QRegularExpression suffix("^[A-Za-z0-9_]+$");
    return path.startsWith(prefix) && suffix.match(path.mid(prefix.size())).hasMatch();
}

QString GlobalShortcuts::preferredTrigger(const QString &raw) {
    if (raw.size() > 80) return {};
    const auto sequence = QKeySequence::fromString(raw, QKeySequence::PortableText);
    if (sequence.count() != 1) return {};
    const auto combination = sequence[0];
    const auto key = combination.key();
    QString name;
    if (key >= Qt::Key_A && key <= Qt::Key_Z) name = QChar('a' + key - Qt::Key_A);
    else if (key >= Qt::Key_0 && key <= Qt::Key_9) name = QChar('0' + key - Qt::Key_0);
    else if (key >= Qt::Key_F1 && key <= Qt::Key_F35) name = "F" + QString::number(key - Qt::Key_F1 + 1);
    else {
        static const QMap<Qt::Key, QString> names{{Qt::Key_Return, "Return"}, {Qt::Key_Enter, "KP_Enter"},
            {Qt::Key_Tab, "Tab"}, {Qt::Key_Backtab, "ISO_Left_Tab"}, {Qt::Key_Escape, "Escape"},
            {Qt::Key_Space, "space"}, {Qt::Key_Backspace, "BackSpace"}, {Qt::Key_Delete, "Delete"},
            {Qt::Key_Insert, "Insert"}, {Qt::Key_Home, "Home"}, {Qt::Key_End, "End"},
            {Qt::Key_Left, "Left"}, {Qt::Key_Right, "Right"}, {Qt::Key_Up, "Up"}, {Qt::Key_Down, "Down"},
            {Qt::Key_PageUp, "Prior"}, {Qt::Key_PageDown, "Next"}, {Qt::Key_Minus, "minus"},
            {Qt::Key_Plus, "plus"}, {Qt::Key_Equal, "equal"}, {Qt::Key_Comma, "comma"},
            {Qt::Key_Period, "period"}, {Qt::Key_Slash, "slash"}, {Qt::Key_Backslash, "backslash"},
            {Qt::Key_BracketLeft, "bracketleft"}, {Qt::Key_BracketRight, "bracketright"},
            {Qt::Key_Semicolon, "semicolon"}, {Qt::Key_Apostrophe, "apostrophe"}, {Qt::Key_QuoteLeft, "grave"}};
        name = names.value(key);
    }
    if (name.isEmpty()) return {}; // Let the desktop choose unsupported key names.
    QStringList parts;
    const auto modifiers = combination.keyboardModifiers();
    if (modifiers & Qt::ControlModifier) parts.append("CTRL");
    if (modifiers & Qt::AltModifier) parts.append("ALT");
    if (modifiers & Qt::ShiftModifier) parts.append("SHIFT");
    if (modifiers & Qt::MetaModifier) parts.append("LOGO");
    if (modifiers & Qt::KeypadModifier) parts.append("NUM");
    parts.append(name);
    return parts.join('+');
}

void GlobalShortcuts::enable(const QVariantList &bindings) {
    release(true);
    m_error.clear();
    if (!m_bus.isConnected()) { fail("The desktop session bus is unavailable. App shortcuts still work inside Relay."); return; }
    QSet<QString> seen;
    for (const auto &value : bindings) {
        const auto binding = value.toMap();
        const auto action = binding.value("action").toString();
        const auto sequence = binding.value("sequence").toString();
        if (!Labels.contains(action) || seen.contains(action) || sequence.trimmed().isEmpty()) continue;
        seen.insert(action);
        QVariantMap properties{{"description", Labels.value(action)}};
        const auto preferred = preferredTrigger(sequence);
        if (!preferred.isEmpty()) properties.insert("preferred_trigger", preferred);
        m_requested.append({action, properties});
        if (m_requested.size() == 3) break;
    }
    if (m_requested.isEmpty()) { fail("Choose at least one voice shortcut before enabling system shortcuts."); return; }
    m_busy = true;
    m_status = "Connecting to desktop shortcuts…";
    const auto sessionToken = randomToken();
    m_expectedSession = DesktopPath + "/session/" + senderPart() + "/" + sessionToken;
    beginRequest("CreateSession", {}, {{"session_handle_token", sessionToken}}, Creating);
    emit changed();
}

void GlobalShortcuts::beginRequest(const QString &method, QVariantList arguments, QVariantMap options, Stage stage) {
    detachRequest();
    m_stage = stage;
    m_requestToken = randomToken();
    m_requestPath = DesktopPath + "/request/" + senderPart() + "/" + m_requestToken;
    options.insert("handle_token", m_requestToken);
    if (!m_bus.connect(m_service, m_requestPath, RequestInterface, "Response", this,
        SLOT(onResponse(uint,QVariantMap,QDBusMessage)))) {
        fail("Relay could not listen for the desktop shortcut response.");
        return;
    }
    arguments.append(options);
    auto call = QDBusMessage::createMethodCall(m_service, DesktopPath, GlobalInterface, method);
    call.setArguments(arguments);
    const auto generation = m_generation;
    const auto token = m_requestToken;
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call, 10000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, generation, token] {
        const QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (generation != m_generation || token != m_requestToken) return;
        if (reply.isError()) {
            fail("Your desktop could not configure system shortcuts. You can also bind Relay commands in desktop keyboard settings.");
            return;
        }
        const auto returned = reply.value().path();
        if (!validPortalPath(returned, "request")) { fail("The desktop returned an invalid shortcut request."); return; }
        if (returned != m_requestPath) {
            detachRequest();
            m_requestPath = returned;
            m_requestToken = token;
            if (!m_bus.connect(m_service, returned, RequestInterface, "Response", this,
                SLOT(onResponse(uint,QVariantMap,QDBusMessage)))) fail("Relay could not listen for the desktop shortcut response.");
        }
    });
    m_deadline.start(stage == Creating ? 10000 : 120000);
}

void GlobalShortcuts::detachRequest() {
    if (!m_requestPath.isEmpty())
        m_bus.disconnect(m_service, m_requestPath, RequestInterface, "Response", this,
            SLOT(onResponse(uint,QVariantMap,QDBusMessage)));
    m_requestPath.clear();
    m_requestToken.clear();
}

void GlobalShortcuts::closeObject(const QString &path, const QString &interface) {
    if (path.isEmpty() || !m_bus.isConnected()) return;
    m_bus.asyncCall(QDBusMessage::createMethodCall(m_service, path, interface, "Close"), 1000);
}

void GlobalShortcuts::release(bool closeSession) {
    ++m_generation;
    m_deadline.stop();
    closeObject(m_requestPath, RequestInterface);
    detachRequest();
    if (closeSession) closeObject(m_sessionPath.isEmpty() ? m_expectedSession : m_sessionPath, SessionInterface);
    if (!m_sessionPath.isEmpty())
        m_bus.disconnect(m_service, m_sessionPath, SessionInterface, "Closed", this,
            SLOT(onSessionClosed(QVariantMap,QDBusMessage)));
    m_sessionPath.clear();
    m_expectedSession.clear();
    m_requested.clear();
    m_boundIds.clear();
    m_pressed.clear();
    m_bindings.clear();
    m_stage = Idle;
    m_active = m_busy = false;
}

void GlobalShortcuts::disable() {
    release(true);
    m_error.clear();
    m_status = "System shortcuts are off.";
    emit changed();
}

void GlobalShortcuts::fail(const QString &error) {
    release(true);
    m_error = error;
    m_status = "System shortcuts are off.";
    emit changed();
}

void GlobalShortcuts::updateBindings(const PortalShortcutList &shortcuts) {
    m_boundIds.clear();
    m_bindings.clear();
    for (const auto &shortcut : shortcuts) {
        bool requested = false;
        for (const auto &candidate : m_requested) if (candidate.id == shortcut.id) { requested = true; break; }
        if (!requested || m_boundIds.contains(shortcut.id)) continue;
        m_boundIds.insert(shortcut.id);
        m_bindings.append(QVariantMap{{"action", shortcut.id}, {"label", Labels.value(shortcut.id)},
            {"trigger", shortcut.properties.value("trigger_description").toString().left(120)}});
        if (m_boundIds.size() == 3) break;
    }
    m_pressed.clear();
    m_active = !m_boundIds.isEmpty();
    m_status = m_active ? "System shortcuts are on." : "No system shortcuts were assigned.";
}

void GlobalShortcuts::onResponse(uint response, const QVariantMap &results, const QDBusMessage &message) {
    if (!m_busy || message.path() != m_requestPath) return;
    const auto stage = m_stage;
    detachRequest();
    m_deadline.stop();
    if (response != 0) {
        release(true);
        m_error = response == 1 ? QString{} : "The desktop could not enable system shortcuts.";
        m_status = response == 1 ? "System shortcut setup was cancelled." : "System shortcuts are off.";
        emit changed();
        return;
    }
    if (stage == Creating) {
        m_sessionPath = results.value("session_handle").toString();
        if (!validPortalPath(m_sessionPath, "session")) { m_sessionPath.clear(); fail("The desktop returned an invalid shortcut session."); return; }
        m_bus.connect(m_service, m_sessionPath, SessionInterface, "Closed", this,
            SLOT(onSessionClosed(QVariantMap,QDBusMessage)));
        m_status = "Choose your shortcuts in the desktop permission window…";
        beginRequest("BindShortcuts", {QVariant::fromValue(QDBusObjectPath(m_sessionPath)),
            QVariant::fromValue(m_requested), QString{}}, {}, Binding);
    } else if (stage == Binding) {
        updateBindings(decodeShortcuts(results.value("shortcuts")));
        m_busy = false;
        m_stage = Idle;
    }
    emit changed();
}

void GlobalShortcuts::onActivated(const QDBusObjectPath &session, const QString &id,
                                qulonglong timestamp, const QVariantMap &options) {
    Q_UNUSED(timestamp); Q_UNUSED(options);
    if (!m_active || session.path() != m_sessionPath || !m_boundIds.contains(id) || m_pressed.contains(id)) return;
    m_pressed.insert(id);
    emit actionRequested(id);
}

void GlobalShortcuts::onDeactivated(const QDBusObjectPath &session, const QString &id,
                                  qulonglong timestamp, const QVariantMap &options) {
    Q_UNUSED(timestamp); Q_UNUSED(options);
    if (session.path() == m_sessionPath) m_pressed.remove(id);
}

void GlobalShortcuts::onShortcutsChanged(const QDBusObjectPath &session, const PortalShortcutList &shortcuts) {
    if (session.path() != m_sessionPath || m_busy) return;
    updateBindings(shortcuts);
    emit changed();
}

void GlobalShortcuts::onSessionClosed(const QVariantMap &details, const QDBusMessage &message) {
    Q_UNUSED(details);
    if (message.path() != m_sessionPath) return;
    release(false);
    m_status = "The desktop closed this shortcut session.";
    emit changed();
}

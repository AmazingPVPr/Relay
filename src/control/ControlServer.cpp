#include "ControlServer.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QTimer>
#include <cmath>
#include <initializer_list>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#ifdef Q_OS_UNIX
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
constexpr int MaximumRequest = 1024;
constexpr int MaximumResponse = 8192;
constexpr int RequestDeadlineMs = 1500;
QVariantMap failure(const QString &error) { return {{"ok", false}, {"error", error}}; }
// Numeric diagnostics never accept a convertible string, an identifier, or
// caller-defined map. Iterate the fixed schema rather than the supplied keys.
QVariantMap counters(const QVariantMap &source, std::initializer_list<const char *> booleanKeys,
                     std::initializer_list<const char *> numberKeys) {
    QVariantMap result;
    for (const auto *key : booleanKeys) {
        const auto value = source.value(QLatin1String(key));
        if (value.metaType().id() == QMetaType::Bool) result.insert(QLatin1String(key), value);
    }
    for (const auto *key : numberKeys) {
        const auto value = source.value(QLatin1String(key));
        const auto type = value.metaType().id();
        if (type != QMetaType::Int && type != QMetaType::UInt && type != QMetaType::LongLong &&
            type != QMetaType::ULongLong && type != QMetaType::Float && type != QMetaType::Double) continue;
        const auto number = value.toDouble();
        if (std::isfinite(number) && std::abs(number) <= 9007199254740991.)
            result.insert(QLatin1String(key), value);
    }
    return result;
}
QVariantMap mapValue(const QVariantMap &source, const char *key) {
    const auto value = source.value(QLatin1String(key));
    return value.metaType().id() == QMetaType::QVariantMap ? value.toMap() : QVariantMap{};
}

bool privateDirectory(const QString &path) {
    const QFileInfo info(path);
    if (!info.isDir() || info.isSymLink()) return false;
#ifdef Q_OS_UNIX
    if (info.ownerId() != uint(geteuid())) return false;
#endif
#ifdef Q_OS_WIN
    // Unix-style bits do not represent Windows ACLs. The named pipe uses
    // QLocalServer::UserAccessOption to restrict access to this user.
    return true;
#else
    const auto unsafe = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
                        QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
    return !(info.permissions() & unsafe);
#endif
}
}

ControlServer::ControlServer(QObject *parent) : QObject(parent), m_server(this) {
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    m_server.setMaxPendingConnections(8);
    connect(&m_server, &QLocalServer::newConnection, this, &ControlServer::acceptConnections);
}

ControlServer::~ControlServer() { stop(); }

QStringList ControlServer::supportedActions() {
    return {"mute", "unmute", "toggle-mute", "deafen", "undeafen", "toggle-deafen", "disconnect-voice", "status", "diagnostics"};
}

QString ControlServer::defaultServerName() {
#ifdef Q_OS_WIN
    // Resolve the same endpoint before GUI/application identity is initialized.
    const auto directory=QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)+"/Relay/control";
    if(!QDir().mkpath(directory) || !privateDirectory(directory))return {};
#else
    auto directory = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (!privateDirectory(directory)) {
#ifdef Q_OS_UNIX
        directory = QDir::tempPath() + "/relay-control-" + QString::number(geteuid());
#else
        directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/control";
#endif
        if (!QFileInfo::exists(directory)) {
            if (!QDir().mkpath(directory)) return {};
            QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        }
        if (!privateDirectory(directory)) return {};
    }
    #endif
    return directory + "/relay-control-v1.sock";
}

bool ControlServer::start(const QString &requestedName) {
    if (running()) return true;
    const auto name = requestedName.isEmpty() ? defaultServerName() : requestedName;
    if (name.isEmpty() || !QDir::isAbsolutePath(name) || !privateDirectory(QFileInfo(name).absolutePath())) {
        m_error = "A private directory for Relay controls is unavailable.";
        emit changed();
        return false;
    }
    auto failed = [this] {
        m_lock.reset();
        m_error = "Relay controls could not start. Another Relay window may already be running.";
        emit changed();
        return false;
    };
    // Qt's UserAccessOption may rename a temporary socket over an existing path.
    // Take a process lock and check the endpoint before listen, not just after it.
    m_lock = std::make_unique<QLockFile>(name + ".lock");
    m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock(0)) return failed();
#ifdef Q_OS_WIN
    // Named pipes have no filesystem entry. Reject an existing live/busy pipe
    // before Qt's UserAccessOption can create another listening instance.
    const auto pipeName = QStringLiteral("\\\\.\\pipe\\") + name;
    const bool pipeAvailable = WaitNamedPipeW(reinterpret_cast<LPCWSTR>(pipeName.utf16()), 1);
    const auto pipeError = pipeAvailable ? ERROR_SUCCESS : GetLastError();
    if (pipeAvailable || pipeError != ERROR_FILE_NOT_FOUND) return failed();
#endif
    const QFileInfo existing(name);
    if (existing.exists() || existing.isSymLink()) {
        bool owned = !existing.isSymLink();
#ifdef Q_OS_UNIX
        struct stat entry{};
        owned = owned && lstat(QFile::encodeName(name).constData(), &entry) == 0 &&
                S_ISSOCK(entry.st_mode) && entry.st_uid == geteuid();
#endif
        if (!owned) return failed();
        QLocalSocket probe;
        probe.connectToServer(name);
        if (probe.waitForConnected(100) || probe.error() != QLocalSocket::ConnectionRefusedError) return failed();
        if (!QLocalServer::removeServer(name)) return failed();
    }
    if (!m_server.listen(name)) return failed();
    m_error.clear();
    emit changed();
    return true;
}

void ControlServer::stop() {
    const auto clients = m_clients;
    for (auto *socket : clients) socket->abort();
    m_server.close();
    m_lock.reset();
    emit changed();
}

void ControlServer::setStatusProvider(std::function<QVariantMap()> provider) {
    m_statusProvider = std::move(provider);
}

void ControlServer::setDiagnosticsProvider(std::function<QVariantMap()> provider) {
    m_diagnosticsProvider = std::move(provider);
}

QVariantMap ControlServer::diagnostics() const {
    const auto state = m_diagnosticsProvider ? m_diagnosticsProvider() : QVariantMap{};
    auto result = counters(state,
        {"voiceConnected", "muted", "deafened", "discordConnected", "micTesting",
         "broadcastActive", "streamActive", "cameraActive"},
        {"voiceGatewayPingMs", "voiceGatewayAveragePingMs"});
    result.insert("version", 1);
    result.insert("running", true);
    result.insert("broadcast", counters(mapValue(state, "broadcast"),
        {"active", "connected", "encrypted", "heartbeatPending", "sharing", "audioActive", "originalForwarded", "awaitingPublishKeyframe", "bandwidthPaused"},
        {"videoPackets", "repairPackets", "audioPackets", "wireBytes", "videoFrames", "offeredFrames",
         "queuedPackets", "queuedRepairs", "queueDrops", "transportErrors", "mediaErrors",
         "keyframeRequests", "nackRequests", "repairMisses", "receiverReports", "fractionLost",
         "maxFractionLost", "rttMs", "rembReports", "rembBitrate", "gatewayMessages",
         "keepalivePings", "keepalivePongs", "receiverBitrate", "selectedBitrate", "bitrateChanges", "congestionResets", "congestionGated", "targetBitrate", "targetFps", "width", "height",
         "sourcePath", "sourceWidth", "sourceHeight", "sourceFps", "outputFps", "originalOffered",
         "originalInitialGated", "originalRecoveryGated", "originalMeasuredBitrate", "publishRejected", "encoderDropped",
         "encoded", "originalRecoveries", "source_videoPackets", "source_decodedVideoFrames",
         "source_convertedVideoFrames", "source_replayedVideoPackets", "source_replayRestores",
         "source_replayLimitRestores", "source_maxReplayWorkUs", "source_backgroundReplayChunks",
         "source_maxBackgroundReplayWorkUs", "source_backgroundBudgetOverruns", "source_replayBytes", "source_replayPackets",
         "source_maximumReplayBytes", "source_maximumReplayPackets"}));
    result.insert("voicePlayback", counters(mapValue(state, "voicePlayback"), {},
        {"receiveQueuedMs", "receiveMaxQueuedMs", "receiveConcealedPackets", "receiveLatePackets",
         "receiveRebases", "receiveSkippedSlots", "receiveDiscardedPackets", "receiveDiscardedSamples",
         "outputPendingMs", "outputDeviceQueuedMs", "outputBufferMs", "outputState", "outputError",
         "audioLateWakes", "maxAudioLateMs", "outputUnderruns", "outputDroppedFrames"}));
    result.insert("streamPlayback", counters(mapValue(state, "streamPlayback"),
        {"audioPrimed", "audioRecoveryPending"},
        {"polls", "pollIntervalMs", "pollLateWakes", "lastPollGapUs", "maxPollGapUs", "lastPollWorkUs",
         "maxPollWorkUs", "decodedFrames", "sinkSubmittedFrames", "decoderDrops", "audioLateWakes",
         "maxAudioLateMs", "audioRebases", "audioRetiredSamples", "audioReceivedPackets",
         "audioDecodedPackets", "audioConcealedPackets", "audioCursorLagMs", "audioQueuedPackets",
         "audioQueuedMs", "audioPendingMs", "audioSinkBufferedMs", "audioSinkIdleEvents",
         "audioSinkState", "audioSinkError"}));
    return result;
}

QVariantMap ControlServer::status() const {
    const auto state = m_statusProvider ? m_statusProvider() : QVariantMap{};
    QVariantMap result{{"running", true}};
    // Explicitly whitelist booleans; arbitrary caller fields never cross IPC.
    for (const auto &key : {"voiceConnected", "muted", "deafened", "discordConnected"})
        result.insert(key, state.value(key).toBool());
    return result;
}

bool ControlServer::execute(const QString &action) {
    if (!supportedActions().contains(action)) {
        m_error = "Unknown Relay control action.";
        emit changed();
        return false;
    }
    m_error.clear();
    if (action != "status" && action != "diagnostics") emit actionRequested(action);
    emit changed();
    return true;
}

bool ControlServer::sameUser(QLocalSocket *socket) {
#if defined(Q_OS_LINUX) && defined(SO_PEERCRED)
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    return getsockopt(int(socket->socketDescriptor()), SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0 &&
           length == sizeof(credentials) && credentials.uid == geteuid();
#else
    Q_UNUSED(socket);
    return true; // UserAccessOption still restricts the socket on supported platforms.
#endif
}

void ControlServer::acceptConnections() {
    while (auto *socket = m_server.nextPendingConnection()) {
        if (m_clients.size() >= 8 || !sameUser(socket)) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        m_clients.insert(socket);
        socket->setReadBufferSize(MaximumRequest + 1);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { handleRequest(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            m_clients.remove(socket);
            socket->deleteLater();
        });
        QTimer::singleShot(RequestDeadlineMs, socket, [this, socket] {
            if (!socket->property("handled").toBool()) finish(socket, failure("Control request timed out."));
        });
        if (socket->bytesAvailable()) handleRequest(socket);
    }
}

void ControlServer::finish(QLocalSocket *socket, const QVariantMap &response) {
    if (socket->property("handled").toBool()) return;
    socket->setProperty("handled", true);
    auto bytes = QJsonDocument(QJsonObject::fromVariantMap(response)).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > MaximumResponse)
        bytes = QJsonDocument(QJsonObject::fromVariantMap(failure("Control response is too large."))).toJson(QJsonDocument::Compact) + '\n';
    socket->write(bytes);
    socket->disconnectFromServer();
    // Also release clients that stop reading the response.
    QTimer::singleShot(200, socket, [socket] { socket->abort(); });
}

void ControlServer::handleRequest(QLocalSocket *socket) {
    if (socket->property("handled").toBool()) return;
    auto bytes = socket->property("request").toByteArray() + socket->readAll();
    if (bytes.size() > MaximumRequest) { finish(socket, failure("Control request is too large.")); return; }
    const auto newline = bytes.indexOf('\n');
    if (newline < 0) { socket->setProperty("request", bytes); return; }
    if (!bytes.mid(newline + 1).trimmed().isEmpty()) {
        finish(socket, failure("Only one control action is allowed per connection."));
        return;
    }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes.left(newline), &parse);
    const auto object = document.object();
    if (parse.error != QJsonParseError::NoError || !document.isObject() ||
        object.size() != 2 || object.value("version").toDouble() != 1 || !object.value("action").isString()) {
        finish(socket, failure("Invalid Relay control request."));
        return;
    }
    const auto action = object.value("action").toString();
    if (!supportedActions().contains(action)) { finish(socket, failure("Unknown Relay control action.")); return; }
    execute(action);
    if (action == "diagnostics") finish(socket, {{"ok", true}, {"diagnostics", diagnostics()}});
    else finish(socket, {{"ok", true}, {"status", status()}});
}

QVariantMap ControlServer::sendCommand(const QString &action, int timeoutMs, const QString &requestedName) {
    if (!supportedActions().contains(action)) return failure("Unknown Relay control action.");
    const auto name = requestedName.isEmpty() ? defaultServerName() : requestedName;
    if (name.isEmpty()) return failure("A private directory for Relay controls is unavailable.");
    QElapsedTimer elapsed;
    elapsed.start();
    const int deadline = qBound(100, timeoutMs, 5000);
    auto remaining = [&] { return qMax(0, deadline - int(elapsed.elapsed())); };
    QLocalSocket socket;
    socket.setReadBufferSize(MaximumResponse + 1);
    socket.connectToServer(name);
    if (!socket.waitForConnected(remaining())) return failure("Relay is not running, or its local controls are unavailable.");
    if (!sameUser(&socket)) return failure("Relay control socket belongs to another user.");
    const auto request = QJsonDocument(QJsonObject{{"version", 1}, {"action", action}}).toJson(QJsonDocument::Compact) + '\n';
    socket.write(request);
    if (socket.bytesToWrite() && !socket.waitForBytesWritten(remaining())) return failure("Relay control request could not be sent.");
    QByteArray bytes;
    while (remaining() > 0) {
        bytes += socket.readAll();
        if (bytes.size() > MaximumResponse) return failure("Relay returned an invalid control response.");
        const auto newline = bytes.indexOf('\n');
        if (newline >= 0) {
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(bytes.left(newline), &parse);
            if (parse.error != QJsonParseError::NoError || !document.isObject() || !document.object().value("ok").isBool())
                return failure("Relay returned an invalid control response.");
            return document.object().toVariantMap();
        }
        if (!socket.waitForReadyRead(remaining()) && socket.bytesAvailable() == 0) break;
    }
    return failure("Relay did not confirm the action. Check its voice controls before trying again.");
}

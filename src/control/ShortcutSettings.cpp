#include "ShortcutSettings.h"

#include <QKeySequence>
#include <QSet>
#include <QStandardPaths>

QMap<QString, QString> ShortcutSettings::defaults() {
    return {{"toggle-mute", "Ctrl+Shift+M"}, {"toggle-deafen", "Ctrl+Shift+D"},
            {"disconnect-voice", "Ctrl+Shift+H"}};
}

ShortcutSettings::ShortcutSettings(QObject *parent)
    : ShortcutSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/shortcuts.ini", parent) {}

ShortcutSettings::ShortcutSettings(const QString &settingsFile, QObject *parent)
    : QObject(parent), m_bindings(defaults()),
      m_settings(std::make_unique<QSettings>(settingsFile.isEmpty()
          ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/shortcuts.ini"
          : settingsFile, QSettings::IniFormat)) {
    for (auto it = m_bindings.begin(); it != m_bindings.end(); ++it) {
        const auto key = "bindings/" + it.key();
        if (!m_settings->contains(key)) continue;
        QString normalized, error;
        if (normalize(m_settings->value(key).toString(), &normalized, &error)) it.value() = normalized;
    }
    // A hand-edited settings file must not make two actions fire together.
    QSet<QString> used;
    for (const auto &action : {"toggle-mute", "toggle-deafen", "disconnect-voice"}) {
        const auto sequence = m_bindings.value(action);
        if (sequence.isEmpty()) continue;
        if (used.contains(sequence)) m_bindings.insert(action, {});
        else used.insert(sequence);
    }
}

QVariantList ShortcutSettings::bindings() const {
    return {QVariantMap{{"action", "toggle-mute"}, {"label", "Toggle microphone"}, {"sequence", muteShortcut()}},
            QVariantMap{{"action", "toggle-deafen"}, {"label", "Toggle deafen"}, {"sequence", deafenShortcut()}},
            QVariantMap{{"action", "disconnect-voice"}, {"label", "Disconnect voice"}, {"sequence", disconnectShortcut()}}};
}

bool ShortcutSettings::normalize(const QString &input, QString *normalized, QString *error) {
    if (input.trimmed().isEmpty()) { normalized->clear(); return true; }
    if (input.size() > 80) { *error = "That shortcut is too long."; return false; }
    const auto sequence = QKeySequence::fromString(input.trimmed(), QKeySequence::PortableText);
    if (sequence.count() != 1 || sequence[0].key() == Qt::Key_unknown || sequence[0].key() == 0) {
        *error = "Enter one shortcut, such as Ctrl+Shift+M.";
        return false;
    }
    const auto combination = sequence[0];
    const bool functionKey = combination.key() >= Qt::Key_F1 && combination.key() <= Qt::Key_F35;
    if (!functionKey && !(combination.keyboardModifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
        *error = "Use Ctrl, Alt, or Meta with the key, or choose a function key.";
        return false;
    }
    *normalized = sequence.toString(QKeySequence::PortableText);
    if (normalized->isEmpty()) { *error = "Enter a valid keyboard shortcut."; return false; }
    return true;
}

bool ShortcutSettings::save() {
    for (auto it = m_bindings.cbegin(); it != m_bindings.cend(); ++it)
        m_settings->setValue("bindings/" + it.key(), it.value());
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError) {
        m_error = "Relay could not save your shortcuts.";
        return false;
    }
    return true;
}

bool ShortcutSettings::setBinding(const QString &action, const QString &sequence) {
    m_error.clear();
    QString normalized;
    if (!m_bindings.contains(action)) m_error = "Unknown shortcut action.";
    else if (normalize(sequence, &normalized, &m_error) && !normalized.isEmpty()) {
        for (auto it = m_bindings.cbegin(); it != m_bindings.cend(); ++it)
            if (it.key() != action && it.value() == normalized) {
                m_error = "That shortcut is already assigned to another voice action.";
                break;
            }
    }
    if (!m_error.isEmpty()) { emit changed(); return false; }
    const auto previous = m_bindings;
    m_bindings.insert(action, normalized);
    if (!save()) m_bindings = previous;
    emit changed();
    return m_error.isEmpty();
}

void ShortcutSettings::reset() {
    m_error.clear();
    const auto previous = m_bindings;
    m_bindings = defaults();
    if (!save()) m_bindings = previous;
    emit changed();
}

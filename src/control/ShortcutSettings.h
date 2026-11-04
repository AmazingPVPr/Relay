#pragma once

#include <QObject>
#include <QSettings>
#include <QVariantList>
#include <memory>

class ShortcutSettings final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString muteShortcut READ muteShortcut NOTIFY changed)
    Q_PROPERTY(QString deafenShortcut READ deafenShortcut NOTIFY changed)
    Q_PROPERTY(QString disconnectShortcut READ disconnectShortcut NOTIFY changed)
    Q_PROPERTY(QVariantList bindings READ bindings NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    explicit ShortcutSettings(QObject *parent = nullptr);
    explicit ShortcutSettings(const QString &settingsFile, QObject *parent = nullptr);
    QString muteShortcut() const { return m_bindings.value("toggle-mute"); }
    QString deafenShortcut() const { return m_bindings.value("toggle-deafen"); }
    QString disconnectShortcut() const { return m_bindings.value("disconnect-voice"); }
    QVariantList bindings() const;
    QString error() const { return m_error; }
    Q_INVOKABLE bool setBinding(const QString &action, const QString &sequence);
    Q_INVOKABLE void reset();
signals:
    void changed();
private:
    friend class ControlTest;
    static QMap<QString, QString> defaults();
    static bool normalize(const QString &input, QString *normalized, QString *error);
    bool save();
    QMap<QString, QString> m_bindings;
    std::unique_ptr<QSettings> m_settings;
    QString m_error;
};

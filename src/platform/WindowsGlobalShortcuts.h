#pragma once
#include <QObject>
#include <QAbstractNativeEventFilter>
#include <QMap>
#include <QVariantList>
class GlobalShortcuts final : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QVariantList bindings READ bindings NOTIFY changed)
public:
    explicit GlobalShortcuts(QObject *parent=nullptr);
    ~GlobalShortcuts() override;
    bool active() const {return !m_actions.isEmpty();}
    bool busy() const {return false;}
    QString status() const {return active()?"System shortcuts are on.":"System shortcuts are off.";}
    QString error() const {return m_error;}
    QVariantList bindings() const {return m_bindings;}
    Q_INVOKABLE void enable(const QVariantList &bindings);
    Q_INVOKABLE void disable();
    Q_INVOKABLE bool isBound(const QString &action) const {return m_actions.values().contains(action);}
    static QString preferredTrigger(const QString &sequence);
signals:
    void changed();
    void actionRequested(const QString &action);
private:
    bool nativeEventFilter(const QByteArray &eventType,void *message,qintptr *result) override;
    void release();
    QMap<int,QString> m_actions;
    QVariantList m_bindings;
    QString m_error;
};

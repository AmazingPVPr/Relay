#pragma once
#include <QObject>
#include <QVariantMap>
#include <QCache>

// A small allowlisted formatter for native Qt text. User HTML is always escaped.
class ChatText final : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    Q_INVOKABLE QString render(const QString &text, const QVariantMap &metadata = {}) const;
    Q_INVOKABLE void copy(const QString &text) const;
    static bool safeUrl(const QString &value);
    void clearCache() { m_rendered.clear(); }
private:
    mutable QCache<QString,QString> m_rendered{4096};
};

#pragma once
#include <QObject>
#include <QHash>
#include <QVariantMap>
#include <memory>

class VideoResolver final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
public:
    explicit VideoResolver(QObject *parent=nullptr);
    explicit VideoResolver(const QString &executable,QObject *parent=nullptr);
    ~VideoResolver() override;
    bool available() const {return !m_executable.isEmpty();}
    Q_INVOKABLE bool resolve(const QString &requestId,const QString &url);
    Q_INVOKABLE void cancel(const QString &requestId);
    Q_INVOKABLE void cancelAll();
    Q_INVOKABLE void invalidate(const QString &url);
signals:
    void resolutionStarted(const QString &requestId);
    void resolved(const QString &requestId,const QVariantMap &media);
    void failed(const QString &requestId,const QString &error);
private:
    friend class VideoResolverTest;
    struct Job;
    void finish(const std::shared_ptr<Job> &job,const QVariantMap &media,const QString &error);
    static QString executablePath();
    static QStringList arguments(const QString &page);
    static QVariantMap parseResult(const QByteArray &output);
    static QString failureText(const QByteArray &error);
    void remember(const QString &page,const QVariantMap &media);
    static QString cacheKey(const QString &url);
    struct CachedMedia { QVariantMap media; qint64 expiresAt=0,lastUse=0; };
    QString m_executable;
    QHash<QString,std::shared_ptr<Job>> m_jobs;
    QHash<QString,std::shared_ptr<Job>> m_running;
    QHash<QString,CachedMedia> m_cache;
    qint64 m_cacheUse=0;
    int m_cacheLimit=32;
    qint64 m_cacheTtlMs=5*60*1000;
    int m_timeoutMs=30000;
};

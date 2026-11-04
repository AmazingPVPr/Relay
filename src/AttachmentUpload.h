#pragma once
#include <QObject>
#include <QHash>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QThreadPool>
#include <QUrl>
#include <QVariantList>
#include <memory>
#include <atomic>
#include <vector>
class DiscordClient;
class QNetworkReply;
class QFile;
class QMimeData;
class QTemporaryDir;

// User-selected files remain on disk. Only metadata reaches QML; account
// authorization stays in DiscordClient and is never sent to signed upload URLs.
class AttachmentUpload final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList attachments READ attachments NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(qint64 totalBytes READ totalBytes NOTIFY changed)
    Q_PROPERTY(qint64 fileLimit READ fileLimit NOTIFY changed)
    Q_PROPERTY(QString limitText READ limitText NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool cancelable READ cancelable NOTIFY changed)
    Q_PROPERTY(QString activeChannelId READ activeChannelId NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
public:
    explicit AttachmentUpload(DiscordClient *client,QObject *parent=nullptr);
    ~AttachmentUpload() override;
    QVariantList attachments() const;
    int count() const;
    qint64 totalBytes() const;
    qint64 fileLimit() const;
    QString limitText() const;
    bool busy() const { return m_preparing || !m_activeChannel.isEmpty(); }
    bool cancelable() const { return busy() && !m_posting; }
    QString activeChannelId() const { return m_activeChannel; }
    double progress() const { return m_progress; }
    QString error() const { return m_error; }
    QString status() const { return m_status; }
    Q_INVOKABLE bool addFiles(const QVariantList &urls);
    // True means attachment MIME was handled (including a displayed error).
    // False leaves ordinary text/links to the editor's native paste action.
    Q_INVOKABLE bool pasteFromClipboard();
    Q_INVOKABLE void removeAttachment(const QString &id);
    Q_INVOKABLE void clearDraft();
    Q_INVOKABLE void setSpoiler(const QString &id,bool spoiler);
    Q_INVOKABLE bool send(const QString &content);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void clearError();
signals:
    void changed();
private:
    friend class AttachmentUploadTest;
    struct File { QString id,path,name,mime,preview; qint64 size=0,modified=0; bool spoiler=false; std::shared_ptr<QTemporaryDir> staging; };
    struct Cloud { QUrl url; QString filename; };
    void syncContext();
    void fail(const QString &reason);
    void finish(bool sent);
    void resetActive();
    void uploadNext(quint64 operation);
    void postMessage(quint64 operation);
    bool currentOperation(quint64 operation) const;
    bool validUploadUrl(const QUrl &url) const;
    bool validateChannel(QString *reason) const;
    bool addFilesWithStaging(const QVariantList &urls,const std::shared_ptr<QTemporaryDir> &staging={});
    bool pasteMimeData(const QMimeData *mime);
    void cancelPreparation();
    static QString friendlyError(const QString &reason);
    static QString sizeLabel(qint64 bytes);
    static QString uploadName(const File &file);
    static std::unique_ptr<QFile> openRegular(const File &file);
    DiscordClient *m_client;
    QNetworkAccessManager m_cloudNetwork;
    QThreadPool m_clipboardPool;
    QHash<QString,QList<File>> m_drafts;
    QList<File> m_activeFiles;
    QList<Cloud> m_cloudFiles;
    std::vector<std::unique_ptr<QFile>> m_openFiles;
    QPointer<QNetworkReply> m_reply;
    QString m_selectedChannel,m_accountId,m_activeChannel,m_content,m_nonce,m_error,m_status;
    quint64 m_generation=0,m_operation=0,m_nextId=0,m_clipboardOperation=0;
    int m_uploadIndex=0;
    qint64 m_sentBytes=0,m_uploadBytes=0;
    double m_progress=0;
    bool m_posting=false,m_preparing=false;
    std::atomic_bool m_clipboardEncoding=false;
    QUrl m_fixtureUploadOrigin; // Test friend only; never a QML or command-line option.
};

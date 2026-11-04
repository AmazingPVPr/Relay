#include "AttachmentUpload.h"
#include "DiscordClient.h"
#include <QDateTime>
#include <QBuffer>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QMimeData>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <fcntl.h>
#ifdef Q_OS_WIN
#include <windows.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
constexpr qint64 MiB=1024*1024,MaximumFile=1024*MiB,MaximumCloudResponse=64*1024;
bool clipboardImageSize(const QSize &size) {return size.isValid() && size.width()<=32768 && size.height()<=32768 && qint64(size.width())*size.height()<=64*1024*1024;}
}
AttachmentUpload::AttachmentUpload(DiscordClient *client,QObject *parent) : QObject(parent),m_client(client),m_cloudNetwork(this) {
    m_clipboardPool.setMaxThreadCount(1);
    m_generation=client->m_generation;m_accountId=client->currentUserId();m_selectedChannel=client->currentChannelId();
    connect(client,&DiscordClient::stateChanged,this,&AttachmentUpload::syncContext);
}
AttachmentUpload::~AttachmentUpload() {
    cancelPreparation();
    // Workers only encode an already-snapshotted image. Keep their callback
    // receiver alive until encoding ends; QObject then removes queued results.
    m_clipboardPool.waitForDone();
    resetActive();
}
QVariantList AttachmentUpload::attachments() const {
    QVariantList rows;for(const auto &file:m_drafts.value(m_selectedChannel)) rows.append(QVariantMap{{"id",file.id},{"name",file.name},{"size",file.size},{"sizeLabel",sizeLabel(file.size)},{"mime",file.mime},{"previewUrl",file.preview},{"spoiler",file.spoiler}});return rows;
}
int AttachmentUpload::count() const { return m_drafts.value(m_selectedChannel).size(); }
qint64 AttachmentUpload::totalBytes() const {qint64 bytes=0;for(const auto &file:m_drafts.value(m_selectedChannel))bytes+=file.size;return bytes;}
QString AttachmentUpload::sizeLabel(qint64 bytes) {
    if(bytes>=1024*MiB)return QString::number(double(bytes)/(1024*MiB),'f',1)+" GiB";
    if(bytes>=MiB)return QString::number(double(bytes)/MiB,'f',1)+" MiB";
    if(bytes>=1024)return QString::number(double(bytes)/1024,'f',1)+" KiB";
    return QString::number(bytes)+" bytes";
}
qint64 AttachmentUpload::fileLimit() const {
    const auto user=m_client->m_users.value(m_client->m_currentUserId);
    qint64 limit=0;
    if(user.contains("premium_type")) {
        switch(user.value("premium_type").toInt(-1)) {
        case 0:limit=20*MiB;break;
        case 1:case 3:limit=50*MiB;break;
        case 2:limit=MaximumFile;break;
        default:break;
        }
    }
    // Missing account or server entitlement data is not evidence of a smaller
    // limit. In that case let Discord validate the allocation authoritatively.
    if(limit==0)return 0;
    const auto guild=m_client->m_guildObjects.value(m_client->m_currentGuildId);
    if(!m_client->m_currentGuildId.isEmpty() && !guild.contains("premium_tier") && limit<MaximumFile)return 0;
    if(guild.contains("premium_tier")) {
        const auto tier=guild.value("premium_tier").toInt();
        if(tier==3)limit=std::max(limit,100*MiB);else if(tier==2)limit=std::max(limit,50*MiB);
    }
    return limit;
}
QString AttachmentUpload::limitText() const {const auto limit=fileLimit();return limit>0?"Up to 10 files · "+sizeLabel(limit)+" per file":"Up to 10 files · Discord checks your account and server upload limits";}
void AttachmentUpload::syncContext() {
    const auto account=m_client->currentUserId();const auto channel=m_client->currentChannelId();
    const bool sessionChanged=m_generation!=m_client->m_generation || (!m_accountId.isEmpty() && !account.isEmpty() && m_accountId!=account);
    bool changedState=false;
    if(sessionChanged) {cancelPreparation();++m_operation;resetActive();m_drafts.clear();m_error.clear();m_status.clear();m_generation=m_client->m_generation;changedState=true;}
    if(account!=m_accountId){m_accountId=account;changedState=true;}
    if(channel!=m_selectedChannel){if(m_preparing){cancelPreparation();m_status.clear();}m_selectedChannel=channel;changedState=true;}
    if(changedState)emit changed();
}
void AttachmentUpload::clearError(){if(m_error.isEmpty())return;m_error.clear();emit changed();}
bool AttachmentUpload::validateChannel(QString *reason) const {
    if(!m_client->connected() || m_client->demoMode() || m_client->m_token.isEmpty()) {*reason="Wait for Discord to reconnect before uploading files.";return false;}
    if(!DiscordClient::validId(m_selectedChannel) || (m_client->m_currentChannelKind!="text" && m_client->m_currentChannelKind!="dm" && m_client->m_currentChannelKind!="thread" && m_client->m_currentChannelKind!="voice")) {*reason="Choose a text channel, conversation, voice chat or existing thread to attach files.";return false;}
    if(m_client->pendingMessageRequest(m_client->m_dmObjects.value(m_selectedChannel))) {*reason="Accept this message request before replying.";return false;}
    return true;
}
bool AttachmentUpload::addFiles(const QVariantList &urls) {
    return addFilesWithStaging(urls);
}
bool AttachmentUpload::addFilesWithStaging(const QVariantList &urls,const std::shared_ptr<QTemporaryDir> &staging) {
    syncContext();QString reason;if(!validateChannel(&reason)){m_error=reason;emit changed();return false;}
    if(busy()){m_error="Wait for the current upload to finish, or cancel it first.";emit changed();return false;}
    if(urls.isEmpty())return false;
    if(urls.size()>10 || count()+urls.size()>10){m_error="You can attach up to 10 files to one message.";emit changed();return false;}
    if(!m_drafts.contains(m_selectedChannel) && m_drafts.size()>=32){m_error="Remove attachments from another draft before adding more.";emit changed();return false;}
    QList<File> added;const auto limit=fileLimit();QMimeDatabase database;
    for(const auto &value:urls) {
        const QUrl url=value.toUrl();
        if(!url.isLocalFile() || (!url.host().isEmpty() && url.host()!="localhost")){m_error="Choose files stored on this computer.";emit changed();return false;}
        const QFileInfo info(url.toLocalFile());
        if(!info.exists() || !info.isFile() || !info.isReadable()){m_error="One of these files could not be opened. Choose a readable file, rather than a folder.";emit changed();return false;}
        if(info.size()>MaximumFile || (limit>0 && info.size()>limit)) {m_error="Your files are too powerful. "+info.fileName().left(120)+" exceeds the "+sizeLabel(limit>0?limit:MaximumFile)+" per-file upload limit.";emit changed();return false;}
        File file;file.id=QString::number(++m_nextId);file.path=info.absoluteFilePath();file.name=info.fileName().left(240);file.name.remove(QRegularExpression("[\\x00-\\x1f\\x7f]"));if(file.name.isEmpty())file.name="Attachment";
        file.spoiler=file.name.startsWith("SPOILER_");if(file.spoiler)file.name=file.name.mid(8);file.size=info.size();file.modified=info.lastModified().toMSecsSinceEpoch();file.mime=database.mimeTypeForFile(info.fileName(),QMimeDatabase::MatchExtension).name();
        const QStringList previewTypes{"image/png","image/jpeg","image/webp","image/avif","image/gif","image/bmp"};
        if(file.size<=8*MiB && previewTypes.contains(file.mime)) {
            // Draft thumbnails and the explicit preview use Image, which decodes
            // only the first frame; animated uploads keep their original bytes.
            QImageReader reader(file.path);const auto size=reader.size();
            if(size.isValid() && size.width()<=4096 && size.height()<=4096 && qint64(size.width())*size.height()<=16000000)file.preview=QUrl::fromLocalFile(file.path).toString();
        }
        file.staging=staging;added.append(std::move(file));
    }
    m_drafts[m_selectedChannel].append(added);m_error.clear();m_status.clear();emit changed();return true;
}
bool AttachmentUpload::pasteFromClipboard() {
    // Clipboard access happens only in response to the editor's explicit paste.
    auto *clipboard=QGuiApplication::clipboard();
    return clipboard && pasteMimeData(clipboard->mimeData(QClipboard::Clipboard));
}
void AttachmentUpload::cancelPreparation() {
    ++m_clipboardOperation;
    m_preparing=false;
    // Keep the single bounded worker alive to release its captured image and
    // reset its encoding flag, even if cancellation precedes worker startup.
}
bool AttachmentUpload::pasteMimeData(const QMimeData *mime) {
    if(!mime)return false;
    QList<QUrl> urls=mime->urls();
    // GNOME's copied-files fallback is also a copy here, even when its source
    // action says "cut". Relay never changes or deletes the original files.
    if(urls.isEmpty() && mime->hasFormat("x-special/gnome-copied-files")) {
        const auto raw=mime->data("x-special/gnome-copied-files");
        if(raw.size()<=1024*1024) {
            const auto lines=raw.split('\n');
            if(!lines.isEmpty() && (lines.first().trimmed()=="copy" || lines.first().trimmed()=="cut"))
                for(qsizetype i=1;i<lines.size();++i)if(!lines[i].trimmed().isEmpty())urls.append(QUrl::fromEncoded(lines[i].trimmed()));
        }
    }
    const bool files=std::any_of(urls.cbegin(),urls.cend(),[](const QUrl &url){return url.scheme()=="file";});
    // File URI MIME takes precedence over any thumbnail supplied by a file
    // manager, retaining original bytes, names and animated images.
    if(files) {
        QVariantList selected;for(const auto &url:urls)selected.append(url);
        addFiles(selected);
        return true;
    }
    QByteArray encodedFormat;
    QString extension;
    // Clipboard backends may expose only an encoded raster MIME, rather than
    // Qt's virtual imageData format. Preserve those bytes and any animation.
    const QList<QPair<QString,QByteArray>> rasterTypes{{"image/png","png"},{"image/jpeg","jpg"},{"image/gif","gif"},{"image/webp","webp"},{"image/avif","avif"},{"image/bmp","bmp"},{"image/tiff","tiff"}};
    for(const auto &format:mime->formats()) {
        for(const auto &type:rasterTypes)if(format==type.first){encodedFormat=type.first.toLatin1();extension=QString::fromLatin1(type.second);break;}
        if(!encodedFormat.isEmpty())break;
    }
    // URI-only network links, ordinary text, and textual paths remain text.
    if(encodedFormat.isEmpty() && !mime->hasImage())return false;
    syncContext();QString reason;
    if(!validateChannel(&reason)){m_error=reason;emit changed();return true;}
    if(busy() || m_clipboardEncoding.load()){m_error="Wait for the current attachment to finish preparing or uploading.";emit changed();return true;}
    if(count()>=10){m_error="You can attach up to 10 files to one message.";emit changed();return true;}
    if(!m_drafts.contains(m_selectedChannel) && m_drafts.size()>=32){m_error="Remove attachments from another draft before adding more.";emit changed();return true;}
    QImage image;QByteArray encoded;
    if(!encodedFormat.isEmpty()) {
        encoded=mime->data(QString::fromLatin1(encodedFormat));
        const auto limit=fileLimit();
        if(encoded.size()>MaximumFile || (limit>0 && encoded.size()>limit)){m_error="Your files are too powerful. This clipboard image exceeds the "+sizeLabel(limit>0?limit:MaximumFile)+" per-file upload limit.";emit changed();return true;}
        if(encoded.isEmpty()){m_error="This clipboard image could not be opened. Copy it again or attach the original file.";emit changed();return true;}
    } else {
        image=qvariant_cast<QImage>(mime->imageData());extension="png";
        if(image.isNull()){m_error="This clipboard image could not be opened. Copy it again or attach the original file.";emit changed();return true;}
        if(!clipboardImageSize(image.size()) || image.sizeInBytes()>512*MiB){m_error="This clipboard image is too large to prepare. Attach the original image file instead.";emit changed();return true;}
    }
    auto staging=std::make_shared<QTemporaryDir>(QDir::tempPath()+"/relay-paste-XXXXXX");
    if(!staging->isValid()){m_error="Relay could not prepare the clipboard image. Check your temporary disk space and try again.";emit changed();return true;}
    const auto path=staging->filePath("image."+extension);
    const auto operation=++m_clipboardOperation,generation=m_generation;
    const auto account=m_accountId,channel=m_selectedChannel;
    m_preparing=true;m_clipboardEncoding=true;m_error.clear();m_status="Preparing pasted image…";emit changed();
    m_clipboardPool.start([this,image,encoded,staging,path,operation,generation,account,channel]{
        QString failure;
        if(!encoded.isEmpty()) {
            QBuffer input;input.setData(encoded);input.open(QIODevice::ReadOnly);QImageReader reader(&input);
            if(!reader.canRead())failure="This clipboard image could not be opened. Copy it again or attach the original file.";
            else if(!clipboardImageSize(reader.size()))failure="This clipboard image is too large to prepare. Attach the original image file instead.";
        }
        bool saved=false;
        if(failure.isEmpty()) {
            QFile output(path);saved=output.open(QIODevice::WriteOnly);
            if(saved){saved=output.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);if(!encoded.isEmpty())saved=saved && output.write(encoded)==encoded.size();else {QImageWriter writer(&output,"png");writer.setCompression(1);saved=saved && writer.write(image);}output.close();}
            if(!saved)failure="Relay could not prepare the clipboard image. Check your temporary disk space and try again.";
        }
        m_clipboardEncoding=false;
        QMetaObject::invokeMethod(this,[this,staging,path,operation,generation,account,channel,failure]{
            if(operation!=m_clipboardOperation || !m_preparing || generation!=m_client->m_generation || account!=m_client->currentUserId() || channel!=m_client->currentChannelId())return;
            m_preparing=false;m_status.clear();
            if(!failure.isEmpty()){m_error=failure;emit changed();return;}
            // addFiles applies the same entitlement, file-count, permission,
            // readability and preview checks as a selected or dropped file.
            addFilesWithStaging({QUrl::fromLocalFile(path)},staging);
        },Qt::QueuedConnection);
    });
    return true;
}
void AttachmentUpload::removeAttachment(const QString &id){if(busy())return;auto &files=m_drafts[m_selectedChannel];files.removeIf([&](const File &file){return file.id==id;});if(files.isEmpty())m_drafts.remove(m_selectedChannel);m_error.clear();emit changed();}
void AttachmentUpload::clearDraft(){if(busy())return;m_drafts.remove(m_selectedChannel);m_error.clear();m_status.clear();emit changed();}
void AttachmentUpload::setSpoiler(const QString &id,bool spoiler){if(busy())return;auto found=m_drafts.find(m_selectedChannel);if(found==m_drafts.end())return;for(auto &file:found.value())if(file.id==id){file.spoiler=spoiler;emit changed();return;}}
QString AttachmentUpload::uploadName(const File &file){return file.spoiler?"SPOILER_"+file.name:file.name;}
std::unique_ptr<QFile> AttachmentUpload::openRegular(const File &file) {
#ifdef Q_OS_WIN
    // No write/delete sharing: retain stable bytes and support Unicode paths.
    const auto handle=CreateFileW(reinterpret_cast<LPCWSTR>(file.path.utf16()),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return {};
    BY_HANDLE_FILE_INFORMATION state{};
    if(GetFileType(handle)!=FILE_TYPE_DISK || !GetFileInformationByHandle(handle,&state) ||
       (state.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) ||
       qint64((quint64(state.nFileSizeHigh)<<32)|state.nFileSizeLow)!=file.size ||
       QFileInfo(file.path).lastModified().toMSecsSinceEpoch()!=file.modified){CloseHandle(handle);return {};}
    const int fd=_open_osfhandle(reinterpret_cast<intptr_t>(handle),_O_RDONLY|_O_BINARY);
    if(fd<0){CloseHandle(handle);return {};}
    auto source=std::make_unique<QFile>();
    if(!source->open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){_close(fd);return {};}
    return source;
#else
    const auto native=QFile::encodeName(file.path);const int fd=::open(native.constData(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);if(fd<0)return {};
    struct stat state{};
    if(::fstat(fd,&state)!=0 || !S_ISREG(state.st_mode) || state.st_size!=file.size || QFileInfo(file.path).lastModified().toMSecsSinceEpoch()!=file.modified){::close(fd);return {};}
    auto source=std::make_unique<QFile>();if(!source->open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(fd);return {};}return source;
#endif
}
bool AttachmentUpload::send(const QString &content) {
    syncContext();QString reason;if(!validateChannel(&reason)){m_error=reason;emit changed();return false;}
    if(busy() || m_client->m_sendingMessage || count()==0)return false;
    if(content.size()>2000){m_error="Keep messages to 2,000 characters in this version.";emit changed();return false;}
    m_activeFiles=m_drafts.value(m_selectedChannel);const auto limit=fileLimit();
    for(const auto &file:m_activeFiles) {
        if(file.size>MaximumFile || (limit>0 && file.size>limit)){m_activeFiles.clear();m_error="One of these files exceeds this conversation’s upload limit.";emit changed();return false;}
        auto source=openRegular(file);if(!source){m_openFiles.clear();m_activeFiles.clear();m_error="A selected file changed or could not be opened. Remove it and choose it again.";emit changed();return false;}m_openFiles.push_back(std::move(source));
    }
    const auto operation=++m_operation;m_activeChannel=m_selectedChannel;m_content=content;m_uploadIndex=0;m_sentBytes=0;m_uploadBytes=0;m_posting=false;m_progress=0;m_error.clear();m_status="Preparing attachments…";
    for(const auto &file:m_activeFiles)m_uploadBytes+=file.size;
    m_nonce=QString::number(QDateTime::currentMSecsSinceEpoch())+QString::number(QRandomGenerator::global()->bounded(10000)).rightJustified(4,'0');
    m_client->m_sendingMessage=true;emit changed();emit m_client->stateChanged();
    QJsonArray files;for(int i=0;i<m_activeFiles.size();++i)files.append(QJsonObject{{"id",QString::number(i)},{"filename",uploadName(m_activeFiles[i])},{"file_size",m_activeFiles[i].size}});
    const QPointer<AttachmentUpload> guard(this);
    m_client->request("POST","/channels/"+m_activeChannel+"/attachments",{{"files",files}},[guard,operation](const QJsonValue &value,const QString &error){
        if(!guard || !guard->currentOperation(operation))return;
        if(!error.isEmpty()){guard->fail(friendlyError(error));return;}
        const auto response=value.toObject().value("attachments").toArray();
        if(response.size()!=guard->m_activeFiles.size()){guard->fail("Discord did not prepare all of the attachments. Try again.");return;}
        QList<Cloud> cloud;cloud.resize(response.size());QSet<int> seen;
        for(const auto &entry:response) {
            const auto object=entry.toObject();bool okay=false;const int index=object.value("id").toVariant().toString().toInt(&okay);const auto filename=object.value("upload_filename").toString();const QUrl url(object.value("upload_url").toString());
            if(!okay || index<0 || index>=cloud.size() || seen.contains(index) || filename.isEmpty() || filename.size()>2048 || filename.contains(QRegularExpression("[\\x00-\\x1f\\x7f]")) || !guard->validUploadUrl(url)){guard->fail("Discord returned an invalid attachment destination. No files were uploaded.");return;}
            seen.insert(index);cloud[index]={url,filename};
        }
        guard->m_cloudFiles=std::move(cloud);guard->uploadNext(operation);
    });return true;
}
bool AttachmentUpload::currentOperation(quint64 operation) const {return !m_activeChannel.isEmpty() && operation==m_operation && m_generation==m_client->m_generation && m_accountId==m_client->currentUserId();}
bool AttachmentUpload::validUploadUrl(const QUrl &url) const {
    if(!m_fixtureUploadOrigin.isEmpty())return url.scheme()==m_fixtureUploadOrigin.scheme() && url.host()=="127.0.0.1" && url.port()==m_fixtureUploadOrigin.port() && url.userInfo().isEmpty();
    return url.isValid() && url.toString().size()<=16384 && url.scheme()=="https" && url.host()=="discord-attachments-uploads-prd.storage.googleapis.com" && url.userInfo().isEmpty() && (url.port()==-1 || url.port()==443) && url.fragment().isEmpty();
}
void AttachmentUpload::uploadNext(quint64 operation) {
    if(!currentOperation(operation))return;
    if(m_uploadIndex>=m_activeFiles.size()){postMessage(operation);return;}
    const auto &file=m_activeFiles[m_uploadIndex];auto *source=m_openFiles[size_t(m_uploadIndex)].get();
    if(source->size()!=file.size || QFileInfo(file.path).lastModified().toMSecsSinceEpoch()!=file.modified){fail("A selected file changed while uploading. Remove it and choose it again.");return;}
    QNetworkRequest request(m_cloudFiles[m_uploadIndex].url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute,QNetworkRequest::Manual);
    request.setRawHeader("Content-Type","");request.setHeader(QNetworkRequest::ContentLengthHeader,file.size);request.setTransferTimeout(60000);
    m_status="Uploading "+QString::number(m_uploadIndex+1)+" of "+QString::number(m_activeFiles.size())+"…";
    auto *reply=m_cloudNetwork.put(request,source);m_reply=reply;reply->setReadBufferSize(MaximumCloudResponse+1);
    connect(reply,&QNetworkReply::uploadProgress,this,[this,operation](qint64 sent,qint64){if(!currentOperation(operation))return;const auto progress=m_uploadBytes>0?std::clamp(double(m_sentBytes+sent)/m_uploadBytes,0.,1.):0.;if(std::abs(progress-m_progress)>=0.005){m_progress=progress;emit changed();}});
    connect(reply,&QNetworkReply::downloadProgress,this,[reply](qint64 received,qint64 total){if(received>MaximumCloudResponse || total>MaximumCloudResponse){reply->setProperty("responseTooLarge",true);reply->abort();}});
    connect(reply,&QNetworkReply::finished,this,[this,reply,operation]{
        reply->deleteLater();if(m_reply==reply)m_reply.clear();if(!currentOperation(operation))return;
        const int status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if(status<200 || status>=300 || reply->error()!=QNetworkReply::NoError || reply->property("responseTooLarge").toBool()){fail(status==413?"Your files are too powerful. Discord rejected the attachment size for this account or server.":"Discord could not upload this file. Check your connection and try again.");return;}
        m_sentBytes+=m_activeFiles[m_uploadIndex].size;++m_uploadIndex;uploadNext(operation);
    });emit changed();
}
void AttachmentUpload::postMessage(quint64 operation) {
    if(!currentOperation(operation))return;
    QJsonArray attachments;for(int i=0;i<m_activeFiles.size();++i)attachments.append(QJsonObject{{"id",QString::number(i)},{"filename",uploadName(m_activeFiles[i])},{"uploaded_filename",m_cloudFiles[i].filename}});
    QJsonObject body{{"content",m_content},{"attachments",attachments},{"nonce",m_nonce},{"enforce_nonce",true},{"tts",false},{"allowed_mentions",DiscordClient::allowedMentions(m_content)}};
    m_posting=true;m_status="Sending message…";m_progress=1;emit changed();const QPointer<AttachmentUpload> guard(this);
    m_client->request("POST","/channels/"+m_activeChannel+"/messages",body,[guard,operation](const QJsonValue &value,const QString &error){
        if(!guard || !guard->currentOperation(operation))return;
        if(!error.isEmpty()){guard->fail(friendlyError(error));return;}
        auto message=value.toObject();
        if(!DiscordClient::validId(message.value("id").toString()) || message.value("channel_id").toString()!=guard->m_activeChannel){guard->fail("Discord did not confirm the message. Check the channel before sending it again.");return;}
        const auto channel=guard->m_activeChannel;const auto content=guard->m_content;
        guard->m_client->recordDmActivity(message);
        if(channel==guard->m_client->currentChannelId()) {if(guard->m_client->m_hasNewerMessages)guard->m_client->jumpToPresent();guard->m_client->acceptMessage(message);}
        guard->finish(true);emit guard->m_client->messageSent(content);
    });
}
QString AttachmentUpload::friendlyError(const QString &reason) {
    if(reason.contains("413") || reason.contains("40005") || reason.contains("too large",Qt::CaseInsensitive) || reason.contains("maximum file",Qt::CaseInsensitive))return "Your files are too powerful. Discord rejected the attachment size for this account or server. Choose a smaller file.";
    if(reason.contains("403") || reason.contains("denied",Qt::CaseInsensitive))return "You do not have permission to attach files in this channel.";
    if(reason.contains("401") || reason.contains("session",Qt::CaseInsensitive))return "Discord rejected this sign-in. Sign in again before uploading.";
    if(reason.contains("429") || reason.contains("slow down",Qt::CaseInsensitive))return "Discord asked Relay to slow down. Wait a moment before sending again.";
    if(reason.contains("404") || reason.contains("no longer",Qt::CaseInsensitive))return "This channel is no longer available.";
    return "Discord could not confirm the upload. Check the conversation before sending again; your draft and attachments are still here.";
}
void AttachmentUpload::resetActive() {
    if(m_reply){auto *reply=m_reply.data();m_reply.clear();reply->disconnect(this);reply->abort();reply->deleteLater();}
    m_openFiles.clear();m_activeFiles.clear();m_cloudFiles.clear();m_activeChannel.clear();m_content.clear();m_nonce.clear();m_posting=false;m_uploadIndex=0;m_progress=0;m_sentBytes=m_uploadBytes=0;
}
void AttachmentUpload::finish(bool sent) {
    const auto channel=m_activeChannel;if(sent)m_drafts.remove(channel);resetActive();m_client->m_sendingMessage=false;m_status=sent?"Attachments sent":"";emit changed();emit m_client->stateChanged();
}
void AttachmentUpload::fail(const QString &reason) {const auto content=m_content;finish(false);m_error=reason;emit changed();emit m_client->messageSendFailed(content,reason);}
void AttachmentUpload::cancel(){if(!cancelable())return;if(m_preparing){cancelPreparation();m_status.clear();m_error="Image preparation canceled. Your draft and attachments are still here.";emit changed();return;}++m_operation;finish(false);m_error="Upload canceled. Your draft and attachments are still here.";emit changed();}

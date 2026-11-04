#include "AttachmentUpload.h"
#include "DiscordClient.h"
#include <QFile>
#include <QBuffer>
#include <QDir>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QMimeData>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

// Generated files, generated account records and private localhost HTTP peers.
// No user's files, live account, credentials or Discord endpoint is accessed.
class UploadHttpPeer final : public QTcpServer {
public:
    struct Request { QByteArray method,path,header,body; qint64 size=0; };
    struct Response { int status=200;QByteArray body;int delay=0;QByteArray extra; };
    QList<Request> requests;
    std::function<Response(const Request &)> respond;
    bool streaming=false;
    qint64 largestChunk=0;
    UploadHttpPeer() {
        connect(this,&QTcpServer::newConnection,this,[this]{while(auto *socket=nextPendingConnection()){
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            auto request=std::make_shared<Request>();auto header=std::make_shared<QByteArray>();auto remaining=std::make_shared<qint64>(-1);
            connect(socket,&QTcpSocket::readyRead,this,[this,socket,request,header,remaining]{
                if(socket->property("done").toBool()){socket->readAll();return;}
                auto bytes=socket->readAll();largestChunk=std::max(largestChunk,qint64(bytes.size()));
                if(*remaining<0){header->append(bytes);const auto end=header->indexOf("\r\n\r\n");if(end<0)return;
                    request->header=header->left(end);const auto first=request->header.split('\n').first().trimmed().split(' ');request->method=first.value(0);request->path=first.value(1);*remaining=0;
                    for(const auto &line:request->header.split('\n'))if(line.toLower().startsWith("content-length:"))*remaining=line.mid(15).trimmed().toLongLong();
                    bytes=header->mid(end+4);header->clear();
                }
                request->size+=bytes.size();if(!streaming)request->body+=bytes;
                if(request->size<*remaining)return;
                socket->setProperty("done",true);requests.append(*request);const auto response=respond?respond(*request):Response{};
                QTimer::singleShot(response.delay,socket,[socket,response]{if(socket->state()!=QAbstractSocket::ConnectedState)return;socket->write("HTTP/1.1 "+QByteArray::number(response.status)+" Generated\r\nContent-Type: application/json\r\nConnection: close\r\n"+response.extra+"Content-Length: "+QByteArray::number(response.body.size())+"\r\n\r\n"+response.body);socket->disconnectFromHost();});
            });
        }});
    }
    QString base() const {return "http://127.0.0.1:"+QString::number(serverPort());}
};

class AttachmentUploadTest final : public QObject {
    Q_OBJECT
        static constexpr qint64 MiB=1024*1024;
    static void configure(DiscordClient &client,UploadHttpPeer &api) {
        client.m_apiBase=api.base();client.m_token="generated-upload-test-token-only";client.m_connected=true;
        client.m_currentUserId="42";client.m_currentChannelId="61";client.m_currentChannelKind="dm";
        client.m_users.insert("42",QJsonObject{{"id","42"},{"username","Generated self"},{"premium_type",0}});
        client.m_dmObjects.insert("61",QJsonObject{{"id","61"},{"type",1},{"recipients",QJsonArray{QJsonObject{{"id","84"},{"username","Generated recipient"}}}}});
    }
    static QUrl generatedFile(QTemporaryDir &directory,const QString &name,qint64 bytes=20) {
        const auto path=directory.filePath(name);QFile file(path);if(!file.open(QIODevice::WriteOnly) || !file.resize(bytes))return {};file.close();return QUrl::fromLocalFile(path);
    }
    static QImage generatedImage() {
        QImage image(16,12,QImage::Format_ARGB32);
        for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)image.setPixelColor(x,y,QColor(x*15,y*20,120,(x+y)*9));
        return image;
    }
    static QByteArray encodedImage(const QImage &image,const char *format="PNG") {
        QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);image.save(&buffer,format);return bytes;
    }
    static QByteArray generatedAnimatedGif() {
        // Two generated 16x8 red/blue frames, matching no external asset.
        return QByteArray::fromHex("47494638396110000800810000ff000000000000000000000021ff0b4e45545343415045322e30030100000021f90400040000002c00000000100008000008150001081c48b0a0c18308132a5cc8b0a1c3870803020021f90401040001002c0000000010000800810000ff00000000000000000008150001081c48b0a0c18308132a5cc8b0a1c387080302003b");
    }
    static void route(UploadHttpPeer &api,UploadHttpPeer &cloud,int delay=0) {
        api.respond=[&cloud,delay](const UploadHttpPeer::Request &request){
            const auto body=QJsonDocument::fromJson(request.body).object();
            if(request.path.endsWith("/attachments")) {
                QJsonArray rows;for(const auto &entry:body.value("files").toArray()) {const auto file=entry.toObject();const auto id=file.value("id").toString();rows.append(QJsonObject{{"id",id},{"upload_url",cloud.base()+"/upload/"+id+"?generated-signature"},{"upload_filename","generated-slot/"+file.value("filename").toString()}});}
                return UploadHttpPeer::Response{200,QJsonDocument(QJsonObject{{"attachments",rows}}).toJson(),delay,{}};
            }
            return UploadHttpPeer::Response{200,QJsonDocument(QJsonObject{{"id","987654321"},{"channel_id",QString::fromUtf8(request.path.split('/').value(2))},{"content",body.value("content")},{"attachments",body.value("attachments")},{"author",QJsonObject{{"id","42"},{"username","Generated self"}}},{"timestamp","2026-10-25T12:00:00Z"}}).toJson(),delay,{}};
        };
    }
private slots:
    void clipboardOrdinaryTextLinksAndTextualPathsFallThrough() {
        UploadHttpPeer api;DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);
        QVERIFY(!uploads.pasteMimeData(nullptr));
        for(const auto &text:{QString("A normal message **with markdown**"),QString("https://example.invalid/image.png"),QString("/tmp/generated-image.png"),QString("file:///tmp/generated-image.png")}) {
            QMimeData mime;mime.setText(text);QVERIFY(!uploads.pasteMimeData(&mime));QCOMPARE(mime.text(),text);
        }
        QMimeData remote;remote.setUrls({QUrl("https://example.invalid/video.mp4")});remote.setText("https://example.invalid/video.mp4");QVERIFY(!uploads.pasteMimeData(&remote));
        QMimeData unsupported;unsupported.setData("application/x-generated-unsupported","generated");QVERIFY(!uploads.pasteMimeData(&unsupported));
        QCOMPARE(uploads.count(),0);QVERIFY(!uploads.busy());QVERIFY(uploads.error().isEmpty());QVERIFY(api.requests.isEmpty());
    }
    void clipboardLocalFileUrlsPreserveOriginalsAndTakePriorityOverThumbnail() {
        QTemporaryDir directory;UploadHttpPeer api;DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);
        QList<QUrl> files;for(const auto &name:{"generated.pdf","generated.zip","generated.mp4","generated.unknown"})files.append(generatedFile(directory,name));
        const auto gif=generatedFile(directory,"generated.gif",0);QFile original(gif.toLocalFile());QVERIFY(original.open(QIODevice::WriteOnly));const auto gifBytes=generatedAnimatedGif();QCOMPARE(original.write(gifBytes),gifBytes.size());original.close();files.append(gif);
        QMimeData mime;mime.setUrls(files);mime.setText("copied file names");mime.setImageData(generatedImage());QVERIFY(uploads.pasteMimeData(&mime));QCOMPARE(uploads.count(),5);QVERIFY(!uploads.busy());
        const auto added=uploads.m_drafts.value("61");for(int i=0;i<files.size();++i){QCOMPARE(added[i].path,files[i].toLocalFile());QVERIFY(!added[i].staging);}
        uploads.clearDraft();for(const auto &url:files)QVERIFY(QFileInfo::exists(url.toLocalFile()));QVERIFY(api.requests.isEmpty());
    }
    void clipboardFileMimeErrorsConsumeAtomicallyRatherThanPastingPaths() {
        QTemporaryDir directory;UploadHttpPeer api;DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);const auto original=generatedFile(directory,"original.bin");QVERIFY(uploads.addFiles({original}));
        for(const auto &urls:{QList<QUrl>{original,QUrl("https://example.invalid/file")},QList<QUrl>{QUrl::fromLocalFile(directory.path())},QList<QUrl>{QUrl::fromLocalFile(directory.filePath("missing.bin"))},QList<QUrl>{generatedFile(directory,"too-large.bin",21*MiB)}}) {
            QMimeData mime;mime.setUrls(urls);QVERIFY(uploads.pasteMimeData(&mime));QCOMPARE(uploads.count(),1);QVERIFY(!uploads.error().isEmpty());
        }
        QMimeData excessive;QList<QUrl> many;for(int i=0;i<10;++i)many.append(original);excessive.setUrls(many);QVERIFY(uploads.pasteMimeData(&excessive));QCOMPARE(uploads.count(),1);QVERIFY(uploads.error().contains("10 files"));
        client.m_connected=false;QMimeData disconnected;disconnected.setUrls({original});QVERIFY(uploads.pasteMimeData(&disconnected));QVERIFY(uploads.error().contains("reconnect"));QCOMPARE(uploads.count(),1);QVERIFY(api.requests.isEmpty());
    }
    void clipboardGnomeCopiedFilesAlwaysCopiesEvenForCut() {
        QTemporaryDir directory;UploadHttpPeer api;DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);const auto original=generatedFile(directory,"generated with spaces.zip");
        QMimeData mime;mime.setData("x-special/gnome-copied-files","cut\n"+original.toEncoded()+"\n");QVERIFY(uploads.pasteMimeData(&mime));QCOMPARE(uploads.count(),1);QCOMPARE(uploads.m_drafts.value("61").first().path,original.toLocalFile());uploads.clearDraft();QVERIFY(QFileInfo::exists(original.toLocalFile()));
    }
    void clipboardScreenshotStagesLosslessAlphaPrivatelyWithoutSending() {
        UploadHttpPeer api;DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);QMimeData mime;const auto image=generatedImage();mime.setImageData(image);
        QVERIFY(uploads.pasteMimeData(&mime));QVERIFY(uploads.busy());QVERIFY(uploads.cancelable());QVERIFY(uploads.status().contains("Preparing"));QTRY_VERIFY(!uploads.busy());QCOMPARE(uploads.count(),1);QVERIFY(api.requests.isEmpty());
        const auto staged=uploads.m_drafts.value("61").first();QVERIFY(staged.staging);QCOMPARE(staged.name,"image.png");QCOMPARE(staged.mime,"image/png");QCOMPARE(QImage(staged.path).convertToFormat(QImage::Format_ARGB32),image);
#ifdef Q_OS_UNIX
        const auto forbidden=QFileDevice::ReadGroup|QFileDevice::WriteGroup|QFileDevice::ExeGroup|QFileDevice::ReadOther|QFileDevice::WriteOther|QFileDevice::ExeOther;
        QVERIFY(!(QFileInfo(staged.path).permissions()&forbidden));QVERIFY(!(QFileInfo(QFileInfo(staged.path).absolutePath()).permissions()&forbidden));
#endif
        const auto row=uploads.attachments().first().toMap();QCOMPARE(QUrl(row.value("previewUrl").toString()).toLocalFile(),staged.path);QVERIFY(api.requests.isEmpty());
    }
    void clipboardEncodedRasterOnlyPreservesOriginalBytesAndAnimationPreference() {
        UploadHttpPeer api;DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);
        for(const auto &type:{QPair<QString,QByteArray>{"image/png",encodedImage(generatedImage())},QPair<QString,QByteArray>{"image/jpeg",encodedImage(generatedImage(),"JPEG")},QPair<QString,QByteArray>{"image/gif",generatedAnimatedGif()}}) {
            QVERIFY(!type.second.isEmpty());QMimeData mime;mime.setData(type.first,type.second);if(type.first=="image/gif")mime.setData("image/png",encodedImage(generatedImage()));
            QVERIFY(uploads.pasteMimeData(&mime));QTRY_VERIFY(!uploads.busy());QCOMPARE(uploads.count(),1);const auto staged=uploads.m_drafts.value("61").first();QCOMPARE(staged.mime,type.first);QFile file(staged.path);QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),type.second);
            QCOMPARE(QUrl(uploads.attachments().first().toMap().value("previewUrl").toString()).toLocalFile(),staged.path);
            if(type.first=="image/gif"){QImageReader reader(staged.path);QVERIFY(reader.supportsAnimation());QCOMPARE(reader.imageCount(),2);}
            file.close();uploads.clearDraft();
        }
        QVERIFY(api.requests.isEmpty());
    }
    void draftPreviewKeepsDimensionAndFileSizeLimits() {
        QTemporaryDir directory;UploadHttpPeer api;DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);
        for(const auto &size:{QSize(4097,1),QSize(4096,4096),QSize(32,32)}) {
            const auto path=directory.filePath(QString::number(size.width())+"x"+QString::number(size.height())+".png");
            QImage image(size,QImage::Format_RGB32);image.fill(Qt::blue);QVERIFY(image.save(path));
            if(size==QSize(32,32)){QFile file(path);QVERIFY(file.open(QIODevice::Append));QVERIFY(file.resize(9*MiB));}
            QVERIFY(uploads.addFiles({QUrl::fromLocalFile(path)}));
            QCOMPARE(uploads.count(),1);QVERIFY(uploads.attachments().first().toMap().value("previewUrl").toString().isEmpty());
            uploads.clearDraft();
        }
        QVERIFY(api.requests.isEmpty());
    }
    void clipboardInvalidOversizedAndBusyImagesAreConsumedWithVisibleErrors() {
        UploadHttpPeer api;DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);
        QMimeData invalid;invalid.setData("image/png","generated invalid raster");QVERIFY(uploads.pasteMimeData(&invalid));QTRY_VERIFY(!uploads.busy());QCOMPARE(uploads.count(),0);QVERIFY(uploads.error().contains("could not be opened"));
        QMimeData tooWide;tooWide.setImageData(QImage(32769,1,QImage::Format_RGB32));QVERIFY(uploads.pasteMimeData(&tooWide));QVERIFY(!uploads.busy());QVERIFY(uploads.error().contains("too large"));
        QMimeData tooManyBytes;tooManyBytes.setData("image/png",QByteArray(21*MiB,'x'));QVERIFY(uploads.pasteMimeData(&tooManyBytes));QVERIFY(!uploads.busy());QVERIFY(uploads.error().contains("upload limit"));
        QMimeData image;image.setImageData(generatedImage());QVERIFY(uploads.pasteMimeData(&image));QVERIFY(uploads.pasteMimeData(&image));QVERIFY(uploads.error().contains("Wait"));
        QMimeData text;text.setText("still text while preparing");QVERIFY(!uploads.pasteMimeData(&text));QTRY_VERIFY(!uploads.busy());QCOMPARE(uploads.count(),1);QVERIFY(api.requests.isEmpty());
    }
    void clipboardPendingPreparationCannotFollowNavigationAccountOrCancellation() {
        for(int change=0;change<4;++change) {
            UploadHttpPeer api;DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);QMimeData mime;mime.setImageData(generatedImage());QVERIFY(uploads.pasteMimeData(&mime));
            if(change==0){client.m_currentChannelId="62";emit client.stateChanged();client.m_currentChannelId="61";emit client.stateChanged();}
            if(change==1){++client.m_generation;emit client.stateChanged();}
            if(change==2){client.m_currentUserId="99";emit client.stateChanged();}
            if(change==3)uploads.cancel();
            uploads.m_clipboardPool.waitForDone();QCoreApplication::sendPostedEvents(&uploads,QEvent::MetaCall);QVERIFY(!uploads.busy());QCOMPARE(uploads.count(),0);QVERIFY(uploads.m_drafts.isEmpty());QVERIFY(api.requests.isEmpty());
            QVERIFY(uploads.pasteMimeData(&mime));QTRY_COMPARE(uploads.count(),1);QVERIFY(uploads.error().isEmpty());
        }
    }
    void clipboardStageCleanupMatchesDraftOwnershipAndNeverTouchesOriginals() {
        UploadHttpPeer api;DiscordClient client;configure(client,api);QMimeData mime;mime.setImageData(generatedImage());QString destroyedPath;
        {
            AttachmentUpload uploads(&client);QVERIFY(uploads.pasteMimeData(&mime));QTRY_COMPARE(uploads.count(),1);auto path=uploads.m_drafts.value("61").first().path;auto id=uploads.attachments().first().toMap().value("id").toString();
            client.m_currentChannelId="62";emit client.stateChanged();QVERIFY(QFileInfo::exists(path));client.m_currentChannelId="61";emit client.stateChanged();uploads.removeAttachment(id);QVERIFY(!QFileInfo::exists(path));QVERIFY(!QFileInfo::exists(QFileInfo(path).absolutePath()));
            QVERIFY(uploads.pasteMimeData(&mime));QTRY_COMPARE(uploads.count(),1);path=uploads.m_drafts.value("61").first().path;uploads.clearDraft();QVERIFY(!QFileInfo::exists(path));
            QVERIFY(uploads.pasteMimeData(&mime));QTRY_COMPARE(uploads.count(),1);path=uploads.m_drafts.value("61").first().path;++client.m_generation;emit client.stateChanged();QCOMPARE(uploads.count(),0);QVERIFY(!QFileInfo::exists(path));
            QVERIFY(uploads.pasteMimeData(&mime));QTRY_COMPARE(uploads.count(),1);destroyedPath=uploads.m_drafts.value("61").first().path;
        }
        QVERIFY(!QFileInfo::exists(destroyedPath));QVERIFY(!QFileInfo::exists(QFileInfo(destroyedPath).absolutePath()));
    }
    void clipboardStageIsOwnedBeforeObserversSeeTheAttachment() {
        UploadHttpPeer api;DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);QMimeData mime;mime.setImageData(generatedImage());QString path;
        connect(&uploads,&AttachmentUpload::changed,this,[&]{if(uploads.count()){path=uploads.m_drafts.value("61").first().path;uploads.clearDraft();}});
        QVERIFY(uploads.pasteMimeData(&mime));QTRY_VERIFY(!uploads.busy());QVERIFY(!path.isEmpty());QCOMPARE(uploads.count(),0);QVERIFY(!uploads.m_drafts.contains("61"));QVERIFY(!QFileInfo::exists(path));
    }
    void clipboardStageSurvivesFailedAndCanceledUploadsThenCleansOnSuccess() {
        UploadHttpPeer api,cloud;QVERIFY(api.listen(QHostAddress::LocalHost));QVERIFY(cloud.listen(QHostAddress::LocalHost));route(api,cloud,50);DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);uploads.m_fixtureUploadOrigin=QUrl(cloud.base());QMimeData mime;mime.setImageData(generatedImage());QVERIFY(uploads.pasteMimeData(&mime));QTRY_COMPARE(uploads.count(),1);const auto path=uploads.m_drafts.value("61").first().path;
        QVERIFY(uploads.send("Generated message"));uploads.cancel();QVERIFY(QFileInfo::exists(path));QCOMPARE(uploads.count(),1);QTest::qWait(100);QCOMPARE(cloud.requests.size(),0);
        const auto handler=api.respond;api.respond=[handler](const UploadHttpPeer::Request &request){return request.path.endsWith("/messages")?UploadHttpPeer::Response{500,"{}"}:handler(request);};QVERIFY(uploads.send("Generated message"));QTRY_VERIFY(!uploads.busy());QCOMPARE(uploads.count(),1);QVERIFY(QFileInfo::exists(path));
        api.respond=handler;QSignalSpy sent(&client,&DiscordClient::messageSent);QVERIFY(uploads.send("Generated message"));QTRY_COMPARE(sent.size(),1);QCOMPARE(uploads.count(),0);QVERIFY(!QFileInfo::exists(path));QVERIFY(!QFileInfo::exists(QFileInfo(path).absolutePath()));
    }
    void metadataDraftsFileTypesLimitsAndNoTransmissionBeforeSend() {
        QTemporaryDir directory;UploadHttpPeer api;QVERIFY(api.listen(QHostAddress::LocalHost));DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);
        const auto file=generatedFile(directory,"archive.weird");QVERIFY(uploads.addFiles({file}));QCOMPARE(uploads.count(),1);QVERIFY(api.requests.isEmpty());
        const auto row=uploads.attachments().first().toMap();QCOMPARE(row.value("name").toString(),"archive.weird");QVERIFY(row.value("previewUrl").toString().isEmpty());
        QCOMPARE(uploads.fileLimit(),20*MiB);uploads.setSpoiler(row.value("id").toString(),true);QVERIFY(uploads.attachments().first().toMap().value("spoiler").toBool());
        client.m_currentChannelId="62";emit client.stateChanged();QCOMPARE(uploads.count(),0);QVERIFY(uploads.addFiles({generatedFile(directory,"image.png")}));QCOMPARE(uploads.count(),1);
        client.m_currentChannelId="61";emit client.stateChanged();QCOMPARE(uploads.count(),1);QCOMPARE(uploads.attachments().first().toMap().value("name").toString(),"archive.weird");
        QVERIFY(!uploads.addFiles({QUrl("https://example.invalid/file")}));QVERIFY(!uploads.addFiles({QUrl::fromLocalFile(directory.path())}));
        QVERIFY(!uploads.addFiles({generatedFile(directory,"too-large.bin",21*MiB)}));QVERIFY(uploads.error().contains("limit"));QCOMPARE(uploads.count(),1);
        QVariantList tooMany;for(int i=0;i<10;++i)tooMany.append(file);QVERIFY(!uploads.addFiles(tooMany));QCOMPARE(uploads.count(),1);
        uploads.removeAttachment(row.value("id").toString());QCOMPARE(uploads.count(),0);
    }
    void nativeCloudFlowStreamsAboveMultipartCapWithNoAuthorizationOnPut() {
        QTemporaryDir directory;UploadHttpPeer api,cloud;QVERIFY(api.listen(QHostAddress::LocalHost));QVERIFY(cloud.listen(QHostAddress::LocalHost));cloud.streaming=true;route(api,cloud);
        DiscordClient client;configure(client,api);client.m_users["42"].insert("premium_type",2);AttachmentUpload uploads(&client);uploads.m_fixtureUploadOrigin=QUrl(cloud.base());
        const auto file=generatedFile(directory,"large-generated.bin",32*MiB);QVERIFY(uploads.addFiles({file}));QSignalSpy sent(&client,&DiscordClient::messageSent);QVERIFY(uploads.send(""));
        QTRY_COMPARE_WITH_TIMEOUT(sent.size(),1,15000);QCOMPARE(api.requests.size(),2);QCOMPARE(cloud.requests.size(),1);QCOMPARE(cloud.requests.first().size,32*MiB);
        QVERIFY(cloud.requests.first().body.isEmpty());QVERIFY(!cloud.requests.first().header.toLower().contains("authorization:"));QVERIFY(!cloud.requests.first().header.toLower().contains("cookie:"));
        QVERIFY(api.requests.first().header.contains("Authorization: generated-upload-test-token-only"));
        const auto payload=QJsonDocument::fromJson(api.requests.last().body).object();QCOMPARE(payload.value("content").toString(),"");QCOMPARE(payload.value("attachments").toArray().size(),1);
        QCOMPARE(payload.value("attachments").toArray().first().toObject().value("uploaded_filename").toString(),"generated-slot/large-generated.bin");QVERIFY(payload.value("allowed_mentions").toObject().value("parse").toArray().isEmpty());
        QVERIFY(payload.value("enforce_nonce").toBool());QVERIFY(!uploads.busy());QCOMPARE(uploads.count(),0);QVERIFY(!client.sendingMessage());QCOMPARE(client.messages()->rowCount(),1);
    }
    void changedFileFailsBeforeAllocation() {
        QTemporaryDir directory;UploadHttpPeer api;QVERIFY(api.listen(QHostAddress::LocalHost));DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);
        const auto file=generatedFile(directory,"generated.bin");QVERIFY(uploads.addFiles({file}));QFile changed(file.toLocalFile());QVERIFY(changed.open(QIODevice::Append));changed.write("changed");changed.close();
        QVERIFY(!uploads.send("Generated text"));QVERIFY(api.requests.isEmpty());QCOMPARE(uploads.count(),1);QVERIFY(!uploads.busy());QVERIFY(uploads.error().contains("changed"));
    }
    void invalidCloudDestinationFailsClosedBeforeAnyFileBytes() {
        QTemporaryDir directory;UploadHttpPeer api;QVERIFY(api.listen(QHostAddress::LocalHost));
        api.respond=[](const UploadHttpPeer::Request &){return UploadHttpPeer::Response{200,QJsonDocument(QJsonObject{{"attachments",QJsonArray{QJsonObject{{"id","0"},{"upload_url","https://evil.invalid/upload"},{"upload_filename","generated-slot/file"}}}}}).toJson()};};
        DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);QVERIFY(uploads.addFiles({generatedFile(directory,"generated.bin")}));QVERIFY(uploads.send("Generated text"));
        QTRY_VERIFY(!uploads.busy());QCOMPARE(api.requests.size(),1);QVERIFY(uploads.error().contains("invalid attachment"));QCOMPARE(uploads.count(),1);QVERIFY(!client.sendingMessage());
        for(const auto &url:{QUrl("http://discord-attachments-uploads-prd.storage.googleapis.com/x"),QUrl("https://u:p@discord-attachments-uploads-prd.storage.googleapis.com/x"),QUrl("https://discord-attachments-uploads-prd.storage.googleapis.com.evil.invalid/x"),QUrl("https://discord-attachments-uploads-prd.storage.googleapis.com:8443/x")})QVERIFY(!uploads.validUploadUrl(url));
        QVERIFY(uploads.validUploadUrl(QUrl("https://discord-attachments-uploads-prd.storage.googleapis.com/generated-slot/file?signature=generated")));
    }
    void cancelDuringAllocationSuppressesLateReplyAndPreservesDraft() {
        QTemporaryDir directory;UploadHttpPeer api,cloud;QVERIFY(api.listen(QHostAddress::LocalHost));QVERIFY(cloud.listen(QHostAddress::LocalHost));route(api,cloud,80);
        DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);uploads.m_fixtureUploadOrigin=QUrl(cloud.base());QVERIFY(uploads.addFiles({generatedFile(directory,"generated.bin")}));QVERIFY(uploads.send("Generated text"));QVERIFY(uploads.cancelable());uploads.cancel();
        QTest::qWait(150);QCOMPARE(cloud.requests.size(),0);QCOMPARE(api.requests.size(),1);QCOMPARE(uploads.count(),1);QVERIFY(!uploads.busy());QVERIFY(!client.sendingMessage());
    }
    void accountChangeCancelsWithoutLeakingDraftOrSendingFinalMessage() {
        QTemporaryDir directory;UploadHttpPeer api,cloud;QVERIFY(api.listen(QHostAddress::LocalHost));QVERIFY(cloud.listen(QHostAddress::LocalHost));route(api,cloud,80);
        DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);uploads.m_fixtureUploadOrigin=QUrl(cloud.base());QVERIFY(uploads.addFiles({generatedFile(directory,"generated.bin")}));QVERIFY(uploads.send("Generated text"));
        client.logout();client.m_currentUserId="99";client.m_currentChannelId="61";emit client.stateChanged();QTest::qWait(150);
        QVERIFY(!uploads.busy());QCOMPARE(uploads.count(),0);QCOMPARE(cloud.requests.size(),0);QVERIFY(!client.sendingMessage());
    }
    void changingConversationDoesNotRetargetUploadsOrClearAnotherDraft() {
        QTemporaryDir directory;UploadHttpPeer api,cloud;QVERIFY(api.listen(QHostAddress::LocalHost));QVERIFY(cloud.listen(QHostAddress::LocalHost));route(api,cloud,40);
        DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);uploads.m_fixtureUploadOrigin=QUrl(cloud.base());QVERIFY(uploads.addFiles({generatedFile(directory,"one.bin")}));
        client.m_currentChannelId="62";emit client.stateChanged();QVERIFY(uploads.addFiles({generatedFile(directory,"two.bin")}));client.m_currentChannelId="61";emit client.stateChanged();
        QSignalSpy sent(&client,&DiscordClient::messageSent);QVERIFY(uploads.send("Generated text"));client.m_currentChannelId="62";emit client.stateChanged();QTRY_COMPARE(sent.size(),1);
        QCOMPARE(api.requests.last().path,QByteArray("/channels/61/messages"));QCOMPARE(uploads.count(),1);QCOMPARE(uploads.attachments().first().toMap().value("name").toString(),"two.bin");QCOMPARE(client.messages()->rowCount(),0);
    }
    void serverLimitAndRateErrorsKeepDraftAndNeverAutomaticallyRetry() {
        for(const int status:{413,429}){
            QTemporaryDir directory;UploadHttpPeer api;QVERIFY(api.listen(QHostAddress::LocalHost));api.respond=[status](const UploadHttpPeer::Request &){return UploadHttpPeer::Response{status,status==413?R"({"code":40005,"message":"Request entity too large"})":R"({"retry_after":0.01})"};};
            DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);QVERIFY(uploads.addFiles({generatedFile(directory,"generated.bin")}));QVERIFY(uploads.send("Generated text"));QTRY_VERIFY(!uploads.busy());QTest::qWait(80);
            QCOMPARE(api.requests.size(),1);QCOMPARE(uploads.count(),1);QVERIFY(uploads.error().contains(status==413?"size":"slow down"));QVERIFY(!client.sendingMessage());
        }
    }
    void cancelDuringPutStopsFinalMessageAndClosesTheFile() {
        QTemporaryDir directory;UploadHttpPeer api,cloud;QVERIFY(api.listen(QHostAddress::LocalHost));QVERIFY(cloud.listen(QHostAddress::LocalHost));route(api,cloud);
        cloud.respond=[](const UploadHttpPeer::Request &){return UploadHttpPeer::Response{200,{},150,{}};};
        DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);uploads.m_fixtureUploadOrigin=QUrl(cloud.base());QVERIFY(uploads.addFiles({generatedFile(directory,"generated.bin")}));QVERIFY(uploads.send("Generated text"));
        QTRY_COMPARE(cloud.requests.size(),1);QVERIFY(uploads.cancelable());QVERIFY(!uploads.m_openFiles.empty());uploads.cancel();QVERIFY(uploads.m_openFiles.empty());QTest::qWait(220);
        QCOMPARE(api.requests.size(),1);QCOMPARE(uploads.count(),1);QVERIFY(!uploads.busy());QVERIFY(!client.sendingMessage());
    }
    void cloudRedirectNeverFollowsOrSendsFinalMessage() {
        QTemporaryDir directory;UploadHttpPeer api,cloud,other;QVERIFY(api.listen(QHostAddress::LocalHost));QVERIFY(cloud.listen(QHostAddress::LocalHost));QVERIFY(other.listen(QHostAddress::LocalHost));route(api,cloud);
        cloud.respond=[&other](const UploadHttpPeer::Request &){return UploadHttpPeer::Response{307,{},0,"Location: "+other.base().toUtf8()+"/forbidden\r\n"};};
        DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);uploads.m_fixtureUploadOrigin=QUrl(cloud.base());QVERIFY(uploads.addFiles({generatedFile(directory,"generated.bin")}));QVERIFY(uploads.send("Generated text"));QTRY_VERIFY(!uploads.busy());
        QTest::qWait(60);QCOMPARE(other.requests.size(),0);QCOMPARE(api.requests.size(),1);QCOMPARE(uploads.count(),1);QVERIFY(!uploads.error().isEmpty());
    }
    void unknownEntitlementsDeferToDiscordAndKnownBoostRaisesStandardLimit() {
        UploadHttpPeer api;DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);QCOMPARE(uploads.fileLimit(),20*MiB);
        client.m_currentGuildId="91";QCOMPARE(uploads.fileLimit(),qint64(0));client.m_guildObjects.insert("91",QJsonObject{{"id","91"},{"premium_tier",3}});QCOMPARE(uploads.fileLimit(),100*MiB);
        client.m_users["42"].remove("premium_type");QCOMPARE(uploads.fileLimit(),qint64(0));client.m_users["42"].insert("premium_type",2);QCOMPARE(uploads.fileLimit(),1024*MiB);
    }
    void missingPermissionsAndPostFailurePreserveDraftWithoutRetry() {
        for(const int status:{403,500}){
            QTemporaryDir directory;UploadHttpPeer api,cloud;QVERIFY(api.listen(QHostAddress::LocalHost));QVERIFY(cloud.listen(QHostAddress::LocalHost));route(api,cloud);auto handler=api.respond;
            api.respond=[handler,status](const UploadHttpPeer::Request &request){if(request.path.endsWith("/messages"))return UploadHttpPeer::Response{status,QJsonDocument(QJsonObject{{"message","Generated failure"}}).toJson()};return handler(request);};
            DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);uploads.m_fixtureUploadOrigin=QUrl(cloud.base());QVERIFY(uploads.addFiles({generatedFile(directory,"generated.bin")}));QSignalSpy sent(&client,&DiscordClient::messageSent);QVERIFY(uploads.send("Generated text"));QTRY_VERIFY(!uploads.busy());QTest::qWait(60);
            QCOMPARE(api.requests.size(),2);QCOMPARE(sent.size(),0);QCOMPARE(uploads.count(),1);QVERIFY(!client.sendingMessage());QVERIFY(uploads.error().contains(status==403?"permission":"confirm"));
        }
    }
    void multipleFilesSpoilersAndResponseIdsAreMatchedRatherThanAssumedOrdered() {
        QTemporaryDir directory;UploadHttpPeer api,cloud;QVERIFY(api.listen(QHostAddress::LocalHost));QVERIFY(cloud.listen(QHostAddress::LocalHost));route(api,cloud);
        auto defaultHandler=api.respond;api.respond=[defaultHandler](const UploadHttpPeer::Request &request){auto response=defaultHandler(request);if(request.path.endsWith("/attachments")){auto object=QJsonDocument::fromJson(response.body).object();auto rows=object.value("attachments").toArray();QJsonArray reversed;for(qsizetype i=rows.size();i>0;--i)reversed.append(rows[i-1]);object.insert("attachments",reversed);response.body=QJsonDocument(object).toJson();}return response;};
        DiscordClient client;configure(client,api);AttachmentUpload uploads(&client);uploads.m_fixtureUploadOrigin=QUrl(cloud.base());QVERIFY(uploads.addFiles({generatedFile(directory,"SPOILER_one.bin"),generatedFile(directory,"two.pdf")}));QSignalSpy sent(&client,&DiscordClient::messageSent);QVERIFY(uploads.send("Generated text"));QTRY_COMPARE(sent.size(),1);
        QCOMPARE(cloud.requests.size(),2);QVERIFY(cloud.requests.first().path.startsWith("/upload/0?"));QVERIFY(cloud.requests.last().path.startsWith("/upload/1?"));
        const auto files=QJsonDocument::fromJson(api.requests.last().body).object().value("attachments").toArray();QCOMPARE(files.first().toObject().value("filename").toString(),"SPOILER_one.bin");
    }
};
QTEST_GUILESS_MAIN(AttachmentUploadTest)
#include "attachment_upload_test.moc"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QSet>
#include <QtCore/private/qzipreader_p.h>
#include <QTimer>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <stdexcept>
#include <iostream>
#include <memory>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
struct UpdateProgress {
    QProgressDialog* dialog=nullptr;
    void setLabelText(const QString& text) { if(dialog)dialog->setLabelText(text); }
    bool wasCanceled() const {return dialog && dialog->wasCanceled();}
};
static void require(bool ok,const char* error) { if(!ok) throw std::runtime_error(error); }
static QByteArray read(const QString& path) { QFile f(path);require(f.open(QIODevice::ReadOnly),"Cannot read update file");return f.readAll(); }
static bool releaseId(const QString& id) {
    bool serial=false;id.mid(17).toULongLong(&serial);
    return id.size()<=64 && serial && QRegularExpression("^[0-9]{8}T[0-9]{6}Z-[0-9]+$").match(id).hasMatch();
}
static bool versionLess(const QString& a,const QString& b) {
    return a.left(16)==b.left(16) ? a.mid(17).toULongLong()<b.mid(17).toULongLong() : a.left(16)<b.left(16);
}
static QString version(const QString& root) { return QJsonDocument::fromJson(read(root+"/build-info.json")).object()["release_id"].toString(); }
static void verify(const QByteArray& message,const QByteArray& signature,const QByteArray& pem=read(":/updater/release-key.pem")) {
    BIO* bio=BIO_new_mem_buf(pem.data(),int(pem.size()));
    EVP_PKEY* key=PEM_read_bio_PUBKEY(bio,nullptr,nullptr,nullptr); BIO_free(bio);
    EVP_MD_CTX* ctx=EVP_MD_CTX_new();
    bool ok=key && ctx && EVP_DigestVerifyInit(ctx,nullptr,nullptr,nullptr,key)==1 &&
        EVP_DigestVerify(ctx,reinterpret_cast<const unsigned char*>(signature.data()),signature.size(),
        reinterpret_cast<const unsigned char*>(message.data()),message.size())==1;
    EVP_MD_CTX_free(ctx);EVP_PKEY_free(key);require(ok,"The Windows update signature is invalid");
}
static QUrl assetUrl(const QString& tag,const QString& name) {
    require(!tag.isEmpty() && !tag.contains('/') && !tag.contains('\\') && tag!="..","Invalid release tag");
    return QUrl("https://github.com/AmazingPVPr/Relay/releases/download/"+QString::fromLatin1(QUrl::toPercentEncoding(tag))+"/"+name);
}
static QByteArray fetch(QNetworkAccessManager& network,const QUrl& url,qint64 limit,int timeout,
                        UpdateProgress& progress,QIODevice* sink=nullptr,QCryptographicHash* hash=nullptr) {
    QNetworkRequest request(url);request.setRawHeader("User-Agent","Relay-Windows-Updater/1");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(15000);
    auto reply=network.get(request);QEventLoop loop;QTimer timer;timer.setSingleShot(true);
    QByteArray data; qint64 count=0;bool failed=false;
    auto consume=[&]{auto chunk=reply->readAll();count+=chunk.size();
        if(count>limit || (sink && sink->write(chunk)!=chunk.size())) {failed=true;reply->abort();return;}
        if(hash)hash->addData(chunk);if(!sink)data+=chunk;
    };
    QObject::connect(reply,&QIODevice::readyRead,&loop,consume);
    QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);
    QObject::connect(&timer,&QTimer::timeout,reply,&QNetworkReply::abort);
    QMetaObject::Connection cancel;
    if(progress.dialog)cancel=QObject::connect(progress.dialog,&QProgressDialog::canceled,reply,&QNetworkReply::abort);
    timer.start(timeout);loop.exec();consume();QObject::disconnect(cancel);
    bool ok=!failed && reply->error()==QNetworkReply::NoError && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()==200;
    reply->deleteLater();require(ok,"Windows update could not be downloaded; the installed version is still available");return data;
}
static void extract(const QString& archive,const QString& destination,UpdateProgress& progress) {
    QZipReader zip(archive);auto entries=zip.fileInfoList();
    require(zip.isReadable() && zip.status()==QZipReader::NoError && !entries.isEmpty() && entries.size()<=30000,"Invalid Windows ZIP archive");
    QSet<QString> seen;qint64 total=0;
    for(const auto& entry:entries) {
        auto name=entry.filePath;
        require(entry.isValid() && !entry.isSymLink && (name.startsWith("Relay/") || (entry.isDir && name=="Relay")) && !name.contains('\\') && !name.contains(':') && !name.contains(QChar(0)),"Unsafe archive entry");
        for(const auto& part:name.split('/')) {
            require(part!=".." && part!="." && !part.endsWith('.') && !part.endsWith(' ') &&
                !QRegularExpression("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\\.|$)",QRegularExpression::CaseInsensitiveOption).match(part).hasMatch(),"Unsafe Windows archive path");
        }
        auto normalized=QDir::cleanPath(name).toCaseFolded();
        require(!seen.contains(normalized),"Duplicate Windows archive path");seen.insert(normalized);
        require(entry.size>=0 && entry.size<=256LL*1024*1024,"Archive entry is too large");total+=entry.size;
        require(total<=4LL*1024*1024*1024,"Windows archive is too large");
    }
    for(const auto& entry:entries) {
        QApplication::processEvents();require(!progress.wasCanceled(),"Update installation was canceled");
        auto path=destination+"/"+entry.filePath;
        if(entry.isDir) {require(QDir().mkpath(path),"Cannot create update directory");continue;}
        require(QDir().mkpath(QFileInfo(path).path()),"Cannot create update directory");
        auto bytes=zip.fileData(entry.filePath);require(zip.status()==QZipReader::NoError && bytes.size()==entry.size,"Cannot read Windows archive entry");
        QSaveFile file(path);require(file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size() && file.commit(),"Cannot extract Windows update file");
    }
}
static QString update(const QString& base,const QString& cache,UpdateProgress& progress,bool checkOnly=false,QString* available=nullptr,bool strictErrors=false) {
    QString selected=base;QString selectedId=version(base);
    require(releaseId(selectedId),"Missing Windows build version");
    QFile pointer(cache+"/current.txt");
    if(pointer.open(QIODevice::ReadOnly)) {
        QString id=QString::fromUtf8(pointer.readAll()).trimmed();QString candidate=cache+"/"+id+"/Relay";
        try { if(releaseId(id) && versionLess(selectedId,id) && QFile::exists(candidate+"/bin/relay.exe") && version(candidate)==id) { selected=candidate;selectedId=id; } } catch(const std::exception&) { /* Fall back to the complete bundled build. */ }
    }
    if(QApplication::arguments().contains("--no-update"))return selected;
    QLockFile lock(cache+"/update.lock");if(!lock.tryLock(0))return selected;
    try {
        QNetworkAccessManager network;
        auto listing=QJsonDocument::fromJson(fetch(network,QUrl("https://api.github.com/repos/AmazingPVPr/Relay/releases?per_page=100"),2*1024*1024,10000,progress));
        require(listing.isArray(),"Invalid Windows release listing");
        QString tag;
        // Releases may be previews. Linux-only releases never hide a Windows update.
        for(auto value:listing.array()) {
            auto release=value.toObject();if(release["draft"].toBool())continue;
            QStringList names;for(auto a:release["assets"].toArray())names<<a.toObject()["name"].toString();
            if(names.contains("relay-update-windows.json") && names.contains("relay-update-windows.sig") && names.contains("Relay-windows-x86_64.zip")) {tag=release["tag_name"].toString();break;}
        }
        require(!tag.isEmpty(),"No Windows update has been published yet");
        auto message=fetch(network,assetUrl(tag,"relay-update-windows.json"),65536,10000,progress);
        auto signature=fetch(network,assetUrl(tag,"relay-update-windows.sig"),1024,10000,progress);verify(message,signature);
        auto metadata=QJsonDocument::fromJson(message).object();auto id=metadata["release_id"].toString();auto digest=metadata["sha256"].toString();
        require(releaseId(id) && metadata["platform"]=="windows-x86_64" && metadata["archive"]=="Relay-windows-x86_64.zip" &&
                QRegularExpression("^[0-9a-f]{64}$").match(digest).hasMatch(),"Invalid Windows update metadata");
        QFile::remove(cache+"/last-update-error.txt");
        if(!versionLess(selectedId,id))return selected;
        if(available)*available=id;
        if(checkOnly)return selected;
        progress.setLabelText("Downloading Relay for Windows…");
        QTemporaryDir temp(cache+"/download-XXXXXX");require(temp.isValid(),"Cannot create update folder");
        QString archive=temp.path()+"/Relay.zip";QSaveFile output(archive);require(output.open(QIODevice::WriteOnly),"Cannot save Windows update");
        QCryptographicHash hash(QCryptographicHash::Sha256);
        fetch(network,assetUrl(tag,"Relay-windows-x86_64.zip"),1024LL*1024*1024,600000,progress,&output,&hash);
        require(hash.result().toHex()==digest.toLatin1(),"Windows update checksum does not match");require(output.commit(),"Cannot save verified update");
        progress.setLabelText("Installing Relay update…");
        extract(archive,temp.path()+"/unpacked",progress);
        QString staged=temp.path()+"/unpacked/Relay";
        require(QFile::exists(staged+"/bin/relay.exe") && version(staged)==id,"Windows package version does not match its signed update");
        // Publish the complete version directory before atomically switching its pointer.
        QString destination=cache+"/"+id;
        if(QFile::exists(destination)) {
            require(version(destination+"/Relay")==id && QFile::exists(destination+"/Relay/bin/relay.exe"),"Incomplete Windows update folder; installed version retained");
        } else require(QDir().rename(temp.path()+"/unpacked",destination),"Cannot install Windows update");
        QSaveFile next(cache+"/current.txt");require(next.open(QIODevice::WriteOnly),"Cannot activate Windows update");
        require(next.write(id.toUtf8())==id.toUtf8().size() && next.commit(),"Cannot activate Windows update");selected=destination+"/Relay";
        QFile::remove(cache+"/last-update-error.txt");
    } catch(const std::exception& error) {
        QFile log(cache+"/last-update-error.txt");if(log.open(QIODevice::WriteOnly))log.write(error.what());
        if(strictErrors)throw;
    }
    return selected;
}
int main(int argc,char** argv) {
    bool command=false;
    for(int i=1;i<argc;++i)if(QByteArray(argv[i])=="--check" || QByteArray(argv[i])=="--install" || QByteArray(argv[i])=="--json")command=true;
    std::unique_ptr<QCoreApplication> application;
    if(command)application=std::make_unique<QCoreApplication>(argc,argv);
    else application=std::make_unique<QApplication>(argc,argv);
    QCoreApplication::setOrganizationName("Relay");QCoreApplication::setApplicationName("Relay Windows Updates");
    auto args=QCoreApplication::arguments().mid(1);
    auto base=qEnvironmentVariable("RELAY_INSTALL_ROOT");
    if(base.isEmpty())base=QDir::cleanPath(QCoreApplication::applicationDirPath()+"/..");
    const QString cache=QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)+"/Relay/WindowsUpdates";
    QDir().mkpath(cache);
    try {
        const int restart=args.indexOf("--restart-after");
        if(restart>=0) {
            bool valid=false;const auto pid=args.value(restart+1).toULong(&valid);require(valid && pid>1,"Invalid restart process");
#ifdef Q_OS_WIN
            HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,DWORD(pid));
            if(process) {auto result=WaitForSingleObject(process,30000);CloseHandle(process);require(result==WAIT_OBJECT_0,"Relay is still closing. Open it again when it has closed.");}
            else require(GetLastError()==ERROR_INVALID_PARAMETER,"Could not wait for Relay to close. Open it again manually.");
#endif
            args.removeAt(restart+1);args.removeAt(restart);
        }
        if(command) {
            UpdateProgress progress;QString available;
            const auto selected=update(base,cache,progress,args.contains("--check"),&available,true);
            QJsonObject result{{"current_version",version(selected)},{"available_version",available},{"runtime_path",selected},{"error",""}};
            std::cout<<QJsonDocument(result).toJson(QJsonDocument::Compact).constData()<<std::endl;
            return 0;
        }
        QProgressDialog dialog("Checking for Relay updates…","Skip update",0,0);
        dialog.setWindowTitle("Relay");dialog.setMinimumDuration(700);dialog.setAutoClose(false);
        UpdateProgress progress{&dialog};auto selected=update(base,cache,progress);dialog.hide();
        args.removeAll("--no-update");
        qputenv("RELAY_INSTALL_ROOT",base.toUtf8());
        require(QProcess::startDetached(selected+"/bin/relay.exe",args,selected),"Relay could not start");return 0;
    } catch(const std::exception& error) {
        if(command) {
            QJsonObject result{{"current_version",QFile::exists(base+"/build-info.json")?version(base):QString()},{"available_version",""},{"runtime_path",base},{"error",QString::fromUtf8(error.what())}};
            std::cout<<QJsonDocument(result).toJson(QJsonDocument::Compact).constData()<<std::endl;
            return 2;
        }
        QMessageBox::critical(nullptr,"Relay",QString::fromUtf8(error.what()));return 1;
    }
}

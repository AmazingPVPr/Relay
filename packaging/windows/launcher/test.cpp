#define main relay_updater_main
#include "updater.cpp"
#undef main
#include <iostream>
#include <QtCore/private/qzipwriter_p.h>
int main(int argc,char** argv) {
    std::cerr<<"Starting Windows updater fixture\n";
    QCoreApplication app(argc,argv);std::cerr<<"Core application ready\n";
    try {
        require(releaseId("20261104T000000Z-123") && !releaseId("../../bad") && !releaseId("20261104T000000Z-123/evil"),"Version path validation failed");
        require(versionLess("20261104T000000Z-99","20261104T000000Z-100") && !versionLess("20261104T000000Z-100","20261104T000000Z-99"),"Numeric release order failed");
        std::cerr<<"Signing key fixture\n"; auto key=EVP_PKEY_Q_keygen(nullptr,nullptr,"ED25519"); require(key,"Cannot generate fixture signing key");
        auto bio=BIO_new(BIO_s_mem());PEM_write_bio_PUBKEY(bio,key);char* raw=nullptr;long length=BIO_get_mem_data(bio,&raw);QByteArray pem(raw,length);BIO_free(bio);
        QByteArray message="signed Windows update fixture";QByteArray signature(64,0);size_t count=64;auto ctx=EVP_MD_CTX_new();
        require(EVP_DigestSignInit(ctx,nullptr,nullptr,nullptr,key)==1 && EVP_DigestSign(ctx,reinterpret_cast<unsigned char*>(signature.data()),&count,reinterpret_cast<const unsigned char*>(message.data()),message.size())==1,"Cannot sign fixture");
        EVP_MD_CTX_free(ctx);EVP_PKEY_free(key);signature.resize(count);verify(message,signature,pem);
        bool rejected=false;try {verify(message+"tampered",signature,pem);}catch(...) {rejected=true;}require(rejected,"Tampered metadata accepted");
        rejected=false;signature[0]^=1;try {verify(message,signature,pem);}catch(...) {rejected=true;}require(rejected,"Tampered signature accepted");
        rejected=false;try{assetUrl("../bad","Relay-windows-x86_64.zip");}catch(...){rejected=true;}require(rejected,"Unsafe tag accepted");
        std::cerr<<"Signature checks passed\n";QTemporaryDir dir;QString base=dir.path()+"/base",cache=dir.path()+"/cache";QDir().mkpath(base+"/bin");QDir().mkpath(cache);
        auto write=[](QString name,QByteArray bytes){QFile file(name);require(file.open(QIODevice::WriteOnly),"Fixture write failed");file.write(bytes);};
        write(base+"/build-info.json",R"({"release_id":"20261103T000000Z-1"})");
        UpdateProgress progress; // update.lock prevents network calls in these generated fixtures.
        QLockFile lock(cache+"/update.lock");require(lock.tryLock(),"Fixture lock failed");
        write(cache+"/current.txt","../../escape");require(update(base,cache,progress)==base,"Unsafe cache pointer accepted");
        QString newer="20261104T000000Z-2",candidate=cache+"/"+newer+"/Relay";QDir().mkpath(candidate+"/bin");
        write(candidate+"/bin/relay.exe","fixture");write(candidate+"/build-info.json",R"({"release_id":"20261104T000000Z-2"})");
        write(cache+"/current.txt",newer.toUtf8());require(update(base,cache,progress)==candidate,"Installed update not selected");
        write(candidate+"/build-info.json","corrupt");require(update(base,cache,progress)==base,"Corrupt cached update did not fall back");
        write(base+"/build-info.json",R"({"release_id":"20261105T000000Z-3"})");
        write(candidate+"/build-info.json",R"({"release_id":"20261104T000000Z-2"})");require(update(base,cache,progress)==base,"Cached update downgraded newer bundle");
        std::cerr<<"Cache checks passed\n";auto archive=dir.path()+"/fixture.zip";
        {QZipWriter writer(archive);writer.addFile("Relay/bin/relay.exe",QByteArray("generated fixture"));writer.addFile("Relay/empty.txt",QByteArray());writer.close();}
        extract(archive,dir.path()+"/unpacked",progress);require(read(dir.path()+"/unpacked/Relay/bin/relay.exe")=="generated fixture","Native ZIP extraction failed");
        for(auto name:QStringList{"Relay/../../escape","Relay/bin/CON.txt","Relay/bin/foo:bar","Relay/bin/trailing.","Relay/bin/../escape"}) {
            {QZipWriter writer(archive);writer.addFile(name,QByteArray("malicious fixture"));writer.close();}
            rejected=false;try{extract(archive,dir.path()+"/rejected",progress);}catch(...){rejected=true;}require(rejected,"Unsafe ZIP path accepted");
        }
        {QZipWriter writer(archive);writer.addSymLink("Relay/link","../../escape");writer.close();}
        rejected=false;try{extract(archive,dir.path()+"/rejected",progress);}catch(...){rejected=true;}require(rejected,"ZIP symlink accepted");
        require(!QFile::exists(dir.path()+"/escape"),"ZIP escaped extraction directory");
        if(argc==4) {
            auto message=read(QString::fromLocal8Bit(argv[1]));verify(message,read(QString::fromLocal8Bit(argv[2])));
            auto metadata=QJsonDocument::fromJson(message).object();auto archivePath=QString::fromLocal8Bit(argv[3]);
            require(metadata["platform"]=="windows-x86_64" && metadata["archive"]=="Relay-windows-x86_64.zip","Wrong signed package platform");
            require(QCryptographicHash::hash(read(archivePath),QCryptographicHash::Sha256).toHex()==metadata["sha256"].toString().toLatin1(),"Signed package checksum failed");
            extract(archivePath,dir.path()+"/signed-package",progress);
            require(version(dir.path()+"/signed-package/Relay")==metadata["release_id"].toString(),"Signed package version failed");
            require(QFile::exists(dir.path()+"/signed-package/Relay/Relay.exe") && QFile::exists(dir.path()+"/signed-package/Relay/bin/relay-launcher.exe"),"Signed package launcher missing");
            std::cout<<"PASS: actual signed Windows release, checksum, native extraction and version\n";
        }
        std::cout<<"PASS: signature, tampering, version/tag paths, cached update, corrupt cache fallback, downgrade protection, native ZIP extraction and unsafe paths\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}
}

#include "BroadcastTransport.h"
#include "StreamEngine.h"
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <atomic>

struct BroadcastTransport::Private {
    explicit Private(BroadcastTransport *owner):q(owner) {}
    BroadcastTransport *q;
    QThread thread;
    QObject *context=nullptr;
    StreamEngine *engine=nullptr;
    QTimer *sample=nullptr;
    mutable QMutex mutex;
    struct State {
        bool available=false,active=false,connected=false,encrypted=false;
        QString key,status,error,summary;
        QVariantMap diagnostics;
    } state;
    std::atomic<quint64> frames{0},generation{0};
    quint64 workerGeneration=0;
    bool stopping=false; // Worker thread only.
    void refresh(){
        State next{engine->available(),engine->active(),engine->connected(),engine->encryptedReady(),
            engine->streamKey(),engine->status(),engine->error(),engine->diagnosticSummary(),engine->publishingDiagnostics()};
        frames=engine->frameCount();
        QMutexLocker lock(&mutex);state=std::move(next);
    }
    void notify(){
        refresh();
        const auto expected=workerGeneration;
        QMetaObject::invokeMethod(q,[this,expected]{if(generation==expected)emit q->changed();},Qt::QueuedConnection);
    }
    template<class Function> void run(Function &&function){
        // Only bounded commands enter this queue. Media is acknowledged before
        // the producer submits another frame, so GUI stalls cannot accumulate
        // unbounded queued Qt events or retain frames from an old session.
        if(QThread::currentThread()==context->thread()){function();return;}
        QMetaObject::invokeMethod(context,std::forward<Function>(function),Qt::BlockingQueuedConnection);
    }
};

BroadcastTransport::BroadcastTransport(QObject *parent):QObject(parent),d(std::make_unique<Private>(this)){
    d->thread.setObjectName("Relay broadcast transport");
    d->context=new QObject;
    d->context->moveToThread(&d->thread);
    connect(&d->thread,&QThread::finished,d->context,&QObject::deleteLater);
    d->thread.start();
    d->run([this]{
        // Construct every socket and timer in its final thread. No multimedia
        // device, QObject child or active timer is moved across threads.
        d->engine=new StreamEngine(d->context);
        connect(d->engine,&StreamEngine::changed,d->engine,[this]{d->notify();});
        connect(d->engine,&StreamEngine::diagnosticsChanged,d->engine,[this]{d->refresh();});
        connect(d->engine,&StreamEngine::keyFrameRequested,d->engine,[this]{
            const auto expected=d->workerGeneration;
            QMetaObject::invokeMethod(this,[this,expected]{if(d->generation==expected)emit keyFrameRequested();},Qt::QueuedConnection);
        });
        connect(d->engine,&StreamEngine::publishingBitrateChanged,d->engine,[this](int bitrate){
            d->refresh();const auto expected=d->workerGeneration;
            QMetaObject::invokeMethod(this,[this,expected,bitrate]{
                if(d->generation==expected)emit publishingBitrateChanged(bitrate);
            },Qt::QueuedConnection);
        });
        connect(d->engine,&StreamEngine::stopRequested,d->engine,[this](const QString &key){
            if(d->stopping)return;
            const auto expected=d->workerGeneration;
            QMetaObject::invokeMethod(this,[this,expected,key]{if(d->generation==expected)emit stopRequested(key);},Qt::QueuedConnection);
        });
        d->sample=new QTimer(d->context);
        d->sample->setInterval(20);
        connect(d->sample,&QTimer::timeout,d->engine,[this]{d->frames=d->engine->frameCount();});
        d->refresh();
    });
}
BroadcastTransport::~BroadcastTransport(){
    ++d->generation;
    d->run([this]{d->stopping=true;delete d->sample;d->sample=nullptr;delete d->engine;d->engine=nullptr;});
    d->thread.quit();d->thread.wait();
}
bool BroadcastTransport::available()const{QMutexLocker lock(&d->mutex);return d->state.available;}
bool BroadcastTransport::active()const{QMutexLocker lock(&d->mutex);return d->state.active;}
bool BroadcastTransport::connected()const{QMutexLocker lock(&d->mutex);return d->state.connected;}
bool BroadcastTransport::encryptedReady()const{QMutexLocker lock(&d->mutex);return d->state.encrypted;}
QString BroadcastTransport::streamKey()const{QMutexLocker lock(&d->mutex);return d->state.key;}
QString BroadcastTransport::status()const{QMutexLocker lock(&d->mutex);return d->state.status;}
QString BroadcastTransport::error()const{QMutexLocker lock(&d->mutex);return d->state.error;}
QString BroadcastTransport::diagnosticSummary()const{QMutexLocker lock(&d->mutex);return d->state.summary;}
QVariantMap BroadcastTransport::publishingDiagnostics()const{QMutexLocker lock(&d->mutex);return d->state.diagnostics;}
quint64 BroadcastTransport::frameCount()const{return d->frames.load();}
int BroadcastTransport::publishingBitrate()const{QMutexLocker lock(&d->mutex);return d->state.diagnostics.value("targetBitrate").toInt();}
void BroadcastTransport::setIdentity(const QString &user,const QString &session){d->run([&]{d->engine->setIdentity(user,session);});}
void BroadcastTransport::publish(const QString &key,int width,int height,int fps,int bitrate){
    const auto generation=++d->generation;
    d->run([&]{
        // Replacement is intentional: its old-session stop event must never
        // arrive later and cancel the newly requested stream.
        d->stopping=true;d->engine->stop();d->stopping=false;d->workerGeneration=generation;
        d->engine->publish(key,width,height,fps,bitrate);d->sample->start();d->refresh();
    });
}
bool BroadcastTransport::publishVideo(const QByteArray &frame,quint32 timestamp,int width,int height){
    bool accepted=false;
    d->run([&]{accepted=d->engine->publishVideo(frame,timestamp,width,height);});
    return accepted;
}
bool BroadcastTransport::publishAudio(const QByteArray &opus,quint32 timestamp){
    bool accepted=false;
    d->run([&]{accepted=d->engine->publishAudio(opus,timestamp);});
    return accepted;
}
void BroadcastTransport::stop(){
    ++d->generation;
    d->run([this]{d->stopping=true;d->sample->stop();d->engine->stop();d->refresh();d->stopping=false;});
}
void BroadcastTransport::onStreamCreate(const QJsonObject &event){d->run([&]{d->engine->onStreamCreate(event);d->refresh();});}
void BroadcastTransport::onStreamServer(const QJsonObject &event){d->run([&]{d->engine->onStreamServer(event);d->refresh();});}
bool BroadcastTransport::configureLocalFixture(const QUrl &url){
    bool accepted=false;
    d->run([&]{accepted=d->engine->configureLocalFixture(url);});
    return accepted;
}

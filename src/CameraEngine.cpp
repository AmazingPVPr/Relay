#include "CameraEngine.h"
#include "VoiceEngine.h"
#include "StreamDecoder.h"
#include <QDateTime>
#include <QVideoSink>

CameraEngine::CameraEngine(VoiceEngine *voice,QObject *parent):QObject(parent),m_voice(voice){
    m_poll.setInterval(10);m_poll.setTimerType(Qt::PreciseTimer);
    connect(&m_poll,&QTimer::timeout,this,&CameraEngine::poll);
    connect(voice,&VoiceEngine::changed,this,[this]{
        if(m_active && !m_voice->connected())reset();
        else if(m_active)emit changed();
    });
    connect(voice,&VoiceEngine::cameraChanged,this,[this]{
        if(m_active && m_voice->cameraUserId()!=m_user)reset();
    });
    connect(voice,&VoiceEngine::cameraDiscontinuity,this,[this]{
        if(!m_active)return;
        if(m_decoder)m_decoder->discardUntilKeyFrame();
        m_receiving=false;emit changed();
    });
    connect(voice,&VoiceEngine::cameraFrameReceived,this,[this](const QByteArray &frame,quint32 stamp,const QString &codec,int rotation,bool mirrored){
        if(!m_active || m_voice->cameraUserId()!=m_user || !m_voice->encryptedReady())return;
        m_rotation=rotation;m_mirrored=mirrored;
        if(!m_decoder || codec!=m_codec){m_codec=codec;m_decoder=std::make_unique<StreamDecoder>(codec);m_decoder->discardUntilKeyFrame();}
        if(!m_decoder->submit(frame,stamp))m_voice->requestCameraKeyframe();
    });
}
CameraEngine::~CameraEngine(){stop();}
bool CameraEngine::available()const{return m_voice->available() && (StreamDecoder::supports("AV1") || StreamDecoder::supports("H265") || StreamDecoder::supports("H264") || StreamDecoder::supports("VP8"));}
bool CameraEngine::connected()const{return m_active && m_voice->connected();}
QString CameraEngine::status()const{
    if(!m_error.isEmpty())return "Camera unavailable";
    if(!m_active)return "Not watching a camera";
    if(!m_voice->encryptedReady())return "Securing camera video…";
    return m_receiving?"Live camera · end-to-end encrypted":"Waiting for camera video…";
}
QObject *CameraEngine::videoSink()const{return m_sink;}
void CameraEngine::setVideoSink(QObject *sink){
    auto *video=qobject_cast<QVideoSink *>(sink);if(m_sink==video)return;
    if(m_sink)m_sink->setVideoFrame({});
    m_sink=video;emit videoSinkChanged();
}
void CameraEngine::reset(){
    const bool announced=m_announced;m_announced=false;
    m_poll.stop();m_active=m_receiving=false;m_decoder.reset();m_user.clear();m_title.clear();m_codec.clear();
    m_width=m_height=m_rotation=0;m_mirrored=false;m_frames=0;m_lastFrame=0;if(m_sink)m_sink->setVideoFrame({});emit changed();if(announced)emit stopRequested();
}
void CameraEngine::watch(const QString &id,const QString &label){
    if(m_active && m_user==id)return;
    stop();m_error.clear();
    if(!available() || !m_voice->connected()){m_error="Join this person's voice channel to watch their camera.";emit changed();return;}
    m_user=id;m_title=label.left(256);m_active=true;
    if(!m_voice->watchCamera(id)){reset();m_error="This camera is no longer available.";emit changed();return;}
    m_poll.start();m_voice->requestCameraKeyframe();m_announced=true;emit watchRequested();emit changed();
}
void CameraEngine::stop(){
    m_active=false;m_voice->stopCamera();m_error.clear();reset();
}
void CameraEngine::poll(){
    if(!m_active)return;
    if(m_decoder){
        auto snapshot=m_decoder->take();
        if(!snapshot.error.isEmpty()){m_error=snapshot.error;m_active=false;m_voice->stopCamera();reset();return;}
        if(snapshot.keyFrameNeeded)m_voice->requestCameraKeyframe();
        if(snapshot.frame.isValid()){
            snapshot.frame.setRotation(QtVideo::Rotation(m_rotation));snapshot.frame.setMirrored(m_mirrored);
            m_width=m_rotation%180?snapshot.height:snapshot.width;m_height=m_rotation%180?snapshot.width:snapshot.height;
            m_frames=snapshot.decoded;m_receiving=true;m_lastFrame=QDateTime::currentMSecsSinceEpoch();
            if(m_sink)m_sink->setVideoFrame(snapshot.frame);
            emit changed();
        }
    }
    if(m_receiving && QDateTime::currentMSecsSinceEpoch()-m_lastFrame>4000){m_receiving=false;emit changed();m_voice->requestCameraKeyframe();}
}

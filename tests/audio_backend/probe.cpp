#include "AppSettings.h"
#include "SoundEffects.h"
#include <QAudioDevice>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaDevices>
#include <QSocketNotifier>
#include <QSoundEffect>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <memory>
#include <unistd.h>

int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);app.setApplicationName("RelayAudioBackendQA");
    const auto socket=qEnvironmentVariable("RELAY_AUDIO_TEST_SOCKET");
    const auto sinkId=qgetenv("RELAY_AUDIO_TEST_SINK");
    if(socket.isEmpty() || !QFileInfo(socket).isAbsolute() || !QFileInfo::exists(socket)
        || qEnvironmentVariable("PULSE_SERVER")!="unix:"+socket
        || qgetenv("QT_AUDIO_BACKEND")!="pulseaudio" || !sinkId.startsWith("relay_backend_test_")) {
        std::fprintf(stderr,"Private test endpoint required; refusing playback.\n");return 2;
    }
    QAudioDevice device;
    const auto outputs=QMediaDevices::audioOutputs();
    if(outputs.size()!=1 || outputs.first().id()!=sinkId) {
        std::fprintf(stderr,"Expected exactly the private virtual sink; refusing playback.\n");return 2;
    }
    device=outputs.first();
    const bool service=argc==2 && QByteArray(argv[1])=="service";
    if(argc!=2 || (!service && QByteArray(argv[1])!="effect"))return 2;
    QTemporaryDir settingsDirectory;
    AppSettings settings(settingsDirectory.filePath("settings.ini"));
    settings.setOutputDeviceId(QString::fromLatin1(sinkId.toBase64()));
    settings.setSoundEffectsVolume(.5);
    SoundEffects cues(&settings);
    std::unique_ptr<QSoundEffect> oldEffect;
    if(!service) {
        oldEffect=std::make_unique<QSoundEffect>(device);
        oldEffect->setLoopCount(QSoundEffect::Infinite);oldEffect->setVolume(.5f);
        oldEffect->setSource(QUrl("qrc:/relay/sounds/join.wav"));
    }
    bool started=false,finished=false;int requests=0;
    QTimer repeated;
    repeated.setInterval(900);
    const auto playCue=[&]{cues.play("join");if(++requests==4)repeated.stop();};
    QObject::connect(&repeated,&QTimer::timeout,&app,playCue);
    // The runner arms playback only after applying private event-role policy.
    QSocketNotifier input(STDIN_FILENO,QSocketNotifier::Read);
    QObject::connect(&input,&QSocketNotifier::activated,&app,[&]{
        char command=0;if(::read(STDIN_FILENO,&command,1)!=1 || command!='P' || started)return;
        input.setEnabled(false);started=true;
        if(service){playCue();repeated.start();}
        else {
            if(oldEffect->status()!=QSoundEffect::Ready){app.exit(3);return;}
            oldEffect->play();
        }
        QTimer::singleShot(4400,&app,[&]{finished=true;app.quit();});
    });
    QTimer::singleShot(12000,&app,[&]{app.exit(4);});
    const int code=app.exec();
    if(oldEffect)oldEffect->stop();
    const QJsonObject result{{"mode",service?"service":"effect"},{"started",started},{"finished",finished},
        {"error",service?cues.error():QString{}},{"diagnostics",service?cues.diagnosticSummary():QString{}}};
    const auto json=QJsonDocument(result).toJson(QJsonDocument::Compact);
    std::fwrite(json.constData(),1,size_t(json.size()),stdout);std::fputc('\n',stdout);std::fflush(stdout);
    return code;
}

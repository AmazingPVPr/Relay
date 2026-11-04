#pragma once
#include <QProcess>
#include <QTcpServer>
#include <QSize>

// FFmpeg's RTMP server serves generated media to a playback client. No OBS,
// account, physical device or LAN destination is involved.
class RtmpPlaybackFixture {
public:
    QProcess process;
    quint16 port = 0;
    QSize size{320,180};
    int fps = 15, bFrames = 0, durationSeconds = 15, gopSeconds = 1, bitrate = 0, earlyKeyframesSeconds = 0, initialBurstSeconds = 0;
    bool motion = false;
    QString pixelFormat = "yuv420p";
    ~RtmpPlaybackFixture() {
        if (process.state() == QProcess::NotRunning) return;
        process.terminate();
        if (!process.waitForFinished(2000)) { process.kill(); process.waitForFinished(); }
    }
    QString server() const { return "rtmp://127.0.0.1:" + QString::number(port) + "/live"; }
    void start() {
        QTcpServer reserve;
        if (!reserve.listen(QHostAddress::LocalHost, 0)) return;
        port = reserve.serverPort();
        reserve.close();
        QStringList arguments{"-hide_banner", "-loglevel", "error"};
        if (initialBurstSeconds > 0) arguments << "-readrate" << "1" << "-readrate_initial_burst" << QString::number(initialBurstSeconds);
        else arguments << "-re";
        arguments << "-f" << "lavfi" << "-i" << (motion
                ? QString("testsrc2=size=%1x%2:rate=%3,drawbox=x=iw/2-50:y=ih/2-50:w=100:h=100:color=red:t=fill")
                : QString("color=c=red:size=%1x%2:rate=%3")).arg(size.width()).arg(size.height()).arg(fps)
            << "-f" << "lavfi" << "-i" << "sine=frequency=440:sample_rate=44100" << "-t" << QString::number(durationSeconds)
            << "-c:v" << "libx264" << "-preset" << "ultrafast" << "-tune" << "zerolatency" << "-g" << QString::number(fps * gopSeconds) << "-bf" << QString::number(bFrames)
            << "-threads" << "2" << "-pix_fmt" << pixelFormat << "-c:a" << "aac" << "-b:a" << "64k";
        if (earlyKeyframesSeconds > 0) arguments << "-force_key_frames" << QString("expr:lt(t,%1)").arg(earlyKeyframesSeconds);
        if (bitrate > 0) arguments << "-b:v" << QString::number(bitrate) << "-maxrate" << QString::number(bitrate) << "-bufsize" << QString::number(bitrate * 2);
        arguments << "-listen" << "1" << "-f" << "flv" << server() + "/fixture-key";
        process.start("ffmpeg", arguments);
    }
    bool listening() {
        if (!port || process.state() != QProcess::Running) return false;
        QTcpServer probe;
        if (probe.listen(QHostAddress::LocalHost, port)) { probe.close(); return false; }
        return true;
    }
};


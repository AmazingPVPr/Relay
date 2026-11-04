#include "SystemAudioCapture.h"
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
using Microsoft::WRL::ComPtr;
struct SystemAudioCapture::Private {
    std::mutex mutex;
    std::condition_variable wake;
    std::atomic_bool stopping{false};
    Snapshot snapshot;
    std::thread worker;
    void fail() {
        std::lock_guard lock(mutex);
        snapshot.active=false;
        snapshot.error="Windows could not capture output audio. Check your playback device and share again.";
    }
    void run() {
        const auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        if(FAILED(initialized)){fail();return;}
        capture();
        CoUninitialize();
    }
    void capture() {
        // COM objects are created, used and released on this worker thread.
        ComPtr<IMMDeviceEnumerator> devices;
        ComPtr<IMMDevice> output;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> captureClient;
        if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&devices))) ||
           FAILED(devices->GetDefaultAudioEndpoint(eRender,eConsole,&output)) ||
           FAILED(output->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(client.GetAddressOf())))) {fail();return;}
        WAVEFORMATEX format{};
        format.wFormatTag=WAVE_FORMAT_PCM;format.nChannels=2;format.nSamplesPerSec=48000;
        format.wBitsPerSample=16;format.nBlockAlign=4;format.nAvgBytesPerSec=192000;
        const DWORD flags=AUDCLNT_STREAMFLAGS_LOOPBACK|AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if(FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,flags,200000,0,&format,nullptr)) ||
           FAILED(client->GetService(IID_PPV_ARGS(&captureClient))) || FAILED(client->Start())) {fail();return;}
        {std::lock_guard lock(mutex);snapshot.active=true;}
        bool failed=false;
        while(!stopping.load()) {
            UINT32 available=0;
            if(FAILED(captureClient->GetNextPacketSize(&available))){failed=true;break;}
            while(available && !stopping.load()) {
                BYTE *data=nullptr;UINT32 frames=0;DWORD packetFlags=0;
                if(FAILED(captureClient->GetBuffer(&data,&frames,&packetFlags,nullptr,nullptr))){failed=true;break;}
                constexpr qsizetype maximum=48000*4/2;
                const auto bytes=qsizetype(frames)*4,keep=std::min(bytes,maximum);
                {
                    std::lock_guard lock(mutex);
                    if(packetFlags&AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)snapshot.pcm.clear();
                    if(packetFlags&AUDCLNT_BUFFERFLAGS_SILENT)snapshot.pcm.append(QByteArray(keep,'\0'));
                    else if(data)snapshot.pcm.append(reinterpret_cast<const char*>(data)+bytes-keep,keep);
                    if(snapshot.pcm.size()>maximum)snapshot.pcm.remove(0,snapshot.pcm.size()-maximum);
                }
                if(FAILED(captureClient->ReleaseBuffer(frames)) || FAILED(captureClient->GetNextPacketSize(&available))){failed=true;break;}
            }
            if(failed)break;
            std::unique_lock lock(mutex);
            wake.wait_for(lock,std::chrono::milliseconds(5),[this]{return stopping.load();});
        }
        client->Stop();
        if(failed)fail();
        else {std::lock_guard lock(mutex);snapshot.active=false;}
    }
};
SystemAudioCapture::SystemAudioCapture():d(std::make_unique<Private>()){}
SystemAudioCapture::~SystemAudioCapture(){stop();}
bool SystemAudioCapture::start(){
    if(d->worker.joinable())return true;
    {std::lock_guard lock(d->mutex);d->snapshot={};}
    d->stopping=false;d->worker=std::thread([this]{d->run();});return true;
}
void SystemAudioCapture::stop(){
    d->stopping=true;d->wake.notify_all();if(d->worker.joinable())d->worker.join();
    std::lock_guard lock(d->mutex);d->snapshot.pcm.clear();d->snapshot.active=false;
}
SystemAudioCapture::Snapshot SystemAudioCapture::take(){std::lock_guard lock(d->mutex);auto value=d->snapshot;d->snapshot.pcm.clear();return value;}

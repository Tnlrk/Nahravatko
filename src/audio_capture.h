#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>

// Skutečný formát audio endpointu (z GetMixFormat) — předává se FFmpegu jako -ar/-ac/-f.
struct AudioFormatInfo {
    int sampleRate = 48000;
    int channels = 2;
    bool isFloat = true;     // float (f32le) vs celočíselný PCM
    int bitsPerSample = 32;  // pro PCM určuje s16le/s24le/s32le
};

// Zachytávání zvuku přes WASAPI (systémový loopback / mikrofon) + VU metr.
//
// Životní cyklus má dva režimy:
//  • Monitor (VU metr i mimo nahrávání):  startMonitor() … stop()
//  • Nahrávání do roury:                  prepareForRecording() → beginRecording(pipe) … stop()
//
// Veškerá COM/WASAPI práce běží na vlastním vlákně (MTA apartment); formát se po
// otevření zařízení nahlásí zpět (prepareForRecording blokuje, dokud není znám),
// aby z něj recorder engine mohl sestavit FFmpeg argumenty před spuštěním ffmpegu.
class AudioCapture : public QObject
{
    Q_OBJECT
public:
    enum class Source { SystemLoopback, Microphone };

    explicit AudioCapture(QObject* parent = nullptr);
    ~AudioCapture() override;

    void setSource(Source source);
    void setDeviceId(const std::wstring& id);   // prázdné = výchozí zařízení

    AudioFormatInfo formatInfo() const { return m_format; }

    // Spustí monitorovací vlákno pro VU metr (bez zápisu do roury).
    bool startMonitor();

    // Otevře zařízení, zjistí formát a čeká na beginRecording(). Blokuje, dokud
    // není formát k dispozici (nebo otevření selže → vrací false).
    bool prepareForRecording();

    // Po prepareForRecording: předá rouru a uvolní capture smyčku.
    void beginRecording(HANDLE pipe);

    // Zastaví běžící vlákno (monitor i nahrávání) a počká na jeho doběhnutí.
    void stop();

signals:
    void level(float rms);          // 0.0–1.0 pro VU metr
    void error(const QString& message);

private:
    void run();   // tělo capture vlákna

    Source m_source = Source::Microphone;
    std::wstring m_deviceId;
    AudioFormatInfo m_format;

    std::thread m_thread;
    HANDLE m_threadHandle = nullptr;            // pro CancelSynchronousIo
    std::atomic<bool> m_stop{false};
    bool m_recordMode = false;

    // Synchronizace: nahlášení formátu
    std::mutex m_fmtMx;
    std::condition_variable m_fmtCv;
    bool m_fmtReady = false;
    bool m_prepareFailed = false;

    // Brána: capture smyčka čeká na beginRecording()
    std::mutex m_goMx;
    std::condition_variable m_goCv;
    std::atomic<bool> m_go{false};
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
};

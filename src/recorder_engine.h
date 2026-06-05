#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>
#include <string>
#include <windows.h>

#include "audio_capture.h"
#include "video_capture.h"

class QProcess;
class QImage;

// Koordinuje sestavení FFmpeg příkazu, named pipes a běh ffmpeg.exe.
// V této fázi je plně implementováno sestavení argumentů (buildFfmpegArgs);
// reálné spuštění capture vláken + zápis do rour se dokončuje v navazujícím kroku.
class RecorderEngine : public QObject
{
    Q_OBJECT
public:
    enum class Mode { VideoPlusAudio, AudioOnly };
    enum class Quality { High, Medium, Low };

    struct Config {
        Mode mode = Mode::VideoPlusAudio;
        Quality quality = Quality::Medium;

        // video
        bool captureWindow = false;
        HWND window = nullptr;
        HMONITOR monitor = nullptr;
        int sourceWidth = 1920;
        int sourceHeight = 1080;

        // audio
        bool useMicrophone = false;
        bool useSystemAudio = false;
        std::wstring micDeviceId;
        AudioFormatInfo micFormat;
        AudioFormatInfo sysFormat;

        // výstup
        QString outputFile;   // plná cesta včetně přípony
    };

    explicit RecorderEngine(QObject* parent = nullptr);
    ~RecorderEngine() override;

    // Sestaví argumenty pro ffmpeg.exe (bez cesty k programu). Čistá funkce — testovatelná.
    QStringList buildFfmpegArgs(const Config& cfg) const;

    // Cesta k přibalenému ffmpeg.exe (vedle aplikace).
    static QString ffmpegPath();

    bool start(const Config& cfg);
    void stop();
    bool isRecording() const { return m_recording; }

signals:
    void started();
    void stopped(const QString& outputPath);
    void error(const QString& message);
    void videoPreview(const QImage& frame);   // živý náhled z video capture
    void micLevel(float rms);                 // VU mikrofonu během nahrávání
    void sysLevel(float rms);                 // VU systémového zvuku během nahrávání

private:
    static int crfForQuality(Quality q);
    static QString scaleExprForQuality(Quality q); // prázdné = bez škálování

    void teardown();   // úklid po skončení ffmpegu

    bool m_recording = false;
    bool m_stopping = false;
    Config m_cfg;

    QProcess* m_ffmpeg = nullptr;
    std::unique_ptr<VideoCapture> m_vidCap;
    std::unique_ptr<AudioCapture> m_micCap;
    std::unique_ptr<AudioCapture> m_sysCap;
};

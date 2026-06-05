#pragma once

#include <QObject>
#include <QImage>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>

// Zachytávání videa přes Windows.Graphics.Capture (WGC).
//
// Stejný vzor jako AudioCapture: veškerá WinRT/D3D11 práce běží na vlastním vlákně
// (MTA apartment). prepareForRecording() otevře D3D + capture item a zjistí rozměr
// (aby z něj engine sestavil FFmpeg -s WxH), pak čeká na beginRecording(pipe).
//
// Snímky se posílají do roury jako BGRA (FFmpeg je převede na YUV). CFR 30 fps:
// pokud WGC nedodá nový snímek, zapisovací vlákno duplikuje poslední.
class VideoCapture : public QObject
{
    Q_OBJECT
public:
    explicit VideoCapture(QObject* parent = nullptr);
    ~VideoCapture() override;

    void setTargetMonitor(HMONITOR monitor);
    void setTargetWindow(HWND window);

    // Otevře zařízení a capture item, zjistí výstupní rozměr. Blokuje do zjištění
    // (nebo selže → false).
    bool prepareForRecording();
    int outWidth() const { return m_outW; }
    int outHeight() const { return m_outH; }

    // Po prepareForRecording: předá rouru a spustí zachytávání + zápis (CFR).
    void beginRecording(HANDLE pipe, int fps);

    // Spustí WGC session jen pro živý náhled (bez zápisu do roury) — pro kontrolu
    // zdroje před nahráváním. Zastavuje se rovněž přes stop().
    bool startPreview();

    void stop();

signals:
    void previewFrame(const QImage& frame);   // ≤320×180 živý náhled
    void error(const QString& message);

private:
    void run();

    HMONITOR m_monitor = nullptr;
    HWND m_window = nullptr;
    int m_outW = 0;
    int m_outH = 0;
    int m_fps = 30;

    std::thread m_thread;
    HANDLE m_threadHandle = nullptr;
    std::atomic<bool> m_stop{false};

    std::mutex m_fmtMx;
    std::condition_variable m_fmtCv;
    bool m_fmtReady = false;
    bool m_prepareFailed = false;

    std::mutex m_goMx;
    std::condition_variable m_goCv;
    std::atomic<bool> m_go{false};
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    bool m_previewOnly = false;   // jen náhled, bez zápisu do roury

    std::mutex m_frameMx;
    std::vector<unsigned char> m_frameData;   // BGRA, m_outW*m_outH*4
    bool m_frameValid = false;
};

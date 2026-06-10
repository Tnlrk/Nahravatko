#include "audio_capture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>

namespace {

constexpr long long kRefTimesPerSec = 10000000LL; // 100ns jednotky → 1 s buffer

// Detekce float vs PCM z mix formátu (shared mode bývá 32-bit float).
bool formatIsFloat(const WAVEFORMATEX* w)
{
    if (w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(w);
        // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT má Data1 == WAVE_FORMAT_IEEE_FLOAT (3)
        return ext->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
    }
    return false;
}

// RMS úrovně bufferu, normalizováno na 0..1 (s mírným boostem pro čitelnost VU).
float computeRms(const BYTE* data, UINT32 frames, const WAVEFORMATEX* w, bool isFloat)
{
    const UINT32 samples = frames * w->nChannels;
    if (samples == 0) return 0.0f;

    double sumSq = 0.0;
    if (isFloat && w->wBitsPerSample == 32) {
        const auto* f = reinterpret_cast<const float*>(data);
        for (UINT32 i = 0; i < samples; ++i) sumSq += static_cast<double>(f[i]) * f[i];
    } else if (w->wBitsPerSample == 16) {
        const auto* s = reinterpret_cast<const int16_t*>(data);
        for (UINT32 i = 0; i < samples; ++i) {
            const double v = s[i] / 32768.0;
            sumSq += v * v;
        }
    } else {
        return 0.0f;
    }
    const double rms = std::sqrt(sumSq / samples);
    // jednoduchý boost, ať VU rozumně reaguje na běžnou řeč
    return static_cast<float>(std::min(1.0, rms * 3.0));
}

// Zapíše celý buffer do roury; vrací false při chybě (např. ffmpeg zavřel konec).
bool writeAll(HANDLE pipe, const BYTE* data, DWORD bytes)
{
    DWORD total = 0;
    while (total < bytes) {
        DWORD written = 0;
        if (!WriteFile(pipe, data + total, bytes - total, &written, nullptr))
            return false;
        if (written == 0) return false;
        total += written;
    }
    return true;
}

} // namespace

AudioCapture::AudioCapture(QObject* parent) : QObject(parent) {}

AudioCapture::~AudioCapture() { stop(); }

void AudioCapture::setSource(Source source) { m_source = source; }
void AudioCapture::setDeviceId(const std::wstring& id) { m_deviceId = id; }

bool AudioCapture::startMonitor()
{
    if (m_thread.joinable()) return true; // už běží
    m_stop = false;
    m_recordMode = false;
    m_fmtReady = false;
    m_prepareFailed = false;
    m_go = true; // monitor nečeká na bránu
    m_thread = std::thread(&AudioCapture::run, this);
    m_threadHandle = m_thread.native_handle();
    return true;
}

bool AudioCapture::prepareForRecording()
{
    if (m_thread.joinable()) return false;
    m_stop = false;
    m_recordMode = true;
    m_fmtReady = false;
    m_prepareFailed = false;
    m_go = false;
    m_pipe = INVALID_HANDLE_VALUE;
    m_thread = std::thread(&AudioCapture::run, this);
    m_threadHandle = m_thread.native_handle();

    std::unique_lock<std::mutex> lk(m_fmtMx);
    m_fmtCv.wait(lk, [this] { return m_fmtReady || m_prepareFailed; });
    return !m_prepareFailed;
}

void AudioCapture::beginRecording(HANDLE pipe)
{
    {
        std::lock_guard<std::mutex> lk(m_goMx);
        m_pipe = pipe;
        m_go = true;
    }
    m_goCv.notify_all();
}

void AudioCapture::stop()
{
    if (!m_thread.joinable()) return;
    m_stop = true;
    {
        std::lock_guard<std::mutex> lk(m_goMx);
        m_go = true;            // uvolnit bránu, pokud na ní vlákno čeká
    }
    m_goCv.notify_all();
    if (m_threadHandle) {
        // Opakovat zrušení I/O, dokud vlákno neskončí — jedno volání by se mohlo
        // minout s okamžikem, kdy vlákno zrovna v blokujícím I/O není.
        CancelSynchronousIo(m_threadHandle);
        while (WaitForSingleObject(m_threadHandle, 100) == WAIT_TIMEOUT)
            CancelSynchronousIo(m_threadHandle);
    }
    m_thread.join();
    m_threadHandle = nullptr;
    m_pipe = INVALID_HANDLE_VALUE;
}

void AudioCapture::run()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX* wfx = nullptr;
    bool started = false;

    auto fail = [&](const QString& msg) {
        {
            std::lock_guard<std::mutex> lk(m_fmtMx);
            m_prepareFailed = true;
            m_fmtReady = true;
        }
        m_fmtCv.notify_all();
        emit error(msg);
    };

    auto cleanup = [&] {
        if (started && client) client->Stop();
        if (wfx) CoTaskMemFree(wfx);
        if (capture) capture->Release();
        if (client) client->Release();
        if (device) device->Release();
        if (enumerator) enumerator->Release();
        if (m_recordMode && m_pipe != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(m_pipe);
            CloseHandle(m_pipe);  // EOF pro ffmpeg
            m_pipe = INVALID_HANDLE_VALUE;
        }
        CoUninitialize();
    };

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) { fail(tr("Nepodařilo se inicializovat zvuk.")); cleanup(); return; }

    const bool loopback = (m_source == Source::SystemLoopback);
    if (loopback) {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    } else if (!m_deviceId.empty()) {
        hr = enumerator->GetDevice(m_deviceId.c_str(), &device);
    } else {
        hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
    }
    if (FAILED(hr) || !device) {
        fail(tr("Nepodařilo se najít zvukové zařízení.")); cleanup(); return;
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    if (FAILED(hr)) { fail(tr("Nepodařilo se otevřít zvukové zařízení.")); cleanup(); return; }

    hr = client->GetMixFormat(&wfx);
    if (FAILED(hr) || !wfx) { fail(tr("Nepodařilo se zjistit formát zvuku.")); cleanup(); return; }

    const bool isFloat = formatIsFloat(wfx);
    const WORD blockAlign = wfx->nBlockAlign;
    const DWORD sampleRate = wfx->nSamplesPerSec;

    const DWORD streamFlags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                            kRefTimesPerSec, 0, wfx, nullptr);
    if (FAILED(hr)) { fail(tr("Nepodařilo se spustit zachytávání zvuku.")); cleanup(); return; }

    hr = client->GetService(IID_PPV_ARGS(&capture));
    if (FAILED(hr)) { fail(tr("Nepodařilo se spustit zachytávání zvuku.")); cleanup(); return; }

    // --- nahlásit formát ---
    {
        std::lock_guard<std::mutex> lk(m_fmtMx);
        m_format.sampleRate = static_cast<int>(sampleRate);
        m_format.channels = wfx->nChannels;
        m_format.isFloat = isFloat;
        m_format.bitsPerSample = wfx->wBitsPerSample;
        m_fmtReady = true;
    }
    m_fmtCv.notify_all();

    // --- počkat na bránu (režim nahrávání) ---
    if (m_recordMode) {
        std::unique_lock<std::mutex> lk(m_goMx);
        m_goCv.wait(lk, [this] { return m_go.load(); });
    }
    if (m_stop) { cleanup(); return; }

    // --- připojit rouru (ffmpeg ji otevírá jako klient) ---
    if (m_recordMode && m_pipe != INVALID_HANDLE_VALUE) {
        if (!ConnectNamedPipe(m_pipe, nullptr)) {
            const DWORD err = GetLastError();
            if (err != ERROR_PIPE_CONNECTED) {
                // ERROR_OPERATION_ABORTED (stop) nebo jiná chyba → konec
                cleanup();
                return;
            }
        }
    }

    client->Start();
    started = true;

    // pacing + silence padding (jen nahrávání, ať loopback drží reálnou délku)
    LARGE_INTEGER freq{}, startTick{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&startTick);
    long long writtenFrames = 0;

    DWORD lastEmit = 0;
    std::vector<BYTE> silence;

    while (!m_stop) {
        UINT32 packet = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0 && !m_stop) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                break;

            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            const DWORD bytes = frames * blockAlign;

            // VU metr (throttle ~40 ms)
            const DWORD now = GetTickCount();
            if (now - lastEmit >= 40) {
                lastEmit = now;
                emit level(silent ? 0.0f : computeRms(data, frames, wfx, isFloat));
            }

            if (m_recordMode && m_pipe != INVALID_HANDLE_VALUE) {
                bool ok = true;
                if (silent) {
                    if (silence.size() < bytes) silence.resize(bytes, 0);
                    ok = writeAll(m_pipe, silence.data(), bytes);
                } else {
                    ok = writeAll(m_pipe, data, bytes);
                }
                writtenFrames += frames;
                if (!ok) { capture->ReleaseBuffer(frames); m_stop = true; break; }
            }

            capture->ReleaseBuffer(frames);
        }

        // Silence padding: dorovnat na reálný čas, ať loopback nezkrátí délku.
        if (m_recordMode && m_pipe != INVALID_HANDLE_VALUE && !m_stop) {
            LARGE_INTEGER nowTick{};
            QueryPerformanceCounter(&nowTick);
            const double elapsed = double(nowTick.QuadPart - startTick.QuadPart) / double(freq.QuadPart);
            const long long expected = static_cast<long long>(elapsed * sampleRate);
            if (expected > writtenFrames) {
                long long pad = expected - writtenFrames;
                const long long maxPad = sampleRate; // max 1 s naráz
                if (pad > maxPad) pad = maxPad;
                const DWORD padBytes = static_cast<DWORD>(pad) * blockAlign;
                if (silence.size() < padBytes) silence.resize(padBytes, 0);
                if (writeAll(m_pipe, silence.data(), padBytes))
                    writtenFrames += pad;
                else
                    m_stop = true;
            }
        }

        Sleep(10);
    }

    cleanup();
}

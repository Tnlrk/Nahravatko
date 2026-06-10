#include "video_capture.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <d3d11.h>
#include <dxgi.h>
#include <inspectable.h>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;
namespace wdx11 = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {

template <typename T>
winrt::com_ptr<T> getDXGIInterface(winrt::Windows::Foundation::IInspectable const& object)
{
    auto access = object.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<T> result;
    winrt::check_hresult(access->GetInterface(winrt::guid_of<T>(), result.put_void()));
    return result;
}

wdx11::IDirect3DDevice createWinrtDevice(IDXGIDevice* dxgiDevice)
{
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice, inspectable.put()));
    return inspectable.as<wdx11::IDirect3DDevice>();
}

bool writeAll(HANDLE pipe, const unsigned char* data, size_t bytes)
{
    size_t total = 0;
    while (total < bytes) {
        DWORD written = 0;
        if (!WriteFile(pipe, data + total, static_cast<DWORD>(bytes - total), &written, nullptr))
            return false;
        if (written == 0) return false;
        total += written;
    }
    return true;
}

} // namespace

VideoCapture::VideoCapture(QObject* parent) : QObject(parent) {}
VideoCapture::~VideoCapture() { stop(); }

void VideoCapture::setTargetMonitor(HMONITOR monitor) { m_monitor = monitor; m_window = nullptr; }
void VideoCapture::setTargetWindow(HWND window) { m_window = window; m_monitor = nullptr; }

bool VideoCapture::prepareForRecording()
{
    if (m_thread.joinable()) return false;
    m_stop = false;
    m_previewOnly = false;
    m_fmtReady = false;
    m_prepareFailed = false;
    m_go = false;
    m_pipe = INVALID_HANDLE_VALUE;
    m_thread = std::thread(&VideoCapture::run, this);
    m_threadHandle = m_thread.native_handle();

    std::unique_lock<std::mutex> lk(m_fmtMx);
    m_fmtCv.wait(lk, [this] { return m_fmtReady || m_prepareFailed; });
    return !m_prepareFailed;
}

bool VideoCapture::startPreview()
{
    if (m_thread.joinable()) return false;
    m_stop = false;
    m_previewOnly = true;
    m_fmtReady = false;
    m_prepareFailed = false;
    m_go = true;   // náhled nečeká na bránu
    m_pipe = INVALID_HANDLE_VALUE;
    m_thread = std::thread(&VideoCapture::run, this);
    m_threadHandle = m_thread.native_handle();
    return true;
}

void VideoCapture::beginRecording(HANDLE pipe, int fps)
{
    {
        std::lock_guard<std::mutex> lk(m_goMx);
        m_pipe = pipe;
        m_fps = (fps > 0) ? fps : 30;
        m_go = true;
    }
    m_goCv.notify_all();
}

void VideoCapture::stop()
{
    if (!m_thread.joinable()) return;
    m_stop = true;
    {
        std::lock_guard<std::mutex> lk(m_goMx);
        m_go = true;
    }
    m_goCv.notify_all();
    if (m_threadHandle) {
        // Opakovat zrušení I/O, dokud vlákno neskončí (viz AudioCapture::stop).
        CancelSynchronousIo(m_threadHandle);
        while (WaitForSingleObject(m_threadHandle, 100) == WAIT_TIMEOUT)
            CancelSynchronousIo(m_threadHandle);
    }
    m_thread.join();
    m_threadHandle = nullptr;
    m_pipe = INVALID_HANDLE_VALUE;
}

void VideoCapture::run()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    auto fail = [&](const QString& msg) {
        {
            std::lock_guard<std::mutex> lk(m_fmtMx);
            m_prepareFailed = true;
            m_fmtReady = true;
        }
        m_fmtCv.notify_all();
        emit error(msg);
    };

    try {
        // Minimalizované okno nemá obsah → před zachytáváním obnovit.
        if (m_window && IsIconic(m_window)) {
            ShowWindow(m_window, SW_RESTORE);
            Sleep(250);
        }

        // --- D3D11 zařízení (BGRA podpora povinná pro WGC interop) ---
        winrt::com_ptr<ID3D11Device> d3dDevice;
        winrt::com_ptr<ID3D11DeviceContext> d3dContext;
        D3D_FEATURE_LEVEL fl{};
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            d3dDevice.put(), &fl, d3dContext.put());
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                d3dDevice.put(), &fl, d3dContext.put());
        }
        if (FAILED(hr)) { fail(QStringLiteral("Nepodařilo se inicializovat grafiku.")); return; }

        auto dxgiDevice = d3dDevice.as<IDXGIDevice>();
        auto winrtDevice = createWinrtDevice(dxgiDevice.get());

        // --- Capture item z HWND nebo HMONITOR (bez picker UI) ---
        auto factory = winrt::get_activation_factory<wgc::GraphicsCaptureItem>();
        auto interop = factory.as<IGraphicsCaptureItemInterop>();
        wgc::GraphicsCaptureItem item{ nullptr };
        if (m_window) {
            hr = interop->CreateForWindow(
                m_window, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item));
        } else if (m_monitor) {
            hr = interop->CreateForMonitor(
                m_monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item));
        } else {
            fail(QStringLiteral("Není vybrán zdroj obrazu.")); return;
        }
        if (FAILED(hr) || !item) { fail(QStringLiteral("Nepodařilo se otevřít zdroj obrazu.")); return; }

        auto size = item.Size();
        // Výstupní rozměr sudý (yuv420p), ale staging musí mít PŘESNĚ velikost itemu
        // (CopyResource vyžaduje shodu) — proto liché okno = staging na size, výstup zarovnán dolů.
        m_outW = size.Width & ~1;
        m_outH = size.Height & ~1;
        if (m_outW <= 0 || m_outH <= 0) { fail(QStringLiteral("Neplatný rozměr zdroje obrazu.")); return; }

        auto makeStaging = [&](int w, int h) -> winrt::com_ptr<ID3D11Texture2D> {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = static_cast<UINT>(w);
            desc.Height = static_cast<UINT>(h);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            winrt::com_ptr<ID3D11Texture2D> tex;
            d3dDevice->CreateTexture2D(&desc, nullptr, tex.put());
            return tex;
        };

        // --- nahlásit rozměr (pro recording engine) ---
        {
            std::lock_guard<std::mutex> lk(m_fmtMx);
            m_fmtReady = true;
        }
        m_fmtCv.notify_all();

        // --- počkat na bránu (jen režim nahrávání) ---
        if (!m_previewOnly) {
            std::unique_lock<std::mutex> lk(m_goMx);
            m_goCv.wait(lk, [this] { return m_go.load(); });
        }
        if (m_stop) return;

        const auto pixelFormat = wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized;
        auto framePool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrtDevice, pixelFormat, 2, size);
        auto session = framePool.CreateCaptureSession(item);
        try { session.IsBorderRequired(false); } catch (...) {}
        try { session.IsCursorCaptureEnabled(true); } catch (...) {}

        const int outW = m_outW;
        const int outH = m_outH;
        const bool previewOnly = m_previewOnly;
        {
            std::lock_guard<std::mutex> lk(m_frameMx);
            m_frameData.assign(static_cast<size_t>(outW) * outH * 4, 0);
            m_frameValid = false;
        }

        auto staging = makeStaging(size.Width, size.Height);   // PŘESNĚ velikost itemu
        winrt::Windows::Graphics::SizeInt32 poolSize = size;
        std::vector<unsigned char> packBuf(static_cast<size_t>(outW) * outH * 4, 0);
        DWORD lastPreview = 0;
        std::atomic<int> handlerBusy{0};   // počet právě běžících FrameArrived callbacků

        auto onFrame = [&](wgc::Direct3D11CaptureFramePool const& sender, auto&&) {
          // RAII počítadlo — po odregistrování čekáme, až všechny callbacky doběhnou,
          // protože lambda drží lokální proměnné této funkce odkazem.
          struct BusyGuard {
              std::atomic<int>& c;
              explicit BusyGuard(std::atomic<int>& cc) : c(cc) { ++c; }
              ~BusyGuard() { --c; }
          } guard{handlerBusy};
          try {
            auto frame = sender.TryGetNextFrame();
            if (!frame) return;

            auto cs = frame.ContentSize();
            if (cs.Width != poolSize.Width || cs.Height != poolSize.Height) {
                poolSize = cs;
                sender.Recreate(winrtDevice, pixelFormat, 2, cs);
                staging = makeStaging(cs.Width, cs.Height);   // opět přesně velikost obsahu
                return;
            }

            auto texture = getDXGIInterface<ID3D11Texture2D>(frame.Surface());
            if (!staging) return;
            d3dContext->CopyResource(staging.get(), texture.get());

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(d3dContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
                return;

            const int cw = (std::min)(static_cast<int>(cs.Width), outW);
            const int ch = (std::min)(static_cast<int>(cs.Height), outH);
            if (cw < outW || ch < outH)
                std::fill(packBuf.begin(), packBuf.end(), static_cast<unsigned char>(0));
            const auto* src = static_cast<const unsigned char*>(mapped.pData);
            for (int y = 0; y < ch; ++y)
                std::memcpy(packBuf.data() + static_cast<size_t>(y) * outW * 4,
                            src + static_cast<size_t>(y) * mapped.RowPitch,
                            static_cast<size_t>(cw) * 4);
            d3dContext->Unmap(staging.get(), 0);

            if (!previewOnly) {
                std::lock_guard<std::mutex> lk(m_frameMx);
                m_frameData = packBuf;
                m_frameValid = true;
            }

            // živý náhled (throttle ~120 ms). BGRA == QImage::Format_RGB32 v paměti (LE).
            const DWORD now = GetTickCount();
            if (now - lastPreview >= 120) {
                lastPreview = now;
                QImage full(packBuf.data(), outW, outH, outW * 4, QImage::Format_RGB32);
                QImage thumb = full.scaled(320, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                emit previewFrame(thumb.copy());
            }
          } catch (...) { /* výjimku na WGC vlákně spolknout, nezhroutit proces */ }
        };

        auto token = framePool.FrameArrived(onFrame);
        session.StartCapture();

        if (previewOnly) {
            while (!m_stop) Sleep(30);
        } else {
            if (m_pipe != INVALID_HANDLE_VALUE && !m_stop) {
                if (!ConnectNamedPipe(m_pipe, nullptr)) {
                    const DWORD err = GetLastError();
                    if (err != ERROR_PIPE_CONNECTED) m_stop = true;
                }
            }

            LARGE_INTEGER freq{}, startTick{};
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&startTick);
            long long frameIdx = 0;
            std::vector<unsigned char> local(static_cast<size_t>(outW) * outH * 4);

            while (!m_stop) {
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                const long long target = startTick.QuadPart + frameIdx * freq.QuadPart / m_fps;
                if (now.QuadPart < target) {
                    double ms = double(target - now.QuadPart) * 1000.0 / double(freq.QuadPart);
                    if (ms > 1.0) Sleep(static_cast<DWORD>((std::min)(ms, 16.0)));
                    continue;
                }
                bool have = false;
                {
                    std::lock_guard<std::mutex> lk(m_frameMx);
                    if (m_frameValid) { local = m_frameData; have = true; }
                }
                if (!have) {
                    QueryPerformanceCounter(&startTick);
                    frameIdx = 0;
                    Sleep(2);
                    continue;
                }
                if (!writeAll(m_pipe, local.data(), local.size())) { m_stop = true; break; }
                ++frameIdx;
            }
        }

        framePool.FrameArrived(token);   // odregistrovat handler před uzavřením
        while (handlerBusy.load() > 0)   // počkat na doběhnutí in-flight callbacku
            Sleep(5);
        session.Close();
        framePool.Close();
    } catch (winrt::hresult_error const& e) {
        fail(QString::fromWCharArray(e.message().c_str()));
    } catch (...) {
        fail(QStringLiteral("Chyba při zachytávání obrazu."));
    }

    if (m_pipe != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(m_pipe);
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
}

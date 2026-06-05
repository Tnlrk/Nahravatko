# API Reference: Screen & Audio Capture Pipeline
## Portable Screen Recorder — implementační podklad pro AI kodéra

Tento dokument shrnuje klíčové API, inicializační sekvence a ukázkový kód pro
implementaci capture pipeline definované v PRD. Všechny API jsou nativní Win32/WinRT
bez externích závislostí. Zdrojové vzorové repozitáře jsou uvedeny v posledním oddílu.

---

## Obsah

1. [Windows.Graphics.Capture (WGC) — video](#1-windowsgraphicscapture-wgc)
2. [WASAPI Loopback — systémový zvuk](#2-wasapi-loopback--systémový-zvuk)
3. [WASAPI Capture — mikrofon](#3-wasapi-capture--mikrofon)
4. [Named Pipes + FFmpeg IPC](#4-named-pipes--ffmpeg-ipc)
5. [Build poznámky (CMake / CppWinRT)](#5-build-poznámky)
6. [Repozitáře a dokumentační zdroje](#6-repozitáře-a-dokumentační-zdroje)

---

## 1. Windows.Graphics.Capture (WGC)

### Požadavky

| Co | Minimum |
|----|---------|
| OS pro běh | Windows 10 build 18362 (1903) — Win32 interop `CreateForWindow`/`CreateForMonitor` |
| Windows SDK | 10.0.18362 nebo novější (doporučen 26100 pro `IsBorderRequired`) |
| C++ standard | C++17 se `/await` nebo C++20 |
| CppWinRT | NuGet: `Microsoft.Windows.CppWinRT` (header-only projekce) |

> **Poznámka:** Minimalizovaná okna WGC zachytí, ale neobsahují obsah — jejich stav
> je třeba ošetřit (viz `Direct3D11CaptureFrame::ContentSize`).

### Hlavičkové soubory

```cpp
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <Windows.Graphics.Capture.Interop.h>   // CreateForWindow / CreateForMonitor
#include <windows.graphics.directx.direct3d11.interop.h>
#include <d3d11.h>
#include <dxgi.h>
```

### Inicializační sekvence

#### 1a. Vytvoření D3D11 zařízení a WinRT wrapperu

```cpp
// D3D11 zařízení — potřebné pro frame pool
winrt::com_ptr<ID3D11Device> d3dDevice;
D3D11CreateDevice(
    nullptr,
    D3D_DRIVER_TYPE_HARDWARE,
    nullptr,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT,   // povinné pro DXGI/WinRT interop
    nullptr, 0,
    D3D11_SDK_VERSION,
    d3dDevice.put(),
    nullptr,
    nullptr);

// Zabalení do WinRT IDirect3DDevice
winrt::com_ptr<IDXGIDevice> dxgiDevice;
d3dDevice->QueryInterface(winrt::guid_of<IDXGIDevice>(), dxgiDevice.put_void());

winrt::com_ptr<IInspectable> inspectable;
CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put());
winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrtDevice =
    inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
```

#### 1b. Vytvoření GraphicsCaptureItem z HWND nebo HMONITOR

Win32 interop nevyužívá picker UI — `GraphicsCaptureItem` se vytvoří přímo z handle.
Factory se získá přes `IGraphicsCaptureItemInterop`:

```cpp
#include <Windows.Graphics.Capture.Interop.h>

namespace winrt { using namespace Windows::Graphics::Capture; }

// Získání interop factory z aktivační factory
auto activationFactory = winrt::get_activation_factory<winrt::GraphicsCaptureItem>();
auto interopFactory = activationFactory.as<IGraphicsCaptureItemInterop>();

winrt::GraphicsCaptureItem item{ nullptr };

// Varianta A — konkrétní okno (HWND hwnd)
winrt::check_hresult(interopFactory->CreateForWindow(
    hwnd,
    winrt::guid_of<winrt::GraphicsCaptureItem>(),
    winrt::put_abi(item)));

// Varianta B — celý monitor (HMONITOR hmon)
winrt::check_hresult(interopFactory->CreateForMonitor(
    hmon,
    winrt::guid_of<winrt::GraphicsCaptureItem>(),
    winrt::put_abi(item)));
```

#### 1c. Frame pool a session

```cpp
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;

// Formát BGRA8 — nejuniverzálnější; alternativně B8G8R8A8_UNormSrgb
auto pixelFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;
auto captureSize  = item.Size();   // winrt::Windows::Graphics::SizeInt32

// CreateFreeThreaded: FrameArrived se spouští na background threadu,
// nevyžaduje DispatcherQueue na volajícím threadu. Pro Win32 doporučeno.
auto framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
    winrtDevice,
    pixelFormat,
    2,              // počet poolovaných framů
    captureSize);

auto session = framePool.CreateCaptureSession(item);

// Volitelně: potlačit žlutý rámeček (Win11 build 25131+)
// Na starších systémech tato property neexistuje — chránit try/catch nebo
// podmínit IsApiContractPresent.
try { session.IsBorderRequired(false); } catch (...) {}

// Registrace handleru — spouští se na background threadu z CreateFreeThreaded
framePool.FrameArrived([&](Direct3D11CaptureFramePool const& sender, auto&&) {
    auto frame = sender.TryGetNextFrame();
    if (!frame) return;
    OnFrameArrived(frame);
});

session.StartCapture();
```

### Zpracování framů — extrakce raw pixelů do paměti

WGC frame obsahuje `IDirect3DSurface` (GPU textura). Pro odeslání do named pipe
(CPU side) je nutný staging texture:

```cpp
void OnFrameArrived(winrt::Direct3D11CaptureFrame const& frame)
{
    // Detekce změny velikosti zachytávaného obsahu
    auto contentSize = frame.ContentSize();
    if (contentSize.Width != m_lastSize.Width ||
        contentSize.Height != m_lastSize.Height)
    {
        m_lastSize = contentSize;
        // Přegenerovat staging texturu a frame pool — viz níže
        RecreateResources();
        return;   // tento frame přeskočit (nekonzistentní rozměry)
    }

    // GPU textura z framu
    auto surfaceTexture = GetDXGIInterfaceFromObject<ID3D11Texture2D>(frame.Surface());

    // Zkopírovat na staging texturu (CPU-accessible)
    m_d3dContext->CopyResource(m_stagingTexture.get(), surfaceTexture.get());

    // Mapovat na CPU
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = m_d3dContext->Map(
        m_stagingTexture.get(), 0,
        D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return;

    // mapped.pData    — pointer na první byte (BGRA8)
    // mapped.RowPitch — skutečný řádkový krok (může být > width * 4 kvůli alignmentu)
    // Délka pro výstup do pipe: contentSize.Height * mapped.RowPitch (nebo převést na NV12)

    // --- zde zapsat do named pipe pro FFmpeg ---
    WriteFrameToPipe(mapped.pData, contentSize, mapped.RowPitch);

    m_d3dContext->Unmap(m_stagingTexture.get(), 0);
}
```

#### Vytvoření staging textury

```cpp
void CreateStagingTexture(int width, int height)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width  = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage     = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.BindFlags = 0;

    m_d3dDevice->CreateTexture2D(&desc, nullptr, m_stagingTexture.put());
}
```

#### CFR — duplikace posledního framu (nutná pro konstantní frame rate)

WGC dodává snímky jen při změně obsahu. Pro CFR 30 fps je nutné časovačem
duplikovat poslední frame, pokud v daném intervalu žádný nepřišel:

```cpp
// V capture threadu: časovač nebo čekání s timeoutem
// Pokud timeoutne bez nového framu → WriteFrameToPipe(m_lastMappedData, ...)
// m_lastMappedData uchovává kopii předchozího framu (vlastní buffer, ne mapped pointer)
```

#### Resize — přegenerování frame poolu

```cpp
void RecreateResources()
{
    framePool.Recreate(winrtDevice, pixelFormat, 2, m_lastSize);
    CreateStagingTexture(m_lastSize.Width, m_lastSize.Height);
}
```

### Zastavení

```cpp
session.Close();
framePool.Close();
// Poté zavřít named pipes → FFmpeg dokončí encoding a zapíše finální fragment
```

---

## 2. WASAPI Loopback — systémový zvuk

Systémový loopback (`AUDCLNT_STREAMFLAGS_LOOPBACK`) je dostupný od Windows Vista.
Nevyžaduje žádný virtuální audio driver. Zachytí mix všeho, co slýší reproduktory/sluchátka.

Oproti standardnímu mic capture (viz sekce 3) jsou potřeba **dvě změny**:

```
GetDefaultAudioEndpoint:  eCapture  →  eRender
IAudioClient::Initialize: 0         →  AUDCLNT_STREAMFLAGS_LOOPBACK
```

> **Event-driven loopback:** od Windows 10 build 1703 lze loopback stream inicializovat
> s `AUDCLNT_STREAMFLAGS_EVENTCALLBACK` přímo. Na starších buildech bylo nutné
> souběžně udržovat render stream jako event source — na Win10 1903+ target to není
> potřeba.

### Kompletní inicializace loopbacku

```cpp
#include <mmdeviceapi.h>
#include <audioclient.h>

// REFERENCE_TIME: 100ns jednotky
#define REFTIMES_PER_SEC      10000000LL
#define REFTIMES_PER_MILLISEC 10000LL

HRESULT InitLoopbackCapture(IAudioClient** ppAudioClient,
                             IAudioCaptureClient** ppCaptureClient,
                             WAVEFORMATEX** ppFormat)
{
    HRESULT hr;
    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDevice* pDevice = nullptr;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                          CLSCTX_ALL, IID_PPV_ARGS(&pEnumerator));
    if (FAILED(hr)) return hr;

    // KLÍČ 1: eRender místo eCapture
    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    if (FAILED(hr)) { pEnumerator->Release(); return hr; }

    hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                           nullptr, (void**)ppAudioClient);
    pEnumerator->Release();
    pDevice->Release();
    if (FAILED(hr)) return hr;

    hr = (*ppAudioClient)->GetMixFormat(ppFormat);
    if (FAILED(hr)) return hr;

    // KLÍČ 2: AUDCLNT_STREAMFLAGS_LOOPBACK jako StreamFlags
    hr = (*ppAudioClient)->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,    // <-- loopback
        REFTIMES_PER_SEC,                // buffer = 1 sekunda
        0,
        *ppFormat,
        nullptr);
    if (FAILED(hr)) return hr;

    hr = (*ppAudioClient)->GetService(IID_PPV_ARGS(ppCaptureClient));
    return hr;
}
```

### Capture smyčka

```cpp
void LoopbackCaptureThread(IAudioClient* pClient,
                            IAudioCaptureClient* pCapture,
                            WAVEFORMATEX* pFormat,
                            HANDLE hStopEvent,
                            HANDLE hPipe)
{
    pClient->Start();

    while (WaitForSingleObject(hStopEvent, 10) == WAIT_TIMEOUT)
    {
        UINT32 packetSize = 0;
        while (SUCCEEDED(pCapture->GetNextPacketSize(&packetSize)) && packetSize > 0)
        {
            BYTE*  pData   = nullptr;
            UINT32 frames  = 0;
            DWORD  flags   = 0;

            HRESULT hr = pCapture->GetBuffer(&pData, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
            {
                // Silence — zapsat nuly, ať FFmpeg dostane kontinuální stream
                std::vector<BYTE> silence(frames * pFormat->nBlockAlign, 0);
                DWORD written = 0;
                WriteFile(hPipe, silence.data(), (DWORD)silence.size(), &written, nullptr);
            }
            else
            {
                DWORD written = 0;
                WriteFile(hPipe, pData,
                          frames * pFormat->nBlockAlign, &written, nullptr);
            }

            pCapture->ReleaseBuffer(frames);
        }
    }

    pClient->Stop();
}
```

---

## 3. WASAPI Capture — mikrofon

Standardní mic capture — totéž jako loopback, jen s `eCapture` a bez `LOOPBACK` flagu.
Formát získaný `GetMixFormat()` je většinou 32-bit float, 48 kHz — FFmpeg to zvládá.

```cpp
HRESULT InitMicCapture(IAudioClient** ppAudioClient,
                        IAudioCaptureClient** ppCaptureClient,
                        WAVEFORMATEX** ppFormat)
{
    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDevice* pDevice = nullptr;
    HRESULT hr;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                          CLSCTX_ALL, IID_PPV_ARGS(&pEnumerator));
    if (FAILED(hr)) return hr;

    // eCapture, eConsole → výchozí mikrofon
    hr = pEnumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &pDevice);
    pEnumerator->Release();
    if (FAILED(hr)) return hr;

    hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                           nullptr, (void**)ppAudioClient);
    pDevice->Release();
    if (FAILED(hr)) return hr;

    hr = (*ppAudioClient)->GetMixFormat(ppFormat);
    if (FAILED(hr)) return hr;

    hr = (*ppAudioClient)->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        0,                          // žádný LOOPBACK flag
        10000000LL,                 // buffer = 1 sekunda
        0,
        *ppFormat,
        nullptr);
    if (FAILED(hr)) return hr;

    return (*ppAudioClient)->GetService(IID_PPV_ARGS(ppCaptureClient));
}
```

Capture smyčka je identická s loopbackem — viz sekce 2, pouze jiný `HANDLE hPipe`.

### Výčet dostupných mikrofonů (pro UI dropdown)

```cpp
IMMDeviceCollection* pCollection = nullptr;
pEnumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pCollection);

UINT count = 0;
pCollection->GetCount(&count);
for (UINT i = 0; i < count; i++)
{
    IMMDevice* pDev = nullptr;
    pCollection->Item(i, &pDev);

    IPropertyStore* pStore = nullptr;
    pDev->OpenPropertyStore(STGM_READ, &pStore);

    PROPVARIANT varName;
    PropVariantInit(&varName);
    pStore->GetValue(PKEY_Device_FriendlyName, &varName);
    // varName.pwszVal obsahuje přátelský název ("Mikrofon (Realtek...)")

    PropVariantClear(&varName);
    pStore->Release();
    pDev->Release();
}
pCollection->Release();
```

---

## 4. Named Pipes + FFmpeg IPC

### Architektura

```
[WGC capture thread] ─── video BGRA frames ──→ \\.\pipe\rec_video ──┐
[WASAPI loopback thread] ─ PCM audio ──────────→ \\.\pipe\rec_audio ─┤→ ffmpeg.exe → output.mp4
[WASAPI mic thread] ──── PCM audio ────────────→ \\.\pipe\rec_mic ──┘
```

FFmpeg čte z pojmenovaných rour jako ze vstupů. Míchání zvuku obstarává `amix`.

### Vytvoření named pipe (server side — naše aplikace)

```cpp
HANDLE CreateRecordingPipe(const wchar_t* pipeName)
{
    return CreateNamedPipeW(
        pipeName,
        PIPE_ACCESS_OUTBOUND,           // jen zápis z naší strany
        PIPE_TYPE_BYTE | PIPE_WAIT,
        1,                              // max. 1 instance
        1024 * 1024,                    // write buffer 1 MB
        0,
        0,
        nullptr);                       // default security
}

// Použití:
HANDLE hVideoPipe = CreateRecordingPipe(L"\\\\.\\pipe\\rec_video");
HANDLE hLoopPipe  = CreateRecordingPipe(L"\\\\.\\pipe\\rec_loopback");
HANDLE hMicPipe   = CreateRecordingPipe(L"\\\\.\\pipe\\rec_mic");
```

### Spuštění FFmpegu (přes QProcess nebo CreateProcess)

FFmpeg spustit **před** `ConnectNamedPipe` — FFmpeg otevírá roury jako klient.

```
ffmpeg.exe
  -f rawvideo -pix_fmt nv12 -s 1920x1080 -r 30 -i \\.\pipe\rec_video
  -f f32le -ar 48000 -ac 2 -i \\.\pipe\rec_loopback
  -f f32le -ar 44100 -ac 1 -i \\.\pipe\rec_mic
  -filter_complex "[1:a][2:a]amix=inputs=2:normalize=0,aresample=async=1[aout]"
  -map 0:v -map "[aout]"
  -c:v libx264 -preset veryfast -crf 24
  -c:a aac -b:a 192k
  -movflags frag_keyframe+empty_moov
  output_20250605_120000.mp4
```

> **Pozor — `-ar`/`-ac`/`-f` u audio vstupů NEhardcodovat.** Hodnoty výše jsou jen
> ilustrace (loopback 48k stereo, mic 44,1k mono). Skutečné hodnoty se musí převzít
> z `GetMixFormat()` každého zařízení (viz sekce 2 a 3): `-f f32le` vs `-f s16le` podle
> bitové hloubky, `-ar` = `nSamplesPerSec`, `-ac` = `nChannels`. `amix` si různé
> sample raty srovná interně, ale špatně deklarovaný formát vstupu = zrychlené/chrčící
> audio nebo rozjetý sync.

> **Video je NV12, ne BGRA.** Capture vrstva převede BGRA → NV12 ještě před zápisem do
> roury (datový tok ~2,5× nižší, libx264 chce stejně YUV). Proto `-pix_fmt nv12`.

> **Kvalita = CRF preset (ne pevný bitrate).** Volba kvality v UI se mapuje na CRF
> a volitelné škálování `-vf scale` (bez upscalingu):
> Vysoká → nativní, `-crf 20` · Střední (default) → max 1080p, `-crf 24` ·
> Nízká → max 720p, `-crf 28` + audio `-b:a 128k`.

> **Formáty:** BGRA raw video (`-pix_fmt bgra`). Pokud konvertuješ na NV12 před pipe,
> použij `-pix_fmt nv12` — snižuje datový tok roury cca 2,5×.
> Pro float PCM z WASAPI `GetMixFormat()` použij `-f f32le` (32-bit float little-endian).
> Pokud formát je integer 16-bit: `-f s16le`.

#### Jen loopback (bez mikrofonu):

```
ffmpeg.exe
  -f rawvideo -pix_fmt bgra -s WxH -r 30 -i \\.\pipe\rec_video
  -f f32le -ar 48000 -ac 2   -i \\.\pipe\rec_loopback
  -c:v libx264 -preset veryfast -crf 23
  -c:a aac -b:a 192k
  -movflags frag_keyframe+empty_moov
  output.mp4
```

#### Pouze audio (režim AAC .m4a):

```
ffmpeg.exe
  -f f32le -ar 48000 -ac 2 -i \\.\pipe\rec_loopback
  -f f32le -ar 48000 -ac 2 -i \\.\pipe\rec_mic
  -filter_complex "[0:a][1:a]amix=inputs=2:normalize=0,aresample=async=1[aout]"
  -map "[aout]"
  -c:a aac -b:a 192k
  output.m4a
```

### ConnectNamedPipe + synchronizace spuštění

```cpp
// Po spuštění FFmpegu: čekat, až FFmpeg otevře všechny roury jako klient
// ConnectNamedPipe blokuje, dokud se klient (FFmpeg) nepřipojí
ConnectNamedPipe(hVideoPipe, nullptr);
ConnectNamedPipe(hLoopPipe, nullptr);
ConnectNamedPipe(hMicPipe, nullptr);
// Od tohoto bodu lze začít zapisovat framy a audio
```

### Graceful stop (povinné — zabraňuje poškození souboru)

Uzavřením rour signalizujeme FFmpegu EOF. FFmpeg dopíše fragmenty a uzavře kontejner:

```cpp
void StopRecording()
{
    // 1. Zastavit capture thready (signal stop event)
    SetEvent(hStopEvent);

    // 2. Zavřít write konce rour → FFmpeg dostane EOF na všech vstupech
    CloseHandle(hVideoPipe);
    CloseHandle(hLoopPipe);
    CloseHandle(hMicPipe);

    // 3. Počkat na ukončení FFmpeg procesu (WaitForSingleObject na process handle)
    // Timeout doporučen: 10–30 sekund, poté force-terminate
    WaitForSingleObject(hFfmpegProcess, 15000);
    CloseHandle(hFfmpegProcess);
}
```

> **NIKDY nepoužívat TerminateProcess na FFmpeg při aktivním záznamu.**
> Způsobí nekompletní MP4 (chybí moov atom nebo finální fragmenty).
> `frag_keyframe+empty_moov` pomáhá při pádu aplikace, ne při tvrdém kill.

---

## 5. Build poznámky

### CMakeLists.txt — klíčové nastavení

```cmake
cmake_minimum_required(VERSION 3.20)
project(PortableRecorder)

set(CMAKE_CXX_STANDARD 20)

# WinRT projekce přes CppWinRT — nainstalovat jako NuGet nebo vcpkg
find_package(cppwinrt CONFIG REQUIRED)   # vcpkg: install cppwinrt

target_compile_options(PortableRecorder PRIVATE
    /await          # coroutines pro WinRT (C++20 nativně)
    /permissive-
)

target_link_libraries(PortableRecorder PRIVATE
    Microsoft::CppWinRT
    d3d11.lib
    dxgi.lib
    ole32.lib           # CoCreateInstance
    propsys.lib         # PKEY_Device_FriendlyName
    windowsapp.lib      # WinRT umbrella lib (linker nutný pro WinRT APIs)
)
```

### Nutné includovat při práci s WGC

```cpp
// Pořadí záleží — WinRT headery před interop
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <d3d11.h>
#include <dxgi.h>
```

### Manifest — DPI awareness (povinné pro správné rozlišení capture)

```xml
<!-- app.manifest -->
<application>
  <windowsSettings>
    <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">
      PerMonitorV2
    </dpiAwareness>
  </windowsSettings>
</application>
```

Bez `PerMonitorV2` v manifestu WGC vrátí fyzické pixely správně,
ale souřadnice oken z Win32 budou v virtuálních pixelech — mismatch.

---

## 6. Repozitáře a dokumentační zdroje

### Vzorové projekty (klonovat a prostudovat)

| Repozitář | Co obsahuje | URL |
|-----------|-------------|-----|
| robmikh/Win32CaptureSample | Nejúplnější WGC Win32 ukázka (C++/WinRT, HWND + HMONITOR, resize handling, dirty regions) | https://github.com/robmikh/Win32CaptureSample |
| microsoft/Windows.UI.Composition-Win32-Samples | Oficiální MS WGC ukázky pro Win32 (C++) a WPF | https://github.com/microsoft/Windows.UI.Composition-Win32-Samples/tree/master/cpp/ScreenCaptureforHWND |

> `Win32CaptureSample` vyžaduje Windows 11 SDK (26100) pro build, ale výsledná
> aplikace poběží na Win10 1903+. Závisí na `robmikh/common` submodule — klonovat
> s `--recursive`.

### Dokumentace (Microsoft Learn)

| Téma | URL |
|------|-----|
| WGC — Screen Capture overview | https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture |
| WGC — Windows.Graphics.Capture namespace | https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture |
| WGC — Interop header (CreateForWindow / CreateForMonitor) | https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/ |
| WASAPI — Capturing a Stream (plný kódový příklad) | https://learn.microsoft.com/en-us/windows/win32/coreaudio/capturing-a-stream |
| WASAPI — Loopback Recording | https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording |
| WASAPI — IMMDeviceEnumerator | https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nn-mmdeviceapi-immdeviceenumerator |
| Named Pipes overview | https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipes |
| CreateNamedPipe | https://learn.microsoft.com/en-us/windows/win32/api/namedpipeapi/nf-namedpipeapi-createnamedpipew |

### FFmpeg dokumentace relevantní pro tento projekt

| Téma | URL |
|------|-----|
| rawvideo demuxer | https://ffmpeg.org/ffmpeg-formats.html#rawvideo |
| amix audio filter | https://ffmpeg.org/ffmpeg-filters.html#amix |
| aresample filter (async drift correction) | https://ffmpeg.org/ffmpeg-filters.html#aresample |
| libx264 encoder options | https://trac.ffmpeg.org/wiki/Encode/H.264 |
| Fragmented MP4 (movflags) | https://ffmpeg.org/ffmpeg-formats.html#Options-9 |

---

## Rychlý checklist pro implementaci

- [ ] D3D11 device s `D3D11_CREATE_DEVICE_BGRA_SUPPORT`
- [ ] `IGraphicsCaptureItemInterop` → `CreateForWindow` / `CreateForMonitor`
- [ ] `Direct3D11CaptureFramePool::CreateFreeThreaded` (ne `Create`)
- [ ] Staging texture s `D3D11_USAGE_STAGING | D3D11_CPU_ACCESS_READ`
- [ ] CFR duplikace posledního framu při timeoutu capture threadu
- [ ] Resize detection: `frame.ContentSize()` → `framePool.Recreate()`
- [ ] WASAPI loopback: `eRender` + `AUDCLNT_STREAMFLAGS_LOOPBACK`
- [ ] WASAPI mic: `eCapture` + 0 flags
- [ ] Named pipes vytvořit **před** spuštěním FFmpegu
- [ ] FFmpeg spustit s `-movflags frag_keyframe+empty_moov`
- [ ] Stop: zavřít pipe handles → čekat na FFmpeg exit (ne TerminateProcess)
- [ ] Manifest: `PerMonitorV2` DPI awareness

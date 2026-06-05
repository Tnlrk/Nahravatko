# Implementační plán: Portable Screen & Meeting Recorder (Nahrávátko)

Tento dokument popisuje architekturu, plán souborové struktury, instalační kroky vývojového prostředí a postup implementace přenosného rekordéru obrazovky a zvuku pro Windows pomocí **C++/Qt6** a **FFmpeg**.

---

## User Review Required

> [!IMPORTANT]
> **Instalace vývojového prostředí:** Na cílovém počítači aktuálně chybí překladač C++, CMake a knihovny Qt6. Pro sestavení aplikace bude nutné:
> 1. Nainstalovat **Visual Studio 2022 Build Tools** (s C++ workloadem) přes Chocolatey.
> 2. Nainstalovat **CMake** a **Ninja** přes Chocolatey.
> 3. Nainstalovat **Qt 6.x** (MSVC 2022 build) pomocí Python utility `aqtinstall` (rychlá instalace bez nutnosti registrace).
> 4. Stáhnout a přibalit binární soubor **FFmpeg 7.x** (`ffmpeg.exe`).
> 
> Potvrďte prosím souhlas s instalací těchto nástrojů v dalším kroku.

---

## Open Questions

> [!NOTE]
> **Způsob dodání FFmpeg:**
> Aplikace podle PRD vyžaduje přibalené binárky FFmpeg. Stáhneme oficiální build `ffmpeg.exe` (např. z gyan.dev) a uložíme ho do výstupní složky vedle sestaveného `.exe`. Souhlasíte s automatickým stažením tohoto buildu v rámci sestavení?

---

## Proposed Changes

Aplikaci rozdělíme do modulární struktury, kde oddělíme uživatelské rozhraní (Qt Widgets) od samotného nahrávacího backendu (Win32/WinRT/FFmpeg).

### Adresářová struktura projektu

Vytvoříme následující soubory v kořenovém adresáři projektu:

```
Nahrávátko/
├── CMakeLists.txt              # Definice sestavení projektu
├── app.manifest                # DPI awareness a kompatibilita pro Windows 10/11
├── prd-screen-recorder.md      # Původní PRD (nemodifikovat)
├── api-reference-screen-recorder.md # Původní API podklady (nemodifikovat)
└── src/
    ├── main.cpp                # Vstupní bod aplikace
    ├── mainwindow.h            # Hlavičkový soubor hlavního okna
    ├── mainwindow.cpp          # Implementace UI, tray ikony a nastavení (.ini)
    ├── recorder_engine.h       # Správa nahrávací pipeline a FFmpeg
    ├── recorder_engine.cpp     # Implementace IPC, named pipes a QProcess
    ├── video_capture.h         # Zachytávání videa (WGC)
    ├── video_capture.cpp       # Implementace WGC, staging textury a frame ratu
    ├── audio_capture.h         # Zachytávání zvuku (WASAPI)
    ├── audio_capture.cpp       # Implementace loopback, mic capture a VU metru
    ├── win32_utils.h           # Pomocné Win32 funkce
    └── win32_utils.cpp         # Výčet oken, monitorů a zvukových zařízení
```

---

### Toolchain & Build System

Tento komponent zajišťuje konfiguraci sestavení, propojování WinRT a Qt6 a správné chování aplikace z hlediska DPI.

#### [NEW] [CMakeLists.txt](../CMakeLists.txt)
- Nastaví C++20 a vyhledá balíček Qt6 (Widgets).
- Nastaví propojení s knihovnami Windows SDK (`windowsapp.lib`, `d3d11.lib`, `dxgi.lib`, `ole32.lib`, `propsys.lib`).
- Nakonfiguruje automatické kopírování Qt DLL a `ffmpeg.exe` do výstupní složky (Target Directory) po sestavení.

#### [NEW] [app.manifest](../app.manifest)
- Deklaruje `PerMonitorV2` DPI awareness, což je kritické pro správné fungování Windows.Graphics.Capture interop API (souřadnice oken vs fyzické pixely).

---

### Capture Engine (Video a Audio)

Tato vrstva provádí nízkoúrovňové zachytávání obrazu a zvuku a posílá je do rour.

#### [NEW] [video_capture.h](../src/video_capture.h) a [video_capture.cpp](../src/video_capture.cpp)
- **DXGI / D3D11:** Inicializace Direct3D11 zařízení s podporou BGRA.
- **WGC Session:** Spuštění `GraphicsCaptureSession` pro vybraný `HWND` nebo `HMONITOR` bez zobrazení systémového dialogu (pomocí `IGraphicsCaptureItemInterop`).
- **Staging Texture & Memory Mapping:** Kopírování GPU textur z `Direct3D11CaptureFramePool` na staging texturu a mapování do CPU paměti.
- **BGRA → NV12 konverze:** Před zápisem do roury se snímek převede z BGRA na NV12 (snižuje datový tok roury ~2,5× a šetří FFmpegu interní konverzi pro libx264). FFmpeg pak čte rouru jako `-pix_fmt nv12`.
- **CFR (Constant Frame Rate):** Vlastní background thread, který hlídá časování (30 fps). Pokud nepřijde nový frame od WGC (které posílá snímky jen při změně obsahu), duplikuje poslední snímek do roury.
- **Resize Handling:** Sledování změny rozlišení okna přes `frame.ContentSize()` a dynamická re-kreativace frame poolu a staging textury.
- **Náhled (Preview tap):** Vedle zápisu do roury vlákno periodicky (cca 5–10×/s) poskytuje zmenšený snímek (≤320×180) pro UI náhled, který běží i mimo nahrávání (monitorovací režim).

#### [NEW] [audio_capture.h](../src/audio_capture.h) a [audio_capture.cpp](../src/audio_capture.cpp)
- **WASAPI Loopback:** Zachytávání systémového zvuku přes defaultní render endpoint (`eRender`) s příznakem `AUDCLNT_STREAMFLAGS_LOOPBACK`.
- **WASAPI Microphone:** Zachytávání mikrofonu přes zvolený capture endpoint (`eCapture`).
- **PCM Stream:** Čtení raw dat a zápis do příslušné audio roury.
- **Detekce formátu zařízení:** Skutečný sample rate, počet kanálů a vzorkový formát (float vs 16-bit) se zjistí z `GetMixFormat()` daného endpointu a předají recorder enginu — ten je vloží jako per-input parametry FFmpegu (`-ar`/`-ac`/`-f f32le|s16le`). Nikdy nehardcodovat 48000/2 — mikrofon bývá mono nebo 44,1 kHz, jinak se rozjede sync.
- **Silence Padding:** Vkládání nulových bajtů při tichu v loopbacku pro udržení synchronizace.
- **VU Meter Data:** Výpočet RMS (Root Mean Square) úrovně mikrofonu v reálném čase a posílání hodnoty přes Qt signály do UI. Běží i mimo nahrávání (monitorovací režim — VU metr reaguje hned po zapnutí mikrofonu).

---

### Recorder Engine & IPC

Tento komponent koordinuje spouštění FFmpegu a přenos dat přes pojmenované roury (Named Pipes).

#### [NEW] [recorder_engine.h](../src/recorder_engine.h) a [recorder_engine.cpp](../src/recorder_engine.cpp)
- **Named Pipes:** Vytvoření rour `\\.\pipe\rec_video`, `\\.\pipe\rec_loopback` a `\\.\pipe\rec_mic` pomocí `CreateNamedPipeW`. Vytvářejí se jen roury pro aktivní zdroje.
- **Dynamické sestavení FFmpeg příkazu:** Argumenty se generují podle aktivních zdrojů a režimu:
  - Video: `-f rawvideo -pix_fmt nv12 -s WxH -r 30 -i \\.\pipe\rec_video`.
  - Audio: per-input parametry (`-ar`/`-ac`/`-f`) z reálného formátu zařízení (viz audio_capture).
  - 0 nebo 1 audio zdroj → bez `amix`; 2 zdroje → `amix=inputs=2:normalize=0` + `aresample=async=1`.
  - Režim „Pouze zvuk" → bez video vstupu, výstup `.m4a` (AAC 192k).
- **Kvalita (CRF preset):** Volba kvality se mapuje na CRF a případné škálování:
  - Vysoká → nativní rozlišení, CRF ~20
  - Střední (výchozí) → `-vf scale` na max. 1080p (bez upscalingu), CRF ~24
  - Nízká → `-vf scale` na max. 720p (bez upscalingu), CRF ~28, audio 128k
  - Vždy `-preset veryfast` (fallback `superfast`, kdyby slabší CPU nestíhal).
- **Synchronizace připojení:** Použití `ConnectNamedPipe` pro synchronizaci spuštění zápisu dat až po otevření rour ze strany FFmpegu.
- **Graceful Shutdown (na worker threadu):** Zastavení vláken, uzavření konců rour (čímž FFmpeg dostane EOF) a bezpečné vyčkání na ukončení procesu FFmpegu (`WaitForSingleObject`, timeout 15 s), aby se korektně zapsal index (moov atom) do MP4/M4A. Čekání **nesmí blokovat UI vlákno** — během něj UI zobrazuje „Ukládám…". Nikdy `TerminateProcess` při aktivním záznamu (poškodí soubor).

---

### User Interface (GUI)

Tento komponent tvoří uživatelské rozhraní, které splňuje požadavky na maximální jednoduchost a absenci technického žargonu.

#### [NEW] [mainwindow.h](../src/mainwindow.h) a [mainwindow.cpp](../src/mainwindow.cpp)
- **Jednostránkový layout:** Dropdown pro video zdroje (Celá obrazovka, konkrétní okna), Checkboxy a dropdowny pro zvukové zdroje.
- **Náhled (Preview):** Malý widget (320x180 px), který periodicky odebírá zmenšené snímky ze zachytávání a vykresluje je. Náhled běží i mimo nahrávání (monitorovací režim — capture session pro náhled startuje při výběru zdroje, ne až při startu záznamu).
- **VU Meter:** Sloupcový indikátor hlasitosti mikrofonu reagující v reálném čase (i když se nenahrává).
- **Dropdown „Kvalita":** Jeden ovladač se třemi volbami (Vysoká / Střední / Nízká) — viz US-004b; mapuje se na CRF + rozlišení v recorder enginu. Při režimu „Pouze zvuk" se skryje.
- **Konfigurace výstupu:** Výběr režimu (Video + Zvuk nebo Pouze zvuk), změna cílové složky, automatické generování názvů souborů (`zaznam_YYYYMMDD_HHMMSS`).
- **Tray Ikona (`QSystemTrayIcon`):** Minimalizace do tray, ikona měnící barvu/vzhled při nahrávání, kontextové menu, tooltip se stopáží, zamezení ukončení aplikace křížkem při nahrávání bez potvrzení.
- **Ošetření odpojení zařízení:** Sledování změn audio endpointů (`IMMNotificationClient`) — při odpojení mikrofonu/sluchátek během záznamu (např. Bluetooth) srozumitelná hláška místo pádu.
- **Single instance:** Zamezení spuštění druhé instance (kolize pevných názvů rour) přes pojmenovaný mutex.
- **Ukládání nastavení:** Třída `QSettings` ukládající stav do souboru `settings.ini` ve složce aplikace.

#### [NEW] [win32_utils.h](../src/win32_utils.h) a [win32_utils.cpp](../src/win32_utils.cpp)
- Helpery pro získání seznamu monitorů (`EnumDisplayMonitors`) a viditelných oken na ploše (`EnumWindows`).
- Helpery pro získání přátelských názvů audio zařízení přes WASAPI `IMMDeviceEnumerator`.

---

## Verification Plan

### Automated Tests
- **Sestavení projektu:** Spuštění CMake konfigurace a Ninja sestavení pro ověření chybějících symbolů a správného linkování WinRT/Qt.
- **FFmpeg integrace:** Ověření, že `ffmpeg.exe` je přítomen a spouští se se správnými argumenty.
- **Smoke test záznamu:** Pořízení cca 5sekundového testovacího záznamu a ověření výstupu přes `ffprobe` — existuje video i audio stopa, délka přibližně odpovídá, soubor není poškozený. Chytí většinu regresí v pipeline.

### Manual Verification
1. **Ověření zdrojů:** Zkontrolovat, že dropdown oken obsahuje reálné názvy (např. "Chrome", "Notepad") a správně rozlišuje monitory.
2. **Nahrávání videa a zvuku:**
   - Spustit záznam celého monitoru se zapnutým systémovým zvukem a mikrofonem.
   - Přehrát video na YouTube (systémový zvuk) a mluvit do mikrofonu.
   - Zastavit nahrávání a ověřit, že výsledný MP4 soubor má synchronizovaný obraz a oba audio zdroje sloučené do jedné stopy.
3. **Nahrávání pouze zvuku:**
   - Zvolit režim "Pouze zvuk (AAC)" a zkontrolovat skrytí video prvků.
   - Nahrát audio a ověřit uložení do `.m4a` o velikosti odpovídající 192kbps AAC.
4. **Chování tray ikony:**
   - Zavřít okno aplikace křížkem a ověřit, že běží v tray a zobrazí se info bublina.
   - Minimalizovat okno během nahrávání a sledovat tooltip s běžícím časem na tray ikoně.
5. **Persistence:** Změnit výstupní složku, zavřít aplikaci, znovu otevřít a ověřit, že nastavení se načetlo ze `settings.ini`.

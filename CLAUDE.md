# Nahrávátko — vývojářský manuál

Přenosné nahrávátko obrazovky a zvuku pro Windows. **C++20 / Qt 6 (Widgets)** + přibalený **FFmpeg**.
Princip: WGC (video) + WASAPI (zvuk) → **named pipes** → `ffmpeg.exe` (kódování H.264/AAC).

GitHub: https://github.com/Tnlrk/Nahravatko (veřejný). Hlavní větev: `main`. Vydaná verze: `v1.0`.

---

## Sestavení (Windows, z kořene projektu)

Nástroje jsou v `tools/` (CMake, Ninja, FFmpeg). Qt 6.8 MSVC a VS 2022 Build Tools jsou nainstalované lokálně.

> **Důležité:** Ninja si MSVC kompilátor sám nenajde — nejdřív načíst `vcvars64.bat`. Před buildem zabít běžící `Nahravatko.exe` (jinak `LNK1104`).

```powershell
# 1) MSVC prostředí
cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && set' |
  ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] } }

# 2) konfigurace (cesty k tools/ a Qt podle stroje)
cmake -G Ninja -DCMAKE_MAKE_PROGRAM="tools\ninja\ninja.exe" `
  -DCMAKE_PREFIX_PATH="C:\path\to\Qt\6.8.0\msvc2022_64" -DCMAKE_BUILD_TYPE=Release -B build

# 3) build (post-build krok přibalí ffmpeg.exe, Qt DLL přes windeployqt, VC++ runtime a licence)
cmake --build build
```

Výstup `build/` je soběstačná složka se spustitelným `.exe`.

---

## Architektura (`src/`)

| Soubor | Odpovědnost |
|---|---|
| `main.cpp` | Vstup, single-instance (QSharedMemory), DPI policy, větší základní písmo |
| `mainwindow.*` | UI, tray ikona, `settings.ini`, světlý/tmavý dle systému, kontrola aktualizací, okno „O aplikaci" |
| `recorder_engine.*` | Sestavení FFmpeg příkazu, named pipes (per-PID, DACL jen vlastník), `QProcess` ffmpeg, graceful stop |
| `video_capture.*` | WGC (D3D11, CFR 30 fps) → BGRA do roury; živý náhled |
| `audio_capture.*` | WASAPI (mikrofon + systémový loopback) → PCM do roury; VU metr |
| `win32_utils.*` | Výčet monitorů, oken a audio zařízení |

Capture vrstvy běží na vlastních vláknech (MTA), data jdou rourami do ffmpegu. Stop = zavřít konce rour (EOF) → ffmpeg dopíše soubor.

---

## Klíčová rozhodnutí a konvence

- **Kvalita:** Vysoká = nativní/CRF 20, Střední = max 1080p/CRF 24, Nízká = max 720p/CRF 28 (scale + pad, bez upscalingu). Video H.264 (libx264 veryfast), zvuk AAC. Audio-only = `.m4a`.
- Roury přenášejí **BGRA** (ne NV12) — kvůli barevné správnosti; NV12 je možná budoucí optimalizace.
- **`settings.ini`** (vedle exe): persistuje `mode/quality/outputDir/micDevice`. **NEpersistuje se** mikrofon / systémový zvuk / „vždy nahoře" — při startu vždy: oba zvuky zapnuté, nahoře vypnuté. Výchozí výstupní složka = **systémové Videa** (`QStandardPaths::MoviesLocation`).
- **Verze:** konstanta `kAppVersion` v `mainwindow.cpp`. Kontrola aktualizací 3 s po startu porovnává s tagem z `releases/latest` na GitHubu.
- **Bezpečnost rour:** názvy unikátní per PID, DACL jen pro vlastníka, `FILE_FLAG_FIRST_PIPE_INSTANCE`.

---

## Vydání nové verze

1. Zvednout **`kAppVersion`** v `mainwindow.cpp` (např. `1.1`).
2. Commit + push na `main`.
3. Vytvořit GitHub **release s tagem `vX.Y`** (musí odpovídat `kAppVersion`, jinak se kolegům nenabídne aktualizace).
4. Přibalit **portable ZIP** z `build/` (exe + DLL + ffmpeg + plugin složky + licence), **BEZ** `settings.ini`, `Záznamy/`, `*.log`, `CMakeCache.txt`.
5. Tag releasu posunout na poslední commit (ať „Source code" archivy odpovídají).

> ⚠️ Do ZIPu **nikdy** nezabalit `settings.ini` — přebil by uživateli výchozí cestu (mířil by na cizí PC).

---

## Licence

Kód: **MIT** (`LICENSE`). Přibalený **FFmpeg = GPL** → balíček musí obsahovat `FFmpeg-LICENSE.txt` + `FFmpeg-README.txt` (CMake je kopíruje automaticky při buildu).

---

## Probíhající práce: anglická lokalizace (větev `i18n`)

**Cíl:** jedna **dvojjazyčná** aplikace (ne separátní anglická verze) přes Qt překladový systém.

1. Uživatelské řetězce `QStringLiteral("…")` → `tr("…")` (čeština zůstává jako zdroj — nepřepisovat). Pozor na: HTML v „O aplikaci", tray menu, dialogy, chybové hlášky v `recorder_engine`/`*_capture` (jsou v `QObject`, `tr()` funguje).
2. `lupdate` → vytvořit `translations/nahravatko_en.ts`, přeložit v Qt Linguist, `lrelease` → `.qm`. (`lupdate`/`lrelease` jsou v `…\Qt\6.8.0\msvc2022_64\bin\`.)
3. `.qm` přibalit (přes `resources.qrc` nebo vedle exe), načíst `QTranslator` podle `QLocale::system()` + přidat volbu **Jazyk: Automaticky / Čeština / English** (uložit do `settings.ini`, aplikovat restartem nebo retranslací UI).
4. Otestovat oba jazyky (přepínač i systémové locale), pak sloučit do `main` a vydat jako novou verzi.

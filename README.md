# Nahrávátko / Captaculum

*Anglické rozhraní aplikace běží pod názvem **Captaculum** (latinsky „malé zachytávátko"); v češtině zůstává **Nahrávátko**. Jde o jednu a tu samou aplikaci.*

**Jednoduché přenosné nahrávátko obrazovky a zvuku pro Windows.**

Žádná instalace, žádný technický žargon. Spustíš `.exe`, vybereš co nahrávat, klikneš na *Nahrávat*. Cílem je, aby si první záznam pořídil i netechnický uživatel do minuty.

<p align="center">
  <img src="docs/screenshot.png" alt="Nahrávátko" width="360">
</p>

<p align="center">
  <a href="https://github.com/Tnlrk/Nahravatko/releases/latest/download/Nahravatko-portable.zip">
    <img src="https://img.shields.io/badge/%E2%AC%87%20St%C3%A1hnout-Windows%2064--bit-2ea44f?style=for-the-badge" alt="Stáhnout">
  </a>
  &nbsp;
  <a href="https://github.com/Tnlrk/Nahravatko/releases/latest">
    <img src="https://img.shields.io/github/v/release/Tnlrk/Nahravatko?style=for-the-badge&label=verze&color=blue" alt="Nejnovější verze">
  </a>
</p>

<p align="center">
  <b><a href="https://github.com/Tnlrk/Nahravatko/releases/latest/download/Nahravatko-portable.zip">⬇ Stáhnout nejnovější verzi (ZIP)</a></b>
  &nbsp;·&nbsp;
  <a href="https://github.com/Tnlrk/Nahravatko/releases">všechna vydání</a>
</p>

---

## Co umí

- 🎥 **Obraz** — celá obrazovka, konkrétní monitor, nebo konkrétní okno. Funguje i pro hardwarově akcelerovaná okna (Chrome, Teams, Spotify, Electron/PWA aplikace).
- 🎙️ **Zvuk** — mikrofon a/nebo systémový zvuk („co je slyšet z počítače"), v libovolné kombinaci sloučené do jedné stopy.
- 🎚️ **Kvalita** — Vysoká / Střední / Nízká (jeden srozumitelný ovladač; Střední = přesně 1080p, Nízká = 720p).
- 🖼️ **Živý náhled** ještě před nahráváním a **ukazatele hlasitosti** (VU) u obou zvukových zdrojů.
- 📦 **Dva výstupy** — *Video + zvuk* (MP4, H.264/AAC) nebo *Pouze zvuk* (M4A, AAC 192 kbps).
- 🧰 **Pohodlí** — ikona v liště (při nahrávání zčervená), volba „Vždy nahoře", zavření do lišty, automatické otevření složky s hotovým záznamem, zapamatování voleb.
- 🌗 **Vzhled a přístupnost** — světlý i tmavý režim automaticky podle nastavení Windows; čitelné ovládání i pro hůře vidící (respektuje zvětšení Windows).
- 🔔 **Hlídání aktualizací** — při startu se tiše zkontroluje, zda je na GitHubu novější verze; pokud ano, nabídne se stažení (nic se neinstaluje samo).
- 📁 **Ukládání** — záznamy jdou rovnou do tvé složky **Videa**, cestu lze kdykoliv změnit.
- 💾 **Portable** — celé to běží z jedné složky, bez instalace a bez admin práv.

---

## Stažení a použití

1. Stáhni **[Nahravatko-portable.zip](https://github.com/Tnlrk/Nahravatko/releases/latest/download/Nahravatko-portable.zip)** a **rozbal** ho kamkoliv — na plochu, do složky, na USB.
2. Spusť **`Nahravatko.exe`**.
3. Nahraj. Hotové soubory se ukládají do tvé složky **Videa** (`C:\Users\…\Videos`); cestu lze změnit tlačítkem *Změnit složku*.

> [!IMPORTANT]
> Celá složka musí zůstat pohromadě — `Nahravatko.exe` potřebuje vedle sebe všechny `.dll`, podsložky (`platforms`, `styles`, …) a `ffmpeg.exe`. Nekopíruj jen samotný `.exe`.

**Požadavky:** Windows 10 (verze 1903 a novější) nebo Windows 11, 64-bit. Nic dalšího — runtime knihovny i FFmpeg jsou přibalené.

> [!NOTE]
> Při prvním spuštění může Windows zobrazit **SmartScreen** („Windows ochránil váš počítač") — aplikace není digitálně podepsaná. Klikni na **Další informace → Přesto spustit**.

---

## Jak to funguje (technicky)

| Vrstva | Technologie |
|---|---|
| Obraz | **Windows.Graphics.Capture** (WGC) — D3D11, CFR 30 fps |
| Zvuk | **WASAPI** (mikrofon + systémový loopback) |
| Spojení | surová data přes **named pipes** → **FFmpeg** (kódování H.264/AAC) |
| UI | **Qt 6** (Widgets), C++20 |

Aplikace nezachytává ani nekóduje sama — capture vrstva posílá raw snímky/PCM rourami do přibaleného `ffmpeg.exe`, který se postará o kódování a kontejner.

---

## Parametry nahrávek

- **Video:** H.264 (`libx264`, preset *veryfast*), kvalita řízená přes **CRF** — tj. *konstantní kvalita*, ne pevný bitrate. Datový tok se proto mění podle obsahu (statický obraz zabírá výrazně méně než pohyblivé video).
- **Zvuk:** AAC, kontejner MP4 (video) nebo M4A (pouze zvuk). Více zdrojů (mikrofon + systém) se sloučí do jedné stopy.
- Snímkování: **30 fps**.

| Kvalita | Rozlišení | Video (H.264) | Zvuk (AAC) | ~ datový tok\* | ~ velikost / hod\* |
|---|---|---|---|---|---|
| **Vysoká** | nativní | CRF 20 | 192 kbit/s | ~3–12 Mbit/s | ~1–2 GB |
| **Střední** | max 1920×1080 | CRF 24 | 192 kbit/s | ~2–6 Mbit/s | ~0,4–0,8 GB |
| **Nízká** | max 1280×720 | CRF 28 | 128 kbit/s | ~1–3 Mbit/s | ~0,15–0,3 GB |
| **Pouze zvuk** | — | — | 192 kbit/s | 192 kbit/s | ~85 MB |

\* Orientačně. Protože video používá CRF, datový tok i výsledná velikost hodně závisí na tom, kolik se na obrazovce hýbe — slidy a statický obraz jsou výrazně menší než plynulé video.

---

## Sestavení ze zdrojů

Potřebné nástroje (Windows):

- **Visual Studio 2022 Build Tools** (C++ workload, MSVC)
- **CMake** ≥ 3.20 a **Ninja**
- **Qt 6** (MSVC 2022, 64-bit) — např. přes `aqtinstall`
- **FFmpeg** 7/8 (`ffmpeg.exe`) ve složce `tools/ffmpeg/...` (viz cesta v `CMakeLists.txt`)

```powershell
# 1) Načíst MSVC prostředí (jinak Ninja nenajde kompilátor)
cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && set' |
  ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] } }

# 2) Konfigurace
cmake -G Ninja -DCMAKE_PREFIX_PATH="C:\path\to\Qt\6.8.0\msvc2022_64" -DCMAKE_BUILD_TYPE=Release -B build

# 3) Sestavení (post-build krok přibalí ffmpeg.exe, Qt DLL i VC++ runtime)
cmake --build build
```

Výsledek je v `build/` — soběstačná složka se spustitelným `.exe`.

---

## Struktura projektu

```
src/
├── main.cpp            # vstupní bod, single-instance
├── mainwindow.*        # UI, tray ikona, nastavení (settings.ini)
├── recorder_engine.*   # sestavení FFmpeg příkazu, named pipes, řízení záznamu
├── video_capture.*     # WGC (obraz)
├── audio_capture.*     # WASAPI (mikrofon + systémový zvuk), VU metr
└── win32_utils.*       # výčet monitorů, oken a audio zařízení
```

Podrobnosti k zadání a architektuře viz [docs/prd-screen-recorder.md](docs/prd-screen-recorder.md), [docs/implementation_plan.md](docs/implementation_plan.md) a [docs/api-reference-screen-recorder.md](docs/api-reference-screen-recorder.md).

---

## Licence

Zdrojový kód Nahrávátka je uvolněn pod licencí **MIT** (viz [LICENSE](LICENSE)) — můžeš ho volně používat, upravovat i šířit, stačí zachovat údaj o autorství.

Aplikace zároveň při běhu volá a v balíčku přibaluje **FFmpeg** (https://ffmpeg.org), který je šířen pod licencí **GPL/LGPL** podle konkrétního buildu. FFmpeg je samostatný program volaný jako externí proces, takže se jeho licence nevztahuje na kód Nahrávátka — ale při šíření balíčku s FFmpegem je nutné dodržet jeho licenční podmínky (přiložit licenci FFmpegu a odkaz na zdrojové kódy příslušného buildu).

---

S pomocí AI vytvořil **Antonín Lerek** v roce 2026 · [tnlrk@tnlrk.cz](mailto:tnlrk@tnlrk.cz)

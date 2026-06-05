# PRD: Portable Screen & Meeting Recorder

## Introduction

Jednoduchá portable aplikace pro Windows umožňující nahrávání obrazovky, okna aplikace nebo zvuku (mikrofon + systémový zvuk). Cíl je maximální jednoduchost — žádná instalace, spustitelná na průměrném kancelářském PC, výstup do MP4 nebo AAC.

Stack: **C++/Qt** + FFmpeg jako backend pro záznam a kódování.

---

## Goals

- Spustitelná bez instalace (portable .exe + bundlované DLL/FFmpeg binárky)
- Nahrávání obrazovky, konkrétního okna, mikrofonu a systémového zvuku v libovolné kombinaci
- Výstup MP4 (H.264/AAC) nebo čisté audio AAC 192kbps (.m4a) dle volby uživatele
- Funkční na průměrném kancelářském HW (8 GB RAM, bez dedikované GPU)
- UI do 1 okna, ovladatelné bez dokumentace

---

## User Stories

### US-001: Spuštění bez instalace
**Description:** Jako uživatel chci spustit aplikaci přímo z USB nebo složky bez nutnosti instalace.

**Acceptance Criteria:**
- [ ] Aplikace se spustí dvojklikem na .exe bez instalátoru
- [ ] Všechny závislosti (Qt DLL, FFmpeg binárky) jsou ve stejné složce jako .exe
- [ ] Nevyžaduje administrátorská práva pro spuštění
- [ ] Funguje na Windows 10 a Windows 11

---

### US-002: Výběr zdroje záznamu
**Description:** Jako uživatel chci před nahráváním zvolit, co chci zachytit — jednoduše, bez technického žargonu.

**Acceptance Criteria:**
- [ ] Dropdown pro výběr zdroje videa: „Celá obrazovka", „Konkrétní okno" (seznam otevřených oken dle názvu)
- [ ] Pokud je připojeno více monitorů, dropdown rozlišuje „Obrazovka 1", „Obrazovka 2" apod.
- [ ] Checkbox „Mikrofon" s dropdownem pro výběr vstupního zařízení (zobrazeny přátelské názvy, ne technické ID)
- [ ] Checkbox „Systémový zvuk" (vše, co slyší reproduktory/sluchátka) — popisek: „Zvuk z počítače"
- [ ] Libovolná kombinace audio zdrojů (mikrofon + systémový zvuk) lze aktivovat zároveň (FFmpeg sloučí do jedné stopy)
- [ ] Volby se pamatují mezi spuštěními (uložení do INI souboru ve stejné složce)

---

### US-003: Volba výstupního formátu
**Description:** Jako uživatel chci zvolit, zda chci video nebo jen audio.

**Acceptance Criteria:**
- [ ] Radio button nebo dropdown se dvěma volbami: „Video + zvuk (MP4)" a „Pouze zvuk (AAC)"
- [ ] Při volbě „Pouze zvuk" se video zdroj automaticky deaktivuje (nebo skryje)
- [ ] Výstupní soubor při volbě „Pouze zvuk" má příponu `.m4a` (AAC 192kbps, stereo)
- [ ] Výběr se pamatuje mezi spuštěními

---

### US-004: Volba výstupní složky a názvu souboru
**Description:** Jako uživatel chci vědět, kam se soubor uloží, a případně to změnit.

**Acceptance Criteria:**
- [ ] Zobrazena cesta k výstupní složce (výchozí: složka `Záznamy` vedle .exe)
- [ ] Tlačítko pro změnu složky přes systémový dialog
- [ ] Název souboru se generuje automaticky jako `zaznam_YYYYMMDD_HHMMSS.mp4` (nebo `.m4a` pro pouze zvuk)
- [ ] Cesta se pamatuje mezi spuštěními

---

### US-004b: Volba kvality výstupu
**Description:** Jako uživatel chci zvolit kvalitu záznamu — bez nutnosti rozumět technickým pojmům. Jeden srozumitelný ovladač, který zároveň ovlivní velikost výsledných souborů.

**Acceptance Criteria:**
- [ ] Jeden dropdown „Kvalita" se třemi lidsky popsanými volbami (každá v sobě nese rozlišení i míru komprese):
  - „Vysoká kvalita (největší soubory)" — nativní rozlišení, vizuálně bezztrátový dojem
  - „Střední kvalita (doporučeno)" — max. Full HD (1920×1080), výrazně menší soubory — **výchozí volba**
  - „Nízká kvalita (malé soubory)" — max. HD (1280×720), nejmenší soubory
- [ ] Všechny volby nahrávají 30 fps
- [ ] Pokud zvolené rozlišení přesahuje rozlišení zdroje, aplikace použije nativní rozlišení (bez upscalingu)
- [ ] Velikost souboru se řídí kvalitativním režimem (CRF), ne pevným bitrate — statický obsah (např. slidy) zabírá výrazně méně místa
- [ ] Volba se pamatuje mezi spuštěními
- [ ] Při volbě „Pouze zvuk" se tento dropdown skryje

---

### US-005: Start a stop nahrávání
**Description:** Jako uživatel chci jednoduše spustit a zastavit nahrávání.

**Acceptance Criteria:**
- [ ] Tlačítko „Nahrávat" spustí záznam
- [ ] Během nahrávání se zobrazuje uplynulý čas (MM:SS)
- [ ] Tlačítko se změní na „Zastavit" (červené)
- [ ] Po kliknutí na „Zastavit" se nahrávání ukončí a soubor se uloží
- [ ] Po uložení se zobrazí hláška s cestou k souboru a tlačítkem „Otevřít složku"
- [ ] Aplikaci nelze zavřít během aktivního nahrávání bez potvrzovacího dialogu

---

### US-006: Náhled před nahráváním
**Description:** Jako uživatel chci vidět, co budu nahrávat, ještě před spuštěním.

**Acceptance Criteria:**
- [ ] Malý náhled (preview) ve formě thumbnailového okna zobrazuje živý obraz z vybraného zdroje
- [ ] Náhled se aktualizuje při změně zdroje
- [ ] Náhled má maximálně 320×180 px, aby nezatěžoval HW

---

### US-007: Indikátor hlasitosti mikrofonu
**Description:** Jako uživatel chci vidět, že mikrofon funguje a zachytává zvuk.

**Acceptance Criteria:**
- [ ] Jednoduchý VU meter (sloupcový indikátor) vedle checkboxu mikrofonu
- [ ] Reaguje na vstupní signál v reálném čase (i když se nenahrává)

---

### US-008: Indikace nahrávání v tray liště
**Description:** Jako uživatel chci vědět, že nahrávání probíhá, i když mám aplikaci minimalizovanou.

**Acceptance Criteria:**
- [ ] Aplikace zobrazuje ikonu v systémovém tray (oznamovací oblast hlavního panelu)
- [ ] Ikona se vizuálně liší podle stavu: klidový stav vs. aktivní nahrávání (červená tečka nebo odlišná ikona)
- [ ] Tooltip nad ikonou zobrazuje stav a uplynulý čas záznamu (např. „Nahrávám — 03:42")
- [ ] Pravým tlačítkem na ikonu: kontextové menu s položkami „Zastavit nahrávání" (aktivní jen při záznamu), „Otevřít aplikaci", „Ukončit"
- [ ] Kliknutí levým tlačítkem na ikonu obnoví okno do popředí
- [ ] Zavření okna křížkem aplikaci neukončí — zůstane v tray; pokud zrovna nenahrává, zobrazí bublinu „Aplikace běží na pozadí. Klikněte sem pro otevření."

---

## Functional Requirements

- FR-1: Aplikace používá FFmpeg (přiložené binárky) pro veškeré kódování
- FR-2: Zachytávání obrazovky i konkrétního okna přes Windows.Graphics.Capture (WGC); funguje i pro GPU-akcelerovaná okna (Chrome, Teams, Office). Vyžaduje Windows 10 build 1903+
- FR-3: Zachytávání systémového zvuku přes WASAPI loopback
- FR-4: Zachytávání mikrofonu přes WASAPI
- FR-5: Video výstup: H.264 (libx264, preset `veryfast`), audio AAC, kontejner MP4; kvalita řízena přes CRF podle zvoleného režimu kvality (Vysoká/Střední/Nízká), ne pevným bitrate
- FR-6: Audio výstup (režim „Pouze zvuk"): AAC 192kbps stereo, kontejner `.m4a`
- FR-7: Při aktivaci více audio zdrojů (mikrofon + systémový zvuk) FFmpeg sloučí streamy do jedné stopy (amix)
- FR-8: Nastavení ukládána do `settings.ini` ve složce aplikace
- FR-9: Aplikace nesmí zapsat nic mimo svoji složku a složku výstupních záznamů
- FR-10: Výstupní soubory jsou běžně přehratelné (standardní MP4/M4A); zároveň jsou bez konverze kompatibilní s lokálním přepisem (Whisper)
- FR-11: UI je výhradně v češtině; žádná lokalizace pro jiné jazyky není vyžadována
- FR-12: Po dokončení záznamu se automaticky otevře složka s výstupním souborem (Windows Explorer, soubor zvýrazněný)
- FR-13: Aplikace zobrazuje tray ikonu po celou dobu běhu; při aktivním nahrávání ikona indikuje červený stav a tooltip zobrazuje uplynulý čas; pravé tlačítko nabízí rychlé akce

---

## Non-Goals

- Žádné živé streamování (RTMP, YouTube Live apod.)
- Žádný střih nebo post-processing záznamů
- Žádné plánování nahrávání (scheduler)
- Žádné nahrávání webkamery
- Žádné cloudové úložiště nebo upload
- Žádná podpora macOS nebo Linuxu
- Žádné nahrávání více zdrojů videa současně
- Žádné zachytávání zvuku jednotlivé aplikace (per-app audio) — v praxi nespolehlivé a vyžaduje aktivní okno; místo toho se používá systémový zvuk (loopback)
- Žádné globální klávesové zkratky (start/stop pouze z okna nebo tray menu)
- Žádné kodeky vyžadující HW akceleraci (NVENC, QuickSync) — SW kódování zajistí kompatibilitu
- Žádný přepis ani AI zpracování přímo v aplikaci (to probíhá externě přes Whisper)

---

## Design Principles (UX)

Cílový uživatel je **netechnický kancelářský pracovník** (ne IT profesionál). UI musí být:

- Bez technického žargonu — žádné zkratky jako „WASAPI", „H.264", „bitrate"; místo toho přirozené popisky
- Jedna obrazovka, žádné záložky, žádné wizardy
- Výchozí nastavení musí fungovat „out of the box" bez jakékoli konfigurace
- Chybové hlášky v češtině a srozumitelném jazyce (např. „Nepodařilo se spustit nahrávání — zkuste vybrat jiný zdroj zvuku")

---

## Technical Considerations

- **Stack:** C++ 20, Qt 6.x (Widgets), FFmpeg 7.x (volán jako přibalený `ffmpeg.exe`)
- **Build:** CMake + Ninja; výstup = jeden adresář se .exe a potřebnými DLL
- **FFmpeg integrace:** Volání přes `QProcess`; surová data z capture vrstvy se předávají přes pojmenované roury (named pipes)
- **Zachytávání obrazu (video):** Windows.Graphics.Capture (WGC) přes Win32 interop (`CreateForWindow` / `CreateForMonitor`) — bez systémového dialogu, zachytí celý monitor i konkrétní okno včetně GPU-akcelerovaných aplikací (Chrome, Teams, Office). Vyžaduje Windows 10 build 1903+
- **Optimalizace přenosu:** snímky se z BGRA převedou na NV12 před zápisem do roury — sníží datový tok ~2,5× a šetří FFmpegu interní konverzi pro libx264
- **Konstantní frame rate:** WGC dodává snímky jen při změně obsahu; časovač duplikuje poslední snímek pro stabilních 30 fps
- **Formáty zvuku:** sample rate, počet kanálů a vzorkový formát se zjišťují z `GetMixFormat()` daného zařízení a před sloučením (`amix`) se resamplují na společnou frekvenci — zabraňuje rozjetí synchronizace
- **Výstup:** standardní MP4/M4A pro běžné přehrávání; bez konverze je čte i lokální Whisper
- **Výkon:** Cílová zátěž CPU < 40 % při záznamu 1080p/30fps s SW kódováním (libx264 preset `veryfast`, CRF) na běžném kancelářském CPU (Intel Core i3 / AMD Ryzen 3)
- **Velikost distribuce:** Celý portable balíček by neměl překročit 150 MB

---

## Success Metrics

- Uživatel spustí aplikaci a pořídí první záznam do 60 sekund bez dokumentace
- CPU zátěž při záznamu 1080p/30fps < 40 % na běžném kancelářském CPU (Intel Core i3 nebo AMD Ryzen 3/5)
- Výstupní soubor se uloží správně po každém zastavení záznamu (žádná poškozená data)
- Aplikace funguje na čistém Windows 10 bez .NET, VC++ Redistributable nebo jiných prerekvizit

---

## Open Questions

Všechny otázky byly zodpovězeny. PRD je kompletní.

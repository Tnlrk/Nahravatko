#pragma once

#include <string>
#include <vector>
#include <windows.h>

// Pomocné Win32 funkce pro naplnění UI zdrojů (monitory, okna, audio zařízení).
namespace win32util {

struct MonitorInfo {
    HMONITOR handle = nullptr;
    std::wstring name;   // přátelský popisek, např. „Obrazovka 1"
    int width = 0;
    int height = 0;
};

struct WindowInfo {
    HWND handle = nullptr;
    std::wstring title;
    std::wstring app;     // název aplikace (z exe/popisu procesu), např. „Google Chrome"
    int width = 0;
    int height = 0;
};

struct AudioDevice {
    std::wstring id;     // endpoint ID (pro IMMDeviceEnumerator)
    std::wstring name;   // přátelský název, např. „Mikrofon (Realtek)"
};

// Výčet připojených monitorů (číslované zleva doprava dle souřadnic).
std::vector<MonitorInfo> enumMonitors();

// Výčet viditelných oken na ploše (s názvem, ne tool/cloaked okna).
std::vector<WindowInfo> enumWindows();

// Výčet aktivních audio zařízení. capture=true → vstupy (mikrofony, eCapture),
// capture=false → výstupy (reproduktory/sluchátka, eRender).
std::vector<AudioDevice> enumAudioDevices(bool capture);

} // namespace win32util

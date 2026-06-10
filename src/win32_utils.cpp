#include "win32_utils.h"

#include <algorithm>
#include <cwchar>
#include <vector>

#include <QCoreApplication>
#include <QString>

#include <windows.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>
#include <propvarutil.h>

namespace win32util {

namespace {

// RAII pro CoInitializeEx — nevadí, pokud už je COM inicializované v jiném apartmentu.
struct ComScope {
    bool initialized = false;
    ComScope() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        initialized = SUCCEEDED(hr); // RPC_E_CHANGED_MODE → už init jinak, neuninitujeme
    }
    ~ComScope() {
        if (initialized) CoUninitialize();
    }
};

BOOL CALLBACK monitorProc(HMONITOR hMon, HDC, LPRECT, LPARAM lparam)
{
    auto* out = reinterpret_cast<std::vector<MonitorInfo>*>(lparam);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        MonitorInfo info;
        info.handle = hMon;
        info.width = mi.rcMonitor.right - mi.rcMonitor.left;
        info.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
        out->push_back(info);
    }
    return TRUE;
}

// Popis aplikace z verze souboru (např. „Google Chrome"); prázdné, když chybí.
std::wstring fileDescription(const wchar_t* path)
{
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &dummy);
    if (size == 0) return L"";
    std::vector<BYTE> buf(size);
    if (!GetFileVersionInfoW(path, 0, size, buf.data())) return L"";

    struct LangCodePage { WORD language; WORD codePage; };
    LangCodePage* trans = nullptr;
    UINT transLen = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<LPVOID*>(&trans), &transLen)
        || transLen < sizeof(LangCodePage)) {
        return L"";
    }
    wchar_t sub[64];
    swprintf(sub, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription",
             trans[0].language, trans[0].codePage);
    LPWSTR desc = nullptr;
    UINT descLen = 0;
    if (VerQueryValueW(buf.data(), sub, reinterpret_cast<LPVOID*>(&desc), &descLen)
        && desc && descLen > 0) {
        return std::wstring(desc);
    }
    return L"";
}

// Název aplikace pro dané okno: popis z verze, fallback na jméno .exe bez přípony.
std::wstring windowAppName(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return L"";
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return L"";

    std::wstring result;
    wchar_t path[MAX_PATH];
    DWORD len = MAX_PATH;
    if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
        result = fileDescription(path);
        if (result.empty()) {
            std::wstring p(path);
            const size_t slash = p.find_last_of(L"\\/");
            std::wstring base = (slash == std::wstring::npos) ? p : p.substr(slash + 1);
            const size_t dot = base.find_last_of(L'.');
            if (dot != std::wstring::npos) base = base.substr(0, dot);
            result = base;
        }
    }
    CloseHandle(proc);
    return result;
}

bool isCloaked(HWND hwnd)
{
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        return cloaked != 0;
    return false;
}

BOOL CALLBACK windowProc(HWND hwnd, LPARAM lparam)
{
    auto* out = reinterpret_cast<std::vector<WindowInfo>*>(lparam);

    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE; // přeskočit owned okna

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;
    if (isCloaked(hwnd)) return TRUE;

    const int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return TRUE;

    std::wstring title(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(hwnd, title.data(), len + 1);
    title.resize(static_cast<size_t>(len));
    if (title.empty()) return TRUE;

    RECT rc{};
    GetClientRect(hwnd, &rc);

    WindowInfo info;
    info.handle = hwnd;
    info.title = title;
    info.app = windowAppName(hwnd);
    info.width = rc.right - rc.left;
    info.height = rc.bottom - rc.top;
    out->push_back(info);
    return TRUE;
}

} // namespace

std::vector<MonitorInfo> enumMonitors()
{
    std::vector<MonitorInfo> monitors;
    EnumDisplayMonitors(nullptr, nullptr, monitorProc,
                        reinterpret_cast<LPARAM>(&monitors));

    for (size_t i = 0; i < monitors.size(); ++i)
        monitors[i].name = QCoreApplication::translate("win32util", "Obrazovka %1")
                               .arg(i + 1).toStdWString();

    return monitors;
}

std::vector<WindowInfo> enumWindows()
{
    std::vector<WindowInfo> windows;
    EnumWindows(windowProc, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

std::vector<AudioDevice> enumAudioDevices(bool capture)
{
    std::vector<AudioDevice> devices;
    ComScope com;

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || !enumerator) return devices;

    IMMDeviceCollection* collection = nullptr;
    const EDataFlow flow = capture ? eCapture : eRender;
    hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr) && collection) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(i, &device)) || !device) continue;

            AudioDevice entry;
            LPWSTR id = nullptr;
            if (SUCCEEDED(device->GetId(&id)) && id) {
                entry.id = id;
                CoTaskMemFree(id);
            }

            IPropertyStore* store = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store) {
                PROPVARIANT name;
                PropVariantInit(&name);
                if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &name))
                    && name.vt == VT_LPWSTR && name.pwszVal) {
                    entry.name = name.pwszVal;
                }
                PropVariantClear(&name);
                store->Release();
            }
            if (entry.name.empty())
                entry.name = QCoreApplication::translate("win32util", "(neznámé zařízení)")
                                 .toStdWString();
            devices.push_back(entry);
            device->Release();
        }
        collection->Release();
    }
    enumerator->Release();
    return devices;
}

} // namespace win32util

#include "render.h"
#include <dbt.h>
#include <shellapi.h>

namespace {

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT_PTR IDT_POLL = 1;
constexpr UINT_PTR IDT_DEBOUNCE = 2;

constexpr UINT IDM_REFRESH = 1001;
constexpr UINT IDM_SETTINGS = 1002;
constexpr UINT IDM_EXIT = 1003;

constexpr int IDC_STYLE = 2001;
constexpr int IDC_INTERVAL = 2002;
constexpr int IDC_SYSTEM = 2003;
constexpr int IDC_DEMO = 2004;
constexpr int IDC_SAVE = 2005;
constexpr int IDC_CANCEL = 2006;
constexpr int IDC_SINGLE = 2007;
constexpr int IDC_ICONSIZE = 2008;
constexpr int IDC_STARTUP = 2009;

constexpr const wchar_t* kStartupRegPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kStartupValueName = L"BatteryIndicatorApp";
constexpr int kIntervals[] = {10, 30, 60, 300, 900};
constexpr GUID kGuidBthPort = {0x0850302a, 0xb344, 0x4fda, {0x9b, 0xe9, 0x90, 0x57, 0x6b, 0x8d, 0x46, 0xf0}};

struct AppSettings {
    int style = 0;
    int iconSize = 2;
    int intervalSec = 60;
    bool showSystemBattery = false;
    bool showDemoWhenEmpty = false;
    bool singleIconMode = false;
};

AppSettings g_settings;
std::vector<DeviceInfo> g_devices;
std::vector<HICON> g_trayIcons;
HWND g_mainHwnd = nullptr;
HWND g_settingsHwnd = nullptr;
HDEVNOTIFY g_hidNotify = nullptr;
HDEVNOTIFY g_bthNotify = nullptr;
UINT g_wmTaskbarCreated = 0;

std::wstring GetIniPath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    size_t slash = s.find_last_of(L"\\/");
    return (slash != std::wstring::npos ? s.substr(0, slash + 1) : L"") + L"settings.ini";
}

void LoadSettings() {
    std::wstring ini = GetIniPath();
    g_settings.style = std::clamp<int>(GetPrivateProfileIntW(L"Settings", L"Style", 0, ini.c_str()), 0, 4);
    g_settings.iconSize = std::clamp<int>(GetPrivateProfileIntW(L"Settings", L"IconSize", 2, ini.c_str()), 0, 3);
    g_settings.intervalSec = std::max<int>(5, GetPrivateProfileIntW(L"Settings", L"IntervalSec", 60, ini.c_str()));
    g_settings.showSystemBattery = GetPrivateProfileIntW(L"Settings", L"ShowSystemBattery", 0, ini.c_str()) != 0;
    g_settings.showDemoWhenEmpty = GetPrivateProfileIntW(L"Settings", L"ShowDemoWhenEmpty", 0, ini.c_str()) != 0;
    g_settings.singleIconMode = GetPrivateProfileIntW(L"Settings", L"SingleIconMode", 0, ini.c_str()) != 0;
}

void SaveSettings() {
    std::wstring ini = GetIniPath();
    WritePrivateProfileStringW(L"Settings", L"Style", std::to_wstring(g_settings.style).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Settings", L"IconSize", std::to_wstring(g_settings.iconSize).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Settings", L"IntervalSec", std::to_wstring(g_settings.intervalSec).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Settings", L"ShowSystemBattery", g_settings.showSystemBattery ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Settings", L"ShowDemoWhenEmpty", g_settings.showDemoWhenEmpty ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Settings", L"SingleIconMode", g_settings.singleIconMode ? L"1" : L"0", ini.c_str());
}

bool IsStartupEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kStartupRegPath, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    DWORD type = 0, cb = 0;
    bool exists = RegQueryValueExW(key, kStartupValueName, nullptr, &type, nullptr, &cb) == ERROR_SUCCESS && type == REG_SZ;
    RegCloseKey(key);
    return exists;
}

void SetStartupEnabled(bool enable) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kStartupRegPath, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (enable) {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring cmd = std::wstring(L"\"") + exePath + L"\"";
        RegSetValueExW(key, kStartupValueName, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kStartupValueName);
    }
    RegCloseKey(key);
}

std::wstring FormatDeviceTip(const DeviceInfo& d) {
    return d.name + L": " + std::to_wstring(d.percent) + L"%" + (d.charging ? L", charging" : L"");
}

void DeleteTraySlot(HWND hwnd, UINT uid) {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd;
    nid.uID = uid;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void PromoteTrayIconsToTaskbar(bool force = false) {
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) != ERROR_SUCCESS)
        return;

    wchar_t subName[128]{};
    for (DWORD idx = 0;; ++idx) {
        DWORD subLen = _countof(subName);
        if (RegEnumKeyExW(root, idx, subName, &subLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;

        HKEY sub = nullptr;
        if (RegOpenKeyExW(root, subName, 0, KEY_READ | KEY_SET_VALUE, &sub) != ERROR_SUCCESS) continue;

        wchar_t exePath[512]{};
        DWORD type = 0, cb = sizeof(exePath) - sizeof(wchar_t);
        if (RegQueryValueExW(sub, L"ExecutablePath", nullptr, &type, reinterpret_cast<PBYTE>(exePath), &cb) == ERROR_SUCCESS &&
            type == REG_SZ && detail::ToUpper(exePath).find(L"BATTERYINDICATOR.EXE") != std::wstring::npos) {
            DWORD cur = 0, curCb = sizeof(cur), curType = 0;
            LONG q = RegQueryValueExW(sub, L"IsPromoted", nullptr, &curType, reinterpret_cast<PBYTE>(&cur), &curCb);
            if (force || q != ERROR_SUCCESS) {
                DWORD val = 1;
                RegSetValueExW(sub, L"IsPromoted", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(val));
            }
        }
        RegCloseKey(sub);
    }
    RegCloseKey(root);
}

void UpdateTrayIcon(HWND hwnd, bool forceAdd = false) {
    const size_t targetCount = (g_settings.singleIconMode || g_devices.empty()) ? 1 : g_devices.size();
    const size_t prevCount = g_trayIcons.size();

    for (size_t i = targetCount; i < prevCount; ++i) {
        DeleteTraySlot(hwnd, static_cast<UINT>(i + 1));
        if (g_trayIcons[i]) DestroyIcon(g_trayIcons[i]);
    }
    g_trayIcons.resize(targetCount, nullptr);

    const auto style = static_cast<IndicatorStyle>(g_settings.style);
    for (size_t i = 0; i < targetCount; ++i) {
        HICON newIcon = nullptr;
        std::wstring tip;
        if (g_devices.empty()) {
            newIcon = CreateDeviceTrayIcon(nullptr, style, g_settings.iconSize);
            tip = L"Battery Indicator: No connected peripherals";
        } else if (g_settings.singleIconMode) {
            newIcon = CreateBatteryTrayIcon(g_devices, style, g_settings.iconSize);
            for (size_t k = 0; k < g_devices.size(); ++k) {
                if (k > 0) tip += L"\n";
                tip += FormatDeviceTip(g_devices[k]);
            }
        } else {
            newIcon = CreateDeviceTrayIcon(&g_devices[i], style, g_settings.iconSize);
            tip = FormatDeviceTip(g_devices[i]);
        }

        NOTIFYICONDATAW nid{sizeof(nid)};
        nid.hWnd = hwnd;
        nid.uID = static_cast<UINT>(i + 1);
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_TRAYICON;
        nid.hIcon = newIcon;
        if (tip.size() >= _countof(nid.szTip)) tip.resize(_countof(nid.szTip) - 1);
        wcscpy_s(nid.szTip, tip.c_str());

        bool added = (forceAdd || i >= prevCount || !Shell_NotifyIconW(NIM_MODIFY, &nid)) &&
                     Shell_NotifyIconW(NIM_ADD, &nid);
        if (added) PromoteTrayIconsToTaskbar(false);

        if (g_trayIcons[i]) DestroyIcon(g_trayIcons[i]);
        g_trayIcons[i] = newIcon;
    }
}

void RefreshDevicesAndTray(HWND hwnd) {
    g_devices = QueryAllDevices(g_settings.showSystemBattery, g_settings.showDemoWhenEmpty);
    UpdateTrayIcon(hwnd);
}

void ShowTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    if (g_devices.empty()) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No connected peripherals");
    } else {
        for (const auto& d : g_devices) {
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, FormatDeviceTip(d).c_str());
        }
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_REFRESH, L"Refresh Now");
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Settings...");
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HFONT s_font = nullptr;
    auto setFont = [](HWND ctrl) { SendMessageW(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE); };
    auto ctlId = [](int id) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); };

    switch (msg) {
    case WM_CREATE: {
        s_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        auto addCombo = [&](int y, const wchar_t* label, int id, int dropH,
                            std::initializer_list<const wchar_t*> items, int sel) {
            setFont(CreateWindowW(L"STATIC", label, WS_CHILD | WS_VISIBLE, 16, y + 4, 120, 22, hwnd, nullptr, nullptr, nullptr));
            HWND cb = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
                                    145, y, 170, dropH, hwnd, ctlId(id), nullptr, nullptr);
            for (const auto* s : items) SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
            SendMessageW(cb, CB_SETCURSEL, sel, 0);
            setFont(cb);
        };
        auto addCheck = [&](int y, const wchar_t* label, int id, bool checked) {
            HWND chk = CreateWindowW(L"BUTTON", label, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                                     16, y, 295, 22, hwnd, ctlId(id), nullptr, nullptr);
            SendMessageW(chk, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
            setFont(chk);
        };

        int selIdx = 2;
        for (int i = 0; i < 5; ++i) {
            if (kIntervals[i] == g_settings.intervalSec) selIdx = i;
        }
        addCombo(14, L"Indicator Design:", IDC_STYLE, 160,
                 {L"Ring + Device Icon", L"Device Icon + Bar", L"Ring + Percentage", L"Percentage Text", L"Classic Battery Bar"},
                 g_settings.style);
        addCombo(50, L"Icon / Text Size:", IDC_ICONSIZE, 140, {L"Small", L"Medium", L"Large", L"Extra Large"}, g_settings.iconSize);
        addCombo(86, L"Update Interval:", IDC_INTERVAL, 150,
                 {L"10 seconds", L"30 seconds", L"1 minute", L"5 minutes", L"15 minutes"}, selIdx);
        addCheck(128, L"Include System / Laptop Battery", IDC_SYSTEM, g_settings.showSystemBattery);
        addCheck(156, L"Show Demo Devices (when none detected)", IDC_DEMO, g_settings.showDemoWhenEmpty);
        addCheck(184, L"Combine into Single Tray Icon", IDC_SINGLE, g_settings.singleIconMode);
        addCheck(212, L"Launch on Windows Startup", IDC_STARTUP, IsStartupEnabled());

        setFont(CreateWindowW(L"BUTTON", L"Save && Apply", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                              115, 254, 98, 28, hwnd, ctlId(IDC_SAVE), nullptr, nullptr));
        setFont(CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                              222, 254, 92, 28, hwnd, ctlId(IDC_CANCEL), nullptr, nullptr));
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_SAVE) {
            int styleIdx = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_STYLE, CB_GETCURSEL, 0, 0));
            int sizeIdx = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_ICONSIZE, CB_GETCURSEL, 0, 0));
            int intIdx = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_INTERVAL, CB_GETCURSEL, 0, 0));
            g_settings.style = std::clamp(styleIdx, 0, 4);
            g_settings.iconSize = std::clamp(sizeIdx, 0, 3);
            g_settings.intervalSec = kIntervals[std::clamp(intIdx, 0, 4)];
            g_settings.showSystemBattery = IsDlgButtonChecked(hwnd, IDC_SYSTEM) == BST_CHECKED;
            g_settings.showDemoWhenEmpty = IsDlgButtonChecked(hwnd, IDC_DEMO) == BST_CHECKED;
            g_settings.singleIconMode = IsDlgButtonChecked(hwnd, IDC_SINGLE) == BST_CHECKED;
            SaveSettings();
            SetStartupEnabled(IsDlgButtonChecked(hwnd, IDC_STARTUP) == BST_CHECKED);
            SetTimer(g_mainHwnd, IDT_POLL, static_cast<UINT>(g_settings.intervalSec) * 1000u, nullptr);
            RefreshDevicesAndTray(g_mainHwnd);
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == IDC_CANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_DESTROY:
        if (s_font) { DeleteObject(s_font); s_font = nullptr; }
        g_settingsHwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void OpenSettingsWindow(HWND owner) {
    if (g_settingsHwnd && IsWindow(g_settingsHwnd)) {
        SetForegroundWindow(g_settingsHwnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = SettingsWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"BatterySettingsWnd";
        RegisterClassW(&wc);
        registered = true;
    }
    int w = 340, h = 330;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    g_settingsHwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"BatterySettingsWnd", L"Battery Indicator Settings",
                                     WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                     x, y, w, h, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
}

HDEVNOTIFY RegisterInterfaceNotify(HWND hwnd, const GUID& guid) {
    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = guid;
    return RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_wmTaskbarCreated && g_wmTaskbarCreated != 0) {
        UpdateTrayIcon(hwnd, true);
        return 0;
    }
    switch (msg) {
    case WM_CREATE: {
        GUID hidGuid{};
        HidD_GetHidGuid(&hidGuid);
        g_hidNotify = RegisterInterfaceNotify(hwnd, hidGuid);
        g_bthNotify = RegisterInterfaceNotify(hwnd, kGuidBthPort);
        PromoteTrayIconsToTaskbar(true);
        SetTimer(hwnd, IDT_POLL, static_cast<UINT>(g_settings.intervalSec) * 1000u, nullptr);
        RefreshDevicesAndTray(hwnd);
        return 0;
    }
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) ShowTrayMenu(hwnd);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_REFRESH: RefreshDevicesAndTray(hwnd); break;
        case IDM_SETTINGS: OpenSettingsWindow(hwnd); break;
        case IDM_EXIT: DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_TIMER:
        if (wParam == IDT_DEBOUNCE) KillTimer(hwnd, IDT_DEBOUNCE);
        if (wParam == IDT_DEBOUNCE || wParam == IDT_POLL) RefreshDevicesAndTray(hwnd);
        return 0;
    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE || wParam == DBT_DEVNODES_CHANGED) {
            SetTimer(hwnd, IDT_DEBOUNCE, 800, nullptr);
        }
        return TRUE;
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMPOWERSTATUSCHANGE || wParam == PBT_APMRESUMEAUTOMATIC) {
            SetTimer(hwnd, IDT_DEBOUNCE, 500, nullptr);
        }
        return TRUE;
    case WM_DESTROY: {
        KillTimer(hwnd, IDT_POLL);
        KillTimer(hwnd, IDT_DEBOUNCE);
        if (g_hidNotify) UnregisterDeviceNotification(g_hidNotify);
        if (g_bthNotify) UnregisterDeviceNotification(g_bthNotify);
        for (size_t i = 0; i < g_trayIcons.size(); ++i) {
            DeleteTraySlot(hwnd, static_cast<UINT>(i + 1));
            if (g_trayIcons[i]) DestroyIcon(g_trayIcons[i]);
        }
        g_trayIcons.clear();
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    if (HWND prev = FindWindowW(L"BatteryIndicatorTrayWnd", nullptr)) {
        SendMessageTimeoutW(prev, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 1000, nullptr);
        Sleep(150);
    }
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\BatteryIndicatorAppMutex");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    LoadSettings();
    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc{};
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"BatteryIndicatorTrayWnd";
    RegisterClassW(&wc);

    g_mainHwnd = CreateWindowW(wc.lpszClassName, L"BatteryIndicator", 0, 0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!g_settingsHwnd || !IsDialogMessageW(g_settingsHwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (mutex) CloseHandle(mutex);
    return 0;
}

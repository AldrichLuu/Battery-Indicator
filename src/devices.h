#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bluetoothapis.h>
#include <cfgmgr32.h>
#include <devpropdef.h>
#include <setupapi.h>
#include <xinput.h>

extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

enum class DeviceKind { Mouse, Controller, Headset, Keyboard, Generic };

struct DeviceInfo {
    std::wstring name;
    int percent = 0;
    bool charging = false;
    DeviceKind kind = DeviceKind::Generic;
};

namespace detail {

inline constexpr DEVPROPKEY kPkeyBluetoothBattery = {
    {0x104ea319, 0x6ee2, 0x4701, {0xbd, 0x47, 0x8d, 0xdb, 0xf4, 0x25, 0xbb, 0xe5}}, 2
};

inline constexpr DEVPROPKEY kPkeyIsConnected = {
    {0x83da6326, 0x97a6, 0x4088, {0x94, 0x53, 0xa1, 0x92, 0x3f, 0x57, 0x3b, 0x29}}, 15
};

inline std::wstring ToUpper(std::wstring s) {
    for (wchar_t& c : s) c = towupper(c);
    return s;
}

inline DeviceKind GuessDeviceKind(const std::wstring& name, DeviceKind fallback = DeviceKind::Generic) {
    std::wstring u = ToUpper(name);
    auto has = [&](std::initializer_list<const wchar_t*> keys) {
        return std::any_of(keys.begin(), keys.end(), [&](const wchar_t* k) { return u.find(k) != std::wstring::npos; });
    };
    if (has({L"CONTROLLER", L"GAMEPAD", L"JOYSTICK", L"VADER", L"APEX", L"XBOX", L"DUALSENSE", L"DUALSHOCK", L"JOY-CON"}))
        return DeviceKind::Controller;
    if (has({L"MOUSE", L"BASILISK", L"VIPER", L"DEATHADDER", L"NAGA", L"COBRA", L"SUPERLIGHT", L"BEAST", L"PULSAR", L"LAMZU"}))
        return DeviceKind::Mouse;
    if (has({L"HEADSET", L"HEADPHONE", L"EARBUD", L"AIRPODS", L"BUDS", L"KRAKEN", L"BLACKSHARK", L"ARCTIS", L"AUDIO", L"WH-", L"WF-"}))
        return DeviceKind::Headset;
    if (has({L"KEYBOARD", L"HUNTSMAN", L"BLACKWIDOW", L"KEYCHRON", L"WOOTING"}))
        return DeviceKind::Keyboard;
    return fallback;
}

inline void AddUniqueDevice(std::vector<DeviceInfo>& out, std::wstring name, int percent,
                            bool charging = false, DeviceKind kind = DeviceKind::Generic) {
    if (name.empty()) name = L"Wireless Device";
    percent = std::clamp(percent, 0, 100);
    for (const auto& d : out) {
        if (_wcsicmp(d.name.c_str(), name.c_str()) == 0) return;
    }
    out.push_back({std::move(name), percent, charging, kind});
}

inline std::wstring GetRegistryProp(HDEVINFO devs, SP_DEVINFO_DATA& info, DWORD prop) {
    wchar_t buf[256]{};
    return SetupDiGetDeviceRegistryPropertyW(devs, &info, prop, nullptr, reinterpret_cast<PBYTE>(buf), sizeof(buf), nullptr)
               ? buf
               : L"";
}

inline bool HasStartedChild(DEVINST inst) {
    DEVINST child = 0;
    if (CM_Get_Child(&child, inst, 0) != CR_SUCCESS) return false;
    while (child != 0) {
        ULONG st = 0, prob = 0;
        if (CM_Get_DevNode_Status(&st, &prob, child, 0) == CR_SUCCESS && (st & DN_STARTED)) return true;
        if (CM_Get_Sibling(&child, child, 0) != CR_SUCCESS) break;
    }
    return false;
}

inline void QueryBluetoothDevices(std::vector<DeviceInfo>& out) {
    std::vector<std::wstring> disconnectedMacs;
    BLUETOOTH_DEVICE_SEARCH_PARAMS sp{sizeof(sp), TRUE, TRUE, FALSE, TRUE, FALSE};
    BLUETOOTH_DEVICE_INFO bdi{sizeof(bdi)};
    if (HBLUETOOTH_DEVICE_FIND hFind = BluetoothFindFirstDevice(&sp, &bdi)) {
        do {
            if (!bdi.fConnected) {
                wchar_t mac[16]{};
                const BYTE* a = bdi.Address.rgBytes;
                swprintf_s(mac, L"%02X%02X%02X%02X%02X%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
                disconnectedMacs.emplace_back(mac);
            }
        } while (BluetoothFindNextDevice(hFind, &bdi));
        BluetoothFindDeviceClose(hFind);
    }

    HDEVINFO devs = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devs == INVALID_HANDLE_VALUE) return;

    SP_DEVINFO_DATA info{sizeof(info)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &info); ++i) {
        DEVPROPTYPE propType = 0;
        BYTE pct = 0;
        if (!SetupDiGetDevicePropertyW(devs, &info, &kPkeyBluetoothBattery, &propType, &pct, sizeof(pct), nullptr, 0) ||
            propType != DEVPROP_TYPE_BYTE || pct > 100)
            continue;

        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, info.DevInst, 0) != CR_SUCCESS || !(status & DN_STARTED))
            continue;

        DEVPROP_BOOLEAN isConn = DEVPROP_FALSE;
        bool hasIsConn = SetupDiGetDevicePropertyW(devs, &info, &kPkeyIsConnected, &propType,
                                                   reinterpret_cast<PBYTE>(&isConn), sizeof(isConn), nullptr, 0) &&
                         propType == DEVPROP_TYPE_BOOLEAN;
        if (hasIsConn && isConn == DEVPROP_FALSE) continue;

        if (!hasIsConn || isConn != DEVPROP_TRUE) {
            wchar_t instBuf[512]{};
            SetupDiGetDeviceInstanceIdW(devs, &info, instBuf, _countof(instBuf), nullptr);
            std::wstring instUpper = ToUpper(instBuf);
            if (instUpper.rfind(L"BTHLE\\DEV_", 0) == 0 && !HasStartedChild(info.DevInst)) continue;
            if (std::any_of(disconnectedMacs.begin(), disconnectedMacs.end(),
                            [&](const std::wstring& mac) { return instUpper.find(mac) != std::wstring::npos; }))
                continue;
        }

        std::wstring name = GetRegistryProp(devs, info, SPDRP_FRIENDLYNAME);
        if (name.empty()) name = GetRegistryProp(devs, info, SPDRP_DEVICEDESC);
        AddUniqueDevice(out, name, pct, false, GuessDeviceKind(name));
    }
    SetupDiDestroyDeviceInfoList(devs);
}

inline void QueryXInputDevices(std::vector<DeviceInfo>& out) {
    using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    using GetBattFn = DWORD(WINAPI*)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);

    static HMODULE hXInput = [] {
        HMODULE m = LoadLibraryW(L"XInput1_4.dll");
        return m ? m : LoadLibraryW(L"XInput9_1_0.dll");
    }();
    if (!hXInput) return;

    static auto pGetState = reinterpret_cast<GetStateFn>(GetProcAddress(hXInput, "XInputGetState"));
    static auto pGetBatt = reinterpret_cast<GetBattFn>(GetProcAddress(hXInput, "XInputGetBatteryInformation"));
    if (!pGetState || !pGetBatt) return;

    for (DWORD i = 0; i < 4; ++i) {
        XINPUT_STATE st{};
        XINPUT_BATTERY_INFORMATION bat{};
        if (pGetState(i, &st) != ERROR_SUCCESS || pGetBatt(i, BATTERY_DEVTYPE_GAMEPAD, &bat) != ERROR_SUCCESS) continue;
        if (bat.BatteryType == BATTERY_TYPE_DISCONNECTED || bat.BatteryType == BATTERY_TYPE_WIRED) continue;

        constexpr int kLevels[] = {10, 30, 65, 100};
        int pct = (bat.BatteryLevel <= 3) ? kLevels[bat.BatteryLevel] : 50;
        AddUniqueDevice(out, L"Controller " + std::to_wstring(i + 1), pct, false, DeviceKind::Controller);
    }
}

inline int QueryStandardHidBattery(HANDLE h, PHIDP_PREPARSED_DATA ppd, const HIDP_CAPS& caps) {
    for (HIDP_REPORT_TYPE rtype : {HidP_Feature, HidP_Input}) {
        USHORT capCount = (rtype == HidP_Feature) ? caps.NumberFeatureValueCaps : caps.NumberInputValueCaps;
        USHORT reportLen = (rtype == HidP_Feature) ? caps.FeatureReportByteLength : caps.InputReportByteLength;
        if (capCount == 0 || reportLen == 0) continue;

        std::vector<HIDP_VALUE_CAPS> vcaps(capCount);
        if (HidP_GetValueCaps(rtype, vcaps.data(), &capCount, ppd) != HIDP_STATUS_SUCCESS) continue;

        for (USHORT i = 0; i < capCount; ++i) {
            const auto& vc = vcaps[i];
            USAGE usage = vc.IsRange ? vc.Range.UsageMin : vc.NotRange.Usage;
            bool isBatt = (vc.UsagePage == 0x06 && usage == 0x20) ||
                          ((vc.UsagePage == 0x84 || vc.UsagePage == 0x85) && (usage == 0x65 || usage == 0x66));
            if (!isBatt) continue;

            std::vector<BYTE> report(reportLen, 0);
            report[0] = vc.ReportID;
            BOOLEAN ok = (rtype == HidP_Feature) ? HidD_GetFeature(h, report.data(), reportLen)
                                                 : HidD_GetInputReport(h, report.data(), reportLen);
            if (!ok) continue;

            ULONG raw = 0;
            if (HidP_GetUsageValue(rtype, vc.UsagePage, vc.LinkCollection, usage, &raw, ppd,
                                   reinterpret_cast<PCHAR>(report.data()), reportLen) == HIDP_STATUS_SUCCESS) {
                if (vc.LogicalMax > vc.LogicalMin) {
                    return static_cast<int>(((static_cast<long long>(raw) - vc.LogicalMin) * 100) /
                                            (vc.LogicalMax - vc.LogicalMin));
                }
                if (raw <= 100) return static_cast<int>(raw);
            }
        }
    }
    return -1;
}

inline bool QueryHidFeatureBattery(HANDLE h, int& outPct, bool& outCharging) {
    auto sendCmd = [h](BYTE txId, BYTE cmdId, BYTE (&resp)[91]) {
        BYTE buf[91]{0, 0, txId, 0, 0, 0, 0x02, 0x07, cmdId};
        buf[89] = cmdId ^ 0x05;
        if (!HidD_SetFeature(h, buf, sizeof(buf))) return false;
        Sleep(20);
        std::fill_n(resp, sizeof(resp), BYTE{0});
        return HidD_GetFeature(h, resp, sizeof(resp)) != FALSE;
    };

    for (BYTE txId : {BYTE{0x1F}, BYTE{0x3F}, BYTE{0xFF}}) {
        BYTE resp[91]{};
        if (sendCmd(txId, 0x80, resp) && resp[1] == 0x02 && resp[8] == 0x80 && resp[10] > 0) {
            outPct = (static_cast<int>(resp[10]) * 100 + 127) / 255;
            BYTE chg[91]{};
            outCharging = sendCmd(txId, 0x84, chg) && chg[1] == 0x02 && chg[10] != 0;
            return true;
        }
    }
    return false;
}

inline bool QueryControllerHidBattery(const wchar_t* devicePath, int& outPct, bool& outCharging) {
    HANDLE h = CreateFileW(devicePath, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    OVERLAPPED ovR{}, ovW{};
    ovR.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool ok = false;

    if (ovR.hEvent) {
        BYTE inBuf[33]{}, outBuf[33]{0x00, 0x5A, 0xA5, 0x01, 0x02, 0x03};
        DWORD rd = 0, wr = 0;
        bool readDone = ReadFile(h, inBuf, sizeof(inBuf), &rd, &ovR) != FALSE;
        bool readPending = !readDone && GetLastError() == ERROR_IO_PENDING;
        if (readDone || readPending) {
            bool writeDone = WriteFile(h, outBuf, sizeof(outBuf), &wr, &ovW) != FALSE;
            bool writePending = !writeDone && GetLastError() == ERROR_IO_PENDING;
            if (writeDone || writePending) {
                if (readPending && WaitForSingleObject(ovR.hEvent, 150) == WAIT_OBJECT_0) {
                    readDone = GetOverlappedResult(h, &ovR, &rd, FALSE) != FALSE;
                    readPending = false;
                }
                if (readDone && rd >= 13 && inBuf[1] == 0x5A && inBuf[2] == 0xA5 &&
                    inBuf[3] == 0x01 && inBuf[6] != 0x00 && inBuf[12] != 0x00) {
                    BYTE b = inBuf[12];
                    outCharging = (b >> 4) >= 1;
                    int raw = b & 0x0F;
                    outPct = (outCharging && raw == 0) ? (((b >> 4) >= 2) ? 100 : 50)
                                                       : ((inBuf[6] == 0x82 || raw > 5) ? raw * 10 : raw * 20);
                    ok = true;
                }
            }
            CancelIo(h);
            DWORD dummy = 0;
            if (writePending) GetOverlappedResult(h, &ovW, &dummy, TRUE);
            if (readPending) GetOverlappedResult(h, &ovR, &dummy, TRUE);
        }
        CloseHandle(ovR.hEvent);
    }

    CloseHandle(h);
    return ok;
}

inline void QueryHidDevices(std::vector<DeviceInfo>& out) {
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devs = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return;

    SP_DEVICE_INTERFACE_DATA ifData{sizeof(ifData)};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devs, nullptr, &hidGuid, i, &ifData); ++i) {
        DWORD req = 0;
        SetupDiGetDeviceInterfaceDetailW(devs, &ifData, nullptr, 0, &req, nullptr);
        if (req == 0) continue;

        std::vector<BYTE> detailBuf(req);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA devInfo{sizeof(devInfo)};
        if (!SetupDiGetDeviceInterfaceDetailW(devs, &ifData, detail, req, nullptr, &devInfo)) continue;

        HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;

        PHIDP_PREPARSED_DATA ppd = nullptr;
        if (HidD_GetPreparsedData(h, &ppd)) {
            HIDP_CAPS caps{};
            if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
                DeviceKind fallbackKind = DeviceKind::Generic;
                if (caps.UsagePage == 0x01) {
                    if (caps.Usage == 0x02) fallbackKind = DeviceKind::Mouse;
                    else if (caps.Usage == 0x04 || caps.Usage == 0x05) fallbackKind = DeviceKind::Controller;
                    else if (caps.Usage == 0x06) fallbackKind = DeviceKind::Keyboard;
                }

                bool charging = false;
                int pct = QueryStandardHidBattery(h, ppd, caps);
                if (pct < 0 && caps.FeatureReportByteLength == 91 && QueryHidFeatureBattery(h, pct, charging)) {
                    fallbackKind = DeviceKind::Mouse;
                } else if (pct < 0 && caps.UsagePage == 0xFFA0 && caps.InputReportByteLength == 33 &&
                           QueryControllerHidBattery(detail->DevicePath, pct, charging)) {
                    fallbackKind = DeviceKind::Controller;
                }

                if (pct >= 0) {
                    wchar_t prod[128]{};
                    std::wstring name = HidD_GetProductString(h, prod, sizeof(prod)) ? prod : L"";
                    if (name.empty()) name = GetRegistryProp(devs, devInfo, SPDRP_DEVICEDESC);
                    DeviceKind kind = (caps.UsagePage == 0xFFA0) ? DeviceKind::Controller : GuessDeviceKind(name, fallbackKind);
                    AddUniqueDevice(out, std::move(name), pct, charging, kind);
                }
            }
            HidD_FreePreparsedData(ppd);
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(devs);
}

inline void QuerySystemBattery(std::vector<DeviceInfo>& out) {
    SYSTEM_POWER_STATUS sps{};
    if (!GetSystemPowerStatus(&sps) || sps.BatteryFlag == 128 || sps.BatteryFlag == 255 || sps.BatteryLifePercent > 100)
        return;
    bool charging = (sps.ACLineStatus == 1) || ((sps.BatteryFlag & 8) != 0);
    AddUniqueDevice(out, L"System Battery", sps.BatteryLifePercent, charging, DeviceKind::Generic);
}

} // namespace detail

inline std::vector<DeviceInfo> QueryAllDevices(bool includeSystemBattery, bool showDemoWhenEmpty) {
    std::vector<DeviceInfo> devices;
    detail::QueryBluetoothDevices(devices);
    detail::QueryXInputDevices(devices);
    detail::QueryHidDevices(devices);
    if (includeSystemBattery) detail::QuerySystemBattery(devices);

    if (devices.empty() && showDemoWhenEmpty) {
        devices.push_back({L"Demo Mouse", 63, true, DeviceKind::Mouse});
        devices.push_back({L"Demo Controller", 18, false, DeviceKind::Controller});
    }
    return devices;
}

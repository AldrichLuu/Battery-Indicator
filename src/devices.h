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
#include <set>
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
    auto hasAny = [&](std::initializer_list<const wchar_t*> keys) {
        for (const wchar_t* k : keys) {
            if (u.find(k) != std::wstring::npos) return true;
        }
        return false;
    };
    if (hasAny({L"CONTROLLER", L"GAMEPAD", L"VADER", L"APEX", L"XBOX", L"DUALSENSE", L"DUALSHOCK", L"JOY-CON", L"FLYDIGI"}))
        return DeviceKind::Controller;
    if (hasAny({L"MOUSE", L"BASILISK", L"VIPER", L"DEATHADDER", L"NAGA", L"COBRA", L"SUPERLIGHT", L"BEAST", L"PULSAR", L"LAMZU"}))
        return DeviceKind::Mouse;
    if (hasAny({L"HEADSET", L"HEADPHONE", L"EARBUD", L"AIRPODS", L"BUDS", L"KRAKEN", L"BLACKSHARK", L"ARCTIS", L"AUDIO", L"WH-", L"WF-"}))
        return DeviceKind::Headset;
    if (hasAny({L"KEYBOARD", L"HUNTSMAN", L"BLACKWIDOW", L"KEYCHRON", L"WOOTING"}))
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
    if (SetupDiGetDeviceRegistryPropertyW(devs, &info, prop, nullptr, reinterpret_cast<PBYTE>(buf), sizeof(buf), nullptr)) {
        return buf;
    }
    return L"";
}

inline bool HasStartedChild(DEVINST inst) {
    DEVINST child = 0;
    if (CM_Get_Child(&child, inst, 0) != CR_SUCCESS) return false;
    while (child != 0) {
        ULONG st = 0, prob = 0;
        if (CM_Get_DevNode_Status(&st, &prob, child, 0) == CR_SUCCESS && (st & DN_STARTED)) {
            return true;
        }
        DEVINST next = 0;
        if (CM_Get_Sibling(&next, child, 0) != CR_SUCCESS) break;
        child = next;
    }
    return false;
}

inline void QueryBluetoothDevices(std::vector<DeviceInfo>& out) {
    std::set<std::wstring> connectedMacs, disconnectedMacs;
    BLUETOOTH_DEVICE_SEARCH_PARAMS sp{sizeof(sp)};
    sp.fReturnAuthenticated = TRUE;
    sp.fReturnRemembered = TRUE;
    sp.fReturnConnected = TRUE;
    sp.fIssueInquiry = FALSE;

    BLUETOOTH_DEVICE_INFO bdi{sizeof(bdi)};
    if (HBLUETOOTH_DEVICE_FIND hFind = BluetoothFindFirstDevice(&sp, &bdi)) {
        do {
            wchar_t mac[16]{};
            const BYTE* a = bdi.Address.rgBytes;
            swprintf_s(mac, L"%02X%02X%02X%02X%02X%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
            if (bdi.fConnected) connectedMacs.insert(mac);
            else disconnectedMacs.insert(mac);
        } while (BluetoothFindNextDevice(hFind, &bdi));
        BluetoothFindDeviceClose(hFind);
    }

    HDEVINFO devs = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devs == INVALID_HANDLE_VALUE) return;

    SP_DEVINFO_DATA info{sizeof(info)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &info); ++i) {
        DEVPROPTYPE propType = 0;
        BYTE pct = 0;
        if (!SetupDiGetDevicePropertyW(devs, &info, &kPkeyBluetoothBattery, &propType, &pct, sizeof(pct), nullptr, 0))
            continue;
        if (propType != DEVPROP_TYPE_BYTE || pct > 100) continue;

        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, info.DevInst, 0) != CR_SUCCESS || !(status & DN_STARTED))
            continue;

        DEVPROP_BOOLEAN isConn = DEVPROP_FALSE;
        bool hasIsConn = SetupDiGetDevicePropertyW(devs, &info, &kPkeyIsConnected, &propType,
                                                   reinterpret_cast<PBYTE>(&isConn), sizeof(isConn), nullptr, 0)
                         && propType == DEVPROP_TYPE_BOOLEAN;
        if (hasIsConn && isConn == DEVPROP_FALSE) continue;

        wchar_t instBuf[512]{};
        SetupDiGetDeviceInstanceIdW(devs, &info, instBuf, _countof(instBuf), nullptr);
        std::wstring instUpper = ToUpper(instBuf);

        if (!hasIsConn || isConn != DEVPROP_TRUE) {
            if (instUpper.rfind(L"BTHLE\\DEV_", 0) == 0 && !HasStartedChild(info.DevInst)) continue;
            bool knownDisconnected = false;
            for (const auto& mac : disconnectedMacs) {
                if (instUpper.find(mac) != std::wstring::npos && connectedMacs.find(mac) == connectedMacs.end()) {
                    knownDisconnected = true;
                    break;
                }
            }
            if (knownDisconnected) continue;
        }

        std::wstring name = GetRegistryProp(devs, info, SPDRP_FRIENDLYNAME);
        if (name.empty()) name = GetRegistryProp(devs, info, SPDRP_DEVICEDESC);
        DeviceKind kind = GuessDeviceKind(name);
        AddUniqueDevice(out, std::move(name), pct, false, kind);
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
        if (pGetState(i, &st) != ERROR_SUCCESS) continue;
        XINPUT_BATTERY_INFORMATION bat{};
        if (pGetBatt(i, BATTERY_DEVTYPE_GAMEPAD, &bat) != ERROR_SUCCESS) continue;
        if (bat.BatteryType == BATTERY_TYPE_DISCONNECTED || bat.BatteryType == BATTERY_TYPE_WIRED) continue;

        constexpr int kLevels[] = {10, 30, 65, 100};
        int pct = (bat.BatteryLevel <= 3) ? kLevels[bat.BatteryLevel] : 50;
        AddUniqueDevice(out, L"Controller " + std::to_wstring(i + 1), pct, false, DeviceKind::Controller);
    }
}

inline bool IsBatteryUsage(USAGE page, USAGE usage) {
    return (page == 0x06 && usage == 0x20) || ((page == 0x84 || page == 0x85) && (usage == 0x65 || usage == 0x66));
}

inline int ReadHidBatteryReport(HANDLE h, PHIDP_PREPARSED_DATA ppd, const HIDP_CAPS& caps, HIDP_REPORT_TYPE rtype) {
    USHORT capCount = (rtype == HidP_Feature) ? caps.NumberFeatureValueCaps : caps.NumberInputValueCaps;
    USHORT reportLen = (rtype == HidP_Feature) ? caps.FeatureReportByteLength : caps.InputReportByteLength;
    if (capCount == 0 || reportLen == 0) return -1;

    std::vector<HIDP_VALUE_CAPS> vcaps(capCount);
    if (HidP_GetValueCaps(rtype, vcaps.data(), &capCount, ppd) != HIDP_STATUS_SUCCESS) return -1;

    for (USHORT i = 0; i < capCount; ++i) {
        const auto& vc = vcaps[i];
        USAGE usage = vc.IsRange ? vc.Range.UsageMin : vc.NotRange.Usage;
        if (!IsBatteryUsage(vc.UsagePage, usage)) continue;

        std::vector<BYTE> report(reportLen, 0);
        report[0] = vc.ReportID;
        BOOLEAN ok = (rtype == HidP_Feature)
            ? HidD_GetFeature(h, report.data(), reportLen)
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
    return -1;
}

inline bool QueryRazerBattery(HANDLE h, int& outPct, bool& outCharging) {
    auto sendCmd = [h](BYTE txId, BYTE cmdId, BYTE (&resp)[91]) {
        BYTE buf[91]{};
        buf[2] = txId;
        buf[6] = 0x02;
        buf[7] = 0x07;
        buf[8] = cmdId;
        BYTE crc = 0;
        for (int i = 3; i < 89; ++i) crc ^= buf[i];
        buf[89] = crc;

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

inline bool QueryFlydigiBattery(const wchar_t* devicePath, int& outPct, bool& outCharging) {
    HANDLE hrw = CreateFileW(devicePath, GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OVERLAPPED, nullptr);
    if (hrw == INVALID_HANDLE_VALUE) return false;

    OVERLAPPED ovR{}, ovW{};
    ovR.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ovW.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool ok = false;

    if (ovR.hEvent && ovW.hEvent) {
        BYTE inBuf[33]{};
        DWORD rd = 0;
        BOOL readStarted = ReadFile(hrw, inBuf, sizeof(inBuf), &rd, &ovR);
        if (readStarted || GetLastError() == ERROR_IO_PENDING) {
            BYTE outBuf[33]{0x00, 0x5A, 0xA5, 0x01, 0x02, 0x03};
            DWORD wr = 0;
            if (!WriteFile(hrw, outBuf, sizeof(outBuf), &wr, &ovW) && GetLastError() == ERROR_IO_PENDING) {
                WaitForSingleObject(ovW.hEvent, 100);
            }
            if (!readStarted && WaitForSingleObject(ovR.hEvent, 150) == WAIT_OBJECT_0) {
                readStarted = GetOverlappedResult(hrw, &ovR, &rd, FALSE);
            }
            if (readStarted && rd >= 13 && inBuf[1] == 0x5A && inBuf[2] == 0xA5 &&
                inBuf[3] == 0x01 && inBuf[6] != 0x00 && inBuf[12] != 0x00) {
                BYTE b = inBuf[12];
                outCharging = (b >> 4) >= 1;
                int raw = b & 0x0F;
                if (outCharging && raw == 0) {
                    outPct = ((b >> 4) >= 2) ? 100 : 50;
                } else {
                    outPct = (inBuf[6] == 0x82 || raw > 5) ? raw * 10 : raw * 20;
                }
                ok = true;
            }
        }
        CancelIo(hrw);
    }

    if (ovR.hEvent) CloseHandle(ovR.hEvent);
    if (ovW.hEvent) CloseHandle(ovW.hEvent);
    CloseHandle(hrw);
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

        HIDD_ATTRIBUTES attr{sizeof(attr)};
        BOOL hasAttr = HidD_GetAttributes(h, &attr);
        PHIDP_PREPARSED_DATA ppd = nullptr;
        if (HidD_GetPreparsedData(h, &ppd)) {
            HIDP_CAPS caps{};
            if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
                int pct = -1;
                bool charging = false;
                DeviceKind fallbackKind = DeviceKind::Generic;
                bool forceKind = false;
                if (hasAttr && attr.VendorID == 0x1532 && caps.FeatureReportByteLength == 91) {
                    if (!QueryRazerBattery(h, pct, charging)) pct = -1;
                    fallbackKind = DeviceKind::Mouse;
                } else if (hasAttr && attr.VendorID == 0x37D7 && caps.UsagePage == 0xFFA0 && caps.InputReportByteLength == 33) {
                    if (!QueryFlydigiBattery(detail->DevicePath, pct, charging)) pct = -1;
                    fallbackKind = DeviceKind::Controller;
                    forceKind = true;
                } else {
                    pct = ReadHidBatteryReport(h, ppd, caps, HidP_Feature);
                    if (pct < 0) pct = ReadHidBatteryReport(h, ppd, caps, HidP_Input);
                    if (caps.UsagePage == 0x01) {
                        if (caps.Usage == 0x02) fallbackKind = DeviceKind::Mouse;
                        else if (caps.Usage == 0x04 || caps.Usage == 0x05) fallbackKind = DeviceKind::Controller;
                        else if (caps.Usage == 0x06) fallbackKind = DeviceKind::Keyboard;
                    }
                }
                if (pct >= 0) {
                    wchar_t prod[128]{};
                    std::wstring name = HidD_GetProductString(h, prod, sizeof(prod)) ? prod : L"";
                    if (name.empty()) name = GetRegistryProp(devs, devInfo, SPDRP_DEVICEDESC);
                    DeviceKind kind = forceKind ? fallbackKind : GuessDeviceKind(name, fallbackKind);
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
    if (!GetSystemPowerStatus(&sps)) return;
    if (sps.BatteryFlag == 128 || sps.BatteryFlag == 255 || sps.BatteryLifePercent > 100) return;
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

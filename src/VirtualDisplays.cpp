#include "VirtualDisplays.h"

#include <windows.h>
#include <setupapi.h>
// initguid.h must come after the other Windows headers so it only defines the device class GUIDs.
#include <initguid.h>
#include <devguid.h>

#include <vector>

#include "Log.h"
#include "Util.h"

namespace {
const wchar_t kDefaultPath[] = L"C:\\VirtualDisplayDriver\\vdd_settings.xml";
const wchar_t kHardwareId[] = L"Root\\MttVDD";

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadAll(const std::wstring& path, std::string* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < 1024 * 1024;
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = size.QuadPart == 0 ||
             (ReadFile(h, out->data(), static_cast<DWORD>(size.QuadPart), &read, nullptr) && read == size.QuadPart);
    }
    CloseHandle(h);
    return ok;
}

bool WriteAllAtomic(const std::wstring& path, const std::string& text) {
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

// Restarts every present Virtual Display Driver device (DIF_PROPERTYCHANGE / DICS_PROPCHANGE is
// what Device Manager does for "disable + enable"). The driver re-reads vdd_settings.xml on start.
bool RestartDriver(std::string* error) {
    HDEVINFO devs = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) {
        *error = Format("cannot list display devices (error %lu)", GetLastError());
        return false;
    }
    int found = 0, restarted = 0;
    SP_DEVINFO_DATA dev{sizeof(dev)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &dev); ++i) {
        wchar_t ids[1024] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(devs, &dev, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(ids),
                                               sizeof(ids) - sizeof(wchar_t), nullptr))
            continue;
        bool match = false;
        for (const wchar_t* p = ids; *p; p += wcslen(p) + 1)
            if (_wcsicmp(p, kHardwareId) == 0) match = true;
        if (!match) continue;
        ++found;
        SP_PROPCHANGE_PARAMS pc{};
        pc.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        pc.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        pc.StateChange = DICS_PROPCHANGE;
        pc.Scope = DICS_FLAG_GLOBAL;
        if (SetupDiSetClassInstallParamsW(devs, &dev, &pc.ClassInstallHeader, sizeof(pc)) &&
            SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devs, &dev)) {
            ++restarted;
        } else {
            *error = Format("restarting the Virtual Display Driver failed (error 0x%08lX)", GetLastError());
        }
    }
    SetupDiDestroyDeviceInfoList(devs);
    if (!found) {
        *error = "the Virtual Display Driver device was not found (is it installed and enabled?)";
        return false;
    }
    return restarted == found;
}
}  // namespace

namespace Vdd {

std::wstring SettingsPath() {
    // Newer driver versions record their folder in the registry; older ones use C:\VirtualDisplayDriver.
    wchar_t dir[MAX_PATH] = {};
    DWORD size = sizeof(dir);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver", L"VDDPATH", RRF_RT_REG_SZ,
                     nullptr, dir, &size) == ERROR_SUCCESS && dir[0]) {
        std::wstring p = dir;
        if (p.back() != L'\\') p += L'\\';
        p += L"vdd_settings.xml";
        if (FileExists(p)) return p;
    }
    return FileExists(kDefaultPath) ? kDefaultPath : std::wstring();
}

bool ReadMonitorCount(int* count, std::string* error) {
    const std::wstring path = SettingsPath();
    if (path.empty()) {
        if (error) *error = "Virtual Display Driver not installed (vdd_settings.xml not found)";
        return false;
    }
    std::string xml;
    if (!ReadAll(path, &xml)) {
        if (error) *error = "cannot read " + WideToUtf8(path);
        return false;
    }
    if (!ParseMonitorCount(xml, count)) {
        if (error) *error = "no <monitors><count> in " + WideToUtf8(path);
        return false;
    }
    return true;
}

bool SetMonitorCount(int count, std::string* error) {
    std::string dummy;
    std::string& err = error ? *error : dummy;
    if (count < 1 || count > kMaxMonitors) {
        err = Format("the number of virtual monitors must be 1-%d", kMaxMonitors);
        return false;
    }
    const std::wstring path = SettingsPath();
    if (path.empty()) {
        err = "Virtual Display Driver not installed (vdd_settings.xml not found)";
        return false;
    }
    std::string xml, updated;
    if (!ReadAll(path, &xml)) {
        err = "cannot read " + WideToUtf8(path);
        return false;
    }
    int old = 0;
    ParseMonitorCount(xml, &old);
    if (!ReplaceMonitorCount(xml, count, &updated)) {
        err = "no <monitors><count> in " + WideToUtf8(path);
        return false;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t suffix[64];
    swprintf_s(suffix, L".bak-%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
               st.wSecond);
    const std::wstring backup = path + suffix;
    if (!CopyFileW(path.c_str(), backup.c_str(), FALSE)) {
        err = Format("cannot back up vdd_settings.xml (error %lu); nothing changed", GetLastError());
        return false;
    }
    if (!WriteAllAtomic(path, updated)) {
        err = Format("cannot write %s (error %lu; run as administrator)", WideToUtf8(path).c_str(), GetLastError());
        return false;
    }
    Log::Info("Virtual monitors: %d -> %d in %s (backup %s)", old, count, WideToUtf8(path).c_str(),
              WideToUtf8(backup).c_str());

    if (!RestartDriver(&err)) {
        err += "; the new count applies after the next restart of the PC";
        return false;
    }
    Log::Info("Virtual Display Driver restarted");
    return true;
}

}  // namespace Vdd

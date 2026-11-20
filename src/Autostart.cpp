#include "Autostart.h"

#include <windows.h>

#include <string>

#include "Log.h"
#include "Util.h"

static const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t kValueName[] = L"NDI2SDI";

static std::wstring RunCommand() { return L"\"" + ExePath() + L"\" --tray"; }

bool IsRunAtStartupEnabled() {
    wchar_t buf[2048];
    DWORD size = sizeof(buf);
    LSTATUS st = RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValueName, RRF_RT_REG_SZ, nullptr, buf, &size);
    return st == ERROR_SUCCESS;
}

bool SetRunAtStartup(bool enable) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        Log::Error("Cannot open HKCU Run key");
        return false;
    }
    LSTATUS st;
    if (enable) {
        std::wstring cmd = RunCommand();
        st = RegSetValueExW(key, kValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        st = RegDeleteValueW(key, kValueName);
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (st != ERROR_SUCCESS) {
        Log::Error("Updating Run-at-startup failed (error %ld)", static_cast<long>(st));
        return false;
    }
    Log::Info("Run at Windows startup %s", enable ? "enabled" : "disabled");
    return true;
}

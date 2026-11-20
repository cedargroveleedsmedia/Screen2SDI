#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>

#include "Autostart.h"
#include "Config.h"
#include "DeckLink.h"
#include "Displays.h"
#include "Log.h"
#include "NdiLib.h"
#include "TrayApp.h"
#include "Util.h"
#include "VirtualDisplays.h"

namespace {
uint64_t g_startMs = 0;

// One-time upgrade from the app's old name (NDI2SDI, up to 1.1): copy its settings and move its
// Windows startup entry to this exe. The old settings folder is left in place as a backup.
void MigrateFromNdi2Sdi() {
    const std::wstring newCfg = ConfigPath();
    const std::wstring newDir = AppDataDir();
    const std::wstring oldCfg = newDir.substr(0, newDir.find_last_of(L'\\')) + L"\\NDI2SDI\\config.json";
    if (GetFileAttributesW(newCfg.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(oldCfg.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (CopyFileW(oldCfg.c_str(), newCfg.c_str(), TRUE))
            Log::Info("Upgrade: copied settings from %s (old folder kept as a backup)", WideToUtf8(oldCfg).c_str());
        else
            Log::Error("Upgrade: could not copy settings from %s (error %lu)", WideToUtf8(oldCfg).c_str(),
                       GetLastError());
    }

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        const bool hadOld = RegQueryValueExW(key, L"NDI2SDI", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
        if (hadOld) RegDeleteValueW(key, L"NDI2SDI");
        RegCloseKey(key);
        if (hadOld) {
            Log::Info("Upgrade: replaced the old NDI2SDI startup entry");
            SetRunAtStartup(true);
        }
    }

    if (HANDLE old = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\NDI2SDI.SingleInstance")) {
        CloseHandle(old);
        Log::Warn("Upgrade: the old NDI2SDI app is still running and may be using the DeckLink outputs; "
                  "exit it from its tray icon");
    }
}

// Unattended operation: if we ever crash after running for a while, log it and relaunch ourselves
// (in the tray) so the SDI output comes back without anyone touching the PC.
LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    void* addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    Log::Error("CRASH: exception 0x%08lX at %p", code, addr);
    if (NowMs() - g_startMs > 30000) {  // avoid a restart loop if we crash during startup
        std::wstring cmd = L"\"" + ExePath() + L"\" --tray --wait-pid=" + std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            Log::Error("Restarting after crash");
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    Log::Shutdown();
    return EXCEPTION_EXECUTE_HANDLER;
}

// --diag: write DeckLink devices/formats and visible NDI sources to the log, then exit.
// Never enables output, so it is safe to run while the card is in use.
int RunDiagnostics() {
    Log::Info("Diagnostics: DeckLink output devices");
    std::string err;
    auto devices = EnumerateDeckLinkOutputs(&err);
    if (!err.empty()) Log::Warn("  %s", err.c_str());
    for (const auto& d : devices) {
        Log::Info("  '%s' (persistent id %lld), %zu output formats", d.name.c_str(),
                  static_cast<long long>(d.persistentId), d.modes.size());
        for (const auto& m : d.modes)
            Log::Info("      %-14s %c%c%c%c %dx%d %s %lld/%lld", m.name.c_str(), (m.id >> 24) & 0xFF, (m.id >> 16) & 0xFF,
                      (m.id >> 8) & 0xFF, m.id & 0xFF, m.width, m.height, m.interlaced ? "i" : "p",
                      static_cast<long long>(m.timeScale), static_cast<long long>(m.frameDuration));
    }
    Log::Info("Diagnostics: Windows displays (left to right)");
    for (const auto& d : EnumerateDisplays())
        Log::Info("  %s | %s | %s | %s", d.label.c_str(), d.gdiName.c_str(),
                  d.monitorName.empty() ? "(no name)" : d.monitorName.c_str(), d.devicePath.c_str());
    int vmon = 0;
    std::string vErr;
    if (Vdd::ReadMonitorCount(&vmon, &vErr))
        Log::Info("Diagnostics: Virtual Display Driver set to %d monitor(s) in %s", vmon,
                  WideToUtf8(Vdd::SettingsPath()).c_str());
    else
        Log::Info("Diagnostics: %s", vErr.c_str());
    Log::Info("Diagnostics: NDI sources (after 5 s discovery)");
    if (!Ndi::Load(&err)) {
        Log::Error("  %s", err.c_str());
    } else {
        Sleep(5000);
        auto names = Ndi::GetSourceNames();
        if (names.empty()) Log::Info("  (none found)");
        for (const auto& n : names) Log::Info("  '%s'", n.c_str());
        Ndi::Unload();
    }
    return 0;
}
}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE inst, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int) {
    g_startMs = NowMs();

    bool trayOnly = false;
    bool diag = false;
    DWORD waitPid = 0;
    int setVirtualMonitors = 0;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            std::wstring a = argv[i];
            if (a == L"--tray") trayOnly = true;
            else if (a == L"--diag") diag = true;
            else if (a.rfind(L"--wait-pid=", 0) == 0) waitPid = static_cast<DWORD>(_wtoi(a.c_str() + 11));
            else if (a.rfind(L"--set-virtual-monitors=", 0) == 0) setVirtualMonitors = _wtoi(a.c_str() + 23);
        }
        LocalFree(argv);
    }

    // Elevated helper started by Settings (or by hand, as administrator): change the number of
    // Virtual Display Driver monitors and exit. Never touches the DeckLink outputs.
    if (setVirtualMonitors != 0) {
        Log::Init(LogDir());
        Log::Info("==== Screen2SDI 1.3.0 set virtual monitors to %d ====", setVirtualMonitors);
        std::string err;
        const bool ok = Vdd::SetMonitorCount(setVirtualMonitors, &err);
        if (!ok) Log::Error("Virtual monitors: %s", err.c_str());
        Log::Shutdown();
        return ok ? 0 : 2;
    }

    if (diag) {
        Log::Init(LogDir());
        Log::Info("==== Screen2SDI 1.3.0 diagnostics ====");
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        int rc = RunDiagnostics();
        if (SUCCEEDED(hr)) CoUninitialize();
        Log::Shutdown();
        return rc;
    }

    // After a crash-restart, wait for the old process to be fully gone before claiming the mutex.
    if (waitPid) {
        if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, waitPid)) {
            WaitForSingleObject(old, 15000);
            CloseHandle(old);
        }
    }

    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\Screen2SDI.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        // Another copy is running: ask it to show Settings (unless this is just the startup entry).
        if (!trayOnly) {
            if (HWND w = FindWindowW(TrayApp::kWindowClass, nullptr)) {
                DWORD pid = 0;
                GetWindowThreadProcessId(w, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(w, TrayApp::ShowSettingsMessage(), 0, 0);
            }
        }
        CloseHandle(mutex);
        return 0;
    }

    Log::Init(LogDir());
    SetUnhandledExceptionFilter(CrashHandler);
    Log::Info("==== Screen2SDI 1.3.0 starting (pid %lu, %s) ====", GetCurrentProcessId(), trayOnly ? "--tray" : "interactive");
    Log::Info("Executable: %s", WideToUtf8(ExePath()).c_str());
#if !SCREEN2SDI_HAS_NDI
    Log::Info("Built without NDI: display capture only");
#endif
    MigrateFromNdi2Sdi();

    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    int rc;
    {
        TrayApp app;
        rc = app.Run(inst, trayOnly);
    }

    if (SUCCEEDED(coHr)) CoUninitialize();
    Log::Info("==== Screen2SDI exited (code %d) ====", rc);
    Log::Shutdown();
    if (mutex) CloseHandle(mutex);
    return rc;
}

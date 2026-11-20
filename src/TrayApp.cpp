#include "TrayApp.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>

#include "../res/resource.h"
#include "Autostart.h"
#include "Log.h"
#include "NdiLib.h"
#include "Util.h"

namespace {
constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_STATUS = WM_APP + 2;
constexpr UINT_PTR kTrayRetryTimer = 1;

enum : UINT {
    IDM_SETTINGS = 40001,
    IDM_TOGGLE_OUTPUT,
    IDM_OPEN_LOGS,
    IDM_RUN_AT_STARTUP,
    IDM_EXIT,
};
}  // namespace

TrayApp::TrayApp() : m_settings(*this) {
    for (size_t i = 0; i < m_pipelines.size(); ++i) m_pipelines[i] = std::make_unique<Pipeline>(static_cast<int>(i));
}

void TrayApp::ApplyAll() {
    for (size_t i = 0; i < m_pipelines.size(); ++i)
        m_pipelines[i]->ApplyConfig(m_cfg.outputs[i], m_cfg.prerollFrames, m_configured);
}

void TrayApp::SetRunEnabled(bool run) {
    m_run = run;
    for (auto& p : m_pipelines) p->SetRunEnabled(run);
}

UINT TrayApp::ShowSettingsMessage() {
    static UINT msg = RegisterWindowMessageW(L"NDI2SDI.ShowSettings");
    return msg;
}

int TrayApp::Run(HINSTANCE inst, bool trayOnly) {
    m_inst = inst;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) {
        Log::Error("RegisterClassEx failed (%lu)", GetLastError());
        return 1;
    }
    // A hidden top-level window (not message-only) so we receive TaskbarCreated and session-end broadcasts.
    m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"NDI2SDI", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst,
                             this);
    if (!m_hwnd) {
        Log::Error("CreateWindow failed (%lu)", GetLastError());
        return 1;
    }
    m_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(m_hwnd, m_taskbarCreatedMsg, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(m_hwnd, ShowSettingsMessage(), MSGFLT_ALLOW, nullptr);

    const int iconIds[3] = {IDI_GREEN, IDI_YELLOW, IDI_RED};
    for (int i = 0; i < 3; ++i) {
        if (FAILED(LoadIconMetric(inst, MAKEINTRESOURCEW(iconIds[i]), LIM_SMALL, &m_icons[i])))
            m_icons[i] = LoadIconW(inst, MAKEINTRESOURCEW(iconIds[i]));
    }

    m_configured = LoadConfig(&m_cfg);
    if (m_configured) {
        Log::Info("Config loaded: buffer=%d autoStart=%d", m_cfg.prerollFrames, m_cfg.autoStartOutput ? 1 : 0);
        for (size_t i = 0; i < m_cfg.outputs.size(); ++i) {
            const OutputConfig& o = m_cfg.outputs[i];
            Log::Info("  Output %zu '%s': %s, %s -> '%s' (id %lld) @ %s", i + 1, o.name.c_str(),
                      o.enabled ? "on" : "off", o.SourceDescription().c_str(), o.deviceName.c_str(),
                      static_cast<long long>(o.devicePersistentId), o.displayModeName.c_str());
        }
    } else {
        Log::Info("No config found at %s", WideToUtf8(ConfigPath()).c_str());
    }

    ApplyAll();
    SetRunEnabled(m_cfg.autoStartOutput);
    for (auto& p : m_pipelines) p->Start(m_hwnd, WM_APP_STATUS);

    if (!AddTrayIcon()) {
        // Explorer may not be ready yet right after logon; keep trying.
        SetTimer(m_hwnd, kTrayRetryTimer, 2000, nullptr);
    }

    if (!m_configured && !trayOnly) m_settings.Show();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        HWND dlg = m_settings.hwnd();
        if (dlg && IsDialogMessageW(dlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ShutdownAll();
    return 0;
}

bool TrayApp::AddTrayIcon() {
    m_nid = {};
    m_nid.cbSize = sizeof(m_nid);
    m_nid.hWnd = m_hwnd;
    m_nid.uID = 1;
    m_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    m_nid.uCallbackMessage = WM_APP_TRAY;
    m_nid.hIcon = m_icons[2];
    wcscpy_s(m_nid.szTip, L"NDI2SDI - starting");
    Shell_NotifyIconW(NIM_DELETE, &m_nid);  // in case a stale icon is left from Explorer restart
    if (!Shell_NotifyIconW(NIM_ADD, &m_nid)) return false;
    m_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &m_nid);
    m_iconAdded = true;
    UpdateTrayIcon();
    return true;
}

void TrayApp::UpdateTrayIcon() {
    // Colour = worst enabled output. Tooltip = one short line per output.
    int idx = -1;
    std::string tipText;
    for (size_t i = 0; i < m_pipelines.size(); ++i) {
        PipelineStatus st = m_pipelines[i]->GetStatus();
        if (st.state == PipelineState::Disabled) continue;
        int c = st.state == PipelineState::Live ? 0 : st.state == PipelineState::Waiting ? 1 : 2;
        idx = std::max(idx, c);
        const char* word = st.state == PipelineState::Live      ? "Live"
                           : st.state == PipelineState::Waiting ? "Black (waiting)"
                           : st.state == PipelineState::Stopped ? "Stopped"
                           : st.state == PipelineState::NotConfigured ? "Not configured"
                                                                      : "Error";
        if (!tipText.empty()) tipText += "\n";
        tipText += m_cfg.outputs[i].name + ": " + word;
    }
    if (idx < 0) {
        idx = 2;
        tipText = m_configured ? "NDI2SDI - all outputs are off" : "NDI2SDI - not configured";
    }
    if (m_iconAdded) {
        m_nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        m_nid.hIcon = m_icons[idx];
        std::wstring tip = Utf8ToWide(tipText);
        if (tip.size() >= ARRAYSIZE(m_nid.szTip)) tip = tip.substr(0, ARRAYSIZE(m_nid.szTip) - 4) + L"...";
        wcsncpy_s(m_nid.szTip, tip.c_str(), _TRUNCATE);
        if (!Shell_NotifyIconW(NIM_MODIFY, &m_nid)) {
            m_iconAdded = false;
            SetTimer(m_hwnd, kTrayRetryTimer, 2000, nullptr);
        }
    }
    m_settings.OnStatusChanged();
}

void TrayApp::ShowMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    const bool running = m_run;
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"&Settings...");
    AppendMenuW(menu, MF_STRING, IDM_TOGGLE_OUTPUT, running ? L"S&top Output" : L"S&tart Output");
    AppendMenuW(menu, MF_STRING, IDM_OPEN_LOGS, L"Open &Log Folder");
    AppendMenuW(menu, MF_STRING | (IsRunAtStartupEnabled() ? MF_CHECKED : 0), IDM_RUN_AT_STARTUP,
                L"&Run at Windows Startup");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"E&xit");
    SetMenuDefaultItem(menu, IDM_SETTINGS, FALSE);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(m_hwnd);  // required so the menu closes when clicking elsewhere
    UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, m_hwnd, nullptr);
    PostMessageW(m_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (cmd) {
        case IDM_SETTINGS: m_settings.Show(); break;
        case IDM_TOGGLE_OUTPUT:
            Log::Info("User %s output", running ? "stopped" : "started");
            SetRunEnabled(!running);
            break;
        case IDM_OPEN_LOGS: ShellExecuteW(nullptr, L"open", LogDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case IDM_RUN_AT_STARTUP: SetRunAtStartup(!IsRunAtStartupEnabled()); break;
        case IDM_EXIT:
            Log::Info("Exit requested from tray menu");
            m_settings.Close();
            ShutdownAll();
            DestroyWindow(m_hwnd);
            break;
        default: break;
    }
}

void TrayApp::OnSettingsSaved(const Config& cfg, bool runAtStartup) {
    SaveConfig(cfg);  // logs on failure; the new settings still apply for this session
    if (runAtStartup != IsRunAtStartupEnabled()) SetRunAtStartup(runAtStartup);
    m_cfg = cfg;
    m_configured = true;
    ApplyAll();
    SetRunEnabled(true);  // saving means "use these settings now"
}

void TrayApp::ShutdownAll() {
    if (m_shutdownDone) return;
    m_shutdownDone = true;
    KillTimer(m_hwnd, kTrayRetryTimer);
    for (auto& p : m_pipelines) p->Shutdown();  // stops playback, releases DeckLink and NDI objects
    Ndi::Unload();
    if (m_iconAdded) {
        Shell_NotifyIconW(NIM_DELETE, &m_nid);
        m_iconAdded = false;
    }
    Log::Info("Shutdown complete");
}

LRESULT CALLBACK TrayApp::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    TrayApp* self;
    if (msg == WM_NCCREATE) {
        self = static_cast<TrayApp*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TrayApp::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == m_taskbarCreatedMsg && m_taskbarCreatedMsg != 0) {
        Log::Info("Explorer restarted; re-adding tray icon");
        m_iconAdded = false;
        if (!AddTrayIcon()) SetTimer(m_hwnd, kTrayRetryTimer, 2000, nullptr);
        return 0;
    }
    if (msg == ShowSettingsMessage()) {
        m_settings.Show();
        return 0;
    }

    switch (msg) {
        case WM_APP_TRAY:
            switch (LOWORD(lp)) {
                case WM_CONTEXTMENU: ShowMenu(); break;
                case WM_LBUTTONDBLCLK:
                case NIN_KEYSELECT: m_settings.Show(); break;
                default: break;
            }
            return 0;
        case WM_APP_STATUS:
            UpdateTrayIcon();
            return 0;
        case WM_TIMER:
            if (wp == kTrayRetryTimer && (m_iconAdded || AddTrayIcon())) KillTimer(m_hwnd, kTrayRetryTimer);
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wp) {
                Log::Info("Windows session ending; shutting down");
                m_settings.Close();
                ShutdownAll();
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}

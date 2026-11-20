#pragma once
#include <windows.h>
#include <shellapi.h>

#include <array>
#include <memory>

#include "Config.h"
#include "Pipeline.h"
#include "SettingsDialog.h"

class TrayApp {
public:
    static constexpr const wchar_t* kWindowClass = L"NDI2SDI.TrayWindow";
    // Registered message a second instance posts to open Settings in the running one.
    static UINT ShowSettingsMessage();

    TrayApp();
    int Run(HINSTANCE inst, bool trayOnly);

    HINSTANCE instance() const { return m_inst; }
    PipelineStatus OutputStatus(int index) { return m_pipelines[static_cast<size_t>(index)]->GetStatus(); }
    const Config& config() const { return m_cfg; }
    bool configured() const { return m_configured; }
    void OnSettingsSaved(const Config& cfg, bool runAtStartup);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    bool AddTrayIcon();
    void UpdateTrayIcon();
    void ShowMenu();
    void ShutdownAll();
    void ApplyAll();
    void SetRunEnabled(bool run);

    HINSTANCE m_inst = nullptr;
    HWND m_hwnd = nullptr;
    NOTIFYICONDATAW m_nid{};
    bool m_iconAdded = false;
    HICON m_icons[3] = {};  // green, yellow, red
    UINT m_taskbarCreatedMsg = 0;
    std::array<std::unique_ptr<Pipeline>, kMaxOutputs> m_pipelines;
    bool m_run = false;
    Config m_cfg;
    bool m_configured = false;
    bool m_shutdownDone = false;
    SettingsDialog m_settings;
};

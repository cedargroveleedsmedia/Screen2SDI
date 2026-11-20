#pragma once
#include <windows.h>
#include <shellapi.h>

#include <memory>
#include <vector>

#include "Config.h"
#include "Pipeline.h"
#include "SettingsDialog.h"

class TrayApp {
public:
    static constexpr const wchar_t* kWindowClass = L"Screen2SDI.TrayWindow";
    // Registered message a second instance posts to open Settings in the running one.
    static UINT ShowSettingsMessage();

    TrayApp();
    int Run(HINSTANCE inst, bool trayOnly);

    HINSTANCE instance() const { return m_inst; }
    int OutputCount() const { return static_cast<int>(m_pipelines.size()); }
    PipelineStatus OutputStatus(int index) {
        if (index < 0 || index >= OutputCount()) return {};
        return m_pipelines[static_cast<size_t>(index)]->GetStatus();
    }
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
    std::vector<std::unique_ptr<Pipeline>> m_pipelines;  // one per configured output, same order
    bool m_started = false;  // pipelines' threads are running (new ones start immediately)
    bool m_run = false;
    Config m_cfg;
    bool m_configured = false;
    bool m_shutdownDone = false;
    SettingsDialog m_settings;
};

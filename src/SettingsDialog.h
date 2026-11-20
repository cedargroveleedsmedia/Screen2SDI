#pragma once
#include <windows.h>

#include <string>
#include <vector>

#include "Config.h"
#include "DeckLink.h"
#include "Displays.h"

class TrayApp;

// Modeless settings window with one row per output. One instance lives for the app's lifetime;
// the HWND is created on demand.
class SettingsDialog {
public:
    explicit SettingsDialog(TrayApp& app) : m_app(app) {}
    void Show();
    void Close();
    HWND hwnd() const { return m_hwnd; }
    void OnStatusChanged();

private:
    static INT_PTR CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    INT_PTR Handle(UINT msg, WPARAM wp, LPARAM lp);
    void OnInit();
    HWND Ctl(int row, int offset) const;
    void RefreshSources();
    void FillNdi(int row, const std::vector<std::string>& names);
    void FillDisplays(int row, const std::string& keepPath, const std::string& keepGdi);
    void FillDevices(int row);
    void FillFormats(int row, uint32_t preferredMode);
    void UpdateRowControls(int row);
    int SelectedDeviceIndex(int row) const;   // index into m_devices, or -1
    int SelectedDisplayIndex(int row) const;  // index into m_displays, or -1
    SourceType SelectedType(int row) const;
    void Save();

    static constexpr LRESULT kMissing = 0x7FFFFFFF;

    TrayApp& m_app;
    HWND m_hwnd = nullptr;
    std::vector<DeckLinkDeviceInfo> m_devices;
    std::vector<DisplayInfo> m_displays;
    // Per row, the (device path, GDI name) behind each display combo item (item data indexes this).
    std::vector<std::pair<std::string, std::string>> m_rowPaths[kMaxOutputs];
    std::wstring m_error;  // shown in the status area instead of the status after a failed Save
};

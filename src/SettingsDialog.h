#pragma once
#include <windows.h>

#include <string>
#include <utility>
#include <vector>

#include "Config.h"
#include "DeckLink.h"
#include "Displays.h"

class TrayApp;

// Modeless settings window. Outputs are rows in a scrolling panel; rows can be added (up to
// kMaxOutputs) and removed. Changes apply on Save. One instance lives for the app's lifetime;
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
    static LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    INT_PTR Handle(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandlePanel(UINT msg, WPARAM wp, LPARAM lp);
    void OnInit();

    // Rows
    HWND Ctl(int row, int offset) const;
    void BuildRows();          // (re)creates all rows from m_work
    void CreateRow(int row);
    void CollectRows();        // reads the rows' controls back into m_work
    OutputConfig NewOutput() const;  // next free SDI output + next free virtual monitor
    void AddOutput();
    void AddAllSdiOutputs();
    void RemoveOutput(int row);
    bool DeviceUsedByOtherRow(const DeckLinkDeviceInfo& d, int row) const;
    bool DisplayUsedByOtherRow(const DisplayInfo& d, int row) const;
    int ContentHeight() const;
    int PageHeight() const;
    void UpdateScroll();
    void ScrollTo(int y);
    void EnsureRowVisible(int row);
    void UpdateCountText();

    void RefreshSources();
    void FillNdi(int row, const std::vector<std::string>& names);
    void FillDisplays(int row, const std::string& keepPath, const std::string& keepGdi);
    void FillDevices(int row);
    void FillFormats(int row, uint32_t preferredMode);
    void UpdateRowControls(int row);
    int SelectedDeviceIndex(int row) const;   // index into m_devices, or -1
    int SelectedDisplayIndex(int row) const;  // index into m_rowPaths[row], or -1
    SourceType SelectedType(int row) const;

    // Virtual monitors
    void ReadVirtualMonitors();
    void ApplyVirtualMonitors();

    void Save();

    static constexpr LRESULT kMissing = 0x7FFFFFFF;

    TrayApp& m_app;
    HWND m_hwnd = nullptr;
    HWND m_panel = nullptr;
    int m_rowHeightPx = 1;
    int m_scrollY = 0;
    std::vector<DeckLinkDeviceInfo> m_devices;
    std::vector<DisplayInfo> m_displays;
    std::vector<OutputConfig> m_work;   // outputs as edited (saved on Save)
    std::vector<int> m_origIndex;       // per row: index of the running output it shows, or -1 if new
    // Per row, the (device path, GDI name) behind each display combo item (item data indexes this).
    std::vector<std::vector<std::pair<std::string, std::string>>> m_rowPaths;
    std::vector<std::vector<HWND>> m_rowWindows;  // every window of each row, for rebuilding
    int m_vmonCount = 0;   // virtual monitors as read from vdd_settings.xml (0 = unavailable)
    std::wstring m_error;  // shown in the status area instead of the summary after a failed action
};

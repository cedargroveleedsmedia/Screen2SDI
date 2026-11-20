#include "SettingsDialog.h"

#include <windowsx.h>

#include <algorithm>

#include "../res/resource.h"
#include "Autostart.h"
#include "Log.h"
#include "NdiLib.h"
#include "TrayApp.h"
#include "Util.h"

namespace {
constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT_PTR kInitialRefreshTimer = 2;

std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(static_cast<size_t>(n));
    return s;
}

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

// Row and offset of a per-output control id, or false for other controls.
bool RowControl(int id, int* row, int* offset) {
    if (id < IDC_OUT_BASE || id >= IDC_OUT_BASE + kMaxOutputs * IDC_OUT_STRIDE) return false;
    *row = (id - IDC_OUT_BASE) / IDC_OUT_STRIDE;
    *offset = (id - IDC_OUT_BASE) % IDC_OUT_STRIDE;
    return true;
}

int CtlId(int row, int offset) { return IDC_OUT_BASE + row * IDC_OUT_STRIDE + offset; }
}  // namespace

void SettingsDialog::Show() {
    if (!m_hwnd) {
        m_hwnd = CreateDialogParamW(m_app.instance(), MAKEINTRESOURCEW(IDD_SETTINGS), nullptr, DlgProc,
                                    reinterpret_cast<LPARAM>(this));
        if (!m_hwnd) {
            Log::Error("Cannot create Settings window (error %lu)", GetLastError());
            return;
        }
    }
    ShowWindow(m_hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(m_hwnd);
}

void SettingsDialog::Close() {
    if (m_hwnd) DestroyWindow(m_hwnd);
}

HWND SettingsDialog::Ctl(int row, int offset) const { return GetDlgItem(m_hwnd, CtlId(row, offset)); }

void SettingsDialog::OnStatusChanged() {
    if (!m_hwnd) return;
    std::wstring text = m_error;
    if (text.empty()) {
        const Config& cfg = m_app.config();
        for (int i = 0; i < kMaxOutputs; ++i) {
            PipelineStatus st = m_app.OutputStatus(i);
            if (!text.empty()) text += L"\n";
            text += Utf8ToWide(cfg.outputs[static_cast<size_t>(i)].name + ": " + st.text);
        }
    }
    if (GetText(GetDlgItem(m_hwnd, IDC_STATUS)) != text) SetDlgItemTextW(m_hwnd, IDC_STATUS, text.c_str());
}

INT_PTR CALLBACK SettingsDialog::DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsDialog* self;
    if (msg == WM_INITDIALOG) {
        self = reinterpret_cast<SettingsDialog*>(lp);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    }
    if (!self) return FALSE;
    INT_PTR r = self->Handle(msg, wp, lp);
    if (msg == WM_NCDESTROY) self->m_hwnd = nullptr;
    return r;
}

INT_PTR SettingsDialog::Handle(UINT msg, WPARAM wp, LPARAM) {
    switch (msg) {
        case WM_INITDIALOG:
            OnInit();
            return TRUE;
        case WM_TIMER:
            if (wp == kStatusTimer) {
                OnStatusChanged();
            } else if (wp == kInitialRefreshTimer) {
                KillTimer(m_hwnd, kInitialRefreshTimer);
                RefreshSources();
            }
            return TRUE;
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            int row, ofs;
            if (RowControl(id, &row, &ofs)) {
                if ((ofs == IDC_OFS_ENABLE && code == BN_CLICKED) || (ofs == IDC_OFS_TYPE && code == CBN_SELCHANGE)) {
                    UpdateRowControls(row);
                } else if (ofs == IDC_OFS_DEVICE && code == CBN_SELCHANGE) {
                    HWND fmt = Ctl(row, IDC_OFS_FORMAT);
                    int sel = ComboBox_GetCurSel(fmt);
                    uint32_t keep = sel >= 0 ? static_cast<uint32_t>(ComboBox_GetItemData(fmt, sel)) : 0;
                    FillFormats(row, keep);
                }
                return TRUE;
            }
            switch (id) {
                case IDOK: Save(); return TRUE;
                case IDCANCEL: Close(); return TRUE;
                case IDC_REFRESH: RefreshSources(); return TRUE;
            }
            break;
        }
        case WM_CLOSE:
            Close();
            return TRUE;
        case WM_DESTROY:
            KillTimer(m_hwnd, kStatusTimer);
            KillTimer(m_hwnd, kInitialRefreshTimer);
            return TRUE;
    }
    return FALSE;
}

void SettingsDialog::OnInit() {
    HICON big = LoadIconW(m_app.instance(), MAKEINTRESOURCEW(IDI_APP));
    HICON small = static_cast<HICON>(LoadImageW(m_app.instance(), MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                                GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
    SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
    m_error.clear();

    const Config& cfg = m_app.config();
    std::string err;
    m_devices = EnumerateDeckLinkOutputs(&err);
    // Hide sub-devices that currently offer no output formats (e.g. a connector set to input).
    m_devices.erase(std::remove_if(m_devices.begin(), m_devices.end(),
                                   [](const DeckLinkDeviceInfo& d) { return d.modes.empty(); }),
                    m_devices.end());
    if (m_devices.empty()) Log::Warn("Settings: no DeckLink outputs found%s%s", err.empty() ? "" : ": ", err.c_str());
    m_displays = EnumerateDisplays();
    std::vector<std::string> ndiNames = Ndi::GetSourceNames();

    for (int r = 0; r < kMaxOutputs; ++r) {
        const OutputConfig& o = cfg.outputs[static_cast<size_t>(r)];
        CheckDlgButton(m_hwnd, CtlId(r, IDC_OFS_ENABLE), o.enabled ? BST_CHECKED : BST_UNCHECKED);
        SendMessageW(Ctl(r, IDC_OFS_NAME), EM_SETLIMITTEXT, 24, 0);
        SetWindowTextW(Ctl(r, IDC_OFS_NAME), Utf8ToWide(o.name).c_str());
        HWND type = Ctl(r, IDC_OFS_TYPE);
        ComboBox_AddString(type, L"Display");
        ComboBox_AddString(type, SCREEN2SDI_HAS_NDI ? L"NDI" : L"NDI (not in this build)");
        ComboBox_SetCurSel(type, o.sourceType == SourceType::Display ? 0 : 1);
        SetWindowTextW(Ctl(r, IDC_OFS_SRC_NDI), Utf8ToWide(o.ndiSource).c_str());
        FillNdi(r, ndiNames);
        FillDisplays(r, o.displayPath, o.displayGdiName);
        FillDevices(r);
        UpdateRowControls(r);
    }

    HWND buffer = GetDlgItem(m_hwnd, IDC_BUFFER);
    static const wchar_t* const kBufferLabels[] = {
        L"2 frames - lowest latency (recommended)",
        L"3 frames",
        L"4 frames - safer on a busy PC",
        L"5 frames - safest",
    };
    for (int i = 0; i < 4; ++i) ComboBox_AddString(buffer, kBufferLabels[i]);
    ComboBox_SetCurSel(buffer, std::clamp(cfg.prerollFrames, 2, 5) - 2);

    CheckDlgButton(m_hwnd, IDC_AUTOSTART, cfg.autoStartOutput ? BST_CHECKED : BST_UNCHECKED);
    // First run: default "Run at Windows startup" on, since the point is zero-touch operation.
    bool runAtStartup = m_app.configured() ? IsRunAtStartupEnabled() : true;
    CheckDlgButton(m_hwnd, IDC_RUNATSTARTUP, runAtStartup ? BST_CHECKED : BST_UNCHECKED);

    OnStatusChanged();
    SetTimer(m_hwnd, kStatusTimer, 1000, nullptr);
    SetTimer(m_hwnd, kInitialRefreshTimer, 2500, nullptr);  // NDI discovery needs a moment after launch
}

void SettingsDialog::UpdateRowControls(int row) {
    const bool on = IsDlgButtonChecked(m_hwnd, CtlId(row, IDC_OFS_ENABLE)) == BST_CHECKED;
    const bool display = SelectedType(row) == SourceType::Display;
    ShowWindow(Ctl(row, IDC_OFS_SRC_NDI), display ? SW_HIDE : SW_SHOW);
    ShowWindow(Ctl(row, IDC_OFS_SRC_DISPLAY), display ? SW_SHOW : SW_HIDE);
    for (int ofs : {IDC_OFS_TYPE, IDC_OFS_SRC_NDI, IDC_OFS_SRC_DISPLAY, IDC_OFS_DEVICE, IDC_OFS_FORMAT})
        EnableWindow(Ctl(row, ofs), on);
    if (on && SelectedDeviceIndex(row) < 0) EnableWindow(Ctl(row, IDC_OFS_FORMAT), FALSE);
}

SourceType SettingsDialog::SelectedType(int row) const {
    return ComboBox_GetCurSel(Ctl(row, IDC_OFS_TYPE)) == 0 ? SourceType::Display : SourceType::Ndi;
}

void SettingsDialog::RefreshSources() {
    std::vector<std::string> names = Ndi::GetSourceNames();
    // Remember each row's current display choice before the list (and its indices) changes.
    std::pair<std::string, std::string> keep[kMaxOutputs];
    for (int r = 0; r < kMaxOutputs; ++r) {
        int idx = SelectedDisplayIndex(r);
        if (idx >= 0) keep[r] = m_rowPaths[r][static_cast<size_t>(idx)];
    }
    m_displays = EnumerateDisplays();
    for (int r = 0; r < kMaxOutputs; ++r) {
        FillNdi(r, names);
        FillDisplays(r, keep[r].first, keep[r].second);
    }
}

void SettingsDialog::FillNdi(int row, const std::vector<std::string>& names) {
    HWND combo = Ctl(row, IDC_OFS_SRC_NDI);
    std::wstring current = GetText(combo);
    ComboBox_ResetContent(combo);
    for (const auto& n : names) ComboBox_AddString(combo, Utf8ToWide(n).c_str());

    // First run convenience: if nothing is chosen yet, pick a Presenter source for output 1.
    if (current.empty() && row == 0 && !m_app.configured()) {
        for (const auto& n : names) {
            if (n.find("(Presenter") != std::string::npos) {
                current = Utf8ToWide(n);
                break;
            }
        }
    }
    int idx = current.empty() ? CB_ERR : ComboBox_FindStringExact(combo, -1, current.c_str());
    if (idx != CB_ERR)
        ComboBox_SetCurSel(combo, idx);
    else
        SetWindowTextW(combo, current.c_str());
}

void SettingsDialog::FillDisplays(int row, const std::string& keepPath, const std::string& keepGdi) {
    HWND combo = Ctl(row, IDC_OFS_SRC_DISPLAY);
    ComboBox_ResetContent(combo);
    auto& paths = m_rowPaths[row];
    paths.clear();
    for (const DisplayInfo& d : m_displays) {
        int idx = ComboBox_AddString(combo, Utf8ToWide(d.label).c_str());
        ComboBox_SetItemData(combo, idx, paths.size());
        paths.emplace_back(d.devicePath, d.gdiName);
    }
    int pick = FindDisplay(m_displays, keepPath, keepGdi);
    if (pick >= 0) {
        ComboBox_SetCurSel(combo, pick);
    } else if (!keepPath.empty() || !keepGdi.empty()) {
        // Configured display is missing right now: keep it so saving doesn't silently change it.
        const OutputConfig& o = m_app.config().outputs[static_cast<size_t>(row)];
        std::string label = (o.displayLabel.empty() ? keepGdi : o.displayLabel) + " (not found)";
        int idx = ComboBox_AddString(combo, Utf8ToWide(label).c_str());
        ComboBox_SetItemData(combo, idx, paths.size());
        paths.emplace_back(keepPath, keepGdi);
        ComboBox_SetCurSel(combo, idx);
    } else {
        // Nothing chosen yet: suggest the n-th virtual display for row n.
        int nth = 0;
        for (size_t i = 0; i < m_displays.size(); ++i) {
            if (m_displays[i].isVirtual && nth++ == row) {
                ComboBox_SetCurSel(combo, static_cast<int>(i));
                break;
            }
        }
    }
}

void SettingsDialog::FillDevices(int row) {
    HWND combo = Ctl(row, IDC_OFS_DEVICE);
    ComboBox_ResetContent(combo);
    const OutputConfig& o = m_app.config().outputs[static_cast<size_t>(row)];
    for (size_t i = 0; i < m_devices.size(); ++i) {
        int idx = ComboBox_AddString(combo, Utf8ToWide(m_devices[i].name).c_str());
        ComboBox_SetItemData(combo, idx, i);
    }
    int pick = -1;
    for (size_t i = 0; i < m_devices.size() && pick < 0; ++i)
        if (o.devicePersistentId != 0 && m_devices[i].persistentId == o.devicePersistentId) pick = static_cast<int>(i);
    for (size_t i = 0; i < m_devices.size() && pick < 0; ++i)
        if (!o.deviceName.empty() && m_devices[i].name == o.deviceName) pick = static_cast<int>(i);

    if (pick >= 0) {
        ComboBox_SetCurSel(combo, pick);  // no CBS_SORT, so combo index == device index
    } else if (!o.deviceName.empty()) {
        // The configured device is missing right now. Keep it selected so saving other changes
        // never silently moves the output to a different channel.
        int idx = ComboBox_AddString(combo, Utf8ToWide(o.deviceName + " (not found)").c_str());
        ComboBox_SetItemData(combo, idx, kMissing);
        ComboBox_SetCurSel(combo, idx);
    } else if (static_cast<size_t>(row) < m_devices.size()) {
        ComboBox_SetCurSel(combo, row);  // unused row: suggest a different connector per row
    } else if (m_devices.empty()) {
        int idx = ComboBox_AddString(combo, L"(no DeckLink output found)");
        ComboBox_SetItemData(combo, idx, kMissing);
        ComboBox_SetCurSel(combo, idx);
    }
    FillFormats(row, o.displayMode);
}

int SettingsDialog::SelectedDeviceIndex(int row) const {
    HWND combo = Ctl(row, IDC_OFS_DEVICE);
    int sel = ComboBox_GetCurSel(combo);
    if (sel < 0) return -1;
    LRESULT data = ComboBox_GetItemData(combo, sel);
    if (data == kMissing || data < 0 || data >= static_cast<LRESULT>(m_devices.size())) return -1;
    return static_cast<int>(data);
}

int SettingsDialog::SelectedDisplayIndex(int row) const {
    HWND combo = Ctl(row, IDC_OFS_SRC_DISPLAY);
    int sel = ComboBox_GetCurSel(combo);
    if (sel < 0) return -1;
    LRESULT data = ComboBox_GetItemData(combo, sel);
    if (data < 0 || data >= static_cast<LRESULT>(m_rowPaths[row].size())) return -1;
    return static_cast<int>(data);
}

void SettingsDialog::FillFormats(int row, uint32_t preferredMode) {
    HWND combo = Ctl(row, IDC_OFS_FORMAT);
    ComboBox_ResetContent(combo);
    int devSel = SelectedDeviceIndex(row);
    if (devSel < 0) {
        // Unknown device: show the configured format so the user can see what will be used.
        const OutputConfig& o = m_app.config().outputs[static_cast<size_t>(row)];
        int idx = ComboBox_AddString(combo, Utf8ToWide(o.displayModeName).c_str());
        ComboBox_SetItemData(combo, idx, o.displayMode);
        ComboBox_SetCurSel(combo, idx);
        EnableWindow(combo, FALSE);
        return;
    }
    EnableWindow(combo, IsDlgButtonChecked(m_hwnd, CtlId(row, IDC_OFS_ENABLE)) == BST_CHECKED);
    const auto& modes = m_devices[static_cast<size_t>(devSel)].modes;
    int pick = -1, fallback = -1;
    for (size_t i = 0; i < modes.size(); ++i) {
        int idx = ComboBox_AddString(combo, Utf8ToWide(modes[i].name).c_str());
        ComboBox_SetItemData(combo, idx, modes[i].id);
        if (modes[i].id == preferredMode) pick = idx;
        if (modes[i].id == kDefaultDisplayMode) fallback = idx;
    }
    if (pick < 0) pick = fallback >= 0 ? fallback : 0;
    if (!modes.empty()) ComboBox_SetCurSel(combo, pick);
}

void SettingsDialog::Save() {
    Config cfg = m_app.config();
    for (int r = 0; r < kMaxOutputs; ++r) {
        OutputConfig& o = cfg.outputs[static_cast<size_t>(r)];
        o.enabled = IsDlgButtonChecked(m_hwnd, CtlId(r, IDC_OFS_ENABLE)) == BST_CHECKED;
        o.name = Trim(WideToUtf8(GetText(Ctl(r, IDC_OFS_NAME))));
        if (o.name.empty()) o.name = "Output " + std::to_string(r + 1);
        o.sourceType = SelectedType(r);
        o.ndiSource = Trim(WideToUtf8(GetText(Ctl(r, IDC_OFS_SRC_NDI))));

        int disp = SelectedDisplayIndex(r);
        if (disp >= 0) {
            const auto& pg = m_rowPaths[r][static_cast<size_t>(disp)];
            int live = FindDisplay(m_displays, pg.first, pg.second);
            if (live >= 0) {
                const DisplayInfo& d = m_displays[static_cast<size_t>(live)];
                o.displayPath = d.devicePath;
                o.displayGdiName = d.gdiName;
                o.displayLabel = d.isVirtual ? Format("Virtual %d", d.virtualIndex)
                                             : (d.monitorName.empty() ? d.gdiName : d.monitorName);
            }  // else: a missing display stays as configured
        }

        int devSel = SelectedDeviceIndex(r);  // -1: keep the configured (missing) device and format
        if (devSel >= 0) {
            const DeckLinkDeviceInfo& dev = m_devices[static_cast<size_t>(devSel)];
            o.deviceName = dev.name;
            o.devicePersistentId = dev.persistentId;
            HWND fmt = Ctl(r, IDC_OFS_FORMAT);
            int fmtSel = ComboBox_GetCurSel(fmt);
            if (fmtSel >= 0) {
                uint32_t id = static_cast<uint32_t>(ComboBox_GetItemData(fmt, fmtSel));
                for (const auto& m : dev.modes) {
                    if (m.id == id) {
                        o.displayMode = m.id;
                        o.displayModeName = m.name;
                    }
                }
            }
        }
    }

    // Two outputs cannot share one SDI connector.
    for (int a = 0; a < kMaxOutputs; ++a) {
        for (int b = a + 1; b < kMaxOutputs; ++b) {
            const OutputConfig& x = cfg.outputs[static_cast<size_t>(a)];
            const OutputConfig& y = cfg.outputs[static_cast<size_t>(b)];
            bool same = (x.devicePersistentId != 0 && x.devicePersistentId == y.devicePersistentId) ||
                        (!x.deviceName.empty() && x.deviceName == y.deviceName);
            if (x.enabled && y.enabled && same) {
                m_error = Utf8ToWide("Not saved: '" + x.name + "' and '" + y.name + "' both use " + x.deviceName +
                                     ". Pick a different SDI output for one of them.");
                OnStatusChanged();
                return;
            }
        }
    }
    m_error.clear();

    int bufSel = ComboBox_GetCurSel(GetDlgItem(m_hwnd, IDC_BUFFER));
    if (bufSel >= 0) cfg.prerollFrames = bufSel + 2;
    cfg.autoStartOutput = IsDlgButtonChecked(m_hwnd, IDC_AUTOSTART) == BST_CHECKED;
    bool runAtStartup = IsDlgButtonChecked(m_hwnd, IDC_RUNATSTARTUP) == BST_CHECKED;

    m_app.OnSettingsSaved(cfg, runAtStartup);
    Close();
}

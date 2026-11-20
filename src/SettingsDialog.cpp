#include "SettingsDialog.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>

#include "../res/resource.h"
#include "Autostart.h"
#include "Log.h"
#include "NdiLib.h"
#include "TrayApp.h"
#include "Util.h"
#include "VirtualDisplays.h"

namespace {
constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT_PTR kInitialRefreshTimer = 2;
constexpr UINT_PTR kVmonRefreshTimer = 3;
constexpr UINT WM_APP_REMOVE_ROW = WM_APP + 20;  // posted, so a row's Remove button isn't destroyed in its own click
const wchar_t kRowsClass[] = L"Screen2SDI.Rows";
constexpr int kRowHeightDlu = 64;

std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(static_cast<size_t>(n));
    return s;
}

void SetTextIfChanged(HWND h, const std::wstring& text) {
    if (h && GetText(h) != text) SetWindowTextW(h, text.c_str());
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

bool UsesDevice(const OutputConfig& o, const DeckLinkDeviceInfo& d) {
    return (o.devicePersistentId != 0 && o.devicePersistentId == d.persistentId) ||
           (!o.deviceName.empty() && o.deviceName == d.name);
}

bool UsesDisplay(const OutputConfig& o, const DisplayInfo& d) {
    if (o.sourceType != SourceType::Display) return false;
    if (!o.displayPath.empty()) return o.displayPath == d.devicePath;
    return !o.displayGdiName.empty() && o.displayGdiName == d.gdiName;
}

std::string DisplayLabelFor(const DisplayInfo& d) {
    return d.isVirtual ? Format("Virtual %d", d.virtualIndex) : (d.monitorName.empty() ? d.gdiName : d.monitorName);
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// Window plumbing

void SettingsDialog::Show() {
    if (!m_hwnd) {
        static bool registered = false;
        if (!registered) {
            INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES};
            InitCommonControlsEx(&icc);
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc = PanelProc;
            wc.hInstance = m_app.instance();
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
            wc.lpszClassName = kRowsClass;
            registered = RegisterClassExW(&wc) != 0;
            if (!registered) Log::Error("Cannot register the Settings rows panel (error %lu)", GetLastError());
        }
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

HWND SettingsDialog::Ctl(int row, int offset) const { return GetDlgItem(m_panel, CtlId(row, offset)); }

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
    if (msg == WM_NCDESTROY) {
        self->m_hwnd = nullptr;
        self->m_panel = nullptr;
    }
    return r;
}

LRESULT CALLBACK SettingsDialog::PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->m_panel == hwnd) return self->HandlePanel(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT SettingsDialog::HandlePanel(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            // Row controls live in the panel; the dialog handles their notifications.
            return SendMessageW(m_hwnd, WM_COMMAND, wp, lp);
        case WM_VSCROLL: {
            SCROLLINFO si{sizeof(si), SIF_ALL};
            GetScrollInfo(m_panel, SB_VERT, &si);
            const int line = std::max(1, m_rowHeightPx / 4);
            int y = m_scrollY;
            switch (LOWORD(wp)) {
                case SB_LINEUP: y -= line; break;
                case SB_LINEDOWN: y += line; break;
                case SB_PAGEUP: y -= static_cast<int>(si.nPage); break;
                case SB_PAGEDOWN: y += static_cast<int>(si.nPage); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: y = si.nTrackPos; break;
                case SB_TOP: y = 0; break;
                case SB_BOTTOM: y = ContentHeight(); break;
                default: break;
            }
            ScrollTo(y);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            ScrollTo(m_scrollY - delta * (m_rowHeightPx / 2) / WHEEL_DELTA);
            return 0;
        }
        default: break;
    }
    return DefWindowProcW(m_panel, msg, wp, lp);
}

INT_PTR SettingsDialog::Handle(UINT msg, WPARAM wp, LPARAM lp) {
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
            } else if (wp == kVmonRefreshTimer) {
                KillTimer(m_hwnd, kVmonRefreshTimer);
                RefreshSources();
                ReadVirtualMonitors();
            }
            return TRUE;
        case WM_MOUSEWHEEL:
            if (m_panel) SendMessageW(m_panel, WM_MOUSEWHEEL, wp, lp);
            return TRUE;
        case WM_APP_REMOVE_ROW:
            RemoveOutput(static_cast<int>(wp));
            return TRUE;
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            int row, ofs;
            if (RowControl(id, &row, &ofs)) {
                if (row >= static_cast<int>(m_work.size())) return TRUE;
                const bool isCombo = ofs == IDC_OFS_TYPE || ofs == IDC_OFS_SRC_NDI || ofs == IDC_OFS_SRC_DISPLAY ||
                                     ofs == IDC_OFS_DEVICE || ofs == IDC_OFS_FORMAT;
                const bool isButton = ofs == IDC_OFS_ENABLE || ofs == IDC_OFS_REMOVE;
                if ((isCombo && code == CBN_SETFOCUS) || (isButton && code == BN_SETFOCUS) ||
                    (ofs == IDC_OFS_NAME && code == EN_SETFOCUS)) {
                    EnsureRowVisible(row);  // keyboard (Tab) navigation scrolls the row into view
                } else if ((ofs == IDC_OFS_ENABLE && code == BN_CLICKED) ||
                           (ofs == IDC_OFS_TYPE && code == CBN_SELCHANGE)) {
                    UpdateRowControls(row);
                } else if (ofs == IDC_OFS_DEVICE && code == CBN_SELCHANGE) {
                    HWND fmt = Ctl(row, IDC_OFS_FORMAT);
                    int sel = ComboBox_GetCurSel(fmt);
                    uint32_t keep = sel >= 0 ? static_cast<uint32_t>(ComboBox_GetItemData(fmt, sel)) : 0;
                    FillFormats(row, keep);
                    UpdateCountText();
                } else if (ofs == IDC_OFS_REMOVE && code == BN_CLICKED) {
                    PostMessageW(m_hwnd, WM_APP_REMOVE_ROW, static_cast<WPARAM>(row), 0);
                }
                return TRUE;
            }
            switch (id) {
                case IDOK: Save(); return TRUE;
                case IDCANCEL: Close(); return TRUE;
                case IDC_REFRESH: RefreshSources(); return TRUE;
                case IDC_ADD: AddOutput(); return TRUE;
                case IDC_ADD_ALL: AddAllSdiOutputs(); return TRUE;
                case IDC_VMON_APPLY: ApplyVirtualMonitors(); return TRUE;
            }
            break;
        }
        case WM_CLOSE:
            Close();
            return TRUE;
        case WM_DESTROY:
            KillTimer(m_hwnd, kStatusTimer);
            KillTimer(m_hwnd, kInitialRefreshTimer);
            KillTimer(m_hwnd, kVmonRefreshTimer);
            m_rowWindows.clear();  // children are destroyed with the dialog
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

    m_panel = GetDlgItem(m_hwnd, IDC_ROWS);
    SetWindowLongPtrW(m_panel, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    RECT rh{0, 0, 0, kRowHeightDlu};
    MapDialogRect(m_hwnd, &rh);
    m_rowHeightPx = std::max(1, static_cast<int>(rh.bottom));
    m_scrollY = 0;
    m_rowWindows.clear();

    const Config& cfg = m_app.config();
    std::string err;
    m_devices = EnumerateDeckLinkOutputs(&err);
    // Hide sub-devices that currently offer no output formats (e.g. a connector set to input).
    m_devices.erase(std::remove_if(m_devices.begin(), m_devices.end(),
                                   [](const DeckLinkDeviceInfo& d) { return d.modes.empty(); }),
                    m_devices.end());
    if (m_devices.empty()) Log::Warn("Settings: no DeckLink outputs found%s%s", err.empty() ? "" : ": ", err.c_str());
    m_displays = EnumerateDisplays();

    m_work = cfg.outputs;
    m_origIndex.clear();
    for (size_t i = 0; i < m_work.size(); ++i) m_origIndex.push_back(static_cast<int>(i));
    if (!m_app.configured() && m_work.empty()) {
        // First run: one row per SDI output found, each paired with the next virtual monitor.
        // Rows that got no monitor start switched off.
        const size_t n = std::max<size_t>(1, std::min<size_t>(m_devices.size(), kMaxOutputs));
        for (size_t i = 0; i < n; ++i) {
            OutputConfig o = NewOutput();
            o.enabled = !o.displayPath.empty() || !o.displayGdiName.empty() || i == 0;
            m_work.push_back(o);
            m_origIndex.push_back(-1);
        }
    }
    BuildRows();

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

    ReadVirtualMonitors();
    OnStatusChanged();
    SetTimer(m_hwnd, kStatusTimer, 1000, nullptr);
    SetTimer(m_hwnd, kInitialRefreshTimer, 2500, nullptr);  // NDI discovery needs a moment after launch
}

// ---------------------------------------------------------------------------------------------
// Rows

int SettingsDialog::ContentHeight() const { return static_cast<int>(m_work.size()) * m_rowHeightPx; }

int SettingsDialog::PageHeight() const {
    RECT rc{};
    GetClientRect(m_panel, &rc);
    return rc.bottom;
}

void SettingsDialog::UpdateScroll() {
    const int maxPos = std::max(0, ContentHeight() - PageHeight());
    m_scrollY = std::clamp(m_scrollY, 0, maxPos);
    SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL};
    si.nMin = 0;
    si.nMax = std::max(0, ContentHeight() - 1);
    si.nPage = static_cast<UINT>(PageHeight());
    si.nPos = m_scrollY;
    SetScrollInfo(m_panel, SB_VERT, &si, TRUE);
}

void SettingsDialog::ScrollTo(int y) {
    y = std::clamp(y, 0, std::max(0, ContentHeight() - PageHeight()));
    const int dy = m_scrollY - y;
    if (dy == 0) return;
    m_scrollY = y;
    // Move the row controls and repaint, rather than ScrollWindowEx: blitting screen bits right
    // after rows were rebuilt can copy not-yet-repainted pixels and leave ghost images.
    for (const auto& rowWindows : m_rowWindows) {
        for (HWND h : rowWindows) {
            RECT r;
            GetWindowRect(h, &r);
            MapWindowPoints(nullptr, m_panel, reinterpret_cast<POINT*>(&r), 2);
            SetWindowPos(h, nullptr, r.left, r.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
        }
    }
    SetScrollPos(m_panel, SB_VERT, y, TRUE);
    RedrawWindow(m_panel, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void SettingsDialog::EnsureRowVisible(int row) {
    const int top = row * m_rowHeightPx;
    const int bottom = top + m_rowHeightPx;
    if (top < m_scrollY)
        ScrollTo(top);
    else if (bottom > m_scrollY + PageHeight())
        ScrollTo(bottom - PageHeight());
}

void SettingsDialog::BuildRows() {
    SendMessageW(m_panel, WM_SETREDRAW, FALSE, 0);
    for (auto& v : m_rowWindows)
        for (HWND h : v) DestroyWindow(h);
    m_rowWindows.assign(m_work.size(), {});
    m_rowPaths.assign(m_work.size(), {});
    UpdateScroll();  // clamps m_scrollY for the new row count before rows are placed

    const std::vector<std::string> ndiNames = Ndi::GetSourceNames();
    for (int r = 0; r < static_cast<int>(m_work.size()); ++r) {
        CreateRow(r);
        const OutputConfig& o = m_work[static_cast<size_t>(r)];
        CheckDlgButton(m_panel, CtlId(r, IDC_OFS_ENABLE), o.enabled ? BST_CHECKED : BST_UNCHECKED);
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
    SendMessageW(m_panel, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(m_panel, nullptr, nullptr, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    UpdateCountText();
    OnStatusChanged();
}

void SettingsDialog::CreateRow(int row) {
    const HFONT font = reinterpret_cast<HFONT>(SendMessageW(m_hwnd, WM_GETFONT, 0, 0));
    const int top = row * m_rowHeightPx - m_scrollY;
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h,
                    DWORD exStyle = 0) {
        RECT r{x, y, x + w, y + h};
        MapDialogRect(m_hwnd, &r);
        HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, r.left, r.top + top,
                                 r.right - r.left, r.bottom - r.top, m_panel,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), m_app.instance(), nullptr);
        if (c) {
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
            m_rowWindows[static_cast<size_t>(row)].push_back(c);
        }
        return c;
    };
    const DWORD drop = CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP;
    const std::wstring title = L"Output " + std::to_wstring(row + 1);
    // Layout in dialog units, relative to the row's top. Creation order = Tab order.
    make(L"BUTTON", title.c_str(), BS_GROUPBOX, CtlId(row, IDC_OFS_GROUP), 2, 2, 402, 60);
    make(L"BUTTON", L"On", BS_AUTOCHECKBOX | BS_NOTIFY | WS_TABSTOP | WS_GROUP, CtlId(row, IDC_OFS_ENABLE), 8, 14, 26, 10);
    make(L"STATIC", L"Name:", SS_LEFT, -1, 38, 15, 24, 8);
    make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER, CtlId(row, IDC_OFS_NAME), 64, 13, 82, 12);
    make(L"STATIC", L"Source:", SS_LEFT, -1, 152, 15, 26, 8);
    make(L"COMBOBOX", L"", drop, CtlId(row, IDC_OFS_TYPE), 180, 13, 52, 60);
    make(L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, CtlId(row, IDC_OFS_SRC_NDI), 236,
         13, 164, 150);
    make(L"COMBOBOX", L"", drop, CtlId(row, IDC_OFS_SRC_DISPLAY), 236, 13, 164, 150);
    make(L"STATIC", L"SDI out:", SS_LEFT, -1, 38, 31, 26, 8);
    make(L"COMBOBOX", L"", drop, CtlId(row, IDC_OFS_DEVICE), 64, 29, 168, 150);
    make(L"STATIC", L"Format:", SS_LEFT, -1, 236, 31, 26, 8);
    make(L"COMBOBOX", L"", drop, CtlId(row, IDC_OFS_FORMAT), 264, 29, 136, 200);
    make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, CtlId(row, IDC_OFS_STATUS), 8, 47, 340, 9);
    make(L"BUTTON", L"Remove", BS_PUSHBUTTON | BS_NOTIFY | WS_TABSTOP, CtlId(row, IDC_OFS_REMOVE), 352, 44, 48, 13);
}

void SettingsDialog::CollectRows() {
    for (int r = 0; r < static_cast<int>(m_work.size()); ++r) {
        OutputConfig& o = m_work[static_cast<size_t>(r)];
        o.enabled = IsDlgButtonChecked(m_panel, CtlId(r, IDC_OFS_ENABLE)) == BST_CHECKED;
        o.name = Trim(WideToUtf8(GetText(Ctl(r, IDC_OFS_NAME))));
        if (o.name.empty()) o.name = DefaultOutputName(static_cast<size_t>(r));
        o.sourceType = SelectedType(r);
        o.ndiSource = Trim(WideToUtf8(GetText(Ctl(r, IDC_OFS_SRC_NDI))));

        int disp = SelectedDisplayIndex(r);
        if (disp >= 0) {
            const auto& pg = m_rowPaths[static_cast<size_t>(r)][static_cast<size_t>(disp)];
            int live = FindDisplay(m_displays, pg.first, pg.second);
            if (live >= 0) {
                const DisplayInfo& d = m_displays[static_cast<size_t>(live)];
                o.displayPath = d.devicePath;
                o.displayGdiName = d.gdiName;
                o.displayLabel = DisplayLabelFor(d);
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
}

bool SettingsDialog::DeviceUsedByOtherRow(const DeckLinkDeviceInfo& d, int row) const {
    for (size_t i = 0; i < m_work.size(); ++i)
        if (static_cast<int>(i) != row && UsesDevice(m_work[i], d)) return true;
    return false;
}

bool SettingsDialog::DisplayUsedByOtherRow(const DisplayInfo& d, int row) const {
    for (size_t i = 0; i < m_work.size(); ++i)
        if (static_cast<int>(i) != row && UsesDisplay(m_work[i], d)) return true;
    return false;
}

OutputConfig SettingsDialog::NewOutput() const {
    OutputConfig o;
    o.enabled = true;
    o.name = DefaultOutputName(m_work.size());
    o.sourceType = SourceType::Display;
    for (const auto& d : m_devices) {
        if (!DeviceUsedByOtherRow(d, -1)) {
            o.deviceName = d.name;
            o.devicePersistentId = d.persistentId;
            break;
        }
    }
    // Next unused virtual monitor (left to right); otherwise leave the display unset.
    for (const auto& d : m_displays) {
        if (d.isVirtual && !DisplayUsedByOtherRow(d, -1)) {
            o.displayPath = d.devicePath;
            o.displayGdiName = d.gdiName;
            o.displayLabel = DisplayLabelFor(d);
            break;
        }
    }
    return o;
}

void SettingsDialog::AddOutput() {
    CollectRows();
    if (m_work.size() >= static_cast<size_t>(kMaxOutputs)) {
        m_error = L"Already at the maximum of " + std::to_wstring(kMaxOutputs) + L" outputs.";
        OnStatusChanged();
        return;
    }
    m_error.clear();
    m_work.push_back(NewOutput());
    m_origIndex.push_back(-1);
    BuildRows();
    const int last = static_cast<int>(m_work.size()) - 1;
    EnsureRowVisible(last);
    SetFocus(Ctl(last, IDC_OFS_NAME));
    Edit_SetSel(Ctl(last, IDC_OFS_NAME), 0, -1);
}

void SettingsDialog::AddAllSdiOutputs() {
    CollectRows();
    const int first = static_cast<int>(m_work.size());
    int added = 0;
    for (const auto& d : m_devices) {
        if (m_work.size() >= static_cast<size_t>(kMaxOutputs)) break;
        if (DeviceUsedByOtherRow(d, -1)) continue;
        OutputConfig o = NewOutput();  // picks this device: the first one not used yet
        o.enabled = !o.displayPath.empty() || !o.displayGdiName.empty();
        m_work.push_back(o);
        m_origIndex.push_back(-1);
        ++added;
    }
    if (added == 0) {
        m_error = m_devices.empty() ? L"No DeckLink outputs found. Check Desktop Video Setup, then Refresh lists."
                                    : L"Every SDI output already has a row.";
        OnStatusChanged();
        return;
    }
    m_error.clear();
    BuildRows();
    EnsureRowVisible(static_cast<int>(m_work.size()) - 1);
    EnsureRowVisible(first);
    Log::Info("Settings: added %d output row(s), one per unused SDI output", added);
}

void SettingsDialog::RemoveOutput(int row) {
    if (row < 0 || row >= static_cast<int>(m_work.size())) return;
    CollectRows();
    m_work.erase(m_work.begin() + row);
    m_origIndex.erase(m_origIndex.begin() + row);
    m_error.clear();
    BuildRows();
    if (!m_work.empty()) EnsureRowVisible(std::min(row, static_cast<int>(m_work.size()) - 1));
}

void SettingsDialog::UpdateCountText() {
    size_t freeDevices = 0;
    for (const auto& d : m_devices)
        if (!DeviceUsedByOtherRow(d, -1)) ++freeDevices;
    const std::wstring text = std::to_wstring(m_work.size()) + L" of max " + std::to_wstring(kMaxOutputs) +
                              L" outputs. SDI outputs found: " + std::to_wstring(m_devices.size()) + L" (" +
                              std::to_wstring(freeDevices) + L" unused)";
    SetTextIfChanged(GetDlgItem(m_hwnd, IDC_COUNT_TEXT), text);
    const bool room = m_work.size() < static_cast<size_t>(kMaxOutputs);
    EnableWindow(GetDlgItem(m_hwnd, IDC_ADD), room);
    EnableWindow(GetDlgItem(m_hwnd, IDC_ADD_ALL), room && freeDevices > 0);
}

void SettingsDialog::OnStatusChanged() {
    if (!m_hwnd || !m_panel) return;
    int live = 0, waiting = 0, other = 0, off = 0;
    for (int r = 0; r < static_cast<int>(m_work.size()); ++r) {
        std::wstring text;
        const int orig = m_origIndex[static_cast<size_t>(r)];
        if (orig >= 0 && orig < m_app.OutputCount()) {
            PipelineStatus st = m_app.OutputStatus(orig);
            text = Utf8ToWide(st.text);
            switch (st.state) {
                case PipelineState::Live: ++live; break;
                case PipelineState::Waiting: ++waiting; break;
                case PipelineState::Disabled: ++off; break;
                default: ++other; break;
            }
        } else {
            text = L"New - click Save to start it";
        }
        SetTextIfChanged(Ctl(r, IDC_OFS_STATUS), L"Status: " + text);
    }
    std::wstring summary = m_error;
    if (summary.empty()) {
        if (m_work.empty()) {
            summary = L"No outputs. Click \"Add output\" or \"Add all SDI outputs\".";
        } else {
            summary = std::to_wstring(live) + L" live, " + std::to_wstring(waiting) + L" black (waiting), " +
                      std::to_wstring(other) + L" not running, " + std::to_wstring(off) + L" off.";
        }
    }
    SetTextIfChanged(GetDlgItem(m_hwnd, IDC_STATUS), summary);
}

void SettingsDialog::UpdateRowControls(int row) {
    const bool on = IsDlgButtonChecked(m_panel, CtlId(row, IDC_OFS_ENABLE)) == BST_CHECKED;
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
    std::vector<std::pair<std::string, std::string>> keep(m_work.size());
    for (int r = 0; r < static_cast<int>(m_work.size()); ++r) {
        int idx = SelectedDisplayIndex(r);
        if (idx >= 0) keep[static_cast<size_t>(r)] = m_rowPaths[static_cast<size_t>(r)][static_cast<size_t>(idx)];
    }
    m_displays = EnumerateDisplays();
    for (int r = 0; r < static_cast<int>(m_work.size()); ++r) {
        FillNdi(r, names);
        FillDisplays(r, keep[static_cast<size_t>(r)].first, keep[static_cast<size_t>(r)].second);
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
    auto& paths = m_rowPaths[static_cast<size_t>(row)];
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
        const OutputConfig& o = m_work[static_cast<size_t>(row)];
        std::string label = (o.displayLabel.empty() ? keepGdi : o.displayLabel) + " (not found)";
        int idx = ComboBox_AddString(combo, Utf8ToWide(label).c_str());
        ComboBox_SetItemData(combo, idx, paths.size());
        paths.emplace_back(keepPath, keepGdi);
        ComboBox_SetCurSel(combo, idx);
    } else {
        // Nothing chosen yet: suggest the first virtual monitor no other row uses.
        for (size_t i = 0; i < m_displays.size(); ++i) {
            if (m_displays[i].isVirtual && !DisplayUsedByOtherRow(m_displays[i], row)) {
                ComboBox_SetCurSel(combo, static_cast<int>(i));
                break;
            }
        }
    }
}

void SettingsDialog::FillDevices(int row) {
    HWND combo = Ctl(row, IDC_OFS_DEVICE);
    ComboBox_ResetContent(combo);
    const OutputConfig& o = m_work[static_cast<size_t>(row)];
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
    } else if (m_devices.empty()) {
        int idx = ComboBox_AddString(combo, L"(no DeckLink output found)");
        ComboBox_SetItemData(combo, idx, kMissing);
        ComboBox_SetCurSel(combo, idx);
    } else {
        // No device chosen yet: suggest the first one no other row uses (or leave it blank).
        for (size_t i = 0; i < m_devices.size(); ++i) {
            if (!DeviceUsedByOtherRow(m_devices[i], row)) {
                ComboBox_SetCurSel(combo, static_cast<int>(i));
                break;
            }
        }
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
    if (data < 0 || data >= static_cast<LRESULT>(m_rowPaths[static_cast<size_t>(row)].size())) return -1;
    return static_cast<int>(data);
}

void SettingsDialog::FillFormats(int row, uint32_t preferredMode) {
    HWND combo = Ctl(row, IDC_OFS_FORMAT);
    ComboBox_ResetContent(combo);
    int devSel = SelectedDeviceIndex(row);
    if (devSel < 0) {
        // Unknown device: show the configured format so the user can see what will be used.
        const OutputConfig& o = m_work[static_cast<size_t>(row)];
        int idx = ComboBox_AddString(combo, Utf8ToWide(o.displayModeName).c_str());
        ComboBox_SetItemData(combo, idx, o.displayMode);
        ComboBox_SetCurSel(combo, idx);
        EnableWindow(combo, FALSE);
        return;
    }
    EnableWindow(combo, IsDlgButtonChecked(m_panel, CtlId(row, IDC_OFS_ENABLE)) == BST_CHECKED);
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

// ---------------------------------------------------------------------------------------------
// Virtual monitors

void SettingsDialog::ReadVirtualMonitors() {
    HWND edit = GetDlgItem(m_hwnd, IDC_VMON_COUNT);
    HWND spin = GetDlgItem(m_hwnd, IDC_VMON_SPIN);
    HWND apply = GetDlgItem(m_hwnd, IDC_VMON_APPLY);
    int n = 0;
    std::string err;
    if (Vdd::ReadMonitorCount(&n, &err)) {
        m_vmonCount = n;
        SendMessageW(spin, UDM_SETRANGE32, 1, Vdd::kMaxMonitors);
        SendMessageW(spin, UDM_SETPOS32, 0, n);
        for (HWND h : {edit, spin, apply}) EnableWindow(h, TRUE);
        SetDlgItemTextW(m_hwnd, IDC_VMON_INFO,
                        (L"Now " + std::to_wstring(n) + L" (max " + std::to_wstring(Vdd::kMaxMonitors) +
                         L"). Changing it asks for admin rights, and all virtual monitors blink while the driver restarts.")
                            .c_str());
    } else {
        m_vmonCount = 0;
        for (HWND h : {edit, spin, apply}) EnableWindow(h, FALSE);
        SetDlgItemTextW(m_hwnd, IDC_VMON_INFO, Utf8ToWide(err + ". See README (virtual monitors).").c_str());
    }
}

void SettingsDialog::ApplyVirtualMonitors() {
    BOOL ok = FALSE;
    const int n = static_cast<int>(GetDlgItemInt(m_hwnd, IDC_VMON_COUNT, &ok, FALSE));
    if (!ok || n < 1 || n > Vdd::kMaxMonitors) {
        SetDlgItemTextW(m_hwnd, IDC_VMON_INFO, (L"Enter a number from 1 to " + std::to_wstring(Vdd::kMaxMonitors) + L".").c_str());
        return;
    }
    if (n == m_vmonCount) {
        SetDlgItemTextW(m_hwnd, IDC_VMON_INFO, (L"Already set to " + std::to_wstring(n) + L".").c_str());
        return;
    }
    const std::wstring msg =
        L"Change the number of virtual monitors from " + std::to_wstring(m_vmonCount) + L" to " + std::to_wstring(n) +
        L"?\n\nWindows will ask for administrator permission. All virtual monitors switch off for a few seconds "
        L"while the driver restarts, so anything shown on them (including other programs' outputs) will blink, "
        L"and windows on them may move.\n\nContinue?";
    if (MessageBoxW(m_hwnd, msg.c_str(), L"Screen2SDI - virtual monitors", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) !=
        IDYES)
        return;

    Log::Info("Settings: changing virtual monitors %d -> %d", m_vmonCount, n);
    const std::wstring exe = ExePath();
    const std::wstring args = L"--set-virtual-monitors=" + std::to_wstring(n);
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd = m_hwnd;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        const DWORD e = GetLastError();
        SetDlgItemTextW(m_hwnd, IDC_VMON_INFO,
                        e == ERROR_CANCELLED ? L"Cancelled - nothing changed."
                                             : (L"Could not start the change (error " + std::to_wstring(e) + L").").c_str());
        return;
    }
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    DWORD rc = 1;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 60000);
        GetExitCodeProcess(sei.hProcess, &rc);
        CloseHandle(sei.hProcess);
    }
    SetCursor(old);
    if (rc == 0) {
        Log::Info("Settings: virtual monitors set to %d", n);
        SetDlgItemTextW(m_hwnd, IDC_VMON_INFO,
                        (L"Set to " + std::to_wstring(n) + L". Monitor lists refresh in a few seconds.").c_str());
    } else {
        Log::Error("Settings: changing virtual monitors failed (exit code %lu); see the log", rc);
        SetDlgItemTextW(m_hwnd, IDC_VMON_INFO, L"Failed - see the log (tray menu > Open Log Folder).");
    }
    SetTimer(m_hwnd, kVmonRefreshTimer, 4000, nullptr);
}

// ---------------------------------------------------------------------------------------------

void SettingsDialog::Save() {
    CollectRows();

    for (size_t a = 0; a < m_work.size(); ++a) {
        const OutputConfig& x = m_work[a];
        if (x.enabled && x.deviceName.empty() && !m_devices.empty()) {
            m_error = Utf8ToWide("Not saved: pick an SDI output for '" + x.name + "'.");
            EnsureRowVisible(static_cast<int>(a));
            OnStatusChanged();
            return;
        }
        // Two outputs cannot share one SDI connector.
        for (size_t b = a + 1; b < m_work.size(); ++b) {
            const OutputConfig& y = m_work[b];
            bool same = (x.devicePersistentId != 0 && x.devicePersistentId == y.devicePersistentId) ||
                        (!x.deviceName.empty() && x.deviceName == y.deviceName);
            if (x.enabled && y.enabled && same) {
                m_error = Utf8ToWide("Not saved: '" + x.name + "' and '" + y.name + "' both use " + x.deviceName +
                                     ". Pick a different SDI output for one of them.");
                EnsureRowVisible(static_cast<int>(b));
                OnStatusChanged();
                return;
            }
        }
    }
    m_error.clear();

    Config cfg = m_app.config();
    cfg.outputs = m_work;
    int bufSel = ComboBox_GetCurSel(GetDlgItem(m_hwnd, IDC_BUFFER));
    if (bufSel >= 0) cfg.prerollFrames = bufSel + 2;
    cfg.autoStartOutput = IsDlgButtonChecked(m_hwnd, IDC_AUTOSTART) == BST_CHECKED;
    bool runAtStartup = IsDlgButtonChecked(m_hwnd, IDC_RUNATSTARTUP) == BST_CHECKED;

    m_app.OnSettingsSaved(cfg, runAtStartup);
    Close();
}

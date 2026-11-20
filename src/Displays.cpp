#include "Displays.h"

#include <windows.h>

#include <algorithm>

#include "Util.h"

namespace {
bool LooksVirtual(const std::string& path, const std::string& name) {
    auto has = [](const std::string& s, const char* sub) {
        std::string a = s, b = sub;
        std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return static_cast<char>(toupper(c)); });
        return a.find(b) != std::string::npos;
    };
    return has(path, "MTT1337") || has(name, "VDD") || has(name, "VIRTUAL");
}
}  // namespace

std::vector<DisplayInfo> EnumerateDisplays() {
    std::vector<DisplayInfo> out;

    UINT32 numPaths = 0, numModes = 0;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG rc = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 3 && rc == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &numPaths, &numModes) != ERROR_SUCCESS) return out;
        paths.resize(numPaths);
        modes.resize(numModes);
        rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &numPaths, paths.data(), &numModes, modes.data(), nullptr);
    }
    if (rc != ERROR_SUCCESS) return out;
    paths.resize(numPaths);

    for (const DISPLAYCONFIG_PATH_INFO& p : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = p.sourceInfo.adapterId;
        src.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;

        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
        tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgt.header.size = sizeof(tgt);
        tgt.header.adapterId = p.targetInfo.adapterId;
        tgt.header.id = p.targetInfo.id;
        DisplayConfigGetDeviceInfo(&tgt.header);  // optional details

        DisplayInfo d;
        d.gdiName = WideToUtf8(src.viewGdiDeviceName);
        // A cloned display has several paths with the same source; keep the first.
        if (std::any_of(out.begin(), out.end(), [&](const DisplayInfo& e) { return e.gdiName == d.gdiName; })) continue;
        d.devicePath = WideToUtf8(tgt.monitorDevicePath);
        d.monitorName = WideToUtf8(tgt.monitorFriendlyDeviceName);
        d.isVirtual = LooksVirtual(d.devicePath, d.monitorName);

        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(src.viewGdiDeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
            d.x = dm.dmPosition.x;
            d.y = dm.dmPosition.y;
            d.width = static_cast<int>(dm.dmPelsWidth);
            d.height = static_cast<int>(dm.dmPelsHeight);
            d.refreshHz = static_cast<int>(dm.dmDisplayFrequency);
        }
        d.primary = d.x == 0 && d.y == 0;
        out.push_back(std::move(d));
    }

    std::sort(out.begin(), out.end(), [](const DisplayInfo& a, const DisplayInfo& b) {
        return a.x != b.x ? a.x < b.x : a.y < b.y;
    });
    int v = 0;
    for (DisplayInfo& d : out) {
        if (d.isVirtual) d.virtualIndex = ++v;
        std::string what = d.isVirtual ? Format("Virtual %d", d.virtualIndex)
                                       : (d.monitorName.empty() ? std::string("Monitor") : d.monitorName);
        d.label = Format("%s - %dx%d @ %d Hz at (%d, %d)%s", what.c_str(), d.width, d.height, d.refreshHz, d.x, d.y,
                         d.primary ? " primary" : "");
    }
    return out;
}

int FindDisplay(const std::vector<DisplayInfo>& displays, const std::string& devicePath, const std::string& gdiName) {
    if (!devicePath.empty())
        for (size_t i = 0; i < displays.size(); ++i)
            if (_stricmp(displays[i].devicePath.c_str(), devicePath.c_str()) == 0) return static_cast<int>(i);
    if (devicePath.empty() && !gdiName.empty())
        for (size_t i = 0; i < displays.size(); ++i)
            if (_stricmp(displays[i].gdiName.c_str(), gdiName.c_str()) == 0) return static_cast<int>(i);
    return -1;
}

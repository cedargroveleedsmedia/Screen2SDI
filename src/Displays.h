#pragma once
#include <string>
#include <vector>

// An active Windows display (monitor) that can be captured.
struct DisplayInfo {
    std::string gdiName;      // "\\.\DISPLAY16" - changes when displays are re-plugged/restarted
    std::string devicePath;   // monitor device interface path - stable across reboots
    std::string monitorName;  // EDID name, e.g. "AOC 2280W"; may be empty
    bool isVirtual = false;   // a Virtual Display Driver (IddCx) monitor
    int virtualIndex = 0;     // 1-based, left to right, among virtual monitors
    int x = 0, y = 0, width = 0, height = 0, refreshHz = 0;
    bool primary = false;
    std::string label;        // e.g. "Virtual 1 - 1920x1080 @ 60 Hz at (7680, 0)"
};

// Active displays sorted left to right.
std::vector<DisplayInfo> EnumerateDisplays();

// Finds the configured display: by device path, then by GDI name. Returns -1 if absent.
int FindDisplay(const std::vector<DisplayInfo>& displays, const std::string& devicePath, const std::string& gdiName);

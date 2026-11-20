#pragma once
#include <cstdlib>
#include <string>

// Controls the number of monitors of the Virtual Display Driver (github.com/VirtualDrivers/
// Virtual-Display-Driver): <monitors><count>N</count></monitors> in vdd_settings.xml, then a
// driver restart. Changing it needs administrator rights (see SetMonitorCount).
namespace Vdd {

constexpr int kMaxMonitors = 16;

// Full path of vdd_settings.xml, or "" if the driver's settings file is not found.
std::wstring SettingsPath();

// Current monitor count from vdd_settings.xml.
bool ReadMonitorCount(int* count, std::string* error);

// Writes the new count (after a timestamped backup of the file) and restarts the driver so it
// takes effect. Needs administrator rights. All virtual monitors switch off for a few seconds.
bool SetMonitorCount(int count, std::string* error);

// ---- Pure helpers (unit tested) ----

// Finds the <count> value inside <monitors>...</monitors>. Sets *begin/*end to the digits' range.
inline bool FindMonitorCount(const std::string& xml, size_t* begin, size_t* end) {
    const size_t mon = xml.find("<monitors>");
    if (mon == std::string::npos) return false;
    const size_t monEnd = xml.find("</monitors>", mon);
    const size_t tag = xml.find("<count>", mon);
    if (tag == std::string::npos || (monEnd != std::string::npos && tag > monEnd)) return false;
    size_t b = tag + 7;
    while (b < xml.size() && (xml[b] == ' ' || xml[b] == '\t')) ++b;
    size_t e = b;
    while (e < xml.size() && xml[e] >= '0' && xml[e] <= '9') ++e;
    if (e == b) return false;
    *begin = b;
    *end = e;
    return true;
}

inline bool ParseMonitorCount(const std::string& xml, int* count) {
    size_t b, e;
    if (!FindMonitorCount(xml, &b, &e) || e - b > 4) return false;
    *count = std::atoi(xml.substr(b, e - b).c_str());
    return true;
}

inline bool ReplaceMonitorCount(const std::string& xml, int count, std::string* out) {
    size_t b, e;
    if (count < 1 || count > kMaxMonitors || !FindMonitorCount(xml, &b, &e)) return false;
    *out = xml.substr(0, b) + std::to_string(count) + xml.substr(e);
    return true;
}

}  // namespace Vdd

#pragma once
#include <sal.h>
#include <string>

// Daily log files in %APPDATA%\NDI2SDI\logs\NDI2SDI_YYYY-MM-DD.log, keeping the last 7 days.
// Thread-safe. Never throws, never shows UI.
namespace Log {
void Init(const std::wstring& dir);
void Shutdown();
// Optional short tag (e.g. "Out2") added to every line logged from the calling thread.
void SetThreadTag(const char* tag);
void Info(_Printf_format_string_ const char* fmt, ...);
void Warn(_Printf_format_string_ const char* fmt, ...);
void Error(_Printf_format_string_ const char* fmt, ...);
}  // namespace Log

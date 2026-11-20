#pragma once
#include <cstdint>
#include <string>

std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);

// %APPDATA%\NDI2SDI (created if missing).
std::wstring AppDataDir();
// %APPDATA%\NDI2SDI\logs (created if missing).
std::wstring LogDir();
// Full path of the running executable.
std::wstring ExePath();
std::wstring ExeDir();

// Monotonic milliseconds.
uint64_t NowMs();

// printf into std::string.
std::string Format(const char* fmt, ...);

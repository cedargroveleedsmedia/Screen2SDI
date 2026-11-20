#include "Util.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdarg>
#include <vector>

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring AppDataDir() {
    std::wstring dir;
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &path))) {
        dir = path;
        CoTaskMemFree(path);
    } else {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
        dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : ExeDir();
    }
    dir += L"\\Screen2SDI";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring LogDir() {
    std::wstring dir = AppDataDir() + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring ExePath() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) return std::wstring(buf.data(), n);
        buf.resize(buf.size() * 2);
    }
}

std::wstring ExeDir() {
    std::wstring p = ExePath();
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? p : p.substr(0, pos);
}

uint64_t NowMs() { return GetTickCount64(); }

std::string Format(const char* fmt, ...) {
    char stackBuf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stackBuf, sizeof(stackBuf), fmt, ap);
    va_end(ap);
    if (n < 0) return {};
    if (static_cast<size_t>(n) < sizeof(stackBuf)) return std::string(stackBuf, static_cast<size_t>(n));
    std::string out(static_cast<size_t>(n) + 1, '\0');
    va_start(ap, fmt);
    vsnprintf(out.data(), out.size(), fmt, ap);
    va_end(ap);
    out.resize(static_cast<size_t>(n));
    return out;
}

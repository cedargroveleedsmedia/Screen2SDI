#include "Log.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <share.h>
#include <vector>

namespace {
std::mutex g_mutex;
std::wstring g_dir;
FILE* g_file = nullptr;
int g_fileDay = 0;  // yyyymmdd of the open file

int DayKey(const SYSTEMTIME& st) { return st.wYear * 10000 + st.wMonth * 100 + st.wDay; }

// Deletes NDI2SDI_*.log files older than the last 7 days (today plus the 6 before it).
void PurgeOldLogs(const SYSTEMTIME& now) {
    FILETIME ft;
    SystemTimeToFileTime(&now, &ft);
    ULARGE_INTEGER t;
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    t.QuadPart -= 6ULL * 24 * 3600 * 10000000ULL;
    ft.dwLowDateTime = t.LowPart;
    ft.dwHighDateTime = t.HighPart;
    SYSTEMTIME cutoff;
    FileTimeToSystemTime(&ft, &cutoff);
    wchar_t cutoffName[64];
    swprintf_s(cutoffName, L"NDI2SDI_%04u-%02u-%02u.log", cutoff.wYear, cutoff.wMonth, cutoff.wDay);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_dir + L"\\NDI2SDI_????-??-??.log").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> old;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, cutoffName) < 0)
            old.emplace_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (const auto& name : old) DeleteFileW((g_dir + L"\\" + name).c_str());
}

void EnsureFile(const SYSTEMTIME& st) {
    int day = DayKey(st);
    if (g_file && day == g_fileDay) return;
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    if (g_dir.empty()) return;
    CreateDirectoryW(g_dir.c_str(), nullptr);
    wchar_t name[64];
    swprintf_s(name, L"\\NDI2SDI_%04u-%02u-%02u.log", st.wYear, st.wMonth, st.wDay);
    g_file = _wfsopen((g_dir + name).c_str(), L"ab", _SH_DENYNO);
    if (g_file && _fseeki64(g_file, 0, SEEK_END) == 0 && _ftelli64(g_file) == 0) fwrite("\xEF\xBB\xBF", 1, 3, g_file);  // UTF-8 BOM for Notepad
    g_fileDay = day;
    PurgeOldLogs(st);
}

thread_local char t_tag[16] = "";

void WriteV(const char* level, const char* fmt, va_list ap) {
    char body[2000];
    vsnprintf(body, sizeof(body), fmt, ap);
    char msg[2048];
    if (t_tag[0])
        snprintf(msg, sizeof(msg), "[%s] %s", t_tag, body);
    else
        snprintf(msg, sizeof(msg), "%s", body);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2200];
    int n = snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u [%-5s] [%5lu] %s\r\n", st.wYear,
                     st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level,
                     GetCurrentThreadId(), msg);
    if (n < 0) return;
    size_t len = std::min(static_cast<size_t>(n), sizeof(line) - 1);

    OutputDebugStringA(line);
    std::lock_guard<std::mutex> lock(g_mutex);
    EnsureFile(st);
    if (g_file) {
        fwrite(line, 1, len, g_file);
        fflush(g_file);
    }
}
}  // namespace

namespace Log {
void Init(const std::wstring& dir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_dir = dir;
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

void SetThreadTag(const char* tag) { strncpy_s(t_tag, tag ? tag : "", _TRUNCATE); }

void Info(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    WriteV("INFO", fmt, ap);
    va_end(ap);
}

void Warn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    WriteV("WARN", fmt, ap);
    va_end(ap);
}

void Error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    WriteV("ERROR", fmt, ap);
    va_end(ap);
}
}  // namespace Log

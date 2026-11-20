#include "NdiLib.h"

#if !SCREEN2SDI_HAS_NDI

// Display-capture-only build: NDI is never available.
namespace Ndi {
bool Load(std::string* error) {
    if (error) *error = "NDI is not included in this build (rebuild with the NDI SDK installed)";
    return false;
}
void Unload() {}
std::vector<std::string> GetSourceNames() { return {}; }
}  // namespace Ndi

#else

#include <windows.h>

#include <mutex>

#include "Log.h"
#include "Util.h"

namespace {
std::mutex g_mutex;
HMODULE g_module = nullptr;
const NDIlib_v6* g_api = nullptr;
NDIlib_find_instance_t g_finder = nullptr;

HMODULE TryLoad(const std::wstring& path) {
    HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (h) Log::Info("Loaded NDI runtime: %s", WideToUtf8(path).c_str());
    return h;
}
}  // namespace

namespace Ndi {

bool Load(std::string* error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_api) return true;

    const std::wstring dllName = Utf8ToWide(NDILIB_LIBRARY_NAME);
    HMODULE h = nullptr;
    wchar_t envDir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(Utf8ToWide(NDILIB_REDIST_FOLDER).c_str(), envDir, MAX_PATH);
    if (n > 0 && n < MAX_PATH) h = TryLoad(std::wstring(envDir) + L"\\" + dllName);
    if (!h) h = TryLoad(ExeDir() + L"\\" + dllName);
    if (!h) h = TryLoad(dllName);
    if (!h) {
        if (error) *error = "NDI runtime not found (install NDI Tools or the NDI 6 Runtime)";
        return false;
    }

    using LoadFn = const NDIlib_v6* (*)(void);
    auto load = reinterpret_cast<LoadFn>(reinterpret_cast<void*>(GetProcAddress(h, "NDIlib_v6_load")));
    if (!load) load = reinterpret_cast<LoadFn>(reinterpret_cast<void*>(GetProcAddress(h, "NDIlib_v5_load")));
    const NDIlib_v6* api = load ? load() : nullptr;
    if (!api) {
        FreeLibrary(h);
        if (error) *error = "NDI runtime is too old (need NDI 5 or 6)";
        return false;
    }
    if (!api->initialize()) {
        FreeLibrary(h);
        if (error) *error = "NDI cannot run on this CPU";
        return false;
    }
    if (api->version) Log::Info("NDI version: %s", api->version());

    NDIlib_find_create_t findSettings;
    findSettings.show_local_sources = true;
    g_finder = api->find_create_v2(&findSettings);
    if (!g_finder) Log::Warn("NDI finder could not be created; source list will be empty");

    g_module = h;
    g_api = api;
    return true;
}

const NDIlib_v6* Api() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_api;
}

void Unload() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_api) return;
    if (g_finder) g_api->find_destroy(g_finder);
    g_finder = nullptr;
    g_api->destroy();
    g_api = nullptr;
    // The module is intentionally not freed: NDI may still have threads winding down at exit.
    g_module = nullptr;
}

std::vector<std::string> GetSourceNames() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<std::string> names;
    if (!g_api || !g_finder) return names;
    uint32_t count = 0;
    const NDIlib_source_t* sources = g_api->find_get_current_sources(g_finder, &count);
    for (uint32_t i = 0; i < count; ++i)
        if (sources[i].p_ndi_name) names.emplace_back(sources[i].p_ndi_name);
    return names;
}

}  // namespace Ndi

#endif  // SCREEN2SDI_HAS_NDI

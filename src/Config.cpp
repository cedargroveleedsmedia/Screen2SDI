#include "Config.h"

#include <windows.h>

#include "Json.h"
#include "Log.h"
#include "Util.h"

namespace {
bool ReadFileUtf8(const std::wstring& path, std::string* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < 1024 * 1024;
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = size.QuadPart == 0 ||
             (ReadFile(h, out->data(), static_cast<DWORD>(size.QuadPart), &read, nullptr) && read == size.QuadPart);
    }
    CloseHandle(h);
    return ok;
}

void ReadOutput(const JsonValue& o, OutputConfig* out) {
    out->enabled = o.GetBool("enabled", out->enabled);
    out->name = o.GetString("name", out->name);
    out->sourceType = o.GetString("sourceType", "ndi") == "display" ? SourceType::Display : SourceType::Ndi;
    out->ndiSource = o.GetString("ndiSource", "");
    out->displayPath = o.GetString("displayPath", "");
    out->displayGdiName = o.GetString("displayGdiName", "");
    out->displayLabel = o.GetString("displayLabel", "");
    out->deviceName = o.GetString("deviceName", "");
    out->devicePersistentId = static_cast<int64_t>(o.GetNumber("devicePersistentId", 0));
    out->displayMode = static_cast<uint32_t>(o.GetNumber("displayMode", kDefaultDisplayMode));
    out->displayModeName = o.GetString("displayModeName", "1080p29.97");
}
}  // namespace

std::string OutputConfig::SourceDescription() const {
    if (sourceType == SourceType::Display) {
        const std::string& d = !displayLabel.empty() ? displayLabel : displayGdiName;
        return "display " + (d.empty() ? std::string("(none)") : d);
    }
    return "NDI '" + ndiSource + "'";
}

std::string DefaultOutputName(size_t i) { return "Output " + std::to_string(i + 1); }

std::wstring ConfigPath() { return AppDataDir() + L"\\config.json"; }

bool LoadConfig(Config* cfg) {
    *cfg = Config{};
    const std::wstring path = ConfigPath();
    std::string text;
    if (!ReadFileUtf8(path, &text)) return false;

    JsonValue root;
    std::string err;
    if (!JsonValue::Parse(text, &root, &err) || !root.IsObject()) {
        Log::Error("Config file %s is invalid (%s); using defaults", WideToUtf8(path).c_str(), err.c_str());
        // Keep the bad file for inspection instead of silently overwriting it later.
        std::wstring bad = path + L".bad";
        MoveFileExW(path.c_str(), bad.c_str(), MOVEFILE_REPLACE_EXISTING);
        return false;
    }

    const JsonValue* outs = root.Get("outputs");
    if (outs && outs->IsArray()) {
        for (size_t i = 0; i < outs->Size() && cfg->outputs.size() < static_cast<size_t>(kMaxOutputs); ++i) {
            if (!outs->At(i).IsObject()) continue;
            OutputConfig o;
            o.name = DefaultOutputName(cfg->outputs.size());
            ReadOutput(outs->At(i), &o);
            if (o.name.empty()) o.name = DefaultOutputName(cfg->outputs.size());
            cfg->outputs.push_back(std::move(o));
        }
        if (outs->Size() > static_cast<size_t>(kMaxOutputs))
            Log::Warn("Config has %zu outputs; only the first %d are used", outs->Size(), kMaxOutputs);
    } else {
        // Version 1.0 file: one output described by top-level fields.
        OutputConfig o;
        ReadOutput(root, &o);
        o.enabled = true;
        o.name = DefaultOutputName(0);
        cfg->outputs.push_back(std::move(o));
        Log::Info("Config: migrated single-output settings to Output 1");
    }
    cfg->autoStartOutput = root.GetBool("autoStartOutput", true);
    int preroll = static_cast<int>(root.GetNumber("prerollFrames", 2));
    cfg->prerollFrames = preroll < 2 ? 2 : (preroll > 5 ? 5 : preroll);
    return true;
}

bool SaveConfig(const Config& cfg) {
    JsonValue root = JsonValue::MakeObject();
    root.Set("autoStartOutput", JsonValue::MakeBool(cfg.autoStartOutput));
    root.Set("prerollFrames", JsonValue::MakeNumber(cfg.prerollFrames));
    JsonValue outs = JsonValue::MakeArray();
    for (const OutputConfig& o : cfg.outputs) {
        JsonValue v = JsonValue::MakeObject();
        v.Set("enabled", JsonValue::MakeBool(o.enabled));
        v.Set("name", JsonValue::MakeString(o.name));
        v.Set("sourceType", JsonValue::MakeString(o.sourceType == SourceType::Display ? "display" : "ndi"));
        v.Set("ndiSource", JsonValue::MakeString(o.ndiSource));
        v.Set("displayPath", JsonValue::MakeString(o.displayPath));
        v.Set("displayGdiName", JsonValue::MakeString(o.displayGdiName));
        v.Set("displayLabel", JsonValue::MakeString(o.displayLabel));
        v.Set("deviceName", JsonValue::MakeString(o.deviceName));
        v.Set("devicePersistentId", JsonValue::MakeNumber(static_cast<double>(o.devicePersistentId)));
        v.Set("displayMode", JsonValue::MakeNumber(o.displayMode));
        v.Set("displayModeName", JsonValue::MakeString(o.displayModeName));
        outs.Push(std::move(v));
    }
    root.Set("outputs", std::move(outs));
    std::string text = root.Serialize(2) + "\n";

    // Write to a temp file, then atomically replace, so a power cut never leaves a half-written config.
    const std::wstring path = ConfigPath();
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log::Error("Cannot write %s (error %lu)", WideToUtf8(tmp).c_str(), GetLastError());
        return false;
    }
    DWORD written = 0;
    bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) {
        Log::Error("Saving config failed (error %lu)", GetLastError());
        DeleteFileW(tmp.c_str());
        return false;
    }
    Log::Info("Config saved: buffer=%d frames autoStart=%d", cfg.prerollFrames, cfg.autoStartOutput ? 1 : 0);
    for (size_t i = 0; i < cfg.outputs.size(); ++i) {
        const OutputConfig& o = cfg.outputs[i];
        Log::Info("  Output %zu '%s': %s, %s -> '%s' (id %lld) @ %s", i + 1, o.name.c_str(), o.enabled ? "on" : "off",
                  o.SourceDescription().c_str(), o.deviceName.c_str(), static_cast<long long>(o.devicePersistentId),
                  o.displayModeName.c_str());
    }
    return true;
}

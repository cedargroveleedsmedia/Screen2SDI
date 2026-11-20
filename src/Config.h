#pragma once
#include <array>
#include <cstdint>
#include <string>

// FourCC of bmdModeHD1080p2997 ('Hp29').
constexpr uint32_t kDefaultDisplayMode = 0x48703239;
constexpr int kMaxOutputs = 3;

enum class SourceType { Ndi, Display };

// One SDI output: where the picture comes from and which DeckLink connector/format it goes to.
struct OutputConfig {
    bool enabled = false;
    std::string name;               // shown in Settings, tooltip and log, e.g. "Main"
    SourceType sourceType = SourceType::Ndi;
    std::string ndiSource;          // e.g. "STUDIO-PC (Presenter)"; may be offline
    // Display capture: the monitor's device path is stable across reboots; the GDI name
    // (\\.\DISPLAYn) is a fallback; the label is only for showing a missing display.
    std::string displayPath;
    std::string displayGdiName;
    std::string displayLabel;
    std::string deviceName;         // e.g. "DeckLink Duo (1)"; empty = first output-capable device
    int64_t devicePersistentId = 0; // 0 = unknown; preferred over name when available
    uint32_t displayMode = kDefaultDisplayMode;
    std::string displayModeName = "1080p29.97";

    bool operator==(const OutputConfig& o) const {
        return enabled == o.enabled && name == o.name && sourceType == o.sourceType && ndiSource == o.ndiSource &&
               displayPath == o.displayPath && displayGdiName == o.displayGdiName && deviceName == o.deviceName &&
               devicePersistentId == o.devicePersistentId && displayMode == o.displayMode;
    }
    bool operator!=(const OutputConfig& o) const { return !(*this == o); }

    // Human-readable source, e.g. "NDI 'PC (Presenter)'" or "display Virtual 1 (1920x1080)".
    std::string SourceDescription() const;
};

struct Config {
    std::array<OutputConfig, kMaxOutputs> outputs;
    bool autoStartOutput = true;
    int prerollFrames = 2;          // DeckLink output queue depth (2..5); lower = less latency

    Config();
};

std::wstring ConfigPath();
// Returns true if a config file existed and was read. On a missing or corrupt file, *cfg gets defaults.
// A single-output config from version 1.0 is migrated into outputs[0].
bool LoadConfig(Config* cfg);
bool SaveConfig(const Config& cfg);

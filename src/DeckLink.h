#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "VideoSource.h"

// COM must be initialised on the calling thread for everything in this header.

struct DeckLinkModeInfo {
    uint32_t id = 0;          // BMDDisplayMode FourCC
    std::string name;         // e.g. "1080p29.97"
    int width = 0, height = 0;
    int64_t frameDuration = 0, timeScale = 0;
    bool interlaced = false;
};

struct DeckLinkDeviceInfo {
    std::string name;         // e.g. "DeckLink Duo (1)"
    int64_t persistentId = 0;
    std::vector<DeckLinkModeInfo> modes;  // modes supported for 8-bit YUV output
};

// Lists devices/sub-devices that can play out, with their supported output modes.
std::vector<DeckLinkDeviceInfo> EnumerateDeckLinkOutputs(std::string* error);

struct DeckLinkSelection {
    std::string deviceName;      // empty + persistentId 0 = first output-capable device
    int64_t persistentId = 0;
    uint32_t displayMode = 0;
    int prerollFrames = 2;       // frames queued ahead of the one on air (1..5)
};

// Time from filling a frame to it going on air, measured per frame and summarised.
struct LatencySummary {
    uint64_t count = 0;
    double avgMs = 0, minMs = 0, maxMs = 0;
};

// Continuous scheduled playback. The DeckLink clock drives everything: each completed frame is
// refilled from the IVideoSource and rescheduled, keeping ~3 frames queued.
class DeckLinkOutput {
public:
    DeckLinkOutput();
    ~DeckLinkOutput();
    DeckLinkOutput(const DeckLinkOutput&) = delete;
    DeckLinkOutput& operator=(const DeckLinkOutput&) = delete;

    // source must outlive the running output. Returns false with a readable reason on failure.
    bool Start(const DeckLinkSelection& sel, IVideoSource* source, std::string* error);
    void Stop();
    bool IsRunning() const;

    struct Stats {
        uint64_t completed = 0, late = 0, dropped = 0, flushed = 0, resyncs = 0, scheduleErrors = 0;
        uint64_t liveFrames = 0;          // frames filled with NDI video (rest were black)
        uint32_t buffered = 0;
        uint64_t lastCallbackMs = 0;      // NowMs() of the last completion callback
        bool playbackStopped = false;     // the driver stopped playback on its own
    };
    Stats GetStats();
    // Queue latency (fill -> scheduled on-air time) since the last call; resets the summary.
    LatencySummary TakeQueueLatency();
    std::string DeviceName() const;
    std::string ModeName() const;

    class Impl;

private:
    Impl* m_impl;  // COM object; lifetime managed manually (see DeckLink.cpp)
};

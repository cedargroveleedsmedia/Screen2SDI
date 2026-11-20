#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "FrameConvert.h"
#include "VideoSource.h"

struct NDIlib_recv_instance_type;
struct NDIlib_framesync_instance_type;

// Connects to an NDI source by name and keeps trying forever. Frames are pulled on demand
// through the NDI frame-sync API from the DeckLink output thread (see Render()).
class NdiReceiver : public IVideoSource {
public:
    NdiReceiver();
    ~NdiReceiver() override;

    // Starts the worker thread (idempotent). tag prefixes the worker's log lines.
    void Start(const std::string& tag = std::string());
    // Stops the worker thread and destroys all NDI objects.
    void Stop();
    // Change the source; "" disconnects. Takes effect asynchronously.
    void SetSource(const std::string& name);

    // IVideoSource: called from the DeckLink callback thread for every output frame.
    bool Render(uint8_t* dst, int rowBytes, int width, int height, bool interlaced) override;

    struct Status {
        bool runtimeLoaded = false;
        std::string runtimeError;
        std::string source;
        bool connected = false;
        bool hasVideo = false;   // connected and at least one frame received
        int width = 0, height = 0;
        int frameRateN = 0, frameRateD = 0;
        int64_t framesReceived = 0;
        int64_t framesDropped = 0;
    };
    Status GetStatus();

    // Age of each *new* NDI frame (sender submit -> picked up for a DeckLink frame) since the last
    // call; resets the summary. Only meaningful when sender and receiver clocks agree (same PC or NTP).
    struct Age {
        uint64_t count = 0;
        double avgMs = 0, minMs = 0, maxMs = 0;
    };
    Age TakeFrameAge();

private:
    void ThreadMain();
    void CreateReceiver(const std::string& name);
    void DestroyReceiver();

    std::thread m_thread;
    std::string m_tag;
    std::mutex m_ctlMutex;            // guards m_wantedSource, m_stop
    std::condition_variable m_cv;
    std::string m_wantedSource;
    bool m_stop = false;

    // Guards m_recv/m_fs swaps vs. Render(). Held by Render() for one frame's conversion only.
    std::mutex m_fsMutex;
    NDIlib_recv_instance_type* m_recv = nullptr;
    NDIlib_framesync_instance_type* m_fs = nullptr;

    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_gotFrame{false};
    std::atomic<int> m_lastW{0}, m_lastH{0}, m_lastFrN{0}, m_lastFrD{0};
    std::atomic<int> m_unsupportedFourCC{0};

    std::mutex m_statusMutex;
    Status m_status;

    FrameRenderer m_renderer;  // only touched from Render()
    int64_t m_lastTimestamp = 0;  // only touched from Render()

    std::mutex m_ageMutex;
    uint64_t m_ageCount = 0;
    double m_ageSum = 0, m_ageMin = 0, m_ageMax = 0;
};

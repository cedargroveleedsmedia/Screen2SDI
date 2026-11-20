#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "FrameConvert.h"
#include "VideoSource.h"

// Captures one Windows display with DXGI Desktop Duplication on a worker thread. The newest frame
// is handed to the DeckLink callback thread through a lock-free-ish triple buffer, so Render()
// never waits on the GPU. The mouse pointer is not included.
class DisplayCapture : public IVideoSource {
public:
    DisplayCapture() = default;
    ~DisplayCapture() override;

    // Starts the worker thread (idempotent). tag prefixes the worker's log lines.
    void Start(const std::string& tag);
    void Stop();
    // Change the display; empty path and name = capture nothing. Takes effect asynchronously.
    void SetDisplay(const std::string& devicePath, const std::string& gdiName, const std::string& label);

    // IVideoSource: called from the DeckLink callback thread for every output frame.
    bool Render(uint8_t* dst, int rowBytes, int width, int height, bool interlaced) override;

    struct Status {
        std::string label;       // configured display
        bool found = false;      // the display currently exists
        bool capturing = false;  // duplication is running
        bool hasVideo = false;   // a frame is available to output
        int width = 0, height = 0, refreshHz = 0;
        int64_t framesCaptured = 0;
        std::string error;
    };
    Status GetStatus();

    // Age of each new frame (desktop present -> picked up for a DeckLink frame) since the last call.
    struct Age {
        uint64_t count = 0;
        double avgMs = 0, minMs = 0, maxMs = 0;
    };
    Age TakeFrameAge();

private:
    struct Buffer {
        std::vector<uint8_t> data;
        int width = 0, height = 0, stride = 0;
        int64_t presentQpc = 0;
    };

    void ThreadMain(std::string tag);

    std::thread m_thread;
    std::mutex m_ctlMutex;  // guards the wanted display and m_stop
    std::condition_variable m_cv;
    std::string m_wantPath, m_wantGdi, m_wantLabel;
    unsigned m_wantVersion = 0;
    bool m_stop = false;

    // Triple buffer: the capture thread owns m_buf[m_back], Render() owns m_buf[m_front],
    // m_buf[m_ready] is the newest complete frame. Indices are swapped under m_bufMutex only.
    std::mutex m_bufMutex;
    Buffer m_buf[3];
    int m_back = 0, m_ready = 1, m_front = 2;
    bool m_readyIsNew = false;
    std::atomic<bool> m_valid{false};  // false => Render() outputs black (no display / no frame yet)

    FrameRenderer m_renderer;  // only touched from Render()
    // Last converted output (UYVY), reused while the desktop is unchanged. Render() only.
    std::vector<uint8_t> m_cache;
    int m_cacheRowBytes = 0, m_cacheW = 0, m_cacheH = 0;

    std::mutex m_statusMutex;
    Status m_status;

    std::mutex m_ageMutex;
    uint64_t m_ageCount = 0;
    double m_ageSum = 0, m_ageMin = 0, m_ageMax = 0;
};

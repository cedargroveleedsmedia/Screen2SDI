#pragma once
#include <windows.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "Config.h"

enum class PipelineState {
    Disabled,       // output switched off in Settings (ignored for the tray colour)
    NotConfigured,  // red
    Stopped,        // red (stopped by the user)
    Error,          // red (DeckLink problem; retrying)
    Waiting,        // yellow (outputting black, waiting for the source)
    Live,           // green
};

struct PipelineStatus {
    PipelineState state = PipelineState::Stopped;
    std::string text;  // UTF-8, suitable for tooltip and status line (without the output name)
};

// One SDI output: owns its source (NDI receiver or display capture) and DeckLink output on a
// dedicated control thread. All public methods are non-blocking (except Shutdown) and safe to call
// from the UI thread.
class Pipeline {
public:
    explicit Pipeline(int index) : m_index(index) {}
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // Starts the control thread. statusMsg is posted to notifyWnd whenever the status changes.
    void Start(HWND notifyWnd, UINT statusMsg);
    // Stops output, releases everything and joins the thread. Safe to call more than once.
    void Shutdown();

    void ApplyConfig(const OutputConfig& cfg, int prerollFrames, bool configured);
    void SetRunEnabled(bool run);
    PipelineStatus GetStatus();

private:
    void ThreadMain();

    const int m_index;  // 0-based
    HWND m_notifyWnd = nullptr;
    UINT m_statusMsg = 0;
    std::thread m_thread;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_shutdown = false;
    bool m_dirty = false;
    OutputConfig m_cfg;
    int m_preroll = 2;
    bool m_configured = false;
    bool m_run = false;
    unsigned m_cfgVersion = 0;
    PipelineStatus m_status;
};

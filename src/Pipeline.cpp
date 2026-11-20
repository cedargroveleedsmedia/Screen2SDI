#include "Pipeline.h"

#include <objbase.h>

#include <algorithm>

#include "DeckLink.h"
#include "DisplayCapture.h"
#include "Log.h"
#include "NdiReceiver.h"
#include "Util.h"

namespace {
constexpr uint64_t kWatchdogMs = 3000;      // no frame callbacks for this long => restart output
constexpr uint64_t kMinRetryMs = 1000;
constexpr uint64_t kMaxRetryMs = 10000;
constexpr uint64_t kStatsIntervalMs = 60000;
const char* const kArrow = "\xE2\x86\x92";  // UTF-8 "→"

const char* StateName(PipelineState s) {
    switch (s) {
        case PipelineState::Disabled: return "off";
        case PipelineState::NotConfigured: return "not configured";
        case PipelineState::Stopped: return "stopped";
        case PipelineState::Error: return "error";
        case PipelineState::Waiting: return "waiting";
        case PipelineState::Live: return "live";
    }
    return "?";
}
}  // namespace

Pipeline::~Pipeline() { Shutdown(); }

void Pipeline::Start(HWND notifyWnd, UINT statusMsg) {
    m_notifyWnd = notifyWnd;
    m_statusMsg = statusMsg;
    if (!m_thread.joinable()) m_thread = std::thread(&Pipeline::ThreadMain, this);
}

void Pipeline::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shutdown = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void Pipeline::ApplyConfig(const OutputConfig& cfg, int prerollFrames, bool configured) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Only restart the output when something that matters to it changed.
        if (cfg == m_cfg && prerollFrames == m_preroll && configured == m_configured && m_cfgVersion != 0) return;
        m_cfg = cfg;
        m_preroll = prerollFrames;
        m_configured = configured;
        ++m_cfgVersion;
        m_dirty = true;
    }
    m_cv.notify_all();
}

void Pipeline::SetRunEnabled(bool run) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_run = run;
        m_dirty = true;
    }
    m_cv.notify_all();
}

PipelineStatus Pipeline::GetStatus() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

void Pipeline::ThreadMain() {
    const std::string tag = Format("Out%d", m_index + 1);
    Log::SetThreadTag(tag.c_str());
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Log::Info("Pipeline thread started");
    {
        NdiReceiver receiver;
        receiver.Start(tag);
        DisplayCapture capture;
        capture.Start(tag);
        DeckLinkOutput output;

        unsigned appliedVersion = 0;
        OutputConfig cfg;
        int preroll = 2;
        bool configured = false, run = false;
        uint64_t nextStartMs = 0, retryDelay = kMinRetryMs;
        std::string lastError, lastLoggedError;
        uint64_t lastStatsMs = NowMs();
        DeckLinkOutput::Stats prevStats;
        PipelineState lastState = PipelineState::Stopped;
        std::string lastText;
        bool firstStatus = true;

        for (;;) {
            unsigned version;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait_for(lock, std::chrono::milliseconds(250), [&] { return m_shutdown || m_dirty; });
                if (m_shutdown) break;
                m_dirty = false;
                version = m_cfgVersion;
                if (version != appliedVersion) {
                    cfg = m_cfg;
                    preroll = m_preroll;
                    configured = m_configured;
                }
                run = m_run;
            }
            const uint64_t now = NowMs();
            const bool useDisplay = cfg.sourceType == SourceType::Display;

            if (version != appliedVersion) {
                appliedVersion = version;
                if (output.IsRunning()) output.Stop();
                nextStartMs = 0;
                retryDelay = kMinRetryMs;
                lastError.clear();
                lastLoggedError.clear();
                prevStats = {};
                if (configured && cfg.enabled)
                    Log::Info("Applying config for '%s': %s %s '%s' @ %s, buffer %d", cfg.name.c_str(),
                              cfg.SourceDescription().c_str(), kArrow,
                              cfg.deviceName.empty() ? "(first available)" : cfg.deviceName.c_str(),
                              cfg.displayModeName.c_str(), preroll);
            }

            const bool wantOutput = run && configured && cfg.enabled;
            receiver.SetSource(wantOutput && !useDisplay ? cfg.ndiSource : std::string());
            if (wantOutput && useDisplay)
                capture.SetDisplay(cfg.displayPath, cfg.displayGdiName, cfg.displayLabel);
            else
                capture.SetDisplay(std::string(), std::string(), std::string());
            IVideoSource* source = useDisplay ? static_cast<IVideoSource*>(&capture) : &receiver;

            if (!wantOutput) {
                if (output.IsRunning()) output.Stop();
            } else if (!output.IsRunning()) {
                if (now >= nextStartMs) {
                    DeckLinkSelection sel;
                    sel.deviceName = cfg.deviceName;
                    sel.persistentId = cfg.devicePersistentId;
                    sel.displayMode = cfg.displayMode;
                    sel.prerollFrames = preroll;
                    std::string err;
                    if (output.Start(sel, source, &err)) {
                        lastError.clear();
                        lastLoggedError.clear();
                        retryDelay = kMinRetryMs;
                        prevStats = {};
                        lastStatsMs = now - (kStatsIntervalMs - 15000);  // first report after 15 s
                        receiver.TakeFrameAge();                         // discard pre-start samples
                        capture.TakeFrameAge();
                    } else {
                        lastError = err;
                        if (err != lastLoggedError) {
                            Log::Error("DeckLink: %s; retrying", err.c_str());
                            lastLoggedError = err;
                        }
                        nextStartMs = now + retryDelay;
                        retryDelay = std::min(retryDelay * 2, kMaxRetryMs);
                    }
                }
            } else {
                // Watchdog: the DeckLink callback must keep ticking.
                DeckLinkOutput::Stats st = output.GetStats();
                if (st.playbackStopped || now - st.lastCallbackMs > kWatchdogMs) {
                    lastError = st.playbackStopped ? "Playback stopped by the driver - restarting"
                                                   : "Output stalled - restarting";
                    Log::Error("DeckLink: %s (late=%llu dropped=%llu buffered=%u)", lastError.c_str(), st.late,
                               st.dropped, st.buffered);
                    output.Stop();
                    nextStartMs = now + kMinRetryMs;
                } else if (now - lastStatsMs >= kStatsIntervalMs) {
                    std::string src;
                    double ageAvg = 0, ageMin = 0, ageMax = 0;
                    uint64_t ageCount = 0;
                    if (useDisplay) {
                        DisplayCapture::Status cs = capture.GetStatus();
                        src = Format("Display %s %dx%d @ %d Hz captured=%lld", cs.hasVideo ? "live" : "waiting",
                                     cs.width, cs.height, cs.refreshHz, static_cast<long long>(cs.framesCaptured));
                        DisplayCapture::Age a = capture.TakeFrameAge();
                        ageAvg = a.avgMs, ageMin = a.minMs, ageMax = a.maxMs, ageCount = a.count;
                    } else {
                        NdiReceiver::Status rs = receiver.GetStatus();
                        src = Format("NDI %s %dx%d @ %d/%d recv=%lld dropped=%lld",
                                     rs.hasVideo ? "live" : (rs.connected ? "connected" : "waiting"), rs.width,
                                     rs.height, rs.frameRateN, rs.frameRateD, static_cast<long long>(rs.framesReceived),
                                     static_cast<long long>(rs.framesDropped));
                        NdiReceiver::Age a = receiver.TakeFrameAge();
                        ageAvg = a.avgMs, ageMin = a.minMs, ageMax = a.maxMs, ageCount = a.count;
                    }
                    Log::Info("Stats/min: out frames=%llu live=%llu late=%llu dropped=%llu resyncs=%llu "
                              "schedErr=%llu buffered=%u | %s",
                              st.completed - prevStats.completed, st.liveFrames - prevStats.liveFrames,
                              st.late - prevStats.late, st.dropped - prevStats.dropped,
                              st.resyncs - prevStats.resyncs, st.scheduleErrors - prevStats.scheduleErrors,
                              st.buffered, src.c_str());
                    LatencySummary q = output.TakeQueueLatency();
                    if (ageCount && q.count) {
                        Log::Info("Latency: %s frame age avg %.0f ms (min %.0f, max %.0f, %llu new frames) + output "
                                  "queue avg %.0f ms (max %.0f) = ~%.0f ms from source to SDI, plus the card's "
                                  "own frame and the display",
                                  useDisplay ? "display" : "NDI", ageAvg, ageMin, ageMax, ageCount, q.avgMs, q.maxMs,
                                  ageAvg + q.avgMs);
                    } else if (q.count) {
                        Log::Info("Latency: output queue avg %.0f ms (max %.0f); no new source frames", q.avgMs,
                                  q.maxMs);
                    }
                    prevStats = st;
                    lastStatsMs = now;
                }
            }

            // Status
            PipelineStatus status;
            if (!configured) {
                status = {PipelineState::NotConfigured, "Not configured - open Settings"};
            } else if (!cfg.enabled) {
                status = {PipelineState::Disabled, "Off"};
            } else if (!run) {
                status = {PipelineState::Stopped, "Output stopped"};
            } else if (!output.IsRunning()) {
                status = {PipelineState::Error, "Error: " + (lastError.empty() ? std::string("starting output") : lastError)};
            } else {
                std::string target = std::string(" ") + kArrow + " " + output.DeviceName() + " @ " + output.ModeName();
                if (useDisplay) {
                    DisplayCapture::Status cs = capture.GetStatus();
                    const std::string what = cfg.displayLabel.empty() ? cfg.displayGdiName : cfg.displayLabel;
                    if (cfg.displayPath.empty() && cfg.displayGdiName.empty())
                        status = {PipelineState::Waiting, "Black (no display selected)" + target};
                    else if (cs.hasVideo)
                        status = {PipelineState::Live, "Live: " + what + target};
                    else if (!cs.error.empty())
                        status = {PipelineState::Waiting, "Black (" + cs.error + ")" + target};
                    else
                        status = {PipelineState::Waiting, "Waiting for " + what + " (black)" + target};
                } else {
                    NdiReceiver::Status rs = receiver.GetStatus();
                    if (rs.hasVideo)
                        status = {PipelineState::Live, "Live: " + cfg.ndiSource + target};
                    else if (!rs.runtimeLoaded && !rs.runtimeError.empty())
                        status = {PipelineState::Waiting, "Black (" + rs.runtimeError + ")" + target};
                    else if (cfg.ndiSource.empty())
                        status = {PipelineState::Waiting, "Black (no NDI source selected)" + target};
                    else
                        status = {PipelineState::Waiting, "Waiting for " + cfg.ndiSource + " (black)" + target};
                }
            }
            if (firstStatus || status.state != lastState || status.text != lastText) {
                if (firstStatus || status.state != lastState)
                    Log::Info("Status '%s': %s - %s", cfg.name.c_str(), StateName(status.state), status.text.c_str());
                firstStatus = false;
                lastState = status.state;
                lastText = status.text;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_status = status;
                }
                if (m_notifyWnd) PostMessageW(m_notifyWnd, m_statusMsg, 0, 0);
            }
        }

        output.Stop();  // must stop before the sources it pulls frames from
        capture.Stop();
        receiver.Stop();
    }
    Log::Info("Pipeline thread stopped");
    if (SUCCEEDED(coHr)) CoUninitialize();
}

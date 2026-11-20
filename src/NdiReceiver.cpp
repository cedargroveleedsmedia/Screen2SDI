#include "NdiReceiver.h"

#include <windows.h>

#include <algorithm>

#include "Log.h"
#include "NdiLib.h"
#include "Util.h"

namespace {
constexpr uint64_t kLoadRetryMs = 10000;      // retry loading a missing NDI runtime
constexpr uint64_t kRecreateAfterMs = 30000;  // rebuild the receiver if disconnected this long
constexpr int kPollMs = 250;

std::atomic<const NDIlib_v6*> g_api{nullptr};

bool MapFourCC(NDIlib_FourCC_video_type_e f, SrcFormat* out) {
    switch (f) {
        case NDIlib_FourCC_video_type_UYVY:
        case NDIlib_FourCC_video_type_UYVA:  // UYVY plane first; alpha plane after it is ignored
            *out = SrcFormat::UYVY;
            return true;
        case NDIlib_FourCC_video_type_BGRA: *out = SrcFormat::BGRA; return true;
        case NDIlib_FourCC_video_type_BGRX: *out = SrcFormat::BGRX; return true;
        case NDIlib_FourCC_video_type_RGBA: *out = SrcFormat::RGBA; return true;
        case NDIlib_FourCC_video_type_RGBX: *out = SrcFormat::RGBX; return true;
        default: return false;
    }
}
}  // namespace

NdiReceiver::NdiReceiver() = default;

NdiReceiver::~NdiReceiver() { Stop(); }

void NdiReceiver::Start(const std::string& tag) {
    std::lock_guard<std::mutex> lock(m_ctlMutex);
    if (m_thread.joinable()) return;
    m_tag = tag;
    m_stop = false;
    m_thread = std::thread(&NdiReceiver::ThreadMain, this);
}

void NdiReceiver::Stop() {
    {
        std::lock_guard<std::mutex> lock(m_ctlMutex);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void NdiReceiver::SetSource(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(m_ctlMutex);
        m_wantedSource = name;
    }
    m_cv.notify_all();
}

NdiReceiver::Status NdiReceiver::GetStatus() {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    Status s = m_status;
    s.connected = m_connected.load();
    s.hasVideo = s.connected && m_gotFrame.load();
    s.width = m_lastW.load();
    s.height = m_lastH.load();
    s.frameRateN = m_lastFrN.load();
    s.frameRateD = m_lastFrD.load();
    return s;
}

NdiReceiver::Age NdiReceiver::TakeFrameAge() {
    std::lock_guard<std::mutex> lock(m_ageMutex);
    Age a;
    a.count = m_ageCount;
    if (m_ageCount) {
        a.avgMs = m_ageSum / static_cast<double>(m_ageCount);
        a.minMs = m_ageMin;
        a.maxMs = m_ageMax;
    }
    m_ageCount = 0;
    m_ageSum = m_ageMin = m_ageMax = 0;
    return a;
}

void NdiReceiver::CreateReceiver(const std::string& name) {
    const NDIlib_v6* api = g_api.load();
    if (!api || name.empty()) return;

    NDIlib_source_t src;
    src.p_ndi_name = name.c_str();
    src.p_url_address = nullptr;  // NDI resolves the name itself and reconnects when it reappears

    NDIlib_recv_create_v3_t settings;
    settings.source_to_connect_to = src;
    settings.color_format = NDIlib_recv_color_format_UYVY_BGRA;
    settings.bandwidth = NDIlib_recv_bandwidth_highest;
    settings.allow_video_fields = true;
    settings.p_ndi_recv_name = "Screen2SDI";

    NDIlib_recv_instance_t recv = api->recv_create_v3(&settings);
    if (!recv) {
        Log::Error("NDI receiver creation failed for '%s'", name.c_str());
        return;
    }
    NDIlib_framesync_instance_t fs = api->framesync_create(recv);
    if (!fs) {
        Log::Error("NDI frame-sync creation failed for '%s'", name.c_str());
        api->recv_destroy(recv);
        return;
    }
    std::lock_guard<std::mutex> lock(m_fsMutex);
    m_recv = recv;
    m_fs = fs;
}

void NdiReceiver::DestroyReceiver() {
    NDIlib_recv_instance_t recv;
    NDIlib_framesync_instance_t fs;
    {
        std::lock_guard<std::mutex> lock(m_fsMutex);  // waits for an in-flight Render()
        recv = m_recv;
        fs = m_fs;
        m_recv = nullptr;
        m_fs = nullptr;
    }
    m_connected = false;
    m_gotFrame = false;
    const NDIlib_v6* api = g_api.load();
    if (!api) return;
    if (fs) api->framesync_destroy(fs);
    if (recv) api->recv_destroy(recv);
}

void NdiReceiver::ThreadMain() {
    Log::SetThreadTag(m_tag.c_str());
    Log::Info("NDI receiver thread started");
    std::string current;
    std::string lastRuntimeError;
    uint64_t lastLoadAttempt = 0;
    bool loadAttempted = false;
    uint64_t lastAliveMs = 0;  // last time connected, or when the receiver was (re)created
    bool wasConnected = false;
    int loggedFourCC = 0;

    for (;;) {
        std::string wanted;
        {
            std::unique_lock<std::mutex> lock(m_ctlMutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(kPollMs), [&] { return m_stop || m_wantedSource != current; });
            if (m_stop) break;
            wanted = m_wantedSource;
        }
        const uint64_t now = NowMs();

        // Load the runtime (retrying if it is missing, e.g. installed later).
        if (!g_api.load() && (!loadAttempted || now - lastLoadAttempt >= kLoadRetryMs)) {
            loadAttempted = true;
            lastLoadAttempt = now;
            std::string err;
            if (Ndi::Load(&err)) {
                g_api = Ndi::Api();
                err.clear();
            }
            if (err != lastRuntimeError) {
                if (!err.empty()) Log::Error("%s; retrying every %llu s", err.c_str(), kLoadRetryMs / 1000);
                lastRuntimeError = err;
            }
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_status.runtimeLoaded = g_api.load() != nullptr;
            m_status.runtimeError = err;
        }
        const NDIlib_v6* api = g_api.load();

        if (wanted != current) {
            if (m_recv) {
                DestroyReceiver();
                if (wasConnected) Log::Info("NDI: disconnected from '%s' (source changed)", current.c_str());
                wasConnected = false;
            }
            current = wanted;
            {
                std::lock_guard<std::mutex> lock(m_statusMutex);
                m_status.source = current;
            }
            if (!current.empty()) Log::Info("NDI: looking for source '%s'", current.c_str());
        }

        if (!api || current.empty()) continue;

        if (!m_recv) {
            CreateReceiver(current);
            lastAliveMs = now;
            if (!m_recv) continue;
        }

        const bool connected = api->recv_get_no_connections(m_recv) > 0;
        m_connected = connected;
        if (connected != wasConnected) {
            if (connected) {
                Log::Info("NDI: connected to '%s'", current.c_str());
            } else {
                Log::Warn("NDI: lost connection to '%s'; outputting black and retrying", current.c_str());
                m_gotFrame = false;
            }
            wasConnected = connected;
        }

        if (connected) {
            lastAliveMs = now;
        } else if (now - lastAliveMs >= kRecreateAfterMs) {
            // NDI reconnects by name on its own; rebuilding periodically also covers network changes
            // (new IP, adapter reset) where a stale receiver can get stuck.
            DestroyReceiver();
            CreateReceiver(current);
            lastAliveMs = now;
        }

        if (m_recv) {
            NDIlib_recv_performance_t total, dropped;
            api->recv_get_performance(m_recv, &total, &dropped);
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_status.framesReceived = total.video_frames;
            m_status.framesDropped = dropped.video_frames;
        }

        const int fourcc = m_unsupportedFourCC.exchange(0);
        if (fourcc && fourcc != loggedFourCC) {
            loggedFourCC = fourcc;
            Log::Warn("NDI: unsupported pixel format %c%c%c%c from '%s'; outputting black", fourcc & 0xFF,
                      (fourcc >> 8) & 0xFF, (fourcc >> 16) & 0xFF, (fourcc >> 24) & 0xFF, current.c_str());
        }
    }

    DestroyReceiver();
    Log::Info("NDI receiver thread stopped");
}

bool NdiReceiver::Render(uint8_t* dst, int rowBytes, int width, int height, bool interlaced) {
    const NDIlib_v6* api = g_api.load();
    std::lock_guard<std::mutex> lock(m_fsMutex);
    if (!api || !m_fs || !m_connected.load()) {
        FillBlackUYVY(dst, rowBytes, width, height);
        return false;
    }

    NDIlib_video_frame_v2_t vf;
    api->framesync_capture_video(m_fs, &vf,
                                 interlaced ? NDIlib_frame_format_type_interleaved : NDIlib_frame_format_type_progressive);
    if (!vf.p_data) {
        FillBlackUYVY(dst, rowBytes, width, height);
        return false;
    }

    SrcFormat fmt;
    bool ok = MapFourCC(vf.FourCC, &fmt);
    if (ok) {
        SrcFrame src;
        src.data = vf.p_data;
        src.width = vf.xres;
        src.height = vf.yres;
        src.format = fmt;
        src.stride = vf.line_stride_in_bytes > 0 ? vf.line_stride_in_bytes
                                                 : vf.xres * (fmt == SrcFormat::UYVY ? 2 : 4);
        src.aspect = vf.picture_aspect_ratio;
        // Latency probe: NDI timestamps are 100 ns units since the Unix epoch, stamped when the sender
        // submitted the frame. Record the age of each frame the first time we use it.
        if (vf.timestamp != NDIlib_recv_timestamp_undefined && vf.timestamp != m_lastTimestamp) {
            m_lastTimestamp = vf.timestamp;
            FILETIME ft;
            GetSystemTimePreciseAsFileTime(&ft);
            const int64_t now = ((static_cast<int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) - 116444736000000000LL;
            const double ageMs = static_cast<double>(now - vf.timestamp) / 10000.0;
            if (ageMs >= 0 && ageMs < 10000) {  // ignore clock skew with a remote sender
                std::lock_guard<std::mutex> ageLock(m_ageMutex);
                if (m_ageCount == 0 || ageMs < m_ageMin) m_ageMin = ageMs;
                if (m_ageCount == 0 || ageMs > m_ageMax) m_ageMax = ageMs;
                m_ageSum += ageMs;
                ++m_ageCount;
            }
        }
        m_renderer.Render(src, dst, rowBytes, width, height);
        m_lastW = vf.xres;
        m_lastH = vf.yres;
        m_lastFrN = vf.frame_rate_N;
        m_lastFrD = vf.frame_rate_D;
        if (!m_gotFrame.exchange(true)) {
            // First frame after (re)connecting; logged from here once per connection.
            Log::Info("NDI: receiving %dx%d @ %d/%d", vf.xres, vf.yres, vf.frame_rate_N, vf.frame_rate_D);
        }
    } else {
        m_unsupportedFourCC = static_cast<int>(vf.FourCC);
        FillBlackUYVY(dst, rowBytes, width, height);
    }
    api->framesync_free_video(m_fs, &vf);
    return ok;
}

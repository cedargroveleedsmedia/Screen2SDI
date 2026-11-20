// All DeckLink COM code lives in this one translation unit so that the type-library #import
// (which writes DeckLinkAPI64.tlh into the build directory) runs exactly once per build.
#include "DeckLink.h"

#include <windows.h>

#include <atomic>
#include <mutex>

#pragma warning(push, 0)
#pragma warning(disable : 4278 4192)
// Type library registered by Blackmagic Desktop Video ("DeckLink API Library").
#import "libid:D864517A-EDD5-466D-867D-C819F1C052BB" raw_interfaces_only no_namespace named_guids no_smart_pointers
#pragma warning(pop)

#include "Log.h"
#include "Util.h"

namespace {

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::string BstrToUtf8(BSTR b) {
    if (!b) return {};
    std::string s = WideToUtf8(std::wstring(b, SysStringLen(b)));
    SysFreeString(b);
    return s;
}

std::string HrText(HRESULT hr) { return Format("0x%08lX", static_cast<unsigned long>(hr)); }

struct RawDevice {
    IDeckLink* deckLink = nullptr;
    std::string name;
    int64_t persistentId = 0;
};

// Returns all devices that support playback (each AddRef'd; caller releases).
std::vector<RawDevice> ListOutputDevices(std::string* error) {
    std::vector<RawDevice> out;
    IDeckLinkIterator* it = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL, IID_IDeckLinkIterator,
                                  reinterpret_cast<void**>(&it));
    if (FAILED(hr) || !it) {
        if (error) *error = "DeckLink driver not found (install Blackmagic Desktop Video), " + HrText(hr);
        return out;
    }
    IDeckLink* dl = nullptr;
    while (it->Next(&dl) == S_OK && dl) {
        RawDevice d;
        d.deckLink = dl;
        BSTR name = nullptr;
        if (SUCCEEDED(dl->GetDisplayName(&name))) d.name = BstrToUtf8(name);

        bool canPlay = false;
        IDeckLinkProfileAttributes* attrs = nullptr;
        if (SUCCEEDED(dl->QueryInterface(IID_IDeckLinkProfileAttributes, reinterpret_cast<void**>(&attrs)))) {
            __int64 v = 0;
            if (SUCCEEDED(attrs->GetInt(BMDDeckLinkVideoIOSupport, &v))) canPlay = (v & bmdDeviceSupportsPlayback) != 0;
            v = 0;
            if (SUCCEEDED(attrs->GetInt(BMDDeckLinkPersistentID, &v))) d.persistentId = v;
            attrs->Release();
        } else {
            IDeckLinkOutput* o = nullptr;
            canPlay = SUCCEEDED(dl->QueryInterface(IID_IDeckLinkOutput, reinterpret_cast<void**>(&o)));
            SafeRelease(o);
        }
        if (canPlay) {
            out.push_back(d);
        } else {
            dl->Release();
        }
        dl = nullptr;
    }
    it->Release();
    if (out.empty() && error && error->empty()) *error = "No DeckLink output device found";
    return out;
}

bool IsInterlaced(_BMDFieldDominance f) {
    return f != bmdProgressiveFrame && f != bmdProgressiveSegmentedFrame && f != bmdUnknownFieldDominance;
}

DeckLinkModeInfo ModeInfoFrom(IDeckLinkDisplayMode* m) {
    DeckLinkModeInfo info;
    info.id = static_cast<uint32_t>(m->GetDisplayMode());
    BSTR name = nullptr;
    if (SUCCEEDED(m->GetName(&name))) info.name = BstrToUtf8(name);
    info.width = static_cast<int>(m->GetWidth());
    info.height = static_cast<int>(m->GetHeight());
    __int64 dur = 0, scale = 0;
    m->GetFrameRate(&dur, &scale);
    info.frameDuration = dur;
    info.timeScale = scale;
    info.interlaced = IsInterlaced(m->GetFieldDominance());
    return info;
}

bool SupportsMode(IDeckLinkOutput* out, uint32_t mode) {
    _BMDDisplayMode actual = static_cast<_BMDDisplayMode>(0);
    long supported = 0;
    HRESULT hr = out->DoesSupportVideoMode(bmdVideoConnectionUnspecified, static_cast<_BMDDisplayMode>(mode),
                                           bmdFormat8BitYUV, bmdNoVideoOutputConversion,
                                           bmdSupportedVideoModeDefault, &actual, &supported);
    return SUCCEEDED(hr) && supported;
}

std::vector<DeckLinkModeInfo> ListModes(IDeckLinkOutput* out) {
    std::vector<DeckLinkModeInfo> modes;
    IDeckLinkDisplayModeIterator* it = nullptr;
    if (FAILED(out->GetDisplayModeIterator(&it)) || !it) return modes;
    IDeckLinkDisplayMode* m = nullptr;
    while (it->Next(&m) == S_OK && m) {
        DeckLinkModeInfo info = ModeInfoFrom(m);
        if (SupportsMode(out, info.id)) modes.push_back(info);
        m->Release();
        m = nullptr;
    }
    it->Release();
    return modes;
}

}  // namespace

std::vector<DeckLinkDeviceInfo> EnumerateDeckLinkOutputs(std::string* error) {
    std::vector<DeckLinkDeviceInfo> result;
    for (RawDevice& d : ListOutputDevices(error)) {
        DeckLinkDeviceInfo info;
        info.name = d.name;
        info.persistentId = d.persistentId;
        IDeckLinkOutput* out = nullptr;
        if (SUCCEEDED(d.deckLink->QueryInterface(IID_IDeckLinkOutput, reinterpret_cast<void**>(&out)))) {
            info.modes = ListModes(out);
            out->Release();
        }
        d.deckLink->Release();
        result.push_back(std::move(info));
    }
    return result;
}

// ---------------------------------------------------------------------------------------------

class DeckLinkOutput::Impl : public IDeckLinkVideoOutputCallback {
public:
    // IUnknown. The object is owned by DeckLinkOutput, not by its COM reference count; the driver
    // only holds it while registered as the completion callback, which Stop() undoes before deletion.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IDeckLinkVideoOutputCallback) {
            *ppv = static_cast<IDeckLinkVideoOutputCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++m_refs); }
    ULONG STDMETHODCALLTYPE Release() override { return static_cast<ULONG>(--m_refs); }

    HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame* frame,
                                                      _BMDOutputFrameCompletionResult result) override {
        switch (result) {
            case bmdOutputFrameDisplayedLate: ++m_late; break;
            case bmdOutputFrameDropped: ++m_dropped; break;
            case bmdOutputFrameFlushed: ++m_flushed; break;
            default: break;
        }
        std::lock_guard<std::mutex> lock(m_cbMutex);
        if (m_stopping || !m_output || !frame) return S_OK;
        m_lastCallbackMs = NowMs();
        ++m_completed;
        try {
            FillFrame(frame);

            // If we fell behind (e.g. the PC stalled), jump ahead instead of scheduling into the past.
            __int64 streamTime = 0;
            double speed = 0;
            if (SUCCEEDED(m_output->GetScheduledStreamTime(m_mode.timeScale, &streamTime, &speed))) {
                // Only resync when the slot is already in the past. (With a 2-frame buffer the next slot is
                // normally just under one frame away, so a stricter test would fire on every frame.)
                if (m_nextTime <= streamTime) {
                    m_nextTime = (streamTime / m_mode.frameDuration + m_preroll) * m_mode.frameDuration;
                    ++m_resyncs;
                }
                // The pixels just filled go on air at m_nextTime; the gap is our queue latency.
                RecordLead(static_cast<double>(m_nextTime - streamTime) * 1000.0 / static_cast<double>(m_mode.timeScale));
            }
            HRESULT hr = m_output->ScheduleVideoFrame(frame, m_nextTime, m_mode.frameDuration, m_mode.timeScale);
            if (FAILED(hr)) ++m_scheduleErrors;
            m_nextTime += m_mode.frameDuration;
        } catch (...) {
            ++m_scheduleErrors;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped() override {
        if (!m_stopping) m_playbackStopped = true;
        return S_OK;
    }

    bool Start(const DeckLinkSelection& sel, IVideoSource* source, std::string* error) {
        Stop();
        m_source = source;
        if (!OpenDevice(sel, error) || !FindMode(sel.displayMode, error)) {
            Release_();
            return false;
        }

        HRESULT hr = m_output->EnableVideoOutput(static_cast<_BMDDisplayMode>(m_mode.id), bmdVideoOutputFlagDefault);
        if (FAILED(hr)) {
            *error = (hr == E_ACCESSDENIED) ? m_deviceName + " is in use by another application"
                                            : "Cannot enable output on " + m_deviceName + " (" + HrText(hr) + ")";
            Release_();
            return false;
        }
        m_outputEnabled = true;

        hr = m_output->SetScheduledFrameCompletionCallback(this);
        if (FAILED(hr)) {
            *error = "Cannot register frame callback (" + HrText(hr) + ")";
            Release_();
            return false;
        }

        m_completed = m_late = m_dropped = m_flushed = m_resyncs = m_scheduleErrors = m_liveFrames = 0;
        m_playbackStopped = false;
        m_stopping = false;
        m_nextTime = 0;

        // 2 is the minimum: one frame on air plus one being filled.
        m_preroll = sel.prerollFrames < 2 ? 2 : (sel.prerollFrames > 5 ? 5 : sel.prerollFrames);
        {
            std::lock_guard<std::mutex> lock(m_latMutex);
            m_lat = {};
        }
        for (int i = 0; i < m_preroll; ++i) {
            IDeckLinkMutableVideoFrame* f = nullptr;
            hr = m_output->CreateVideoFrame(m_mode.width, m_mode.height, m_mode.width * 2, bmdFormat8BitYUV,
                                            bmdFrameFlagDefault, &f);
            if (FAILED(hr) || !f) {
                *error = "Cannot allocate output frames (" + HrText(hr) + ")";
                Release_();
                return false;
            }
            m_frames.push_back(f);
            FillFrame(f);
            hr = m_output->ScheduleVideoFrame(f, m_nextTime, m_mode.frameDuration, m_mode.timeScale);
            if (FAILED(hr)) {
                *error = "Cannot schedule preroll frame (" + HrText(hr) + ")";
                Release_();
                return false;
            }
            m_nextTime += m_mode.frameDuration;
        }

        m_lastCallbackMs = NowMs();
        hr = m_output->StartScheduledPlayback(0, m_mode.timeScale, 1.0);
        if (FAILED(hr)) {
            *error = "Cannot start playback (" + HrText(hr) + ")";
            Release_();
            return false;
        }
        m_running = true;
        Log::Info("DeckLink: output started on '%s' @ %s (%dx%d%s, %lld/%lld), buffer %d frames (%.0f ms)",
                  m_deviceName.c_str(), m_mode.name.c_str(), m_mode.width, m_mode.height,
                  m_mode.interlaced ? " interlaced" : "", static_cast<long long>(m_mode.timeScale),
                  static_cast<long long>(m_mode.frameDuration), m_preroll,
                  m_preroll * 1000.0 * static_cast<double>(m_mode.frameDuration) / static_cast<double>(m_mode.timeScale));
        return true;
    }

    void Stop() {
        if (!m_output) return;
        m_stopping = true;
        if (m_running) {
            __int64 actual = 0;
            m_output->StopScheduledPlayback(0, &actual, 0);
            Log::Info("DeckLink: output stopped on '%s'", m_deviceName.c_str());
        }
        Release_();
    }

    bool IsRunning() const { return m_running; }

    DeckLinkOutput::Stats GetStats() {
        DeckLinkOutput::Stats s;
        s.completed = m_completed;
        s.late = m_late;
        s.dropped = m_dropped;
        s.flushed = m_flushed;
        s.resyncs = m_resyncs;
        s.scheduleErrors = m_scheduleErrors;
        s.liveFrames = m_liveFrames;
        s.lastCallbackMs = m_lastCallbackMs;
        s.playbackStopped = m_playbackStopped;
        if (m_output && m_running) {
            unsigned int buffered = 0;
            if (SUCCEEDED(m_output->GetBufferedVideoFrameCount(&buffered))) s.buffered = buffered;
        }
        return s;
    }

    LatencySummary TakeQueueLatency() {
        std::lock_guard<std::mutex> lock(m_latMutex);
        LatencySummary s;
        s.count = m_lat.count;
        if (m_lat.count) {
            s.avgMs = m_lat.sum / static_cast<double>(m_lat.count);
            s.minMs = m_lat.min;
            s.maxMs = m_lat.max;
        }
        m_lat = {};
        return s;
    }

    std::string m_deviceName;
    DeckLinkModeInfo m_mode;

private:
    void RecordLead(double ms) {
        std::lock_guard<std::mutex> lock(m_latMutex);
        if (m_lat.count == 0 || ms < m_lat.min) m_lat.min = ms;
        if (m_lat.count == 0 || ms > m_lat.max) m_lat.max = ms;
        m_lat.sum += ms;
        ++m_lat.count;
    }

    bool OpenDevice(const DeckLinkSelection& sel, std::string* error) {
        std::vector<RawDevice> devices = ListOutputDevices(error);
        int pick = -1;
        for (size_t i = 0; i < devices.size() && pick < 0; ++i)
            if (sel.persistentId != 0 && devices[i].persistentId == sel.persistentId) pick = static_cast<int>(i);
        for (size_t i = 0; i < devices.size() && pick < 0; ++i)
            if (!sel.deviceName.empty() && devices[i].name == sel.deviceName) pick = static_cast<int>(i);
        if (pick < 0 && sel.deviceName.empty() && sel.persistentId == 0 && !devices.empty()) pick = 0;

        for (size_t i = 0; i < devices.size(); ++i) {
            if (static_cast<int>(i) == pick) {
                m_device = devices[i].deckLink;
                m_deviceName = devices[i].name;
            } else {
                devices[i].deckLink->Release();
            }
        }
        if (!m_device) {
            if (!devices.empty() || error->empty())
                *error = "DeckLink device '" + (sel.deviceName.empty() ? std::string("(any)") : sel.deviceName) +
                         "' not found";
            return false;
        }
        HRESULT hr = m_device->QueryInterface(IID_IDeckLinkOutput, reinterpret_cast<void**>(&m_output));
        if (FAILED(hr) || !m_output) {
            *error = m_deviceName + " has no output interface (" + HrText(hr) + ")";
            return false;
        }
        return true;
    }

    bool FindMode(uint32_t modeId, std::string* error) {
        IDeckLinkDisplayModeIterator* it = nullptr;
        if (FAILED(m_output->GetDisplayModeIterator(&it)) || !it) {
            *error = "Cannot list display modes on " + m_deviceName;
            return false;
        }
        bool found = false;
        IDeckLinkDisplayMode* m = nullptr;
        while (!found && it->Next(&m) == S_OK && m) {
            if (static_cast<uint32_t>(m->GetDisplayMode()) == modeId) {
                m_mode = ModeInfoFrom(m);
                found = true;
            }
            m->Release();
            m = nullptr;
        }
        it->Release();
        if (!found || m_mode.frameDuration <= 0 || m_mode.timeScale <= 0) {
            *error = Format("Video format %c%c%c%c is not available on %s", (modeId >> 24) & 0xFF, (modeId >> 16) & 0xFF,
                            (modeId >> 8) & 0xFF, modeId & 0xFF, m_deviceName.c_str());
            return false;
        }
        if (!SupportsMode(m_output, modeId)) {
            *error = m_mode.name + " 8-bit YUV output is not supported by " + m_deviceName;
            return false;
        }
        return true;
    }

    void FillFrame(IDeckLinkVideoFrame* frame) {
        IDeckLinkVideoBuffer* buf = nullptr;
        if (FAILED(frame->QueryInterface(IID_IDeckLinkVideoBuffer, reinterpret_cast<void**>(&buf))) || !buf) return;
        if (SUCCEEDED(buf->StartAccess(bmdBufferAccessWrite))) {
            void* bytes = nullptr;
            if (SUCCEEDED(buf->GetBytes(&bytes)) && bytes) {
                const int rowBytes = static_cast<int>(frame->GetRowBytes());
                bool live = m_source && m_source->Render(static_cast<uint8_t*>(bytes), rowBytes, m_mode.width,
                                                         m_mode.height, m_mode.interlaced);
                if (live) ++m_liveFrames;
            }
            buf->EndAccess(bmdBufferAccessWrite);
        }
        buf->Release();
    }

    // Tears down in the order the SDK expects. Safe to call in any partial state.
    void Release_() {
        m_stopping = true;
        if (m_output) {
            m_output->SetScheduledFrameCompletionCallback(nullptr);
            { std::lock_guard<std::mutex> lock(m_cbMutex); }  // wait for an in-flight callback to finish
            if (m_outputEnabled) m_output->DisableVideoOutput();
        }
        m_outputEnabled = false;
        for (auto* f : m_frames) f->Release();
        m_frames.clear();
        SafeRelease(m_output);
        SafeRelease(m_device);
        m_running = false;
    }

    std::atomic<long> m_refs{1};
    std::mutex m_cbMutex;
    std::mutex m_latMutex;
    struct {
        uint64_t count = 0;
        double sum = 0, min = 0, max = 0;
    } m_lat;
    int m_preroll = 2;
    IDeckLink* m_device = nullptr;
    IDeckLinkOutput* m_output = nullptr;
    std::vector<IDeckLinkMutableVideoFrame*> m_frames;
    IVideoSource* m_source = nullptr;
    bool m_outputEnabled = false;
    bool m_running = false;
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_playbackStopped{false};
    __int64 m_nextTime = 0;
    std::atomic<uint64_t> m_completed{0}, m_late{0}, m_dropped{0}, m_flushed{0}, m_resyncs{0}, m_scheduleErrors{0},
        m_liveFrames{0}, m_lastCallbackMs{0};
};

DeckLinkOutput::DeckLinkOutput() : m_impl(new Impl()) {}

DeckLinkOutput::~DeckLinkOutput() {
    m_impl->Stop();
    delete m_impl;
}

bool DeckLinkOutput::Start(const DeckLinkSelection& sel, IVideoSource* source, std::string* error) {
    std::string err;
    bool ok = m_impl->Start(sel, source, &err);
    if (!ok && error) *error = err;
    return ok;
}

void DeckLinkOutput::Stop() { m_impl->Stop(); }
bool DeckLinkOutput::IsRunning() const { return m_impl->IsRunning(); }
DeckLinkOutput::Stats DeckLinkOutput::GetStats() { return m_impl->GetStats(); }
LatencySummary DeckLinkOutput::TakeQueueLatency() { return m_impl->TakeQueueLatency(); }
std::string DeckLinkOutput::DeviceName() const { return m_impl->m_deviceName; }
std::string DeckLinkOutput::ModeName() const { return m_impl->m_mode.name; }

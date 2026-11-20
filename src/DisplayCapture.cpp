#include "DisplayCapture.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cstring>

#include "Displays.h"
#include "Log.h"
#include "Util.h"

namespace {
constexpr UINT kAcquireTimeoutMs = 100;
constexpr uint64_t kRetryMs = 1000;

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::string HrText(HRESULT hr) {
    switch (hr) {
        case E_ACCESSDENIED: return "access denied (secure desktop, lock screen or UAC prompt?)";
        case DXGI_ERROR_UNSUPPORTED: return "desktop duplication is not supported for this display";
        case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE: return "too many applications are capturing this display";
        case DXGI_ERROR_SESSION_DISCONNECTED: return "the Windows session is disconnected";
        case DXGI_ERROR_ACCESS_LOST: return "access lost (display mode change)";
        default: return Format("error 0x%08lX", static_cast<unsigned long>(hr));
    }
}

int64_t Qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double QpcFreq() {
    static const double f = [] {
        LARGE_INTEGER t;
        QueryPerformanceFrequency(&t);
        return static_cast<double>(t.QuadPart);
    }();
    return f;
}

// The D3D objects for one duplication session. Owned by the capture thread only.
struct DupSession {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGIOutputDuplication* dup = nullptr;
    ID3D11Texture2D* staging = nullptr;
    D3D11_TEXTURE2D_DESC stagingDesc{};

    void Reset() {
        SafeRelease(staging);
        SafeRelease(dup);
        SafeRelease(ctx);
        SafeRelease(dev);
    }
    ~DupSession() { Reset(); }
};

// Finds the DXGI output for a GDI device name and starts duplicating it.
HRESULT OpenDuplication(const std::string& gdiName, DupSession* s, std::string* error) {
    s->Reset();
    const std::wstring wanted = Utf8ToWide(gdiName);
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) {
        *error = "cannot create DXGI factory (" + HrText(hr) + ")";
        return hr;
    }
    IDXGIAdapter1* foundAdapter = nullptr;
    IDXGIOutput* foundOutput = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; !foundOutput && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        IDXGIOutput* output = nullptr;
        for (UINT o = 0; !foundOutput && adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && _wcsicmp(desc.DeviceName, wanted.c_str()) == 0) {
                foundOutput = output;
                foundAdapter = adapter;
                foundAdapter->AddRef();
            } else {
                output->Release();
            }
        }
        adapter->Release();
    }
    factory->Release();
    if (!foundOutput) {
        *error = "display " + gdiName + " is not attached to any graphics adapter";
        SafeRelease(foundAdapter);
        return DXGI_ERROR_NOT_FOUND;
    }

    hr = D3D11CreateDevice(foundAdapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr,
                           0, D3D11_SDK_VERSION, &s->dev, nullptr, &s->ctx);
    foundAdapter->Release();
    if (FAILED(hr)) {
        foundOutput->Release();
        *error = "cannot create a Direct3D device (" + HrText(hr) + ")";
        return hr;
    }
    IDXGIOutput1* output1 = nullptr;
    hr = foundOutput->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
    foundOutput->Release();
    if (FAILED(hr)) {
        s->Reset();
        *error = "desktop duplication is not available on this Windows version";
        return hr;
    }
    hr = output1->DuplicateOutput(s->dev, &s->dup);
    output1->Release();
    if (FAILED(hr)) {
        s->Reset();
        *error = "cannot capture " + gdiName + ": " + HrText(hr);
        return hr;
    }
    return S_OK;
}
}  // namespace

DisplayCapture::~DisplayCapture() { Stop(); }

void DisplayCapture::Start(const std::string& tag) {
    std::lock_guard<std::mutex> lock(m_ctlMutex);
    if (m_thread.joinable()) return;
    m_stop = false;
    m_thread = std::thread(&DisplayCapture::ThreadMain, this, tag);
}

void DisplayCapture::Stop() {
    {
        std::lock_guard<std::mutex> lock(m_ctlMutex);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void DisplayCapture::SetDisplay(const std::string& devicePath, const std::string& gdiName, const std::string& label) {
    {
        std::lock_guard<std::mutex> lock(m_ctlMutex);
        if (devicePath == m_wantPath && gdiName == m_wantGdi) return;
        m_wantPath = devicePath;
        m_wantGdi = gdiName;
        m_wantLabel = label;
        ++m_wantVersion;
    }
    m_cv.notify_all();
}

DisplayCapture::Status DisplayCapture::GetStatus() {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    Status s = m_status;
    s.hasVideo = m_valid.load();
    return s;
}

DisplayCapture::Age DisplayCapture::TakeFrameAge() {
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

bool DisplayCapture::Render(uint8_t* dst, int rowBytes, int width, int height, bool) {
    bool isNew = false;
    {
        std::lock_guard<std::mutex> lock(m_bufMutex);
        if (m_readyIsNew) {
            std::swap(m_front, m_ready);
            m_readyIsNew = false;
            isNew = true;
        }
    }
    const Buffer& b = m_buf[m_front];
    if (!m_valid.load() || b.width <= 0 || b.data.empty()) {
        FillBlackUYVY(dst, rowBytes, width, height);
        m_cacheW = 0;
        return false;
    }
    const size_t bytes = static_cast<size_t>(rowBytes) * static_cast<size_t>(height);
    if (!isNew && m_cacheW == width && m_cacheH == height && m_cacheRowBytes == rowBytes && m_cache.size() == bytes) {
        memcpy(dst, m_cache.data(), bytes);  // desktop unchanged: reuse the last conversion
        return true;
    }
    SrcFrame src;
    src.data = b.data.data();
    src.width = b.width;
    src.height = b.height;
    src.stride = b.stride;
    src.format = SrcFormat::BGRX;  // desktop alpha is undefined
    m_renderer.Render(src, dst, rowBytes, width, height);
    m_cache.assign(dst, dst + bytes);
    m_cacheW = width;
    m_cacheH = height;
    m_cacheRowBytes = rowBytes;

    if (isNew && b.presentQpc > 0) {
        double ageMs = static_cast<double>(Qpc() - b.presentQpc) * 1000.0 / QpcFreq();
        if (ageMs >= 0 && ageMs < 10000) {
            std::lock_guard<std::mutex> lock(m_ageMutex);
            if (m_ageCount == 0 || ageMs < m_ageMin) m_ageMin = ageMs;
            if (m_ageCount == 0 || ageMs > m_ageMax) m_ageMax = ageMs;
            m_ageSum += ageMs;
            ++m_ageCount;
        }
    }
    return true;
}

void DisplayCapture::ThreadMain(std::string tag) {
    Log::SetThreadTag(tag.c_str());
    Log::Info("Display capture thread started");
    // Capture must keep up with the display even when the PC is busy.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    DupSession s;
    unsigned applied = ~0u;
    std::string path, gdi, label, lastError;
    uint64_t nextAttemptMs = 0;
    int64_t frames = 0;

    auto setStatus = [&](bool found, bool capturing, const std::string& err, int w, int h, int hz) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_status.label = label;
        m_status.found = found;
        m_status.capturing = capturing;
        m_status.error = err;
        if (w > 0) {
            m_status.width = w;
            m_status.height = h;
            m_status.refreshHz = hz;
        }
        m_status.framesCaptured = frames;
    };
    auto fail = [&](const std::string& err, bool found) {
        if (err != lastError) {
            Log::Warn("Display capture: %s; retrying", err.c_str());
            lastError = err;
        }
        if (!found) m_valid = false;  // display gone: output black. Otherwise keep the last frame.
        setStatus(found, false, err, 0, 0, 0);
        nextAttemptMs = NowMs() + kRetryMs;
    };

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(m_ctlMutex);
            if (!s.dup) {
                uint64_t now = NowMs();
                bool idle = path.empty() && gdi.empty() && applied == m_wantVersion;
                uint64_t waitMs = idle ? 1000 : (nextAttemptMs > now ? nextAttemptMs - now : 0);
                if (waitMs) m_cv.wait_for(lock, std::chrono::milliseconds(waitMs),
                                          [&] { return m_stop || applied != m_wantVersion; });
            }
            if (m_stop) break;
            if (applied != m_wantVersion) {
                applied = m_wantVersion;
                path = m_wantPath;
                gdi = m_wantGdi;
                label = m_wantLabel;
                s.Reset();
                m_valid = false;
                lastError.clear();
                nextAttemptMs = 0;
                frames = 0;
                {
                    std::lock_guard<std::mutex> st(m_statusMutex);
                    m_status = Status{};
                    m_status.label = label;
                }
                if (!path.empty() || !gdi.empty()) Log::Info("Display capture: looking for %s", label.c_str());
                continue;
            }
        }
        if (path.empty() && gdi.empty()) continue;

        if (!s.dup) {
            if (NowMs() < nextAttemptMs) continue;
            std::vector<DisplayInfo> displays = EnumerateDisplays();
            int idx = FindDisplay(displays, path, gdi);
            if (idx < 0) {
                fail("display '" + label + "' not found (outputting black)", false);
                continue;
            }
            const DisplayInfo& d = displays[static_cast<size_t>(idx)];
            std::string err;
            if (FAILED(OpenDuplication(d.gdiName, &s, &err))) {
                fail(err, true);
                continue;
            }
            Log::Info("Display capture: started on %s (%s, %dx%d @ %d Hz)", d.label.c_str(), d.gdiName.c_str(), d.width,
                      d.height, d.refreshHz);
            lastError.clear();
            setStatus(true, true, "", d.width, d.height, d.refreshHz);
        }

        DXGI_OUTDUPL_FRAME_INFO info{};
        IDXGIResource* res = nullptr;
        HRESULT hr = s.dup->AcquireNextFrame(kAcquireTimeoutMs, &info, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;  // screen unchanged; the last frame stays on air
        if (FAILED(hr)) {
            s.Reset();
            fail("capture interrupted: " + HrText(hr), true);
            nextAttemptMs = NowMs() + 100;  // usually a mode change; reopen quickly
            continue;
        }

        if (info.LastPresentTime.QuadPart != 0) {  // 0 = only the mouse pointer moved
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
                D3D11_TEXTURE2D_DESC desc{};
                tex->GetDesc(&desc);
                if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
                    std::string err = Format("unsupported desktop format %d (turn off HDR on this display)",
                                             static_cast<int>(desc.Format));
                    if (err != lastError) Log::Warn("Display capture: %s", err.c_str());
                    lastError = err;
                } else {
                    if (!s.staging || s.stagingDesc.Width != desc.Width || s.stagingDesc.Height != desc.Height ||
                        s.stagingDesc.Format != desc.Format) {
                        SafeRelease(s.staging);
                        D3D11_TEXTURE2D_DESC sd = desc;
                        sd.Usage = D3D11_USAGE_STAGING;
                        sd.BindFlags = 0;
                        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        sd.MiscFlags = 0;
                        sd.MipLevels = 1;
                        sd.ArraySize = 1;
                        sd.SampleDesc = {1, 0};
                        if (SUCCEEDED(s.dev->CreateTexture2D(&sd, nullptr, &s.staging))) s.stagingDesc = sd;
                    }
                    if (s.staging) {
                        s.ctx->CopyResource(s.staging, tex);
                        D3D11_MAPPED_SUBRESOURCE map{};
                        if (SUCCEEDED(s.ctx->Map(s.staging, 0, D3D11_MAP_READ, 0, &map))) {
                            Buffer& b = m_buf[m_back];
                            const int w = static_cast<int>(desc.Width), h = static_cast<int>(desc.Height);
                            const size_t rowBytes = static_cast<size_t>(w) * 4;
                            b.data.resize(rowBytes * static_cast<size_t>(h));
                            const uint8_t* srcRow = static_cast<const uint8_t*>(map.pData);
                            for (int y = 0; y < h; ++y)
                                memcpy(b.data.data() + rowBytes * static_cast<size_t>(y), srcRow + static_cast<size_t>(map.RowPitch) * static_cast<size_t>(y), rowBytes);
                            s.ctx->Unmap(s.staging, 0);
                            b.width = w;
                            b.height = h;
                            b.stride = static_cast<int>(rowBytes);
                            b.presentQpc = info.LastPresentTime.QuadPart;
                            {
                                std::lock_guard<std::mutex> lock(m_bufMutex);
                                std::swap(m_back, m_ready);
                                m_readyIsNew = true;
                            }
                            m_valid = true;
                            if (++frames % 60 == 1) {
                                std::lock_guard<std::mutex> lock(m_statusMutex);
                                m_status.framesCaptured = frames;
                            }
                        }
                    }
                }
                tex->Release();
            }
        }
        res->Release();
        s.dup->ReleaseFrame();
    }

    s.Reset();
    m_valid = false;
    Log::Info("Display capture thread stopped");
}

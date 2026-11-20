// NdiReceiver for display-capture-only builds (no NDI SDK). Compiled instead of NdiReceiver.cpp
// when CMake does not find the NDI SDK: an NDI output just stays black with a clear status.
#include "NdiReceiver.h"

#include "NdiLib.h"

NdiReceiver::NdiReceiver() {
    std::string err;
    Ndi::Load(&err);  // always fails in this build; gives the user-facing message
    m_status.runtimeLoaded = false;
    m_status.runtimeError = err;
}

NdiReceiver::~NdiReceiver() = default;

void NdiReceiver::Start(const std::string&) {}

void NdiReceiver::Stop() {}

void NdiReceiver::SetSource(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    m_status.source = name;
}

bool NdiReceiver::Render(uint8_t* dst, int rowBytes, int width, int height, bool) {
    FillBlackUYVY(dst, rowBytes, width, height);
    return false;
}

NdiReceiver::Status NdiReceiver::GetStatus() {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    return m_status;
}

NdiReceiver::Age NdiReceiver::TakeFrameAge() { return {}; }

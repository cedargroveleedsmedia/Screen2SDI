#pragma once
#include <cstdint>

// Supplies pixels for each DeckLink output frame. Implementations must be fast and must never
// block for long: they run on the DeckLink completion callback thread.
class IVideoSource {
public:
    virtual ~IVideoSource() = default;
    // Fill an 8-bit UYVY buffer. Returns true if live video was rendered, false if black was output.
    virtual bool Render(uint8_t* dst, int rowBytes, int width, int height, bool interlaced) = 0;
};

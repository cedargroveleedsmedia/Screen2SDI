#pragma once
#include <cstdint>
#include <vector>

// Pixel formats we can render from (as delivered by NDI with NDIlib_recv_color_format_UYVY_BGRA).
enum class SrcFormat { UYVY, BGRA, BGRX, RGBA, RGBX };

struct SrcFrame {
    const uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;          // bytes per row
    SrcFormat format = SrcFormat::UYVY;
    float aspect = 0.0f;     // display aspect ratio; 0 = width/height
};

// Destination is always 8-bit UYVY (bmdFormat8BitYUV), BT.709 limited range.
void FillBlackUYVY(uint8_t* dst, int rowBytes, int width, int height);

// Renders src into dst: direct copy/convert when sizes match, otherwise bilinear scale with
// letterbox/pillarbox to preserve aspect ratio. Never reads outside src or writes outside dst.
// Not thread-safe per instance (holds scaling tables); use one per output thread.
class FrameRenderer {
public:
    void Render(const SrcFrame& src, uint8_t* dst, int dstRowBytes, int dstWidth, int dstHeight);

private:
    struct Tap {
        int i0, i1;   // source indices
        int w1;       // weight of i1, 0..256
    };
    std::vector<Tap> m_xLuma, m_xChroma, m_y;
    int m_keySrcW = -1, m_keySrcH = -1, m_keyDstW = -1, m_keyDstH = -1;

    void BuildTables(int srcW, int srcH, int dstW, int dstH);
};

#include "FrameConvert.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// BT.709 limited range, 16.16 fixed point.
inline uint8_t RgbToY(int r, int g, int b) {
    return static_cast<uint8_t>(((11966 * r + 40254 * g + 4064 * b + 32768) >> 16) + 16);
}
inline uint8_t RgbToU(int r, int g, int b) {
    return static_cast<uint8_t>((-6597 * r - 22187 * g + 28784 * b + (128 << 16) + 32768) >> 16);
}
inline uint8_t RgbToV(int r, int g, int b) {
    return static_cast<uint8_t>((28784 * r - 26148 * g - 2636 * b + (128 << 16) + 32768) >> 16);
}

struct Rgb {
    int r, g, b;
};

// Reads one pixel from a 4-byte-per-pixel row, compositing any alpha over black.
inline Rgb ReadRgb(const uint8_t* p, SrcFormat fmt) {
    int r, g, b, a = 255;
    if (fmt == SrcFormat::BGRA || fmt == SrcFormat::BGRX) {
        b = p[0]; g = p[1]; r = p[2];
    } else {
        r = p[0]; g = p[1]; b = p[2];
    }
    if (fmt == SrcFormat::BGRA || fmt == SrcFormat::RGBA) a = p[3];
    if (a != 255) {
        r = (r * a + 127) / 255;
        g = (g * a + 127) / 255;
        b = (b * a + 127) / 255;
    }
    return {r, g, b};
}

inline int Lerp(int a, int b, int w) { return a + (((b - a) * w + 128) >> 8); }

bool IsRgb(SrcFormat f) { return f != SrcFormat::UYVY; }

}  // namespace

void FillBlackUYVY(uint8_t* dst, int rowBytes, int width, int height) {
    const uint32_t black = 0x10801080;  // bytes in memory: 80 10 80 10 = U Y V Y
    for (int y = 0; y < height; ++y) {
        uint32_t* row = reinterpret_cast<uint32_t*>(dst + static_cast<size_t>(y) * rowBytes);
        for (int x = 0; x < width / 2; ++x) row[x] = black;
    }
}

void FrameRenderer::BuildTables(int srcW, int srcH, int dstW, int dstH) {
    if (srcW == m_keySrcW && srcH == m_keySrcH && dstW == m_keyDstW && dstH == m_keyDstH) return;
    m_keySrcW = srcW;
    m_keySrcH = srcH;
    m_keyDstW = dstW;
    m_keyDstH = dstH;

    auto build = [](std::vector<Tap>& taps, int srcN, int dstN) {
        taps.resize(static_cast<size_t>(dstN));
        const double scale = static_cast<double>(srcN) / dstN;
        for (int i = 0; i < dstN; ++i) {
            double s = (i + 0.5) * scale - 0.5;
            if (s < 0) s = 0;
            int i0 = static_cast<int>(std::floor(s));
            if (i0 > srcN - 1) i0 = srcN - 1;
            int i1 = std::min(i0 + 1, srcN - 1);
            int w1 = static_cast<int>((s - i0) * 256.0 + 0.5);
            taps[static_cast<size_t>(i)] = {i0, i1, std::clamp(w1, 0, 256)};
        }
    };
    build(m_xLuma, srcW, dstW);
    build(m_xChroma, std::max(1, srcW / 2), std::max(1, dstW / 2));
    build(m_y, srcH, dstH);
}

void FrameRenderer::Render(const SrcFrame& src, uint8_t* dst, int dstRowBytes, int dstW, int dstH) {
    const int bpp = IsRgb(src.format) ? 4 : 2;
    if (!src.data || src.width < 2 || src.height < 2 || dstW < 2 || dstH < 2 ||
        src.stride < src.width * bpp) {
        FillBlackUYVY(dst, dstRowBytes, dstW, dstH);
        return;
    }

    // Fast paths: identical size.
    if (src.width == dstW && src.height == dstH) {
        if (src.format == SrcFormat::UYVY) {
            const size_t rowCopy = static_cast<size_t>(dstW) * 2;
            for (int y = 0; y < dstH; ++y)
                memcpy(dst + static_cast<size_t>(y) * dstRowBytes, src.data + static_cast<size_t>(y) * src.stride, rowCopy);
            return;
        }
        for (int y = 0; y < dstH; ++y) {
            const uint8_t* s = src.data + static_cast<size_t>(y) * src.stride;
            uint8_t* d = dst + static_cast<size_t>(y) * dstRowBytes;
            for (int x = 0; x + 1 < dstW; x += 2, s += 8, d += 4) {
                Rgb p0 = ReadRgb(s, src.format);
                Rgb p1 = ReadRgb(s + 4, src.format);
                int r = (p0.r + p1.r + 1) >> 1, g = (p0.g + p1.g + 1) >> 1, b = (p0.b + p1.b + 1) >> 1;
                d[0] = RgbToU(r, g, b);
                d[1] = RgbToY(p0.r, p0.g, p0.b);
                d[2] = RgbToV(r, g, b);
                d[3] = RgbToY(p1.r, p1.g, p1.b);
            }
        }
        return;
    }

    // Fit the source into the destination, preserving display aspect ratio.
    double srcAspect = src.aspect > 0.01f ? src.aspect : static_cast<double>(src.width) / src.height;
    double dstAspect = (dstW == 720) ? 4.0 / 3.0 : static_cast<double>(dstW) / dstH;  // SD modes are 4:3
    int rw = dstW, rh = dstH;
    if (std::fabs(srcAspect - dstAspect) / dstAspect > 0.01) {
        if (srcAspect > dstAspect) {
            rh = static_cast<int>(std::lround(dstH * dstAspect / srcAspect));
        } else {
            rw = static_cast<int>(std::lround(dstW * srcAspect / dstAspect));
        }
    }
    rw = std::clamp(rw & ~1, 2, dstW & ~1);
    rh = std::clamp(rh, 2, dstH);
    const int ox = ((dstW - rw) / 2) & ~1;
    const int oy = (dstH - rh) / 2;

    if (rw != dstW || rh != dstH) FillBlackUYVY(dst, dstRowBytes, dstW, dstH);
    BuildTables(src.width, src.height, rw, rh);

    for (int y = 0; y < rh; ++y) {
        const Tap& ty = m_y[static_cast<size_t>(y)];
        const uint8_t* r0 = src.data + static_cast<size_t>(ty.i0) * src.stride;
        const uint8_t* r1 = src.data + static_cast<size_t>(ty.i1) * src.stride;
        const int wy = ty.w1;
        uint8_t* d = dst + static_cast<size_t>(y + oy) * dstRowBytes + static_cast<size_t>(ox) * 2;

        if (src.format == SrcFormat::UYVY) {
            auto lumaAt = [&](int x) {
                const Tap& tx = m_xLuma[static_cast<size_t>(x)];
                int a = Lerp(r0[tx.i0 * 2 + 1], r0[tx.i1 * 2 + 1], tx.w1);
                int b = Lerp(r1[tx.i0 * 2 + 1], r1[tx.i1 * 2 + 1], tx.w1);
                return Lerp(a, b, wy);
            };
            for (int x = 0; x + 1 < rw; x += 2, d += 4) {
                const Tap& tc = m_xChroma[static_cast<size_t>(x / 2)];
                int u = Lerp(Lerp(r0[tc.i0 * 4], r0[tc.i1 * 4], tc.w1), Lerp(r1[tc.i0 * 4], r1[tc.i1 * 4], tc.w1), wy);
                int v = Lerp(Lerp(r0[tc.i0 * 4 + 2], r0[tc.i1 * 4 + 2], tc.w1),
                             Lerp(r1[tc.i0 * 4 + 2], r1[tc.i1 * 4 + 2], tc.w1), wy);
                d[0] = static_cast<uint8_t>(u);
                d[1] = static_cast<uint8_t>(lumaAt(x));
                d[2] = static_cast<uint8_t>(v);
                d[3] = static_cast<uint8_t>(lumaAt(x + 1));
            }
        } else {
            auto rgbAt = [&](int x) {
                const Tap& tx = m_xLuma[static_cast<size_t>(x)];
                Rgb a0 = ReadRgb(r0 + tx.i0 * 4, src.format), a1 = ReadRgb(r0 + tx.i1 * 4, src.format);
                Rgb b0 = ReadRgb(r1 + tx.i0 * 4, src.format), b1 = ReadRgb(r1 + tx.i1 * 4, src.format);
                return Rgb{Lerp(Lerp(a0.r, a1.r, tx.w1), Lerp(b0.r, b1.r, tx.w1), wy),
                           Lerp(Lerp(a0.g, a1.g, tx.w1), Lerp(b0.g, b1.g, tx.w1), wy),
                           Lerp(Lerp(a0.b, a1.b, tx.w1), Lerp(b0.b, b1.b, tx.w1), wy)};
            };
            for (int x = 0; x + 1 < rw; x += 2, d += 4) {
                Rgb p0 = rgbAt(x), p1 = rgbAt(x + 1);
                int r = (p0.r + p1.r + 1) >> 1, g = (p0.g + p1.g + 1) >> 1, b = (p0.b + p1.b + 1) >> 1;
                d[0] = RgbToU(r, g, b);
                d[1] = RgbToY(p0.r, p0.g, p0.b);
                d[2] = RgbToV(r, g, b);
                d[3] = RgbToY(p1.r, p1.g, p1.b);
            }
        }
    }
}

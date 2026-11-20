// Small self-checking tests for the pure-C++ parts (no NDI or DeckLink needed).
// Build target: Screen2SDI_tests. Exit code 0 = all passed.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "FrameConvert.h"
#include "Json.h"
#include "VirtualDisplays.h"

static int g_failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

static bool Near(int a, int b, int tol = 1) { return std::abs(a - b) <= tol; }

static void TestBgraSameSize() {
    const int w = 4, h = 2, stride = w * 4 + 8;  // padded rows
    std::vector<uint8_t> src(static_cast<size_t>(stride) * h, 0xCD);
    auto put = [&](int x, int y, int b, int g, int r, int a) {
        uint8_t* p = &src[static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4];
        p[0] = static_cast<uint8_t>(b); p[1] = static_cast<uint8_t>(g); p[2] = static_cast<uint8_t>(r); p[3] = static_cast<uint8_t>(a);
    };
    for (int x = 0; x < w; ++x) { put(x, 0, 255, 255, 255, 255); put(x, 1, 0, 0, 0, 255); }
    const int dstRow = w * 2 + 16;
    std::vector<uint8_t> dst(static_cast<size_t>(dstRow) * h, 0);
    FrameRenderer r;
    SrcFrame f{src.data(), w, h, stride, SrcFormat::BGRA, 0};
    r.Render(f, dst.data(), dstRow, w, h);
    // White -> Y 235, U/V 128. Black -> Y 16, U/V 128.
    CHECK(Near(dst[1], 235)); CHECK(Near(dst[3], 235)); CHECK(Near(dst[0], 128)); CHECK(Near(dst[2], 128));
    CHECK(Near(dst[dstRow + 1], 16)); CHECK(Near(dst[dstRow + 0], 128));
    CHECK(dst[w * 2] == 0);  // padding untouched
}

static void TestPrimaries() {
    // BT.709 limited: red Y=63 U=102 V=240; green Y=173 U=42 V=26; blue Y=32 U=240 V=118.
    struct { int r, g, b, y, u, v; } cases[] = {
        {255, 0, 0, 63, 102, 240}, {0, 255, 0, 173, 42, 26}, {0, 0, 255, 32, 240, 118}};
    for (auto& c : cases) {
        uint8_t src[8] = {static_cast<uint8_t>(c.b), static_cast<uint8_t>(c.g), static_cast<uint8_t>(c.r), 255,
                          static_cast<uint8_t>(c.b), static_cast<uint8_t>(c.g), static_cast<uint8_t>(c.r), 255};
        uint8_t src2[16];
        std::memcpy(src2, src, 8);
        std::memcpy(src2 + 8, src, 8);
        uint8_t dst[8];
        FrameRenderer r;
        SrcFrame f{src2, 2, 2, 8, SrcFormat::BGRA, 0};
        r.Render(f, dst, 4, 2, 2);
        CHECK(Near(dst[1], c.y)); CHECK(Near(dst[0], c.u)); CHECK(Near(dst[2], c.v));
    }
}

static void TestAlphaOverBlack() {
    uint8_t src[16];
    for (int i = 0; i < 4; ++i) { src[i * 4] = 255; src[i * 4 + 1] = 255; src[i * 4 + 2] = 255; src[i * 4 + 3] = 0; }
    uint8_t dst[8];
    FrameRenderer r;
    SrcFrame f{src, 2, 2, 8, SrcFormat::BGRA, 0};
    r.Render(f, dst, 4, 2, 2);
    CHECK(Near(dst[1], 16));  // fully transparent white -> black
}

static void TestUyvyCopyAndScale() {
    // Same-size UYVY with stride padding.
    const int w = 8, h = 4, stride = w * 2 + 6;
    std::vector<uint8_t> src(static_cast<size_t>(stride) * h);
    for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint8_t>(i * 7);
    std::vector<uint8_t> dst(static_cast<size_t>(w) * 2 * h);
    FrameRenderer r;
    SrcFrame f{src.data(), w, h, stride, SrcFormat::UYVY, 0};
    r.Render(f, dst.data(), w * 2, w, h);
    for (int y = 0; y < h; ++y) CHECK(std::memcmp(&dst[static_cast<size_t>(y) * w * 2], &src[static_cast<size_t>(y) * stride], w * 2) == 0);

    // Flat grey 1280x720 UYVY scaled to 1920x1080: every pixel must stay grey.
    const int sw = 1280, sh = 720, dw = 1920, dh = 1080;
    std::vector<uint8_t> s(static_cast<size_t>(sw) * 2 * sh);
    for (size_t i = 0; i < s.size(); i += 2) { s[i] = 128; s[i + 1] = 100; }
    std::vector<uint8_t> d(static_cast<size_t>(dw) * 2 * dh, 0);
    SrcFrame g{s.data(), sw, sh, sw * 2, SrcFormat::UYVY, 0};
    r.Render(g, d.data(), dw * 2, dw, dh);
    bool ok = true;
    for (size_t i = 0; i < d.size(); i += 2) ok = ok && d[i] == 128 && d[i + 1] == 100;
    CHECK(ok);
}

static void TestLetterbox() {
    // 4:3 source into 16:9 output -> pillarbox: black at the far left, picture in the middle.
    const int sw = 640, sh = 480, dw = 1920, dh = 1080;
    std::vector<uint8_t> s(static_cast<size_t>(sw) * 4 * sh, 255);
    std::vector<uint8_t> d(static_cast<size_t>(dw) * 2 * dh, 0);
    FrameRenderer r;
    SrcFrame f{s.data(), sw, sh, sw * 4, SrcFormat::BGRX, 0};
    r.Render(f, d.data(), dw * 2, dw, dh);
    const size_t mid = static_cast<size_t>(dh / 2) * dw * 2;
    CHECK(Near(d[mid + 1], 16));                             // left edge black
    CHECK(Near(d[mid + static_cast<size_t>(dw) + 1], 235));  // centre white
    CHECK(Near(d[mid + static_cast<size_t>(dw) * 2 - 1], 16));  // right edge black
}

static void TestOddAndDegenerate() {
    FrameRenderer r;
    std::vector<uint8_t> d(1920 * 2 * 1080, 0xAA);
    SrcFrame none{};
    r.Render(none, d.data(), 1920 * 2, 1920, 1080);  // null -> black
    CHECK(d[1] == 16 && d[0] == 128);
    std::vector<uint8_t> s(1001 * 4 * 333, 77);
    SrcFrame odd{s.data(), 1001, 333, 1001 * 4, SrcFormat::RGBA, 0};
    r.Render(odd, d.data(), 1920 * 2, 1920, 1080);   // odd sizes must not crash
    SrcFrame badStride{s.data(), 1001, 333, 10, SrcFormat::RGBA, 0};
    r.Render(badStride, d.data(), 1920 * 2, 1920, 1080);  // invalid stride -> black, no overrun
    CHECK(d[1] == 16);
}

static void TestJson() {
    JsonValue v;
    std::string err;
    CHECK(JsonValue::Parse("{\"a\":\"x\\\"y\\u00e9\",\"n\":1234567890123,\"b\":true,\"arr\":[1,2,{}]}", &v, &err));
    CHECK(v.GetString("a", "") == "x\"y\xC3\xA9");
    CHECK(v.GetNumber("n", 0) == 1234567890123.0);
    CHECK(v.GetBool("b", false));
    JsonValue back;
    CHECK(JsonValue::Parse(v.Serialize(2), &back, &err));
    CHECK(back.GetString("a", "") == v.GetString("a", "-"));
    CHECK(!JsonValue::Parse("{\"a\":}", &back, &err));
    CHECK(!JsonValue::Parse("{\"a\":1} junk", &back, &err));
    CHECK(!JsonValue::Parse("", &back, &err));
}

static void TestVddCount() {
    const std::string xml =
        "<?xml version='1.0'?>\n<vdd_settings>\n    <monitors>\n        <count>3</count>\n    </monitors>\n"
        "    <resolutions><resolution><width>1920</width><count>9</count></resolution></resolutions>\n"
        "</vdd_settings>\n";
    int n = 0;
    CHECK(Vdd::ParseMonitorCount(xml, &n) && n == 3);
    std::string out;
    CHECK(Vdd::ReplaceMonitorCount(xml, 12, &out));
    CHECK(Vdd::ParseMonitorCount(out, &n) && n == 12);
    CHECK(out.find("<count>12</count>") != std::string::npos);
    CHECK(out.find("<count>9</count>") != std::string::npos);  // other <count> tags untouched
    CHECK(out.size() == xml.size() + 1);                        // nothing else changed
    CHECK(!Vdd::ReplaceMonitorCount(xml, 0, &out));             // out of range
    CHECK(!Vdd::ReplaceMonitorCount(xml, Vdd::kMaxMonitors + 1, &out));
    CHECK(!Vdd::ParseMonitorCount("<vdd_settings><count>2</count></vdd_settings>", &n));  // not inside <monitors>
    CHECK(!Vdd::ParseMonitorCount("<monitors><count></count></monitors>", &n));
    CHECK(Vdd::ParseMonitorCount("<monitors><count> 4</count></monitors>", &n) && n == 4);
}

int main() {
    TestBgraSameSize();
    TestPrimaries();
    TestAlphaOverBlack();
    TestUyvyCopyAndScale();
    TestLetterbox();
    TestOddAndDegenerate();
    TestJson();
    TestVddCount();
    if (g_failures == 0) std::printf("All tests passed\n");
    return g_failures == 0 ? 0 : 1;
}

#include "screenshot.h"

#include <lvgl.h>

namespace shot {
namespace {

constexpr int W = 240, H = 240;
constexpr size_t PX_LEN  = (size_t)W * H * 2;          // RGB565 snapshot
constexpr size_t BMP_HDR = 54;
constexpr size_t BMP_LEN = BMP_HDR + (size_t)W * H * 3; // 24-bit, rows are 4-byte aligned (720)

constexpr uint32_t MIN_INTERVAL_MS = 1500;   // min spacing between captures

volatile bool requested = false;
volatile bool have_shot = false;
uint32_t shot_ms = 0;

uint8_t* px_buf  = nullptr;   // PSRAM, LVGL renders here
uint8_t* bmp_buf = nullptr;   // PSRAM, encoded BMP served from here

void put32(uint8_t* p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

void write_bmp_header() {
    memset(bmp_buf, 0, BMP_HDR);
    bmp_buf[0] = 'B'; bmp_buf[1] = 'M';
    put32(bmp_buf + 2,  BMP_LEN);     // file size
    put32(bmp_buf + 10, BMP_HDR);     // pixel data offset
    put32(bmp_buf + 14, 40);          // BITMAPINFOHEADER
    put32(bmp_buf + 18, W);
    put32(bmp_buf + 22, H);
    bmp_buf[26] = 1;                  // planes
    bmp_buf[28] = 24;                 // bpp
    put32(bmp_buf + 34, BMP_LEN - BMP_HDR);
}

bool ensure_buffers() {
    if (!px_buf)  px_buf  = (uint8_t*)ps_malloc(PX_LEN);
    if (!bmp_buf) bmp_buf = (uint8_t*)ps_malloc(BMP_LEN);
    return px_buf && bmp_buf;
}

void capture() {
    lv_draw_buf_t dbuf;
    lv_draw_buf_init(&dbuf, W, H, LV_COLOR_FORMAT_RGB565, W * 2, px_buf, PX_LEN);
    if (lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565,
                                     &dbuf) != LV_RESULT_OK) {
        log_w("[shot] snapshot failed");
        return;
    }

    write_bmp_header();
    // RGB565 → BGR888; BMP rows run bottom-up.
    for (int y = 0; y < H; y++) {
        const uint16_t* src = (const uint16_t*)(px_buf + (size_t)y * W * 2);
        uint8_t* dst = bmp_buf + BMP_HDR + (size_t)(H - 1 - y) * W * 3;
        for (int x = 0; x < W; x++) {
            uint16_t v = src[x];
            *dst++ = (v & 0x1F) << 3;          // B
            *dst++ = ((v >> 5) & 0x3F) << 2;   // G
            *dst++ = ((v >> 11) & 0x1F) << 3;  // R
        }
    }
    shot_ms   = millis();
    have_shot = true;
    log_i("[shot] captured %ux%u", W, H);
}

}  // namespace

bool request() {
    if (!ensure_buffers()) return false;
    // Rate-limit: each capture renders the whole screen via lv_snapshot, which
    // leans on the LVGL heap. Firing them back-to-back can exhaust it, and with
    // LV_ASSERT_MALLOC the failure path is while(1) — a frozen device. One
    // capture per MIN_INTERVAL is plenty for a manual screenshot.
    uint32_t now = millis();
    if (have_shot && now - shot_ms < MIN_INTERVAL_MS) return true;  // reuse last
    requested = true;
    return true;
}

void loop_tick() {
    if (!requested) return;
    requested = false;
    if (ensure_buffers()) capture();
}

const uint8_t* bmp(size_t& len_out) {
    if (!have_shot) { len_out = 0; return nullptr; }
    len_out = BMP_LEN;
    return bmp_buf;
}

uint32_t age_ms() {
    return have_shot ? millis() - shot_ms : UINT32_MAX;
}

}  // namespace shot

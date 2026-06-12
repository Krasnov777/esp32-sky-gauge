#include "display.h"
#include "board_config.h"

#include <LovyanGFX.hpp>   // LGFX_USE_V1 is set via build_flags
#include <lvgl.h>

namespace display {
namespace {

// ── LovyanGFX panel definition for Waveshare ESP32-S3-LCD-1.28 ───────────────
class LGFX_GC9A01 : public lgfx::LGFX_Device {
    lgfx::Panel_GC9A01 _panel;
    lgfx::Bus_SPI      _bus;
    lgfx::Light_PWM    _light;
public:
    LGFX_GC9A01() {
        {
            auto cfg = _bus.config();
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = board::LCD_SPI_FREQ_HZ;
            cfg.freq_read   = 16'000'000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = board::LCD_SCLK;
            cfg.pin_mosi    = board::LCD_MOSI;
            cfg.pin_miso    = -1;
            cfg.pin_dc      = board::LCD_DC;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs           = board::LCD_CS;
            cfg.pin_rst          = board::LCD_RST;
            cfg.pin_busy         = -1;
            cfg.memory_width     = board::LCD_WIDTH;
            cfg.memory_height    = board::LCD_HEIGHT;
            cfg.panel_width      = board::LCD_WIDTH;
            cfg.panel_height     = board::LCD_HEIGHT;
            cfg.offset_x         = 0;
            cfg.offset_y         = 0;
            cfg.offset_rotation  = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = false;
            cfg.invert           = true;   // GC9A01 typically requires inverted colors
            cfg.rgb_order        = false;
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl      = board::LCD_BL;
            cfg.invert      = false;
            cfg.freq        = 12000;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};

LGFX_GC9A01 tft;

// ── LVGL draw buffers (partial mode, 1/6 screen each) ────────────────────────
// Large bands keep the number of visible flush seams low (6 per full-screen
// refresh) — important for the radar mode, which animates every frame.
// Sized as a compromise: 60-line buffers ate enough internal RAM to starve
// the TLS handshakes in the radar/weather pollers (start_ssl_client: -1).
constexpr size_t BUF_LINES = 40;  // 240 / 6
constexpr size_t BUF_PIXELS = static_cast<size_t>(board::LCD_WIDTH) * BUF_LINES;
constexpr size_t BUF_BYTES  = BUF_PIXELS * sizeof(lv_color_t);

DMA_ATTR uint8_t buf_a[BUF_BYTES];
DMA_ATTR uint8_t buf_b[BUF_BYTES];

lv_display_t* lv_disp = nullptr;
uint8_t       bl_value = 200;
uint32_t      last_tick_ms = 0;

// LVGL flush callback — push rendered region to the panel via DMA.
// Signalling flush_ready immediately is safe with exactly two LVGL buffers:
// LVGL renders the next band into the *other* buffer, and the next
// pushImageDMA call waits for this transfer to finish before reusing the bus,
// so a buffer is never overwritten while DMA is still reading it.
void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    const uint32_t w = area->x2 - area->x1 + 1;
    const uint32_t h = area->y2 - area->y1 + 1;
    // LVGL renders RGB565 little-endian; the GC9A01 expects big-endian.
    lv_draw_sw_rgb565_swap(px_map, w * h);

    if (tft.getStartCount() == 0) tft.startWrite();   // hold the bus open
    tft.pushImageDMA(area->x1, area->y1, w, h,
                     reinterpret_cast<lgfx::swap565_t*>(px_map));
    lv_display_flush_ready(disp);
}

}  // namespace

void begin() {
    // Display init
    tft.init();
    tft.setRotation(0);
    tft.fillScreen(TFT_BLACK);

    // LVGL init
    lv_init();

    lv_disp = lv_display_create(board::LCD_WIDTH, board::LCD_HEIGHT);
    lv_display_set_flush_cb(lv_disp, flush_cb);
    lv_display_set_buffers(lv_disp, buf_a, buf_b, BUF_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_color_format(lv_disp, LV_COLOR_FORMAT_RGB565);

    last_tick_ms = millis();
    set_brightness(bl_value);
}

void tick() {
    const uint32_t now = millis();
    const uint32_t elapsed = now - last_tick_ms;
    if (elapsed > 0) {
        lv_tick_inc(elapsed);
        last_tick_ms = now;
    }
    lv_timer_handler();
}

void set_brightness(uint8_t value) {
    bl_value = value;
    tft.setBrightness(value);
}

uint8_t brightness() { return bl_value; }

}  // namespace display

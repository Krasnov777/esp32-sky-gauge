// ─────────────────────────────────────────────────────────────────────────────
// Hardware pin map for Waveshare ESP32-S3-LCD-1.28
// Source: https://www.waveshare.com/wiki/ESP32-S3-LCD-1.28 (official pinout)
// If your board revision differs, change values here only — nothing else hard-codes pins.
//
// NOTE: a community "corrected" pinout (RST=14, BL=2) exists for a different
// board revision. This unit needs the Waveshare-documented RST=12, BL=40 —
// verified on hardware: BL=2 left the screen dark (backlight off).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <Arduino.h>

namespace board {

// ── LCD (GC9A01, 240x240 round IPS, SPI) ─────────────────────────────────────
constexpr int LCD_SCLK = 10;
constexpr int LCD_MOSI = 11;
constexpr int LCD_DC   = 8;
constexpr int LCD_CS   = 9;
constexpr int LCD_RST  = 12;
constexpr int LCD_BL   = 40;

constexpr int LCD_WIDTH  = 240;
constexpr int LCD_HEIGHT = 240;
constexpr uint32_t LCD_SPI_FREQ_HZ = 40 * 1000 * 1000;

// ── QMI8658 IMU (I²C, unused for now — wired for future gesture modes) ───────
constexpr int IMU_SDA = 6;
constexpr int IMU_SCL = 7;
constexpr int IMU_INT1 = 4;
constexpr int IMU_INT2 = 3;

// ── Battery sense ────────────────────────────────────────────────────────────
constexpr int BAT_ADC = 1;

}  // namespace board

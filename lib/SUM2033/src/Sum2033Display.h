// Adafruit_GFX canvas for SUM2033 panels.
//
// Draw with any Adafruit_GFX function (text, lines, bitmaps, ...) or set 24 bit
// pixels directly, then call flip() to put the frame on the panel. Drawing
// happens in a normal RAM framebuffer, flip() takes a snapshot of it (about as
// long as one panel refresh at most), so you can start the next frame right
// away.
//
//   Sum2033Display display;          // 32x32, standard 2-scan panel
//   Sum2033Display display(32, 32, true);     // 32x32, 1/4-scan (four-scan)
//   Sum2033Display display(64, 32, true, 2);  // two 32x32 four-scan panels
//   display.begin();                 // default config = rev2.2 adapter wiring
//   display.fillScreen(0);
//   display.print("Hi");
//   display.flip();
//
// Four-scan panels light four rows per row address. Electrically they are
// wired as a chain twice as wide and half as high as the physical panel, so
// the driver is configured for width*2 and scan = height/4, and flip() maps
// the canvas onto that doubled buffer (same mapping as the
// FOUR_SCAN_32PX_HIGH mode of ESP32-HUB75-MatrixPanel-I2S-DMA).
//
// For a horizontal chain of N identical panels, pass the total canvas width
// and chainLength = N (e.g. 64x32 for two 32x32 panels). Each panel occupies
// a doubled (2 * width/N) slice of the DMA stream.

#pragma once

#include <Adafruit_GFX.h>

#include "Sum2033.h"

class Sum2033Display : public Adafruit_GFX {
 public:
  // fourScan: 1/4-scan panel (4 rows lit per address). Keep width/height at
  // the physical canvas resolution; the doubled DMA geometry is handled here.
  // chainLength: number of identical panels chained horizontally.
  explicit Sum2033Display(uint16_t width = 32, uint16_t height = 32, bool fourScan = false,
                          uint8_t chainLength = 1);
  ~Sum2033Display();

  // config.width and config.scan are taken from the canvas size (and from the
  // fourScan flag).
  bool begin(Sum2033Config config = Sum2033Config());
  void end();

  // Queues the canvas for display. Blocks only while the previous frame has not
  // reached the panel yet (frame rate is capped at the panel refresh rate).
  void flip();

  // Adafruit_GFX interface (RGB565 colors).
  void drawPixel(int16_t x, int16_t y, uint16_t color) override;
  void fillScreen(uint16_t color) override;

  // 24 bit color access, in rotated coordinates like the GFX functions.
  void setPixel(int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b);
  void clear() { fillScreen(0); }

  // Raw RGB888 framebuffer, WIDTH * HEIGHT * 3 bytes, not rotated.
  uint8_t* buffer() { return fb_; }

  Sum2033& panel() { return panel_; }

  // Four-scan pixel mapping of ONE panel: DMA pixel (dx, dy) of that panel's
  // slice of the stream (dx = 0 .. 2*panelW-1 in shift order, dy = 0 .. 2*scan-1,
  // dy < scan is R1/G1/B1 with row address dy, dy >= scan is R2/G2/B2 with row
  // address dy - scan) -> canvas pixel (x, y) of that panel. Return false to
  // leave the LED black. Panels of a chain are handled by flip().
  using FourScanMap = bool (*)(int dx, int dy, int panelW, int scan, int& x, int& y);
  // Default: FOUR_SCAN_32PX_HIGH of ESP32-HUB75-MatrixPanel-DMA (rows 0..scan-1
  // in the second half of the slice, rows scan..2*scan-1 in the first half).
  static bool defaultFourScanMap(int dx, int dy, int panelW, int scan, int& x, int& y);
  // nullptr restores the default. Takes effect with the next flip().
  void setFourScanMap(FourScanMap map) { fourScanMap_ = map ? map : defaultFourScanMap; }

  static uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
    return uint16_t(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }

 private:
  uint8_t* pixelAt(int16_t x, int16_t y);

  Sum2033 panel_;
  uint8_t* fb_ = nullptr;
  bool fourScan_ = false;
  uint8_t chainLength_ = 1;
  FourScanMap fourScanMap_ = defaultFourScanMap;
};

#include <Arduino.h>
#include <Sum2033Display.h>

// Two 32x32 P6 RGB modules (1/4 scan) chained horizontally: 64x32 canvas.
// RGB color bands scroll horizontally.
//
// Four-scan is handled by Sum2033Display (fourScan = true, chainLength = 2):
// the driver is configured for a 128x16 DMA frame (8 row addresses, 4 physical
// rows lit per address) and flip() maps the 64x32 canvas onto it.
//
// Wiring: Seengreat / Waveshare-style RGB Matrix Adapter Board (E), rev2.2,
// with an ESP32-S3-DevKitC-1. This is the V2.x S3 pin mapping from the board
// documentation:
//
//   R1=18  G1=8   B1=17
//   R2=16  G2=1   B2=15
//   A=7    B=48   C=6   D=47   E=2
//   CLK=5  LAT=21 OE=4 (OE is the GCLK input of the SUM2033)
static Sum2033Config panelConfig()
{
  Sum2033Config cfg;
  cfg.pins.r1 = 18;
  cfg.pins.g1 = 8;
  cfg.pins.b1 = 17;
  cfg.pins.r2 = 16;
  cfg.pins.g2 = 1;
  cfg.pins.b2 = 15;

  cfg.pins.a = 7;
  cfg.pins.b = 48;
  cfg.pins.c = 6;
  cfg.pins.d = 47;
  cfg.pins.e = 2;

  cfg.pins.clk = 5;
  cfg.pins.lat = 21;
  cfg.pins.oe  = 4; /* also GCLK */

  // width and scan are set by Sum2033Display::begin() from the canvas size:
  // two four-scan 32x32 panels -> DMA 128x16, scan = 8 (8 row addresses).

  cfg.brightness = 16;      // 0-63  (6 bits), 16 ~= 25 % for a dimmer test
  cfg.colorBits = 13;       // 10-13 (2 bits)
  cfg.gclkMultiplier = 16;  // 8 or 16

  // Every panel refresh also uploads a complete frame, so this sets both the
  // refresh rate and the max frame rate. Lower it if you see random speckles.
  cfg.clockHz = 10000000;
  return cfg;
}

// 64x32 canvas: two 32x32 four-scan panels chained horizontally.
Sum2033Display display(64, 32, /*fourScan=*/true, /*chainLength=*/2);

void setup()
{
  Serial.begin(115200);

  if (!display.begin(panelConfig())) {
    while (true) {
      Serial.println("SUM2033 init failed (bad config or not enough internal RAM)");
      delay(1000);
    }
  }

  Sum2033& panel = display.panel();
  Serial.printf("SUM2033: %dx%d canvas, CLK %u Hz, refresh %.1f Hz, DMA memory %u bytes\n",
                display.width(), display.height(), unsigned(panel.clockHz()),
                panel.refreshRate(), unsigned(panel.memoryUsage()));
}

void loop()
{
  static const uint16_t colors[] = {
      Sum2033Display::color565(255, 0, 0),    // red
      Sum2033Display::color565(0, 255, 0),    // green
      Sum2033Display::color565(0, 0, 255),    // blue
  };
  static const int kBandWidth = 16;  // pixels per color band
  static int offset = 0;

  const int w = display.width();
  const int h = display.height();
  for (int x = 0; x < w; ++x) {
    const int band = ((x + offset) / kBandWidth) % 3;
    display.drawFastVLine(x, 0, h, colors[band]);
  }
  display.flip();

  offset = (offset + 1) % (kBandWidth * 3);
  delay(30);
}

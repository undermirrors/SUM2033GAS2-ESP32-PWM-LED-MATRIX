#include <Arduino.h>
#include <Sum2033Display.h>

// Minimal bring-up test: fill the whole panel green, then red, then blue.
//
// Target panel: 32x32 P6 RGB module, HUB75 wiring.
//
// NOTE ON SCAN MODE: this driver lights 2 physical rows per row address
// (R1/G1/B1 + R2/G2/B2), which is the standard HUB75 arrangement. A 32x32
// panel driven that way has 16 row addresses (height = 2 * scan), so the
// config below uses scan = 16. A true 1/4-scan module (4 rows lit per
// address) needs a different driver; see the README.
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

  cfg.width = 32;           // 32 pixels wide = 2 x 16-channel chips
  cfg.scan  = 16;           // 32 rows / 2 rows per address

  cfg.brightness = 16;      // 0-63  (6 bits), 16 ~= 25 % for a dimmer test
  cfg.colorBits = 13;       // 10-13 (2 bits)
  cfg.gclkMultiplier = 16;  // 8 or 16

  // Every panel refresh also uploads a complete frame, so this sets both the
  // refresh rate and the max frame rate. For a 32x32 panel 10 MHz is already
  // ~580 Hz; go lower if you see random speckles.
  cfg.clockHz = 10000000;
  return cfg;
}

Sum2033Display display(32, 32);

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
  Serial.printf("SUM2033: CLK %u Hz, refresh %.1f Hz, DMA memory %u bytes\n",
                unsigned(panel.clockHz()), panel.refreshRate(), unsigned(panel.memoryUsage()));
}

void loop()
{
  static const uint16_t colors[] = {
      Sum2033Display::color565(0, 255, 0),    // green
      Sum2033Display::color565(255, 0, 0),    // red
      Sum2033Display::color565(0, 0, 255),    // blue
  };

  for (uint16_t color : colors) {
    display.fillScreen(color);
    display.flip();
    delay(1000);
  }
}

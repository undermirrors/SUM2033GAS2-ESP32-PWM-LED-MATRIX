#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoWebsockets.h>
#include <Sum2033Display.h>

#include <string.h>

#include "secrets.h"

using namespace websockets;

// Two 32x32 P6 RGB modules (1/4 scan) chained horizontally: 64x32 canvas.
// Frames received over WebSocket are shown on the chain.
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

// WebSocket server (same protocol as the original single-panel project).
// The server sends raw RGB888 frames; we display them and answer "ACK".
static const char* kServerHost = "192.168.1.97";
static const uint16_t kServerPort = 18080;
static const char* kServerPath = "/esp/ws";

// Geometry of one physical panel and of the chain.
static constexpr uint16_t kPanelW = 32;
static constexpr uint16_t kPanelH = 32;
static constexpr uint8_t kPanelCount = 2;

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

  cfg.brightness = 16;      // 0-63  (6 bits), 16 ~= 25 %
  cfg.colorBits = 13;       // 10-13 (2 bits)
  cfg.gclkMultiplier = 16;  // 8 or 16

  // Every panel refresh also uploads a complete frame, so this sets both the
  // refresh rate and the max frame rate. Lower it if you see random speckles.
  cfg.clockHz = 10000000;
  return cfg;
}

// 64x32 canvas: two 32x32 four-scan panels chained horizontally.
static Sum2033Display display(kPanelW * kPanelCount, kPanelH, /*fourScan=*/true,
                              /*chainLength=*/kPanelCount);
static WebsocketsClient webSocket;

// Copy an RGB888 frame into the display canvas.
//  - >= canvas size (64x32x3): treated as a full-canvas frame.
//  - >= one panel (32x32x3): treated as a single-panel image and tiled across
//    the chain, so the same picture shows on every panel.
// Returns false if the payload is too small.
static bool showFrame(const uint8_t* payload, size_t length) {
  uint8_t* fb = display.buffer();
  if (!fb) return false;

  const size_t panelBytes = size_t(kPanelW) * kPanelH * 3;
  const size_t canvasBytes = size_t(display.width()) * display.height() * 3;

  if (length >= canvasBytes) {
    memcpy(fb, payload, canvasBytes);
  } else if (length >= panelBytes) {
    for (uint8_t p = 0; p < kPanelCount; ++p) {
      for (uint16_t y = 0; y < kPanelH; ++y) {
        const uint8_t* src = payload + size_t(y) * kPanelW * 3;
        uint8_t* dst = fb + (size_t(y) * display.width() + size_t(p) * kPanelW) * 3;
        memcpy(dst, src, size_t(kPanelW) * 3);
      }
    }
  } else {
    return false;
  }

  display.flip();
  return true;
}

static void onMessageCallback(WebsocketsMessage message) {
  if (!message.isBinary()) {
    Serial.printf("WS text: %s\n", message.data().c_str());
    return;
  }

  const uint8_t* payload = reinterpret_cast<const uint8_t*>(message.c_str());
  const size_t length = message.length();
  Serial.printf("WS frame: %u bytes\n", unsigned(length));

  if (showFrame(payload, length)) {
    // Acknowledge so the server can send the next frame.
    webSocket.send("ACK");
  } else {
    Serial.printf("WS frame too small (need >= %u bytes)\n",
                  unsigned(size_t(kPanelW) * kPanelH * 3));
  }
}

static void onEventsCallback(WebsocketsEvent event, String data) {
  (void)data;
  switch (event) {
    case WebsocketsEvent::ConnectionOpened:
      Serial.println("WS connected");
      break;
    case WebsocketsEvent::ConnectionClosed:
      Serial.println("WS closed");
      break;
    case WebsocketsEvent::GotPing:
      Serial.println("WS ping");
      break;
    case WebsocketsEvent::GotPong:
      Serial.println("WS pong");
      break;
  }
}

static void connectWebSocket() {
  Serial.printf("WS connecting to ws://%s:%u%s\n", kServerHost, kServerPort, kServerPath);
  webSocket.connect(kServerHost, kServerPort, kServerPath);
}

void setup()
{
  Serial.begin(115200);
  delay(200);

  if (!display.begin(panelConfig())) {
    while (true) {
      Serial.println("SUM2033 init failed (bad config or not enough internal RAM)");
      delay(1000);
    }
  }

  display.fillScreen(0);
  display.flip();

  Sum2033& panel = display.panel();
  Serial.printf("SUM2033: %dx%d canvas, CLK %u Hz, refresh %.1f Hz, DMA memory %u bytes\n",
                display.width(), display.height(), unsigned(panel.clockHz()),
                panel.refreshRate(), unsigned(panel.memoryUsage()));

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("WiFi connecting to %s", WIFI_SSID);
  const uint32_t wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 30000) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi not connected yet, will keep retrying in loop");
  }

  webSocket.onMessage(onMessageCallback);
  webSocket.onEvent(onEventsCallback);
  connectWebSocket();
}

void loop()
{
  if (WiFi.status() != WL_CONNECTED) {
    // The ESP32 WiFi stack reconnects automatically; just wait for it.
    delay(100);
    return;
  }

  if (webSocket.available()) {
    webSocket.poll();
  } else {
    // Reconnect every 5 s while the server is unreachable.
    static uint32_t lastReconnectAttempt = 0;
    if (millis() - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = millis();
      connectWebSocket();
    }
  }
}

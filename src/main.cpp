// Diagnostic et réglage d'une dalle 32x32 "four-scan" à puces SUM2030GAS2.
//
// Le problème : la lib a été écrite pour le SUM2033 et met "scan - 1" (= 7 pour
// 8 adresses de ligne A/B/C) dans le champ "scan" du registre REG2. Ce champ
// n'a été vérifié que pour 1/32 scan (tout à 1). Le SUM2030 (1/16 scan max)
// semble le lire autrement : sa mémoire de lignes reboucle tous les 4 au lieu
// de 8, d'où "seules les adresses 0..3 s'affichent, et chacune 4 lignes plus
// loin aussi". Ce sketch permet de changer ce champ à chaud et de le balayer,
// sans toucher à la géométrie (64x16 en DMA, scan = 8).
//
// Moniteur série à 115200 bauds, taper "h" pour l'aide.
//
// Mires (toutes sauf canvas écrivent directement dans le flux DMA, sans le
// mapping four-scan) :
//   a <n>   (défaut, n = 7) allume TOUS les pixels de l'adresse de ligne n
//           (R1 en rouge, R2 en bleu). Ne dépend pas du mapping des pixels.
//           BON réglage : exactement 4 lignes physiques (2 rouges en haut,
//           2 bleues en bas), et 4 lignes différentes pour chaque n = 0..7.
//           MAUVAIS : 8 lignes (adresse doublée) ou rien.
//   stairs  chaque adresse a (0..7) allume une barre de 4*(a+1) pixels :
//           4, 8, ..., 32. Couleur selon la moitié de la chaîne et les lignes
//           de données :
//             R1, 1re moitié = rouge     R1, 2e moitié = vert
//             R2, 1re moitié = bleu      R2, 2e moitié = blanc
//           Si le mapping par défaut est le bon : 4 blocs de 8 lignes, chacun
//           un escalier complet 4 -> 32 px. Sinon les barres peuvent être
//           coupées en morceaux ou partir de la droite, mais il doit y avoir
//           8 longueurs différentes par couleur, sans motif qui se répète
//           toutes les 4 lignes.
//   walk    un seul pixel DMA à la fois (Entrée = suivant), pour relever le
//           mapping réel si le mapping four-scan par défaut est faux.
//   canvas  passe par Sum2033Display (mapping four-scan) : cadre, diagonale,
//           coins colorés (HG rouge, HD vert, BG bleu, BD jaune) et "OK".

#include <Arduino.h>
#include <Sum2033Display.h>

static const uint16_t kPanelW = 32;
static const uint16_t kPanelCount = 1;
static const uint16_t kWidth = kPanelW * kPanelCount;  // 32
static const uint16_t kHeight = 32;

// Valeur du champ "scan" de REG2 au démarrage. -1 = scan - 1 = 7, c'est le
// comportement d'origine (cassé sur SUM2030). 15 = hypothèse "le SUM2030 lit
// le champ un bit plus haut". À confirmer avec "sweep", puis à reporter ici
// (et dans cfg.scanField de ton vrai sketch).
static const int kScanField = 15;

static Sum2033Display display(kWidth, kHeight, /*fourScan=*/true, /*chainLength=*/kPanelCount);

static Sum2033Config panelConfig() {
  Sum2033Config cfg;
  cfg.pins.r1 = 18; cfg.pins.g1 = 8; cfg.pins.b1 = 17;
  cfg.pins.r2 = 16; cfg.pins.g2 = 1; cfg.pins.b2 = 15;
  cfg.pins.a = 7; cfg.pins.b = 48; cfg.pins.c = 6;
  cfg.pins.d = -1;
  cfg.pins.e = -1;
  cfg.pins.clk = 5; cfg.pins.lat = 21; cfg.pins.oe = 4;

  cfg.brightness = 16;
  cfg.colorBits = 10;
  cfg.gclkMultiplier = 8;  // le SUM2030 ne fait que x8 (datasheet : "最高可达8倍频")
  cfg.clockHz = 4000000;

  cfg.scanField = kScanField;
  return cfg;
}

// ---------------------------------------------------------------------------

enum class Mode { Address, Stairs, Walk, Canvas };

static Mode mode = Mode::Address;
static bool dirty = true;
static int probeAddr = 7;  // dernière adresse : la première à disparaître si la puce reboucle trop tôt
static int walkX = 0, walkY = 0;

static bool sweeping = false;
static int sweepValue = 0;
static int sweepSaved = -1;
static uint32_t sweepPeriodMs = 4000;
static uint32_t sweepNextMs = 0;

static Sum2033& panel() { return display.panel(); }

static sum2033::Rgb16 rgb(uint8_t r, uint8_t g, uint8_t b) {
  return sum2033::Rgb16{panel().toLinear(r), panel().toLinear(g), panel().toLinear(b)};
}

static void printRegs() {
  Serial.printf("scanField = %d (%s)  REG1 = 0x%04X  REG2 = 0x%04X  REG3 = 0x%04X  luminosite = %u\n",
                panel().scanField(),
                panel().scanField() == panel().layout().scan() - 1 ? "= scan-1, comme le SUM2033" : "force",
                panel().regValue(1), panel().regValue(2), panel().regValue(3), panel().brightness());
}

static void printHelp() {
  Serial.println();
  Serial.println("=== SUM2030 four-scan : commandes ===");
  Serial.println("  s <n>        champ scan de REG2 = n (0..31), s -1 = auto (scan-1 = 7)");
  Serial.println("  sweep [ms]   balaye s = 0..31 sur la mire 'a' (defaut 4000 ms/valeur)");
  Serial.println("  stop         arrete le balayage");
  Serial.println("  a <n>        allume toute l'adresse de ligne n (0..7) : R1 rouge, R2 bleu");
  Serial.println("  stairs | walk | canvas   autres mires");
  Serial.println("  (walk) Entree = pixel suivant, w <x> <y> = aller au pixel DMA (x, y)");
  Serial.println("  b <n>        luminosite globale 0..63");
  Serial.println("  r1|r2|r3 <v> valeur brute d'un registre (0x.... ou decimal), -1 = calculee");
  Serial.println("  h            cette aide");
  Serial.println("Bon reglage : 'a <n>' allume exactement 4 lignes (2 rouges, 2 bleues), differentes");
  Serial.println("pour chaque n. 8 lignes ou aucune = mauvaise valeur de s.");
  Serial.printf("DMA : %d x %d, scan = %d adresses\n", panel().width(), panel().height(), panel().layout().scan());
  printRegs();
}

static void drawAddress() {
  const int scan = panel().layout().scan();
  panel().render([&](int, int dy) -> sum2033::Rgb16 {
    if (dy % scan != probeAddr) return rgb(0, 0, 0);
    return dy < scan ? rgb(255, 0, 0) : rgb(0, 0, 255);
  });
  if (!sweeping) Serial.printf("Adresse %d allumee (R1 rouge, R2 bleu) : il faut exactement 4 lignes.\n", probeAddr);
}

static void drawStairs() {
  const int scan = panel().layout().scan();
  const int sliceW = 2 * kPanelW;  // largeur DMA d'un panneau
  panel().render([&](int dx, int dy) -> sum2033::Rgb16 {
    const int local = dx % sliceW;
    const bool second = local >= kPanelW;
    const int x = second ? local - kPanelW : local;
    const bool lower = dy >= scan;
    const int addr = lower ? dy - scan : dy;
    const int bar = (addr + 1) * kPanelW / scan;  // 4, 8, ..., 32
    if (x >= bar) return rgb(0, 0, 0);
    if (!lower) return second ? rgb(0, 255, 0) : rgb(255, 0, 0);
    return second ? rgb(160, 160, 160) : rgb(0, 0, 255);
  });
}

static void drawWalk() {
  const int scan = panel().layout().scan();
  panel().render([&](int dx, int dy) -> sum2033::Rgb16 {
    return (dx == walkX && dy == walkY) ? rgb(255, 255, 255) : rgb(0, 0, 0);
  });
  const int sliceW = 2 * kPanelW;
  const int panelIdx = walkX / sliceW;
  const int local = walkX % sliceW;
  int cx = 0, cy = 0;
  Sum2033Display::defaultFourScanMap(local, walkY, kPanelW, scan, cx, cy);
  Serial.printf("DMA (x=%2d, y=%2d) : %s, adresse %d, %s moitie de la chaine, panneau %d"
                "  -> mapping par defaut = canvas (%d, %d)\n",
                walkX, walkY, walkY < scan ? "R1" : "R2", walkY % scan, local < kPanelW ? "1re" : "2e",
                panelIdx, cx + panelIdx * kPanelW, cy);
}

static void drawCanvas() {
  display.fillScreen(0);
  const uint16_t white = Sum2033Display::color565(90, 90, 90);
  display.drawRect(0, 0, kWidth, kHeight, white);
  display.drawLine(0, 0, kWidth - 1, kHeight - 1, Sum2033Display::color565(255, 0, 255));
  display.fillRect(0, 0, 3, 3, Sum2033Display::color565(255, 0, 0));
  display.fillRect(kWidth - 3, 0, 3, 3, Sum2033Display::color565(0, 255, 0));
  display.fillRect(0, kHeight - 3, 3, 3, Sum2033Display::color565(0, 0, 255));
  display.fillRect(kWidth - 3, kHeight - 3, 3, 3, Sum2033Display::color565(255, 255, 0));
  display.setTextColor(Sum2033Display::color565(0, 255, 255));
  display.setTextSize(1);
  display.setCursor(kWidth / 2 - 5, kHeight / 2 - 4);
  display.print("OK");
  display.flip();
}

static void redraw() {
  switch (mode) {
    case Mode::Address: drawAddress(); break;
    case Mode::Stairs: drawStairs(); break;
    case Mode::Walk: drawWalk(); break;
    case Mode::Canvas: drawCanvas(); break;
  }
  dirty = false;
}

static void startSweep(uint32_t periodMs) {
  sweepSaved = panel().scanField();
  sweepPeriodMs = periodMs < 500 ? 500 : periodMs;
  sweepValue = -1;
  sweepNextMs = millis();
  sweeping = true;
  mode = Mode::Address;
  Serial.printf("Balayage du champ scan 0..31, %lu ms par valeur, adresse %d allumee. Note les valeurs"
                " qui allument exactement 4 lignes (2 rouges, 2 bleues).\n",
                (unsigned long)sweepPeriodMs, probeAddr);
}

static void stopSweep(bool restore) {
  if (!sweeping) return;
  sweeping = false;
  if (restore) panel().setScanField(sweepSaved);
  dirty = true;
  Serial.println("Balayage arrete.");
  printRegs();
}

static void stepSweep() {
  if (!sweeping || int32_t(millis() - sweepNextMs) < 0) return;
  if (++sweepValue > 31) {
    Serial.println("Balayage termine : tape s <valeur> avec la bonne valeur trouvee.");
    stopSweep(true);
    return;
  }
  panel().setScanField(sweepValue);
  dirty = true;
  sweepNextMs = millis() + sweepPeriodMs;
  Serial.printf("[sweep] scanField = %2d  (REG2 = 0x%04X)\n", sweepValue, panel().regValue(2));
}

static long parseNumber(const char* s, bool* ok) {
  while (*s == ' ') ++s;
  char* end = nullptr;
  const long v = strtol(s, &end, 0);  // accepte 0x...
  *ok = end != s;
  return v;
}

static void handleCommand(char* line) {
  while (*line == ' ') ++line;
  bool ok = false;

  if (*line == '\0' || strcmp(line, "n") == 0) {  // Entrée : pixel suivant en mode walk
    if (mode != Mode::Walk) return;
    if (++walkX >= panel().width()) {
      walkX = 0;
      if (++walkY >= panel().height()) walkY = 0;
    }
    dirty = true;
    return;
  }
  if (strcmp(line, "h") == 0 || strcmp(line, "?") == 0) {
    printHelp();
    return;
  }
  if (strncmp(line, "sweep", 5) == 0) {
    const long ms = parseNumber(line + 5, &ok);
    startSweep(ok && ms > 0 ? uint32_t(ms) : 4000);
    return;
  }
  if (strcmp(line, "stop") == 0) {
    stopSweep(false);
    return;
  }
  if (strcmp(line, "stairs") == 0 || strcmp(line, "walk") == 0 || strcmp(line, "canvas") == 0) {
    stopSweep(true);
    mode = line[0] == 's' ? Mode::Stairs : line[0] == 'w' ? Mode::Walk : Mode::Canvas;
    dirty = true;
    return;
  }
  if (line[0] == 'a' && line[1] == ' ') {
    const long v = parseNumber(line + 1, &ok);
    if (ok && v >= 0 && v < panel().layout().scan()) {
      probeAddr = int(v);
      if (!sweeping) mode = Mode::Address;
      dirty = true;
    } else {
      Serial.printf("a <n> : n = 0..%d\n", panel().layout().scan() - 1);
    }
    return;
  }
  if (line[0] == 'w' && line[1] == ' ') {
    char* end = nullptr;
    const long x = strtol(line + 2, &end, 0);
    const long y = strtol(end, nullptr, 0);
    if (x >= 0 && x < panel().width() && y >= 0 && y < panel().height()) {
      walkX = int(x);
      walkY = int(y);
      mode = Mode::Walk;
      dirty = true;
    } else {
      Serial.println("w <x> <y> : hors limites");
    }
    return;
  }
  if (line[0] == 's' && (line[1] == ' ' || line[1] == '-' || isdigit((unsigned char)line[1]))) {
    const long v = parseNumber(line + 1, &ok);
    if (!ok || v > 31) {
      Serial.println("s <n> : n = 0..31, ou -1 pour auto");
      return;
    }
    stopSweep(false);
    panel().setScanField(int(v));
    dirty = true;
    printRegs();
    return;
  }
  if (line[0] == 'b' && line[1] == ' ') {
    const long v = parseNumber(line + 1, &ok);
    if (ok && v >= 0 && v <= 63) {
      panel().setBrightness(uint8_t(v));
      dirty = true;
      printRegs();
    } else {
      Serial.println("b <n> : n = 0..63");
    }
    return;
  }
  if (line[0] == 'r' && line[1] >= '1' && line[1] <= '3' && line[2] == ' ') {
    const long v = parseNumber(line + 2, &ok);
    if (ok && v >= -1 && v <= 0xFFFF) {
      panel().setRegOverride(line[1] - '0', int32_t(v));
      dirty = true;
      printRegs();
    } else {
      Serial.println("r1|r2|r3 <v> : v = 0..0xFFFF, ou -1");
    }
    return;
  }
  Serial.printf("Commande inconnue : \"%s\" (h pour l'aide)\n", line);
}

static void pollSerial() {
  static char buf[48];
  static size_t len = 0;
  static bool lastCr = false;
  while (Serial.available() > 0) {
    const char c = char(Serial.read());
    if (c == '\n' && lastCr) {  // CR LF = une seule fin de ligne
      lastCr = false;
      continue;
    }
    lastCr = c == '\r';
    if (c == '\r' || c == '\n') {
      buf[len] = '\0';
      handleCommand(buf);
      len = 0;
    } else if (c == '\b' || c == 127) {
      if (len > 0) --len;
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  if (!display.begin(panelConfig())) {
    Serial.println("Erreur d'initialisation SUM2033/SUM2030 !");
    while (true) delay(1000);
  }
  printHelp();
}

void loop() {
  pollSerial();
  stepSweep();
  if (dirty) redraw();
  delay(5);
}

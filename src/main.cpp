// CYD (ESP32-2432S028) weather & clock dashboard
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <TFT_eSPI.h>
#include <time.h>
#include <algorithm>
#include "config.h"
#include "fonts.h"
#include "web_index.h"

#define FIRMWARE_VERSION "1.3"

// ---------------------------------------------------------------- settings
struct Settings {
  bool use24h = DEFAULT_24H, fahrenheit = DEFAULT_FAHRENHEIT;
  uint8_t brightDay = DEFAULT_BRIGHT_DAY, brightNight = DEFAULT_BRIGHT_NIGHT;
  bool autoDim = true;
  uint8_t dimStart = DEFAULT_DIM_START, dimEnd = DEFAULT_DIM_END;
  bool wifiOn = true, airplane = false;
  bool autoLoc = true, showCity = true;
  float lat = 0, lon = 0;
  String city = "";
  uint8_t rotation = DEFAULT_ROTATION;
  uint16_t refreshMin = DEFAULT_REFRESH_MIN;
  int16_t tsX0 = 200, tsX1 = 3700, tsY0 = 240, tsY1 = 3800;  // touch calibration
  bool anim = true;                    // animated weather effects
  bool ledOn = true;                   // ambient LED on the back
  uint8_t ledBright = 60;              // 5-100 %
} cfg;

Preferences prefs;

static void loadSettings() {
  prefs.begin("dash", true);
  cfg.use24h = prefs.getBool("h24", cfg.use24h);
  cfg.fahrenheit = prefs.getBool("fahr", cfg.fahrenheit);
  cfg.brightDay = prefs.getUChar("bday", cfg.brightDay);
  cfg.brightNight = prefs.getUChar("bnight", cfg.brightNight);
  cfg.autoDim = prefs.getBool("autodim", cfg.autoDim);
  cfg.dimStart = prefs.getUChar("dimS", cfg.dimStart);
  cfg.dimEnd = prefs.getUChar("dimE", cfg.dimEnd);
  cfg.wifiOn = prefs.getBool("wifi", cfg.wifiOn);
  cfg.airplane = prefs.getBool("air", cfg.airplane);
  cfg.autoLoc = prefs.getBool("autoloc", cfg.autoLoc);
  cfg.showCity = prefs.getBool("showcity", cfg.showCity);
  cfg.lat = prefs.getFloat("lat", cfg.lat);
  cfg.lon = prefs.getFloat("lon", cfg.lon);
  cfg.city = prefs.getString("city", cfg.city);
  cfg.rotation = prefs.getUChar("rot", cfg.rotation);
  cfg.refreshMin = prefs.getUShort("refresh", cfg.refreshMin);
  cfg.tsX0 = prefs.getShort("tsx0", cfg.tsX0);
  cfg.tsX1 = prefs.getShort("tsx1", cfg.tsX1);
  cfg.tsY0 = prefs.getShort("tsy0", cfg.tsY0);
  cfg.tsY1 = prefs.getShort("tsy1", cfg.tsY1);
  cfg.anim = prefs.getBool("anim", cfg.anim);
  cfg.ledOn = prefs.getBool("led", cfg.ledOn);
  cfg.ledBright = prefs.getUChar("ledb", cfg.ledBright);
  prefs.end();
}

static void saveSettings() {
  prefs.begin("dash", false);
  prefs.putBool("h24", cfg.use24h);
  prefs.putBool("fahr", cfg.fahrenheit);
  prefs.putUChar("bday", cfg.brightDay);
  prefs.putUChar("bnight", cfg.brightNight);
  prefs.putBool("autodim", cfg.autoDim);
  prefs.putUChar("dimS", cfg.dimStart);
  prefs.putUChar("dimE", cfg.dimEnd);
  prefs.putBool("wifi", cfg.wifiOn);
  prefs.putBool("air", cfg.airplane);
  prefs.putBool("autoloc", cfg.autoLoc);
  prefs.putBool("showcity", cfg.showCity);
  prefs.putFloat("lat", cfg.lat);
  prefs.putFloat("lon", cfg.lon);
  prefs.putString("city", cfg.city);
  prefs.putUChar("rot", cfg.rotation);
  prefs.putUShort("refresh", cfg.refreshMin);
  prefs.putShort("tsx0", cfg.tsX0);
  prefs.putShort("tsx1", cfg.tsX1);
  prefs.putShort("tsy0", cfg.tsY0);
  prefs.putShort("tsy1", cfg.tsY1);
  prefs.putBool("anim", cfg.anim);
  prefs.putBool("led", cfg.ledOn);
  prefs.putUChar("ledb", cfg.ledBright);
  prefs.end();
}

// ---------------------------------------------------------------- display
static const int W = 320, H = 240, STRIP = 40;  // rendered in 6 strips of 320x40
TFT_eSPI tft;
TFT_eSprite spr(&tft);
static int stripY = 0;
static int clipX0 = 0, clipX1 = W;  // horizontal range being rendered
static int clipY1 = STRIP;          // end row (exclusive) of the strip being rendered

SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

struct RGB { float r, g, b; };

static inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static inline float smooth(float e0, float e1, float x) {
  float t = clamp01((x - e0) / (e1 - e0));
  return t * t * (3 - 2 * t);
}
static inline RGB mix(RGB a, RGB b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
static inline uint16_t to565(RGB c) {
  int r = c.r < 0 ? 0 : (c.r > 255 ? 255 : (int)c.r);
  int g = c.g < 0 ? 0 : (c.g > 255 ? 255 : (int)c.g);
  int b = c.b < 0 ? 0 : (c.b > 255 ? 255 : (int)c.b);
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static inline RGB from565(uint16_t c) {
  return {(float)(((c >> 11) & 31) * 255 / 31), (float)(((c >> 5) & 63) * 255 / 63), (float)((c & 31) * 255 / 31)};
}

static uint16_t* sbuf = nullptr;  // strip buffer (RGB565, byte-swapped)

// Blend a colour onto the current strip at screen coordinates.
static inline void blendPx(int x, int y, RGB c, float a) {
  int sy = y - stripY;
  if (x < clipX0 || x >= clipX1 || sy < 0 || y >= clipY1 || a <= 0.003f) return;
  uint16_t& px = sbuf[sy * W + x];
  uint16_t v;
  if (a >= 0.997f) {
    v = to565(c);
  } else {
    uint16_t d = (px >> 8) | (px << 8);
    int ia = (int)(a * 256), dr = (d >> 8) & 0xF8, dg = (d >> 3) & 0xFC, db = (d << 3) & 0xF8;
    int r = dr + ((((int)c.r - dr) * ia) >> 8), g = dg + ((((int)c.g - dg) * ia) >> 8), b = db + ((((int)c.b - db) * ia) >> 8);
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  }
  px = (v >> 8) | (v << 8);
}

// Fill any shape described by a coverage function (0..1 at pixel centre).
template <typename F>
void fillShape(float x0, float y0, float x1, float y1, RGB c, float alpha, F cov) {
  int ys = max((int)floorf(y0), stripY), ye = min((int)ceilf(y1), clipY1 - 1);
  int xs = max((int)floorf(x0), clipX0), xe = min((int)ceilf(x1), clipX1 - 1);
  for (int y = ys; y <= ye; y++)
    for (int x = xs; x <= xe; x++) {
      float a = cov(x + 0.5f, y + 0.5f) * alpha;
      if (a > 0.003f) blendPx(x, y, c, a);
    }
}

// Same, with the colour chosen per pixel.
template <typename C, typename F>
void fillShapeCol(float x0, float y0, float x1, float y1, float alpha, C col, F cov) {
  int ys = max((int)floorf(y0), stripY), ye = min((int)ceilf(y1), clipY1 - 1);
  int xs = max((int)floorf(x0), clipX0), xe = min((int)ceilf(x1), clipX1 - 1);
  for (int y = ys; y <= ye; y++)
    for (int x = xs; x <= xe; x++) {
      float a = cov(x + 0.5f, y + 0.5f) * alpha;
      if (a > 0.003f) blendPx(x, y, col(x + 0.5f, y + 0.5f), a);
    }
}

static inline float circCov(float px, float py, float cx, float cy, float r) {
  float dx = px - cx, dy = py - cy;
  return clamp01(r - sqrtf(dx * dx + dy * dy) + 0.5f);
}
static inline float capsCov(float px, float py, float ax, float ay, float bx, float by, float r) {
  float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
  float h = clamp01((pax * bax + pay * bay) / (bax * bax + bay * bay));
  float dx = pax - bax * h, dy = pay - bay * h;
  return clamp01(r - sqrtf(dx * dx + dy * dy) + 0.5f);
}
static inline float rrectCov(float px, float py, float x0, float y0, float x1, float y1, float r) {
  float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
  float qx = fabsf(px - cx) - ((x1 - x0) / 2 - r), qy = fabsf(py - cy) - ((y1 - y0) / 2 - r);
  float ox = fmaxf(qx, 0), oy = fmaxf(qy, 0);
  float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r;
  return clamp01(0.5f - d);
}
// Same result, but skips the square root away from the corners.
static inline float rrectCovFast(float px, float py, float x0, float y0, float x1, float y1, float r) {
  if ((px > x0 + r && px < x1 - r) || (py > y0 + r && py < y1 - r)) {
    float d = px - x0, e = x1 - px, f = py - y0, g = y1 - py;
    if (e < d) d = e;
    if (f < d) d = f;
    if (g < d) d = g;
    d += 0.5f;
    return d <= 0 ? 0 : (d >= 1 ? 1 : d);
  }
  return rrectCov(px, py, x0, y0, x1, y1, r);
}
static bool inTri(float px, float py, float ax, float ay, float bx, float by, float cx, float cy) {
  float d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
  float d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
  float d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
  bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
  return !(neg && pos);
}
// 3x3 supersampled triangle coverage
static float triCov(float px, float py, float ax, float ay, float bx, float by, float cx, float cy) {
  int hit = 0;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      if (inTri(px + (i - 1) / 3.0f, py + (j - 1) / 3.0f, ax, ay, bx, by, cx, cy)) hit++;
  return hit / 9.0f;
}
static void fillRRect(float x0, float y0, float x1, float y1, float r, RGB c, float a) {
  fillShape(x0, y0, x1, y1, c, a, [=](float px, float py) { return rrectCovFast(px, py, x0, y0, x1, y1, r); });
}
// Rounded rectangle with a thin light outline (the frosted "glass" panels).
static void glassRect(float x0, float y0, float x1, float y1, float r, RGB c, float a, float border) {
  int ys = max((int)floorf(y0), stripY), ye = min((int)ceilf(y1), clipY1 - 1);
  int xs = max((int)floorf(x0), clipX0), xe = min((int)ceilf(x1), clipX1 - 1);
  for (int y = ys; y <= ye; y++) {
    float py = y + 0.5f;
    bool midRow = py > y0 + r && py < y1 - r, innerRow = py > y0 + 2 && py < y1 - 2;
    for (int x = xs; x <= xe; x++) {
      float px = x + 0.5f;
      // well inside the panel: plain fill, no edge maths
      if ((midRow && px > x0 + 2 && px < x1 - 2) || (innerRow && px > x0 + r && px < x1 - r)) { blendPx(x, y, c, a); continue; }
      float outer = rrectCovFast(px, py, x0, y0, x1, y1, r);
      if (outer <= 0.003f) continue;
      float inner = rrectCovFast(px, py, x0 + 1, y0 + 1, x1 - 1, y1 - 1, r - 1);
      blendPx(x, y, c, a * outer);
      if (outer - inner > 0.003f) blendPx(x, y, {255, 255, 255}, border * (outer - inner));
    }
  }
}

// ---------------------------------------------------------------- state
struct Weather {
  bool valid = false;
  float temp = 0, feels = 0, tmax = 0, tmin = 0;  // always °C
  int code = 0;
  bool isDay = true;
  int hourCount = 0;
  time_t hourTime[3];
  float hourTemp[3];
  int hourCode[3];
  bool hourDay[3];
  time_t fetchedAt = 0;
  // daily forecast (index 0 = today)
  int dayCount = 0;
  time_t dayTime[6], sunrise[6], sunset[6];
  float dayMax[6], dayMin[6], dayUv[6];
  int dayCode[6], dayRain[6];
  // rain over the next 12 hours
  int rainCount = 0;
  time_t rainTime[12];
  uint8_t rainProb[12];
  float rainMm[12];
  // air quality
  bool airValid = false, aqiEU = true, pollenValid = false;
  int aqi = 0;
  float pm25 = 0, pm10 = 0, ozone = 0, no2 = 0, uv = 0, pollen = 0;
} wx;

static float lat = 0, lon = 0;
static String ipCity = "";
static long utcOffset = 0;  // seconds, from IP lookup / Open-Meteo
static bool haveLocation = false;
static int hourOverride = -1;  // preview a time of day
static uint32_t previewUntil = 0;
static bool screenshotPending = false;
static bool forceFetch = true;
static bool refreshing = false;

enum Mode { MODE_MESSAGE, MODE_DASH, MODE_CC, MODE_CAL, MODE_WIFI, MODE_KEYB };
static Mode mode = MODE_MESSAGE;

// Dashboard pages, swiped left/right
enum Page { PAGE_HOME, PAGE_FORECAST, PAGE_SUN, PAGE_RAIN, PAGE_AIR, PAGE_COUNT };
static int page = PAGE_HOME;
static uint32_t lastInteraction = 0;

static int codeOverride = -1;  // preview a weather condition (effects + LED)
static uint32_t codeOverrideUntil = 0;
static int curCode() { return codeOverride >= 0 ? codeOverride : wx.code; }

// ---------------------------------------------------------------- background
enum Scheme { MORNING, AFTERNOON, EVENING, NIGHT };
struct Palette { RGB top, mid, horizon, far, nearB; float bokeh; };
static const Palette PALETTES[] = {
  {{48, 80, 122}, {134, 158, 188}, {242, 190, 142}, {92, 100, 122}, {38, 44, 60}, 0.35f},   // morning
  {{30, 88, 148}, {92, 146, 198}, {186, 212, 230}, {88, 106, 130}, {36, 46, 64}, 0.12f},    // afternoon
  {{34, 48, 72}, {76, 86, 104}, {218, 140, 82}, {58, 60, 72}, {20, 22, 30}, 0.95f},         // evening
  {{6, 10, 24}, {18, 26, 50}, {56, 50, 80}, {20, 22, 36}, {7, 8, 15}, 1.0f},                // night
};
static Scheme scheme = EVENING;
static bool customBg[4];
static File bgFile;  // kept open so animation frames can read small pieces quickly
static int bgFileSlot = -1;

static String bgPath(int slot) { return String("/bg") + slot + ".raw"; }
static void closeBgFile() {
  if (bgFile) bgFile.close();
  bgFileSlot = -1;
}
static void scanBackgrounds() {
  closeBgFile();
  for (int i = 0; i < 4; i++) customBg[i] = LittleFS.exists(bgPath(i));
}

static int16_t farTop[W], nearTop[W];
struct Bokeh { int16_t x, y; uint8_t r; RGB c; float a; };
static Bokeh bokeh[56];
struct Star { int16_t x, y; float r, a; uint16_t period, offset; };
static Star stars[26];
static bool starsOn = false;  // clear night sky on the built-in background

// 0..1 sawtooth that repeats every periodMs — wraps seamlessly, unlike millis()/1000.0f
static float phase(uint32_t periodMs, uint32_t offset = 0) { return ((millis() + offset) % periodMs) / (float)periodMs; }

static uint32_t rngState = 0x2432028;
static uint32_t rnd() { rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5; return rngState; }

static void buildSkyline(int16_t* out, int minW, int maxW, int minTop, int maxTop) {
  int16_t raw[W];
  int x = 0;
  while (x < W) {
    int w = minW + rnd() % (maxW - minW);
    int top = minTop + rnd() % (maxTop - minTop);
    if (rnd() % 6 == 0) top -= 18 + rnd() % 22;  // the odd tower
    for (int i = 0; i < w && x + i < W; i++) raw[x + i] = top;
    x += w;
  }
  for (int i = 0; i < W; i++) {  // soften — the city is out of focus
    int sum = 0, n = 0;
    for (int k = -3; k <= 3; k++) { int j = i + k; if (j >= 0 && j < W) { sum += raw[j]; n++; } }
    out[i] = sum / n;
  }
}

static void initBackground() {
  buildSkyline(farTop, 10, 30, 112, 168);
  buildSkyline(nearTop, 14, 44, 150, 205);
  static const RGB lights[] = {{255, 196, 120}, {255, 222, 168}, {255, 156, 86}, {210, 226, 255}, {255, 180, 100}};
  for (auto& b : bokeh) {
    b.x = rnd() % W;
    int top = min(farTop[b.x], nearTop[b.x]);
    b.y = top + 4 + rnd() % max(8, H - top - 4);
    b.r = 3 + rnd() % 8;
    b.c = lights[rnd() % 5];
    b.a = 0.25f + (rnd() % 100) / 260.0f;
  }
  for (auto& s : stars) {
    do {
      s.x = 4 + rnd() % (W - 8);
      s.y = 4 + rnd() % 120;
    } while (s.y > farTop[s.x] - 10);
    s.r = 0.5f + (rnd() % 100) / 140.0f;
    s.a = 0.35f + (rnd() % 100) / 160.0f;
    s.period = 1800 + rnd() % 3200;
    s.offset = rnd() % 5000;
  }
}

static float starAlpha(const Star& s) { return s.a * (0.5f + 0.5f * sinf(phase(s.period, s.offset) * TWO_PI)); }

static void drawStars() {
  for (auto& s : stars) {
    if (s.y + 3 < stripY || s.y - 3 >= clipY1 || s.x + 3 < clipX0 || s.x - 3 >= clipX1) continue;
    float a = starAlpha(s), r = s.r;
    fillShape(s.x - 3, s.y - 3, s.x + 3, s.y + 3, {255, 250, 235}, a, [=](float px, float py) {
      float dx = px - s.x, dy = py - s.y;
      return 1.0f - smooth(r * 0.4f, r + 1.2f, sqrtf(dx * dx + dy * dy));
    });
  }
}

static bool drawCustomBackground() {
  if (!customBg[scheme]) return false;
  if (bgFileSlot != scheme) {
    closeBgFile();
    bgFile = LittleFS.open(bgPath(scheme), "r");
    if (!bgFile) return false;
    bgFileSlot = scheme;
  }
  uint16_t* buf = (uint16_t*)spr.getPointer();
  int rows = clipY1 - stripY;
  if (clipX0 == 0 && clipX1 == W) {
    bgFile.seek((size_t)stripY * W * 2);
    return bgFile.read((uint8_t*)buf, rows * W * 2) == (size_t)rows * W * 2;
  }
  size_t len = (clipX1 - clipX0) * 2;
  for (int r = 0; r < rows; r++) {
    bgFile.seek(((size_t)(stripY + r) * W + clipX0) * 2);
    if (bgFile.read((uint8_t*)(buf + r * W + clipX0), len) != len) return false;
  }
  return true;
}

static void drawBackground() {
  if (drawCustomBackground()) return;
  const Palette& p = PALETTES[scheme];
  static float colV[W];  // vignette, split into column and row parts
  if (colV[0] == 0)
    for (int x = 0; x < W; x++) { float dx = (x - W / 2) / (float)W; colV[x] = 0.55f * dx * dx + 1e-6f; }
  for (int y = stripY; y < clipY1; y++) {
    float t = (float)y / H;
    RGB sky = t < 0.5f ? mix(p.top, p.mid, smooth(0, 0.5f, t)) : mix(p.mid, p.horizon, smooth(0.5f, 0.82f, t));
    RGB farC = mix(p.far, p.horizon, 0.18f * (1 - t));
    float dy = (y - H / 2) / (float)H, rowV = 1.0f - 0.55f * dy * dy;
    uint16_t* out = sbuf + (y - stripY) * W;
    for (int x = clipX0; x < clipX1; x++) {
      RGB c = sky;
      int ft = farTop[x], nt = nearTop[x];
      if (y > ft - 5) c = mix(c, farC, (y >= ft + 5 ? 1.0f : smooth(ft - 5, ft + 5, y)) * 0.85f);
      if (y > nt - 4) c = mix(c, p.nearB, (y >= nt + 6 ? 1.0f : smooth(nt - 4, nt + 6, y)) * 0.92f);
      float v = rowV - colV[x];  // gentle vignette so text stays readable
      uint16_t q = to565({c.r * v, c.g * v, c.b * v});
      out[x] = (q >> 8) | (q << 8);
    }
  }
  if (starsOn) drawStars();
  for (auto& b : bokeh) {
    if (b.y + b.r < stripY || b.y - b.r > clipY1) continue;
    float a = b.a * p.bokeh;
    float r = b.r;
    fillShape(b.x - r - 1, b.y - r - 1, b.x + r + 1, b.y + r + 1, b.c, a, [&](float px, float py) {
      float dx = px - b.x, dy = py - b.y;
      float d = sqrtf(dx * dx + dy * dy);
      return 1.0f - smooth(r * 0.55f, r, d);
    });
  }
}

// Darken (and slightly cool) the strip — backdrop for overlays.
static void dimStrip(float k) {
  uint16_t* buf = (uint16_t*)spr.getPointer();
  for (int y = 0; y < clipY1 - stripY; y++)
    for (int x = clipX0; x < clipX1; x++) {
      uint16_t& v = buf[y * W + x];
      uint16_t c = (v >> 8) | (v << 8);
      int r = ((c >> 11) & 31) * k, g = ((c >> 5) & 63) * k, b = (c & 31) * (k + 0.08f);
      if (b > 31) b = 31;
      c = (r << 11) | (g << 5) | b;
      v = (c >> 8) | (c << 8);
    }
}

// ---------------------------------------------------------------- text
// Smooth (anti-aliased) fonts in TFT_eSPI's .vlw format, parsed once at boot so that
// switching fonts costs nothing and glyphs can be clipped to the area being drawn.
struct Glyph { uint16_t code; uint8_t w, h, adv; int8_t dx; int16_t dy; uint32_t off; };
struct Font { const uint8_t* data; int count, ascent, descent, space; Glyph* g; int16_t ascii[128]; };
static Font fonts[6];

static uint32_t be32(const uint8_t* p) { return (pgm_read_byte(p) << 24) | (pgm_read_byte(p + 1) << 16) | (pgm_read_byte(p + 2) << 8) | pgm_read_byte(p + 3); }

static void parseFont(Font& f, const uint8_t* data) {
  f.data = data;
  f.count = be32(data);
  f.ascent = be32(data + 16);
  f.descent = be32(data + 20);
  f.space = (f.ascent + f.descent) * 2 / 7;  // same guess TFT_eSPI uses
  f.g = (Glyph*)malloc(sizeof(Glyph) * f.count);
  for (auto& a : f.ascii) a = -1;
  uint32_t bitmap = 24 + f.count * 28;
  for (int i = 0; i < f.count; i++) {
    const uint8_t* m = data + 24 + i * 28;
    Glyph& g = f.g[i];
    g.code = be32(m); g.h = be32(m + 4); g.w = be32(m + 8); g.adv = be32(m + 12);
    g.dy = (int16_t)be32(m + 16); g.dx = (int8_t)be32(m + 20);
    g.off = bitmap;
    bitmap += g.w * g.h;
    if (g.code < 128) f.ascii[g.code] = i;
  }
}
static void loadFonts() {
  const uint8_t* all[6] = {F_GREET, F_GREET_S, F_TIME, F_TEMP, F_MED, F_SMALL};
  for (int i = 0; i < 6; i++) parseFont(fonts[i], all[i]);
}
static const Font& fontFor(const uint8_t* data) {
  for (auto& f : fonts)
    if (f.data == data) return f;
  return fonts[5];
}
static const Glyph* findGlyph(const Font& f, uint16_t u) {
  if (u < 128) return f.ascii[u] >= 0 ? &f.g[f.ascii[u]] : nullptr;
  for (int i = 0; i < f.count; i++)
    if (f.g[i].code == u) return &f.g[i];
  return nullptr;
}
static uint16_t nextChar(const char*& p) {
  uint16_t u = (uint8_t)*p++;
  if ((u & 0xE0) == 0xC0 && (*p & 0xC0) == 0x80) u = ((u & 0x1F) << 6) | (*p++ & 0x3F);  // 2-byte UTF-8
  return u;
}

static int textW(const uint8_t* fd, const String& s) {
  const Font& f = fontFor(fd);
  const char* p = s.c_str();
  int w = 0;
  while (*p) {
    uint16_t u = nextChar(p);
    if (u == ' ') { w += f.space; continue; }
    const Glyph* g = findGlyph(f, u);
    if (!g) { w += f.space + 1; continue; }
    if (w == 0 && g->dx < 0) w -= g->dx;
    w += *p ? g->adv : g->dx + g->w;
  }
  return w;
}
static int fontAscent(const uint8_t* fd) { return fontFor(fd).ascent; }
static int fontH(const uint8_t* fd) { const Font& f = fontFor(fd); return f.ascent + f.descent + 2; }

static bool colonHidden = false;  // the big clock's colon blinks every second

// Draw a string, touching only pixels inside the area being rendered. y = top of the line.
static void drawGlyphs(const Font& f, const String& s, int x, int y, RGB col) {
  const char* p = s.c_str();
  int cx = x;
  while (*p) {
    uint16_t u = nextChar(p);
    if (u == ' ') { cx += f.space; continue; }
    const Glyph* g = findGlyph(f, u);
    if (!g) continue;
    if (u == ':' && colonHidden && f.data == F_TIME) { cx += g->adv; continue; }
    int gx = cx + g->dx, gy = y + f.ascent - g->dy;
    cx += g->adv;
    int x0 = max(gx, clipX0), x1 = min(gx + g->w, clipX1), y0 = max(gy, stripY), y1 = min(gy + g->h, clipY1);
    if (x0 >= x1 || y0 >= y1) continue;
    const uint8_t* bm = f.data + g->off;
    for (int yy = y0; yy < y1; yy++) {
      const uint8_t* row = bm + (yy - gy) * g->w - gx;
      for (int xx = x0; xx < x1; xx++) {
        uint8_t a = pgm_read_byte(row + xx);
        if (a) blendPx(xx, yy, col, a * (1 / 255.0f));
      }
    }
  }
}

enum Align { LEFT, RIGHT, CENTER };
static const RGB WHITE = {244, 244, 246}, CREAM = {248, 232, 200}, SOFT = {206, 212, 222}, DARK = {28, 30, 36};

static void text(const uint8_t* f, const String& s, int x, int y, RGB col, Align al = LEFT, bool shadow = false) {
  if (y >= clipY1 || y + fontH(f) < stripY || !s.length()) return;
  const Font& fn = fontFor(f);
  int w = textW(f, s);
  int xx = al == LEFT ? x : (al == RIGHT ? x - w : x - w / 2);
  if (xx > clipX1 || xx + w + 2 < clipX0) return;
  if (shadow) drawGlyphs(fn, s, xx + 1, y + 1, {0, 0, 0});
  drawGlyphs(fn, s, xx, y, col);
}

// ---------------------------------------------------------------- icons
static const RGB SUN = {255, 200, 72}, MOON = {246, 212, 112}, CLOUD = {244, 246, 250},
                 CLOUD_GREY = {176, 186, 198}, RAIN = {96, 176, 244}, BOLT = {255, 214, 64};

static void drawSun(float cx, float cy, float r, RGB col = SUN, float rot = 0) {
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4 + rot, ca = cosf(a), sa = sinf(a);
    float ax = cx + ca * r * 1.38f, ay = cy + sa * r * 1.38f, bx = cx + ca * r * 1.75f, by = cy + sa * r * 1.75f;
    fillShape(fminf(ax, bx) - 3, fminf(ay, by) - 3, fmaxf(ax, bx) + 3, fmaxf(ay, by) + 3, col, 1,
              [=](float px, float py) { return capsCov(px, py, ax, ay, bx, by, r * 0.11f + 0.4f); });
  }
  fillShape(cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1, col, 1, [=](float px, float py) { return circCov(px, py, cx, cy, r); });
}

static void drawMoon(float cx, float cy, float r, RGB col = MOON) {
  fillShape(cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1, col, 1, [=](float px, float py) {
    return circCov(px, py, cx, cy, r) * (1 - circCov(px, py, cx + r * 0.5f, cy - r * 0.38f, r * 0.82f));
  });
}

// Moon phase 0..1 (0 = new, 0.5 = full), from the mean synodic month.
static float moonPhase(time_t t) {
  double p = fmod((t - 947182440.0) / 86400.0 / 29.530588853, 1.0);  // 947182440 = new moon, 6 Jan 2000
  return p < 0 ? p + 1 : p;
}
static float moonIllum(float p) { return (1 - cosf(p * TWO_PI)) / 2; }

// Moon showing its real phase. darkA = how visible the unlit part is.
static void drawMoonPhase(float cx, float cy, float r, float p, RGB col, float darkA, bool craters = false) {
  float k = cosf(p * TWO_PI);
  bool waxing = p < 0.5f, south = lat < 0;
  float x0 = cx - r - 1, y0 = cy - r - 1, x1 = cx + r + 1, y1 = cy + r + 1;
  if (darkA > 0)
    fillShape(x0, y0, x1, y1, mix(col, {40, 44, 60}, 0.75f), darkA, [=](float px, float py) { return circCov(px, py, cx, cy, r); });
  auto lit = [=](float px, float py) {
    float dx = px - cx, dy = py - cy;
    if (south) dx = -dx;
    float xt = k * sqrtf(fmaxf(0, r * r - dy * dy));
    return circCov(px, py, cx, cy, r) * (waxing ? clamp01(dx - xt + 0.5f) : clamp01(-xt - dx + 0.5f));
  };
  fillShape(x0, y0, x1, y1, col, 1, lit);
  if (!craters) return;
  static const float CR[][3] = {{-0.30f, -0.25f, 0.22f}, {0.22f, 0.18f, 0.16f}, {-0.05f, 0.42f, 0.12f}, {0.35f, -0.35f, 0.10f}, {-0.45f, 0.15f, 0.09f}};
  for (auto& c : CR) {
    float ccx = cx + c[0] * r, ccy = cy + c[1] * r, cr = c[2] * r;
    fillShape(ccx - cr - 1, ccy - cr - 1, ccx + cr + 1, ccy + cr + 1, {150, 130, 90}, 0.22f,
              [=](float px, float py) { return circCov(px, py, ccx, ccy, cr) * lit(px, py); });
  }
}

static void drawCloud(float cx, float cy, float w, RGB col) {
  auto cov = [=](float px, float py) {
    float c = circCov(px, py, cx + 0.08f * w, cy - 0.10f * w, 0.26f * w);
    c = fmaxf(c, circCov(px, py, cx - 0.20f * w, cy + 0.02f * w, 0.18f * w));
    c = fmaxf(c, circCov(px, py, cx + 0.30f * w, cy + 0.05f * w, 0.16f * w));
    c = fmaxf(c, rrectCov(px, py, cx - 0.40f * w, cy + 0.02f * w, cx + 0.46f * w, cy + 0.21f * w, 0.095f * w));
    return c;
  };
  float x0 = cx - 0.42f * w, y0 = cy - 0.38f * w, x1 = cx + 0.48f * w, y1 = cy + 0.23f * w;
  fillShape(x0, y0, x1, y1, col, 1, cov);
  RGB shade = mix(col, {90, 104, 124}, 0.5f);
  fillShape(x0, y0, x1, y1, shade, 0.35f, [=](float px, float py) { return cov(px, py) * clamp01((py - cy + 0.04f * w) / (0.25f * w)); });
}

// fall < 0: static drops; 0..1: animation phase (drops fall and fade out)
static void drawDrops(float cx, float cy, float s, int n, float len, float fall = -1) {
  for (int i = 0; i < n; i++) {
    float x = cx + (i - (n - 1) / 2.0f) * 0.2f * s, y = cy + ((i & 1) ? 0.05f * s : 0), a = 1;
    if (fall >= 0) {
      float f = fmodf(fall + i * 0.37f, 1.0f);
      y = cy - 0.04f * s + f * 0.2f * s;
      x -= f * 0.05f * s;
      a = f < 0.2f ? f / 0.2f : 1 - smooth(0.6f, 1.0f, f);
    }
    float ax = x, ay = y, bx = x - 0.05f * s, by = y + len * s;
    fillShape(bx - 3, ay - 3, ax + 3, by + 3, RAIN, a, [=](float px, float py) { return capsCov(px, py, ax, ay, bx, by, 0.035f * s + 0.3f); });
  }
}

static void drawFlakes(float cx, float cy, float s, float fall = -1) {
  for (int i = 0; i < 3; i++) {
    float x = cx + (i - 1) * 0.2f * s, y = cy + ((i & 1) ? 0.1f * s : 0.02f * s), a = 1;
    if (fall >= 0) {
      float f = fmodf(fall + i * 0.41f, 1.0f);
      y = cy - 0.04f * s + f * 0.24f * s;
      x += sinf(f * TWO_PI + i) * 0.03f * s;
      a = f < 0.2f ? f / 0.2f : 1 - smooth(0.65f, 1.0f, f);
    }
    fillShape(x - 4, y - 4, x + 4, y + 4, CLOUD, a, [=](float px, float py) { return circCov(px, py, x, y, 0.05f * s + 0.4f); });
  }
}

static void drawBolt(float cx, float cy, float s, float glow = 0) {
  float p[6][2] = {{0.06f, -0.02f}, {-0.12f, 0.2f}, {0.02f, 0.2f}, {-0.06f, 0.4f}, {0.14f, 0.14f}, {0.0f, 0.14f}};
  for (auto& q : p) { q[0] = cx + q[0] * s; q[1] = cy + q[1] * s; }
  fillShape(cx - 0.15f * s, cy - 0.05f * s, cx + 0.17f * s, cy + 0.42f * s, mix(BOLT, {255, 255, 255}, glow), 1, [&](float px, float py) {
    return fmaxf(fmaxf(triCov(px, py, p[0][0], p[0][1], p[1][0], p[1][1], p[2][0], p[2][1]),
                       triCov(px, py, p[2][0], p[2][1], p[1][0], p[1][1], p[4][0], p[4][1])),
                 triCov(px, py, p[5][0], p[5][1], p[4][0], p[4][1], p[3][0], p[3][1]));
  });
}

static float flashLevel = 0;  // lightning flash, 0..1

static void drawCelestial(float cx, float cy, float r, bool day, float rot = 0) {
  if (day) drawSun(cx, cy, r * 0.85f, SUN, rot);
  else drawMoonPhase(cx, cy, r * 0.92f, moonPhase(time(nullptr)), MOON, 0.3f);
}

// s = nominal icon size in px; (cx, cy) = centre. anim = gently animate (main icon only).
static void drawIcon(int code, bool day, float cx, float cy, float s, bool anim = false) {
  if (cy + s < stripY || cy - s >= clipY1 || cx + s < clipX0 || cx - s >= clipX1) return;
  float rot = anim ? phase(8000) * PI / 4 : 0;                       // sun rays turn slowly
  float sway = anim ? sinf(phase(7000) * TWO_PI) * 0.035f * s : 0;  // clouds drift
  float fall = anim ? phase(1400) : -1;                             // drops & flakes fall
  if (code == 0) { drawCelestial(cx, cy, 0.3f * s, day, rot); return; }
  if (code <= 2) {
    drawCelestial(cx + 0.16f * s, cy - 0.16f * s, 0.22f * s, day, rot);
    drawCloud(cx - 0.06f * s + sway, cy + 0.1f * s, (code == 1 ? 0.68f : 0.82f) * s, CLOUD);
    return;
  }
  if (code == 3) {
    drawCloud(cx + 0.14f * s - sway * 0.6f, cy - 0.12f * s, 0.62f * s, CLOUD_GREY);
    drawCloud(cx - 0.06f * s + sway, cy + 0.1f * s, 0.82f * s, CLOUD);
    return;
  }
  if (code == 45 || code == 48) {
    drawCloud(cx + sway * 0.5f, cy - 0.08f * s, 0.78f * s, CLOUD_GREY);
    for (int i = 0; i < 2; i++) {
      float y = cy + (0.2f + 0.12f * i) * s, ax = cx - (0.32f - 0.08f * i) * s + sway * (i ? -1 : 1), bx = ax + (0.68f - 0.14f * i) * s;
      fillShape(ax - 3, y - 3, bx + 3, y + 3, CLOUD, 0.9f, [=](float px, float py) { return capsCov(px, py, ax, y, bx, y, 0.035f * s + 0.3f); });
    }
    return;
  }
  bool showers = (code >= 80 && code <= 86);
  if (showers) drawCelestial(cx + 0.18f * s, cy - 0.2f * s, 0.2f * s, day, rot);
  bool storm = code >= 95;
  bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
  bool drizzle = code >= 51 && code <= 57;
  // precipitation first so it falls out from behind the cloud
  if (snow) drawFlakes(cx, cy + 0.22f * s, s, fall);
  else if (!storm) drawDrops(cx, cy + 0.2f * s, s, 3, drizzle ? 0.08f : 0.14f, fall);
  drawCloud(cx - 0.02f * s + sway, cy - 0.08f * s, 0.84f * s, storm ? CLOUD_GREY : CLOUD);
  if (storm) drawBolt(cx, cy + 0.04f * s, s, anim ? flashLevel : 0);
}

static void drawAirplane(float cx, float cy, float s, RGB col) {
  auto cov = [=](float px, float py) {
    float c = capsCov(px, py, cx, cy - 0.42f * s, cx, cy + 0.36f * s, 0.075f * s);
    c = fmaxf(c, capsCov(px, py, cx, cy - 0.06f * s, cx - 0.42f * s, cy + 0.14f * s, 0.07f * s));
    c = fmaxf(c, capsCov(px, py, cx, cy - 0.06f * s, cx + 0.42f * s, cy + 0.14f * s, 0.07f * s));
    c = fmaxf(c, capsCov(px, py, cx, cy + 0.28f * s, cx - 0.18f * s, cy + 0.40f * s, 0.055f * s));
    c = fmaxf(c, capsCov(px, py, cx, cy + 0.28f * s, cx + 0.18f * s, cy + 0.40f * s, 0.055f * s));
    return c;
  };
  fillShape(cx - 0.5f * s, cy - 0.5f * s, cx + 0.5f * s, cy + 0.5f * s, col, 1, cov);
}

// bars: 1..3 = signal strength (weaker parts are drawn faint)
static void drawWifi(float cx, float cy, float s, RGB col, int bars = 3) {
  float ox = cx, oy = cy + 0.3f * s;  // origin of the arcs
  for (int part = 0; part < 3; part++) {
    float R = part == 1 ? 0.32f * s : 0.58f * s;
    auto cov = [=](float px, float py) {
      if (part == 0) return circCov(px, py, ox, oy, 0.085f * s);
      float dx = px - ox, dy = py - oy, d = sqrtf(dx * dx + dy * dy);
      if (dy >= 0) return 0.0f;
      float ang = fabsf(atan2f(dx, -dy));
      return clamp01(0.055f * s - fabsf(d - R) + 0.5f) * clamp01((0.78f - ang) * d + 0.5f);
    };
    fillShape(cx - 0.5f * s, cy - 0.4f * s, cx + 0.5f * s, cy + 0.42f * s, col, part < bars ? 1.0f : 0.28f, cov);
  }
}

static void drawLock(float cx, float cy, float s, RGB col) {
  float bx0 = cx - 0.3f * s, by0 = cy - 0.05f * s, bx1 = cx + 0.3f * s, by1 = cy + 0.42f * s;
  float sy = cy - 0.2f * s, R = 0.19f * s, th = 0.07f * s;
  fillShape(cx - 0.4f * s, cy - 0.5f * s, cx + 0.4f * s, cy + 0.5f * s, col, 1, [=](float px, float py) {
    float body = rrectCovFast(px, py, bx0, by0, bx1, by1, 0.08f * s);
    float dx = px - cx, dy = py - sy, d = sqrtf(dx * dx + dy * dy);
    float shackle = py <= sy ? clamp01(th - fabsf(d - R) + 0.5f) : (py < by0 + 1 ? clamp01(th - fabsf(fabsf(dx) - R) + 0.5f) : 0);
    return fmaxf(body, shackle);
  });
}

static void drawCheck(float cx, float cy, float s, RGB col) {
  float ax = cx - 0.35f * s, ay = cy, bx = cx - 0.1f * s, by = cy + 0.28f * s, ex = cx + 0.38f * s, ey = cy - 0.3f * s;
  fillShape(cx - 0.5f * s, cy - 0.5f * s, cx + 0.5f * s, cy + 0.5f * s, col, 1, [=](float px, float py) {
    return fmaxf(capsCov(px, py, ax, ay, bx, by, 0.07f * s), capsCov(px, py, bx, by, ex, ey, 0.07f * s));
  });
}

static void drawChevronLeft(float cx, float cy, float s, RGB col) {
  float tx = cx - 0.2f * s;
  fillShape(cx - 0.4f * s, cy - 0.5f * s, cx + 0.4f * s, cy + 0.5f * s, col, 1, [=](float px, float py) {
    return fmaxf(capsCov(px, py, tx, cy, cx + 0.15f * s, cy - 0.35f * s, 0.07f * s), capsCov(px, py, tx, cy, cx + 0.15f * s, cy + 0.35f * s, 0.07f * s));
  });
}

static void drawRefreshIcon(float cx, float cy, float s, RGB col) {
  float R = 0.3f * s, th = 0.085f * s;
  float ea = -100 * DEG_TO_RAD;  // arc end, arrowhead continues clockwise from here
  float ex = cx + R * cosf(ea), ey = cy + R * sinf(ea);
  float tx = -sinf(ea), ty = cosf(ea), nx = cosf(ea), ny = sinf(ea);
  float t0x = ex + tx * 0.17f * s, t0y = ey + ty * 0.17f * s;
  float b1x = ex + nx * 0.15f * s, b1y = ey + ny * 0.15f * s, b2x = ex - nx * 0.15f * s, b2y = ey - ny * 0.15f * s;
  fillShape(cx - 0.45f * s, cy - 0.5f * s, cx + 0.45f * s, cy + 0.45f * s, col, 1, [=](float px, float py) {
    float dx = px - cx, dy = py - cy, d = sqrtf(dx * dx + dy * dy);
    float a = atan2f(dy, dx) * RAD_TO_DEG;  // -180..180, clockwise on screen
    float ring = clamp01(th - fabsf(d - R) + 0.5f);
    float gap = (a > -100 && a < -40) ? 0 : 1;
    return fmaxf(ring * gap, triCov(px, py, t0x, t0y, b1x, b1y, b2x, b2y));
  });
}

static const char* conditionName(int c) {
  switch (c) {
    case 0: return "Clear";
    case 1: return "Mainly Clear";
    case 2: return "Partly Cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing Drizzle";
    case 61: return "Light Rain";
    case 63: return "Rain";
    case 65: return "Heavy Rain";
    case 66: case 67: return "Freezing Rain";
    case 71: return "Light Snow";
    case 73: return "Snow";
    case 75: return "Heavy Snow";
    case 77: return "Snow Grains";
    case 80: case 81: return "Rain Showers";
    case 82: return "Heavy Showers";
    case 85: case 86: return "Snow Showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Thunder & Hail";
  }
  return "";
}

// ---------------------------------------------------------------- time helpers
static const char* DAYS[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char* MONTHS[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
static const char* MON3[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static bool timeValid() { return time(nullptr) > 1700000000; }
static struct tm localNow() {
  time_t t = time(nullptr) + utcOffset;
  struct tm tm;
  gmtime_r(&t, &tm);
  if (hourOverride >= 0) tm.tm_hour = hourOverride;
  return tm;
}
static int currentHour() { return timeValid() || hourOverride >= 0 ? localNow().tm_hour : 20; }

static Scheme schemeFor(int h) {
  if (h >= 5 && h < 12) return MORNING;
  if (h >= 12 && h < 17) return AFTERNOON;
  if (h >= 17 && h < 22) return EVENING;
  return NIGHT;
}

static bool isNightDim(int h) {
  if (!cfg.autoDim) return false;
  return cfg.dimStart > cfg.dimEnd ? (h >= cfg.dimStart || h < cfg.dimEnd) : (h >= cfg.dimStart && h < cfg.dimEnd);
}
static uint8_t& activeBrightness() { return isNightDim(currentHour()) ? cfg.brightNight : cfg.brightDay; }
static void applyBrightness() { ledcWrite(0, max<int>(10, activeBrightness())); }

static float dispTemp(float c) { return cfg.fahrenheit ? c * 1.8f + 32 : c; }
static String tempStr(float c) { return String((int)lroundf(dispTemp(c))) + "\xC2\xB0"; }

static String hourLabel(time_t t) {
  t += utcOffset;
  struct tm tm;
  gmtime_r(&t, &tm);
  char b[8];
  if (cfg.use24h) snprintf(b, sizeof b, "%02d:00", tm.tm_hour);
  else snprintf(b, sizeof b, "%d %s", tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12, tm.tm_hour < 12 ? "AM" : "PM");
  return String(b);
}

static String cityLabel() {
  if (!cfg.showCity) return "";
  return cfg.autoLoc ? ipCity : cfg.city;
}

// ---------------------------------------------------------------- dashboard
// Touch regions on the dashboard
static const int TIME_X1 = 175, TIME_Y0 = 140;  // clock: x < 175, y > 140
static const int TEMP_X0 = 165, TEMP_Y1 = 96;   // weather: x > 165, y < 96

struct Layout {
  String greet1, greet2, date, clock, ampm, cond, sub;
  const uint8_t* greetFont;
  const uint8_t* dateFont;
  int clockW, clockBase, ampmY;
  int colX0, colY0, colX1, colY1;  // where the clock's colon is
} L;

static void prepareLayout() {
  bool tv = timeValid();
  struct tm tm = localNow();
  int h = tm.tm_hour;
  if (tv) {
    L.greet1 = "Good";
    L.greet2 = h >= 5 && h < 12 ? "Morning!" : h >= 12 && h < 17 ? "Afternoon!" : h >= 17 && h < 22 ? "Evening!" : "Night!";
  } else {
    L.greet1 = "Hello";
    L.greet2 = "there!";
  }
  L.greetFont = textW(F_GREET, L.greet2) > 188 ? F_GREET_S : F_GREET;

  L.dateFont = F_MED;
  if (tv) {
    L.date = String(DAYS[tm.tm_wday]) + ", " + MONTHS[tm.tm_mon] + " " + tm.tm_mday;
    if (textW(F_MED, L.date) > 150) {
      L.date = String(DAYS[tm.tm_wday]) + ", " + MON3[tm.tm_mon] + " " + tm.tm_mday;
      if (textW(F_MED, L.date) > 150) L.dateFont = F_SMALL;
    }
  } else {
    L.date = cfg.wifiOn ? "Waiting for time..." : "Wi-Fi is off";
  }

  char b[8];
  if (!tv) { strcpy(b, "--:--"); L.ampm = ""; }
  else if (cfg.use24h) { snprintf(b, sizeof b, "%02d:%02d", h, tm.tm_min); L.ampm = ""; }
  else { snprintf(b, sizeof b, "%d:%02d", h % 12 == 0 ? 12 : h % 12, tm.tm_min); L.ampm = h < 12 ? "AM" : "PM"; }
  L.clock = b;
  L.clockW = textW(F_TIME, L.clock);
  L.clockBase = 178 + fontAscent(F_TIME);
  L.ampmY = L.clockBase - fontAscent(F_MED);
  {
    const Font& f = fontFor(F_TIME);
    int cx = 14;
    L.colX0 = L.colX1 = 0;
    for (const char* p = L.clock.c_str(); *p; p++) {
      const Glyph* g = findGlyph(f, (uint8_t)*p);
      if (!g) continue;
      if (*p == ':') {
        int gy = 178 + f.ascent - g->dy;
        L.colX0 = cx + g->dx - 1; L.colX1 = cx + g->dx + g->w + 2;
        L.colY0 = gy - 1; L.colY1 = gy + g->h + 2;
        break;
      }
      cx += g->adv;
    }
  }

  if (wx.valid) {
    L.cond = conditionName(curCode());
    String hl = "H " + tempStr(wx.tmax) + "  L " + tempStr(wx.tmin);
    String city = cityLabel();
    L.sub = city.length() ? city + " \xC2\xB7 " + hl : hl;
    // keep the second greeting line clear of the weather text beside it
    auto limit = [&]() { return 300 - max(textW(F_SMALL, L.cond), textW(F_SMALL, L.sub)) - 16; };
    if (16 + textW(L.greetFont, L.greet2) > limit()) L.greetFont = F_GREET_S;
    if (16 + textW(L.greetFont, L.greet2) > limit()) L.sub = hl;
  }
}

static void drawHome() {
  // greeting
  text(L.greetFont, L.greet1, 16, 12, CREAM, LEFT, true);
  text(L.greetFont, L.greet2, 16, L.greetFont == F_GREET ? 54 : 50, CREAM, LEFT, true);

  // date & time
  text(L.dateFont, L.date, 18, 152, SOFT, LEFT, true);
  text(F_TIME, L.clock, 14, 178, WHITE, LEFT, true);
  if (L.ampm.length()) text(F_MED, L.ampm, 14 + L.clockW + 6, L.ampmY, WHITE, LEFT, true);

  if (!wx.valid) {
    text(F_SMALL, cfg.wifiOn ? "Fetching weather..." : "No weather (offline)", 308, 40, SOFT, RIGHT, true);
    return;
  }

  // current conditions
  drawIcon(curCode(), wx.isDay, 200, 38, 58, cfg.anim);
  text(F_TEMP, tempStr(wx.temp) + (cfg.fahrenheit ? "F" : "C"), 310, 10, WHITE, RIGHT, true);
  text(F_SMALL, L.cond, 308, 60, WHITE, RIGHT, true);
  text(F_SMALL, L.sub, 308, 78, SOFT, RIGHT, true);

  // hourly strip
  const int px0 = 170, px1 = 312, py0 = 124, py1 = 158;
  glassRect(px0, py0, px1, py1, 10, {16, 22, 34}, 0.32f, 0.10f);
  float colW = (px1 - px0) / 3.0f;
  for (int i = 0; i < wx.hourCount; i++) {
    int cx = px0 + colW * (i + 0.5f);
    text(F_SMALL, hourLabel(wx.hourTime[i]), cx, 104, SOFT, CENTER, true);
    drawIcon(wx.hourCode[i], wx.hourDay[i], cx, 141, 32);
    text(F_MED, tempStr(wx.hourTemp[i]), cx + 3, 161, WHITE, CENTER, true);
  }
}

// ---------------------------------------------------------------- extra pages
static const char* DAY3[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static struct tm localTm(time_t t) {
  t += utcOffset;
  struct tm tm;
  gmtime_r(&t, &tm);
  return tm;
}
static String clockStr(time_t t) {
  struct tm tm = localTm(t);
  char b[12];
  if (cfg.use24h) snprintf(b, sizeof b, "%02d:%02d", tm.tm_hour, tm.tm_min);
  else snprintf(b, sizeof b, "%d:%02d %s", tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
  return String(b);
}
static String dateStr(time_t t) {
  struct tm tm = localTm(t);
  return String(MON3[tm.tm_mon]) + " " + tm.tm_mday;
}
static String durStr(long secs) {
  long m = max(0L, secs / 60);
  return m >= 60 ? String(m / 60) + "h " + (m % 60) + "m" : String(m) + " min";
}
// index of today in the daily arrays
static int todayIndex() {
  time_t now = time(nullptr);
  for (int i = 0; i < wx.dayCount; i++)
    if (now >= wx.dayTime[i] && now < wx.dayTime[i] + 86400) return i;
  return 0;
}

static void card(float x0, float y0, float x1, float y1) { glassRect(x0, y0, x1, y1, 14, {14, 18, 28}, 0.46f, 0.10f); }
static void hline(float x0, float x1, float y, float a) {
  fillShape(x0, y - 1, x1, y + 1, WHITE, a, [=](float px, float py) { return clamp01(0.5f - fabsf(py - y) + 0.5f); });
}
static void pageHeader(const char* title) {
  text(F_MED, title, 16, 8, CREAM, LEFT, true);
  if (timeValid()) text(F_MED, clockStr(time(nullptr)), 304, 12, WHITE, RIGHT, true);
}
static bool pageNeedsWeather() {
  if (wx.valid) return false;
  text(F_MED, cfg.wifiOn ? "Waiting for weather..." : "No weather (offline)", W / 2, 110, SOFT, CENTER, true);
  return true;
}

// temperature → colour for the range bars
static RGB tempColor(float c) {
  static const float T[] = {-10, 0, 10, 18, 24, 30, 38};
  static const RGB C[] = {{150, 120, 255}, {90, 170, 255}, {80, 210, 200}, {160, 220, 90}, {255, 206, 70}, {255, 140, 50}, {240, 70, 60}};
  if (c <= T[0]) return C[0];
  for (int i = 1; i < 7; i++)
    if (c <= T[i]) return mix(C[i - 1], C[i], (c - T[i - 1]) / (T[i] - T[i - 1]));
  return C[6];
}

static void drawForecastPage() {
  pageHeader("5-Day Forecast");
  if (pageNeedsWeather()) return;
  card(10, 32, 310, 218);
  int t0 = todayIndex(), n = min(5, wx.dayCount - t0);
  float lo = 99, hi = -99;
  for (int i = 0; i < n; i++) { lo = fminf(lo, wx.dayMin[t0 + i]); hi = fmaxf(hi, wx.dayMax[t0 + i]); }
  if (hi - lo < 1) hi = lo + 1;
  const float bx0 = 194, bx1 = 262;
  for (int i = 0; i < n; i++) {
    int d = t0 + i;
    float cy = 53 + i * 36.4f;
    if (i) hline(22, 298, cy - 18, 0.08f);
    text(F_MED, i == 0 ? String("Today") : String(DAY3[localTm(wx.dayTime[d]).tm_wday]), 22, cy - 10, WHITE, LEFT, true);
    drawIcon(wx.dayCode[d], true, 99, cy, 30);
    if (wx.dayRain[d] >= 20) text(F_SMALL, String(wx.dayRain[d]) + "%", 117, cy - 8, RAIN, LEFT, true);
    text(F_MED, tempStr(wx.dayMin[d]), 186, cy - 10, SOFT, RIGHT, true);
    text(F_MED, tempStr(wx.dayMax[d]), 302, cy - 10, WHITE, RIGHT, true);
    // range bar: track, then this day's span coloured by temperature
    fillShape(bx0 - 3, cy - 3, bx1 + 3, cy + 3, WHITE, 0.16f, [=](float px, float py) { return capsCov(px, py, bx0, cy, bx1, cy, 2.2f); });
    float a = bx0 + (wx.dayMin[d] - lo) / (hi - lo) * (bx1 - bx0), b = bx0 + (wx.dayMax[d] - lo) / (hi - lo) * (bx1 - bx0);
    fillShapeCol(a - 3, cy - 3, b + 3, cy + 3, 1, [=](float px, float) { return tempColor(lo + (px - bx0) / (bx1 - bx0) * (hi - lo)); },
                 [=](float px, float py) { return capsCov(px, py, a, cy, b, cy, 2.2f); });
    if (i == 0) {  // current temperature dot
      float x = bx0 + (constrain(wx.temp, lo, hi) - lo) / (hi - lo) * (bx1 - bx0);
      fillShape(x - 5, cy - 5, x + 5, cy + 5, {20, 24, 34}, 0.9f, [=](float px, float py) { return circCov(px, py, x, cy, 4.2f); });
      fillShape(x - 4, cy - 4, x + 4, cy + 4, WHITE, 1, [=](float px, float py) { return circCov(px, py, x, cy, 2.8f); });
    }
  }
}

static const char* moonName(float p) {
  if (p < 0.0339f || p > 0.9661f) return "New\nMoon";
  if (p < 0.216f) return "Waxing\nCrescent";
  if (p < 0.284f) return "First\nQuarter";
  if (p < 0.466f) return "Waxing\nGibbous";
  if (p < 0.534f) return "Full\nMoon";
  if (p < 0.716f) return "Waning\nGibbous";
  if (p < 0.784f) return "Last\nQuarter";
  return "Waning\nCrescent";
}

static void drawSunPage() {
  pageHeader("Sun & Moon");
  time_t now = time(nullptr);
  int d = todayIndex();
  // sun arc
  card(10, 32, 192, 218);
  if (wx.valid && wx.dayCount) {
    time_t rise = wx.sunrise[d], set = wx.sunset[d];
    float f = (float)(now - rise) / (float)(set - rise);
    bool up = f >= 0 && f <= 1;
    const float cx = 101, cy = 146, R = 66;
    float fs = constrain(f, 0.0f, 1.0f), sa = PI * (1 - fs);  // angle of the sun on the arc
    // dotted track over the whole arc, solid where the sun has been
    fillShape(cx - R - 3, cy - R - 3, cx + R + 3, cy + 1, WHITE, 0.45f, [=](float px, float py) {
      float dx = px - cx, dy = py - cy, a = atan2f(-dy, dx);
      if (a < 0) return 0.0f;
      float ring = clamp01(1.0f - fabsf(sqrtf(dx * dx + dy * dy) - R) + 0.5f);
      return (a >= sa && up) ? 0.0f : ring * (fmodf(a * R / 7.0f, 1.0f) < 0.5f ? 1.0f : 0.0f);
    });
    if (up)
      fillShapeCol(cx - R - 3, cy - R - 3, cx + R + 3, cy + 1, 0.95f,
                   [=](float px, float) { return mix({255, 150, 60}, SUN, clamp01((px - cx + R) / (2 * R))); },
                   [=](float px, float py) {
                     float dx = px - cx, dy = py - cy, a = atan2f(-dy, dx);
                     if (a < sa - 0.02f) return 0.0f;
                     return clamp01(1.4f - fabsf(sqrtf(dx * dx + dy * dy) - R) + 0.5f) * clamp01((cy - py) + 0.5f);
                   });
    float sx = cx + R * cosf(sa), sy = cy - R * sinf(sa);
    fillShape(sx - 16, sy - 16, sx + 16, sy + 16, SUN, up ? 0.28f : 0.12f, [=](float px, float py) {
      float dx = px - sx, dy = py - sy;
      return 1 - smooth(5, 15, sqrtf(dx * dx + dy * dy));
    });
    fillShape(sx - 9, sy - 9, sx + 9, sy + 9, SUN, up ? 1.0f : 0.5f, [=](float px, float py) { return circCov(px, py, sx, sy, 7.5f); });
    // ground hides the sun below the horizon
    fillShape(14, cy, 188, cy + 12, {12, 16, 26}, 0.8f, [=](float, float py) { return clamp01(py - cy) * (1 - smooth(cy + 2, cy + 12, py)); });
    hline(22, 180, cy, 0.4f);
    text(F_SMALL, "Sunrise", 22, 156, SOFT, LEFT, true);
    text(F_MED, clockStr(rise), 22, 172, WHITE, LEFT, true);
    text(F_SMALL, "Sunset", 180, 156, SOFT, RIGHT, true);
    text(F_MED, clockStr(set), 180, 172, WHITE, RIGHT, true);
    String foot;
    if (up) foot = durStr(set - now) + " of daylight left";
    else {
      time_t next = f < 0 ? rise : (d + 1 < wx.dayCount ? wx.sunrise[d + 1] : rise + 86400);
      foot = "Sunrise in " + durStr(next - now);
    }
    text(F_SMALL, foot, cx, 198, SOFT, CENTER, true);
  } else {
    text(F_SMALL, "Waiting for data...", 101, 120, SOFT, CENTER, true);
  }

  // moon
  card(200, 32, 310, 218);
  float p = moonPhase(now);
  drawMoonPhase(255, 82, 28, p, {238, 232, 214}, 0.2f, true);
  String nm = moonName(p);
  int nl = nm.indexOf('\n');
  text(F_MED, nm.substring(0, nl), 255, 120, WHITE, CENTER, true);
  text(F_MED, nm.substring(nl + 1), 255, 141, WHITE, CENTER, true);
  text(F_SMALL, String((int)lroundf(moonIllum(p) * 100)) + "% lit", 255, 168, SOFT, CENTER, true);
  const float SYN = 29.530588853f * 86400;
  time_t full = now + (time_t)(fmodf(1.5f - p, 1.0f) * SYN), nu = now + (time_t)((1 - p) * SYN);
  text(F_SMALL, "Full: " + dateStr(full), 255, 185, SOFT, CENTER, true);
  text(F_SMALL, "New: " + dateStr(nu), 255, 201, SOFT, CENTER, true);
}

static void drawRainPage() {
  pageHeader("Rain");
  if (pageNeedsWeather()) return;
  card(10, 32, 310, 218);
  int n = wx.rainCount;
  if (!n) { text(F_MED, "No rain data", W / 2, 110, SOFT, CENTER, true); return; }
  // headline
  float total = 0;
  int maxP = 0, firstWet = -1;
  for (int i = 0; i < n; i++) {
    total += wx.rainMm[i];
    maxP = max(maxP, (int)wx.rainProb[i]);
    if (firstWet < 0 && (wx.rainProb[i] >= 50 || wx.rainMm[i] >= 0.3f)) firstWet = i;
  }
  String head;
  if (firstWet == 0) {
    int dry = -1;
    for (int i = 1; i < n && dry < 0; i++)
      if (wx.rainProb[i] < 30 && wx.rainMm[i] < 0.1f) dry = i;
    head = dry > 0 ? "Rain easing by " + hourLabel(wx.rainTime[dry]) : "Rain for the next 12h";
  } else if (firstWet > 0) {
    head = "Rain likely around " + hourLabel(wx.rainTime[firstWet]);
  } else if (maxP >= 25) {
    head = String("Small chance of rain (") + maxP + "%)";
  } else if (total >= 0.1f) {
    head = "A little drizzle possible";
  } else {
    head = "No rain expected";
  }
  text(F_MED, head, 22, 42, WHITE, LEFT, true);
  char sub[48];
  if (total >= 0.1f) snprintf(sub, sizeof sub, "Next 12 hours \xC2\xB7 %.1f mm in total", total);
  else snprintf(sub, sizeof sub, "Next 12 hours");
  text(F_SMALL, sub, 22, 66, SOFT, LEFT, true);
  // chart
  const float base = 188, top = 98, x0 = 22, x1 = 298, slot = (x1 - x0) / 12;
  hline(x0, x1, top, 0.10f);
  hline(x0, x1, (top + base) / 2, 0.10f);
  hline(x0, x1, base + 1, 0.25f);
  text(F_SMALL, "100%", x1, top - 15, SOFT, RIGHT, false);
  text(F_SMALL, "50%", x1, (top + base) / 2 - 15, SOFT, RIGHT, false);
  for (int i = 0; i < n; i++) {
    float cx = x0 + slot * (i + 0.5f), h = fmaxf(3, (base - top) * wx.rainProb[i] / 100.0f);
    float a = wx.rainProb[i] ? 0.45f + 0.55f * clamp01(wx.rainMm[i] / 1.5f) : 0.25f;
    float bx0 = cx - 6.5f, bx1 = cx + 6.5f, by0 = base - h;
    fillShape(bx0, by0, bx1, base, RAIN, a, [=](float px, float py) { return rrectCov(px, py, bx0, by0, bx1, base + 4, 3.5f) * clamp01(base - py + 0.5f); });
    if (i % 3 == 0) text(F_SMALL, i == 0 ? String("Now") : hourLabel(wx.rainTime[i]), cx, 194, SOFT, CENTER, true);
  }
}

struct Level { float upTo; const char* name; RGB col; };
static const Level AQI_EU[] = {{20, "Good", {80, 200, 120}}, {40, "Fair", {170, 214, 80}}, {60, "Moderate", {240, 200, 60}},
                               {80, "Poor", {250, 140, 60}}, {100, "Very poor", {235, 80, 80}}, {1e9, "Extremely poor", {170, 90, 200}}};
static const Level AQI_US[] = {{50, "Good", {80, 200, 120}}, {100, "Moderate", {240, 200, 60}}, {150, "Unhealthy*", {250, 140, 60}},
                               {200, "Unhealthy", {235, 80, 80}}, {300, "Very unhealthy", {170, 90, 200}}, {1e9, "Hazardous", {140, 40, 70}}};
static const Level UV_LV[] = {{3, "Low", {80, 200, 120}}, {6, "Moderate", {240, 200, 60}}, {8, "High", {250, 140, 60}},
                              {11, "Very high", {235, 80, 80}}, {1e9, "Extreme", {170, 90, 200}}};
static const Level& levelFor(const Level* lv, float v) {
  int i = 0;
  while (v >= lv[i].upTo) i++;
  return lv[i];
}
static const char* pollenName(float g) { return g < 1 ? "None" : g < 10 ? "Low" : g < 50 ? "Moderate" : g < 200 ? "High" : "Very high"; }

static void drawAirPage() {
  pageHeader("Air & UV");
  if (pageNeedsWeather()) return;
  // AQI gauge
  card(10, 32, 158, 218);
  text(F_SMALL, wx.aqiEU ? "European AQI" : "US AQI", 84, 42, SOFT, CENTER, true);
  if (wx.airValid) {
    const Level* lv = wx.aqiEU ? AQI_EU : AQI_US;
    float vmax = wx.aqiEU ? 120 : 300;
    const float cx = 84, cy = 150, R = 56;
    fillShapeCol(cx - R - 7, cy - R - 7, cx + R + 7, cy + 1, 1,
                 [=](float px, float py) { return levelFor(lv, (atan2f(py - cy, px - cx) + PI) / PI * vmax).col; },
                 [=](float px, float py) {
                   float dx = px - cx, dy = py - cy;
                   return clamp01(4.5f - fabsf(sqrtf(dx * dx + dy * dy) - R) + 0.5f) * clamp01(-dy + 0.5f);
                 });
    float a = PI + clamp01(wx.aqi / vmax) * PI, kx = cx + R * cosf(a), ky = cy + R * sinf(a);
    fillShape(kx - 9, ky - 9, kx + 9, ky + 9, {20, 24, 34}, 0.9f, [=](float px, float py) { return circCov(px, py, kx, ky, 7.5f); });
    fillShape(kx - 8, ky - 8, kx + 8, ky + 8, WHITE, 1, [=](float px, float py) { return circCov(px, py, kx, ky, 5.5f); });
    text(F_TEMP, String(wx.aqi), cx, 104, WHITE, CENTER, true);
    const Level& l = levelFor(lv, wx.aqi);
    text(l.name[0] && strlen(l.name) > 10 ? F_SMALL : F_MED, l.name, cx, 162, l.col, CENTER, true);
    text(F_SMALL, String("PM2.5 ") + String(wx.pm25, 1), cx, 190, SOFT, CENTER, true);
  } else {
    text(F_SMALL, "No data", 84, 120, SOFT, CENTER, true);
  }

  // UV and pollutants
  card(166, 32, 310, 218);
  int d = todayIndex();
  float uv = wx.airValid ? wx.uv : 0, uvMax = wx.dayCount ? wx.dayUv[d] : 0;
  const Level& ul = levelFor(UV_LV, uv);
  text(F_SMALL, "UV index", 178, 42, SOFT, LEFT, true);
  String uvs = String((int)lroundf(uv));
  text(F_TEMP, uvs, 176, 56, WHITE, LEFT, true);
  int ux = 176 + textW(F_TEMP, uvs) + 10;
  text(F_MED, ul.name, ux, 66, ul.col, LEFT, true);
  text(F_SMALL, "Max " + String((int)lroundf(uvMax)) + " today", ux, 88, SOFT, LEFT, true);
  const float gx0 = 180, gx1 = 296, gy = 120;
  fillShapeCol(gx0 - 4, gy - 4, gx1 + 4, gy + 4, 1, [=](float px, float) { return levelFor(UV_LV, (px - gx0) / (gx1 - gx0) * 12).col; },
               [=](float px, float py) { return capsCov(px, py, gx0, gy, gx1, gy, 2.5f); });
  float mx = gx0 + clamp01(uv / 12) * (gx1 - gx0);
  fillShape(mx - 6, gy - 6, mx + 6, gy + 6, {20, 24, 34}, 0.9f, [=](float px, float py) { return circCov(px, py, mx, gy, 5); });
  fillShape(mx - 5, gy - 5, mx + 5, gy + 5, WHITE, 1, [=](float px, float py) { return circCov(px, py, mx, gy, 3.5f); });
  if (wx.airValid) {
    struct { const char* k; String v; } rows[] = {
      {"PM10", String(wx.pm10, 1)},
      {"Ozone", String((int)lroundf(wx.ozone))},
      {"NO2", String(wx.no2, 1)},
      {"Pollen", wx.pollenValid ? String(pollenName(wx.pollen)) : String("n/a")},
    };
    for (int i = 0; i < 4; i++) {
      int y = 136 + i * 20;
      text(F_SMALL, rows[i].k, 178, y, SOFT, LEFT, true);
      text(F_SMALL, rows[i].v, 298, y, WHITE, RIGHT, true);
    }
  }
}

static void drawPageDots() {
  for (int i = 0; i < PAGE_COUNT; i++) {
    float x = 236 + i * 12, y = 228;
    fillShape(x - 4, y - 4, x + 4, y + 4, WHITE, i == page ? 0.95f : 0.35f, [=](float px, float py) { return circCov(px, py, x, y, 2.6f); });
  }
}

static void drawDashboard() {
  switch (page) {
    case PAGE_FORECAST: drawForecastPage(); break;
    case PAGE_SUN: drawSunPage(); break;
    case PAGE_RAIN: drawRainPage(); break;
    case PAGE_AIR: drawAirPage(); break;
    default: drawHome();
  }
  // control-centre grabber in the top-right corner
  fillShape(272, 2, 300, 7, WHITE, 0.45f, [](float px, float py) { return capsCov(px, py, 275, 4.5f, 297, 4.5f, 1.6f); });
  drawPageDots();
  if (cfg.airplane) drawAirplane(306, 226, 15, SOFT);
}

// ---------------------------------------------------------------- control centre
static const RGB GLASS = {58, 62, 74}, TILE_ON = {246, 246, 248}, ORANGE = {255, 149, 0}, BLUE = {10, 132, 255},
                 OFF_BTN = {92, 96, 108}, INDIGO = {94, 92, 230};
struct Rect {
  int x0, y0, x1, y1;
  bool hit(int x, int y, int pad = 4) const { return x >= x0 - pad && x <= x1 + pad && y >= y0 - pad && y <= y1 + pad; }
};
static const Rect CC_CONN = {16, 22, 156, 122}, CC_AIR = {30, 38, 74, 82}, CC_WIFI = {98, 38, 142, 82},
                  CC_24 = {16, 132, 82, 198}, CC_UNIT = {90, 132, 156, 198}, CC_SLIDER = {168, 22, 232, 198},
                  CC_REFRESH = {244, 22, 304, 104}, CC_NIGHT = {244, 116, 304, 198};
static uint32_t ccLastTouch = 0;

static float sliderFillTop() {
  return CC_SLIDER.y1 - (activeBrightness() - 10) / 245.0f * (CC_SLIDER.y1 - CC_SLIDER.y0);
}

static void drawSlider() {
  const Rect& r = CC_SLIDER;
  fillRRect(r.x0, r.y0, r.x1, r.y1, 20, GLASS, 0.82f);
  float top = sliderFillTop();
  fillShape(r.x0, top - 1, r.x1, r.y1, TILE_ON, 1, [=](float px, float py) {
    return rrectCov(px, py, r.x0, r.y0, r.x1, r.y1, 20) * clamp01(py - top + 0.5f);
  });
  float cy = r.y1 - 22;
  drawSun((r.x0 + r.x1) / 2.0f, cy, 6, cy > top ? RGB{120, 122, 132} : WHITE);
}

static void drawControlCenter() {
  dimStrip(0.42f);
  // connectivity block
  fillRRect(CC_CONN.x0, CC_CONN.y0, CC_CONN.x1, CC_CONN.y1, 20, GLASS, 0.82f);
  bool air = cfg.airplane, wifi = cfg.wifiOn;
  float ax = (CC_AIR.x0 + CC_AIR.x1) / 2.0f, ay = (CC_AIR.y0 + CC_AIR.y1) / 2.0f;
  float wxc = (CC_WIFI.x0 + CC_WIFI.x1) / 2.0f, wyc = (CC_WIFI.y0 + CC_WIFI.y1) / 2.0f;
  fillShape(ax - 23, ay - 23, ax + 23, ay + 23, air ? ORANGE : OFF_BTN, 1, [=](float px, float py) { return circCov(px, py, ax, ay, 22); });
  fillShape(wxc - 23, wyc - 23, wxc + 23, wyc + 23, wifi ? BLUE : OFF_BTN, 1, [=](float px, float py) { return circCov(px, py, wxc, wyc, 22); });
  drawAirplane(ax, ay, 26, WHITE);
  drawWifi(wxc, wyc, 26, WHITE);
  text(F_SMALL, "Airplane", ax, 92, air ? WHITE : SOFT, CENTER);
  String wl = !wifi ? "Wi-Fi Off" : (WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "Wi-Fi...");
  if (textW(F_SMALL, wl) > 72) wl = "Wi-Fi";
  text(F_SMALL, wl, wxc, 92, wifi ? WHITE : SOFT, CENTER);

  // 12/24h and unit tiles
  fillRRect(CC_24.x0, CC_24.y0, CC_24.x1, CC_24.y1, 18, GLASS, 0.82f);
  text(F_TEMP, cfg.use24h ? "24" : "12", (CC_24.x0 + CC_24.x1) / 2, 136, WHITE, CENTER);
  text(F_SMALL, "hour", (CC_24.x0 + CC_24.x1) / 2, 178, SOFT, CENTER);
  fillRRect(CC_UNIT.x0, CC_UNIT.y0, CC_UNIT.x1, CC_UNIT.y1, 18, GLASS, 0.82f);
  text(F_TEMP, cfg.fahrenheit ? "\xC2\xB0" "F" : "\xC2\xB0" "C", (CC_UNIT.x0 + CC_UNIT.x1) / 2 + 2, 136, WHITE, CENTER);
  text(F_SMALL, "units", (CC_UNIT.x0 + CC_UNIT.x1) / 2, 178, SOFT, CENTER);

  drawSlider();

  // refresh weather
  fillRRect(CC_REFRESH.x0, CC_REFRESH.y0, CC_REFRESH.x1, CC_REFRESH.y1, 18, GLASS, 0.82f);
  drawRefreshIcon(274, 52, 34, refreshing ? SOFT : WHITE);
  text(F_SMALL, refreshing ? "Updating" : "Weather", 274, 80, SOFT, CENTER);

  // night dimming
  bool nd = cfg.autoDim;
  fillRRect(CC_NIGHT.x0, CC_NIGHT.y0, CC_NIGHT.x1, CC_NIGHT.y1, 18, nd ? TILE_ON : GLASS, nd ? 1.0f : 0.82f);
  drawMoon(274, 148, 14, nd ? INDIGO : WHITE);
  text(F_SMALL, "Night", 274, 174, nd ? DARK : SOFT, CENTER);

  // where to manage it
  String foot = WiFi.status() == WL_CONNECTED ? "Manage at http://" + WiFi.localIP().toString()
                                               : (cfg.wifiOn ? "Connecting to Wi-Fi..." : "Wi-Fi is off");
  text(F_SMALL, foot, W / 2, 214, SOFT, CENTER);
}

// ---------------------------------------------------------------- Wi-Fi networks
// Long-press the Wi-Fi button in the control centre to pick a network.
struct Net { String ssid; int rssi; bool secure; };
static Net nets[20];
static int netCount = 0, netScroll = 0;
enum WifiState { WS_SCANNING, WS_LIST, WS_CONNECTING, WS_OK, WS_FAIL };
static WifiState wstate = WS_SCANNING;
static String joinSsid, wifiMsg;
static const int NET_Y0 = 40, NET_ROW = 38, NET_ROWS = 5;
static const Rect WL_BACK = {0, 0, 90, 34}, WL_RESCAN = {250, 0, 320, 34};

static String fitText(const uint8_t* f, String s, int maxW) {
  if (textW(f, s) <= maxW) return s;
  while (s.length() > 1 && textW(f, s + "...") > maxW) s.remove(s.length() - 1);
  return s + "...";
}
static int bars(int rssi) { return rssi > -60 ? 3 : rssi > -72 ? 2 : 1; }

static void drawWifiList() {
  dimStrip(0.32f);
  drawChevronLeft(16, 17, 18, BLUE);
  text(F_MED, "Wi-Fi", 30, 6, WHITE);
  if (wstate == WS_SCANNING) text(F_SMALL, "Scanning...", 308, 10, SOFT, RIGHT);
  else drawRefreshIcon(296, 17, 26, WHITE);
  glassRect(8, NET_Y0 - 4, 312, NET_Y0 + NET_ROW * NET_ROWS + 4, 14, GLASS, 0.82f, 0.10f);
  if (!netCount) {
    text(F_MED, wstate == WS_SCANNING ? "Looking for networks..." : "No networks found", W / 2, 118, SOFT, CENTER);
  }
  String cur = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "";
  for (int r = 0; r < NET_ROWS && netScroll + r < netCount; r++) {
    const Net& n = nets[netScroll + r];
    int y = NET_Y0 + r * NET_ROW, cy = y + NET_ROW / 2;
    if (r) hline(22, 298, y, 0.10f);
    bool on = n.ssid == cur;
    if (on) drawCheck(22, cy, 16, BLUE);
    text(F_MED, fitText(F_MED, n.ssid, 200), 36, cy - 11, on ? BLUE : WHITE);
    if (n.secure) drawLock(268, cy - 1, 16, SOFT);
    drawWifi(292, cy, 20, WHITE, bars(n.rssi));
  }
  if (netCount > NET_ROWS) {  // scroll indicator
    float h = (NET_ROW * NET_ROWS - 8) * NET_ROWS / (float)netCount;
    float y0 = NET_Y0 + 2 + (NET_ROW * NET_ROWS - 8 - h) * netScroll / (float)(netCount - NET_ROWS);
    fillShape(305, y0 - 2, 311, y0 + h + 2, WHITE, 0.35f, [=](float px, float py) { return capsCov(px, py, 308, y0 + 2, 308, y0 + h - 2, 1.5f); });
  }
  if (wstate == WS_CONNECTING || wstate == WS_OK || wstate == WS_FAIL) {
    glassRect(40, 80, 280, 160, 16, {30, 32, 40}, 0.95f, 0.12f);
    String t = wstate == WS_CONNECTING ? "Connecting to" : wstate == WS_OK ? "Connected to" : "Couldn't connect to";
    text(F_SMALL, t, W / 2, 96, SOFT, CENTER);
    text(F_MED, fitText(F_MED, joinSsid, 220), W / 2, 116, wstate == WS_FAIL ? ORANGE : WHITE, CENTER);
    if (wifiMsg.length()) text(F_SMALL, wifiMsg, W / 2, 140, SOFT, CENTER);
  }
}

// On-screen keyboard: letters (with shift), and two pages of numbers & symbols.
enum KeyKind : uint8_t { K_CHAR, K_SHIFT, K_BKSP, K_LAYER, K_ALT, K_SPACE, K_JOIN };
struct Key { int16_t x0, y0, x1, y1; KeyKind kind; char ch; };
static Key keys[40];
static int keyCount = 0, kbLayer = 0, kbPressed = -1;
static bool kbShift = false;
static String kbText, kbError;
static const Rect KB_CANCEL = {0, 0, 80, 30}, KB_FIELD = {10, 34, 310, 70};
static const int KB_ROW_Y[4] = {78, 117, 156, 195}, KB_KEY_H = 34;

static void addKey(float x0, int row, float w, KeyKind k, char c = 0) {
  keys[keyCount++] = {(int16_t)x0, (int16_t)KB_ROW_Y[row], (int16_t)(x0 + w), (int16_t)(KB_ROW_Y[row] + KB_KEY_H), k, c};
}
static void addRow(const char* chars, int row, float x0, float step, float w) {
  for (int i = 0; chars[i]; i++) addKey(x0 + i * step, row, w, K_CHAR, chars[i]);
}
static void buildKeys() {
  keyCount = 0;
  const float st = 31.2f, kw = 28;
  if (kbLayer == 0) {
    addRow("qwertyuiop", 0, 4, st, kw);
    addRow("asdfghjkl", 1, 4 + st / 2, st, kw);
    addKey(4, 2, 40, K_SHIFT);
    addRow("zxcvbnm", 2, 51, st, kw);
  } else if (kbLayer == 1) {
    addRow("1234567890", 0, 4, st, kw);
    addRow("-/:;()$&@\"", 1, 4, st, kw);
    addKey(4, 2, 40, K_ALT);
    addRow(".,?!'", 2, 56, 42, 36);
  } else {
    addRow("[]{}#%^*+=", 0, 4, st, kw);
    addRow("_\\|~<>`", 1, 4 + st * 1.5f, st, kw);
    addKey(4, 2, 40, K_ALT);
    addRow(".,?!'", 2, 56, 42, 36);
  }
  addKey(272, 2, 44, K_BKSP);
  addKey(4, 3, 66, K_LAYER);
  addKey(76, 3, 162, K_SPACE);
  addKey(244, 3, 72, K_JOIN);
}

static void drawKey(int i) {
  const Key& k = keys[i];
  bool pressed = i == kbPressed, special = k.kind != K_CHAR && k.kind != K_SPACE;
  RGB col = k.kind == K_JOIN ? BLUE : (k.kind == K_SHIFT && kbShift) ? TILE_ON : special ? RGB{70, 74, 86} : RGB{112, 116, 130};
  if (pressed) col = mix(col, {255, 255, 255}, 0.45f);
  fillRRect(k.x0, k.y0, k.x1, k.y1, 6, col, 0.95f);
  float cx = (k.x0 + k.x1) / 2.0f, cy = (k.y0 + k.y1) / 2.0f;
  switch (k.kind) {
    case K_CHAR: {
      char c = (kbLayer == 0 && kbShift) ? toupper(k.ch) : k.ch;
      text(F_MED, String(c), cx, cy - 12, WHITE, CENTER);
      break;
    }
    case K_SHIFT: {  // up arrow
      RGB a = kbShift ? DARK : WHITE;
      fillShape(cx - 9, cy - 10, cx + 9, cy + 10, a, 1, [=](float px, float py) {
        return fmaxf(triCov(px, py, cx, cy - 8, cx - 8, cy + 1, cx + 8, cy + 1), rrectCovFast(px, py, cx - 3.5f, cy, cx + 3.5f, cy + 8, 1));
      });
      break;
    }
    case K_BKSP:  // left-pointing tag with an x
      fillShape(cx - 13, cy - 9, cx + 13, cy + 9, WHITE, 1, [=](float px, float py) {
        float tag = fmaxf(triCov(px, py, cx - 12, cy, cx - 5, cy - 8, cx - 5, cy + 8), rrectCovFast(px, py, cx - 6, cy - 8, cx + 12, cy + 8, 2.5f));
        float x = fmaxf(capsCov(px, py, cx - 1, cy - 4, cx + 7, cy + 4, 1.2f), capsCov(px, py, cx - 1, cy + 4, cx + 7, cy - 4, 1.2f));
        return tag * (1 - x);
      });
      break;
    case K_LAYER: text(F_MED, kbLayer == 0 ? "123" : "ABC", cx, cy - 12, WHITE, CENTER); break;
    case K_ALT: text(F_MED, kbLayer == 1 ? "#+=" : "123", cx, cy - 12, WHITE, CENTER); break;
    case K_SPACE: text(F_SMALL, "space", cx, cy - 9, SOFT, CENTER); break;
    case K_JOIN: text(F_MED, "Join", cx, cy - 12, WHITE, CENTER); break;
  }
}

static void drawKeyField() {
  const Rect& f = KB_FIELD;
  glassRect(f.x0, f.y0, f.x1, f.y1, 10, {20, 22, 30}, 0.9f, 0.18f);
  int maxW = f.x1 - f.x0 - 26;
  String shown = kbText;  // show the end of long passwords
  while (shown.length() && textW(F_MED, shown) > maxW) shown.remove(0, 1);
  int tx = f.x0 + 10;
  if (shown.length()) text(F_MED, shown, tx, f.y0 + 6, WHITE);
  else text(F_MED, "Password", tx, f.y0 + 6, {120, 124, 136});
  float cx = tx + (shown.length() ? textW(F_MED, shown) + 2 : 0);
  fillShape(cx - 2, f.y0 + 7, cx + 2, f.y1 - 7, BLUE, 1, [=](float px, float py) { return capsCov(px, py, cx, f.y0 + 9, cx, f.y1 - 9, 1.1f); });
}

static void drawKeyboard() {
  dimStrip(0.3f);
  text(F_MED, "Cancel", 10, 5, BLUE);
  if (kbError.length()) text(F_SMALL, kbError, 310, 9, ORANGE, RIGHT);
  else text(F_MED, fitText(F_MED, joinSsid, 200), 310, 5, WHITE, RIGHT);
  drawKeyField();
  for (int i = 0; i < keyCount; i++) drawKey(i);
}

// ---------------------------------------------------------------- touch calibration
static int calStep = 0;
static const int CAL_PTS[2][2] = {{30, 30}, {290, 210}};
static int calRaw[2][2];

static void drawCalibration() {
  dimStrip(0.25f);
  int cx = CAL_PTS[calStep][0], cy = CAL_PTS[calStep][1];
  fillShape(cx - 14, cy - 14, cx + 14, cy + 14, WHITE, 1, [=](float px, float py) {
    return fmaxf(capsCov(px, py, cx - 12, cy, cx + 12, cy, 1), capsCov(px, py, cx, cy - 12, cx, cy + 12, 1));
  });
  fillShape(cx - 10, cy - 10, cx + 10, cy + 10, ORANGE, 1, [=](float px, float py) {
    float d = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
    return clamp01(1.4f - fabsf(d - 7) + 0.5f);
  });
  text(F_MED, "Touch calibration", W / 2, 96, WHITE, CENTER);
  text(F_SMALL, String("Tap the orange target (") + (calStep + 1) + " of 2)", W / 2, 124, SOFT, CENTER);
}

// ---------------------------------------------------------------- messages
static String msgTitle, msgLine1, msgLine2;
static void drawMessage() {
  text(F_GREET_S, msgTitle, W / 2, 46, CREAM, CENTER, true);
  text(F_MED, msgLine1, W / 2, 112, WHITE, CENTER, true);
  text(F_MED, msgLine2, W / 2, 140, SOFT, CENTER, true);
}

// ---------------------------------------------------------------- weather effects
// Rain and snow fall across the whole screen. Each frame only the small rectangles a
// particle moved through are re-rendered, so the rest of the screen is left alone.
enum Fx { FX_NONE, FX_RAIN, FX_SNOW };
struct Particle { float x, y, vx, vy, len, a, r, ph; };
static Particle parts[44];
static int partCount = 0;
static Fx fx = FX_NONE;
static int fxKey = -9999;

static bool isRainCode(int c) { return (c >= 51 && c <= 67) || (c >= 80 && c <= 82) || c >= 95; }
static bool isSnowCode(int c) { return (c >= 71 && c <= 77) || c == 85 || c == 86; }

static void spawnParticle(Particle& p, bool anywhere) {
  if (fx == FX_RAIN) {
    p.vy = 6 + (rnd() % 30) / 10.0f;
    p.vx = -0.2f * p.vy;
    p.len = 7 + p.vy * 0.8f;
    p.a = 0.3f + (rnd() % 100) / 330.0f;
    p.x = rnd() % (W + 40);
    p.y = anywhere ? (float)(rnd() % H) : -p.len - (rnd() % 40);
  } else {
    p.vy = 0.45f + (rnd() % 100) / 110.0f;
    p.vx = 0;
    p.r = 1.1f + (rnd() % 100) / 80.0f;
    p.a = 0.55f + (rnd() % 100) / 250.0f;
    p.ph = (rnd() % 628) / 100.0f;
    p.x = rnd() % W;
    p.y = anywhere ? (float)(rnd() % H) : -4.0f - (rnd() % 30);
  }
}

static int wantedFxKey() { return wx.valid ? curCode() * 2 + cfg.anim : -1; }

static void setupFx() {
  fxKey = wantedFxKey();
  int c = curCode();
  fx = FX_NONE;
  partCount = 0;
  if (cfg.anim && wx.valid) {
    if (isRainCode(c)) {
      fx = FX_RAIN;
      bool light = c <= 57 || c == 61 || c == 80, heavy = c == 65 || c == 67 || c == 82 || c >= 95;
      partCount = light ? 14 : heavy ? 30 : 22;
    } else if (isSnowCode(c)) {
      fx = FX_SNOW;
      partCount = (c == 71 || c == 85) ? 20 : (c == 75 || c == 86) ? 34 : 26;
    }
  }
  for (int i = 0; i < partCount; i++) spawnParticle(parts[i], true);
}

static void particleBox(const Particle& p, int& x0, int& y0, int& x1, int& y1) {
  if (fx == FX_RAIN) {
    float ex = p.x + p.vx * p.len / p.vy;
    x0 = (int)floorf(fminf(p.x, ex)) - 2; x1 = (int)ceilf(fmaxf(p.x, ex)) + 3;
    y0 = (int)floorf(p.y) - 2; y1 = (int)ceilf(p.y + p.len) + 3;
  } else {
    x0 = (int)floorf(p.x - p.r) - 2; x1 = (int)ceilf(p.x + p.r) + 3;
    y0 = (int)floorf(p.y - p.r) - 2; y1 = (int)ceilf(p.y + p.r) + 3;
  }
}

static void drawParticles() {
  for (int i = 0; i < partCount; i++) {
    const Particle& p = parts[i];
    int x0, y0, x1, y1;
    particleBox(p, x0, y0, x1, y1);
    if (y1 < stripY || y0 >= clipY1 || x1 < clipX0 || x0 >= clipX1) continue;
    if (fx == FX_RAIN) {
      float ax = p.x, ay = p.y, bx = p.x + p.vx * p.len / p.vy, by = p.y + p.len;
      fillShape(x0, y0, x1, y1, {205, 225, 255}, p.a, [=](float px, float py) {
        // streak fades in from the tail
        return capsCov(px, py, ax, ay, bx, by, 0.55f) * (0.35f + 0.65f * clamp01((py - ay) / (by - ay)));
      });
    } else {
      float cx = p.x, cy = p.y, r = p.r;
      fillShape(x0, y0, x1, y1, {250, 252, 255}, p.a, [=](float px, float py) {
        float dx = px - cx, dy = py - cy;
        return 1 - smooth(r * 0.45f, r + 0.7f, sqrtf(dx * dx + dy * dy));
      });
    }
  }
}

// ---------------------------------------------------------------- rendering
typedef void (*ContentFn)();
typedef void (*StripSink)();

static ContentFn contentFor(Mode m) {
  switch (m) {
    case MODE_DASH: return drawDashboard;
    case MODE_CC: return drawControlCenter;
    case MODE_CAL: return drawCalibration;
    case MODE_WIFI: return drawWifiList;
    case MODE_KEYB: return drawKeyboard;
    default: return drawMessage;
  }
}

static void prepareScene() {
  scheme = schemeFor(currentHour());
  colonHidden = timeValid() && time(nullptr) % 2;
  starsOn = scheme == NIGHT && !customBg[NIGHT] && (!wx.valid || curCode() <= 2);
}

// Background behind the animated weather icon, so the icon can be redrawn without
// recomputing the (slow) procedural background.
static const int ICON_X0 = 158, ICON_Y0 = 0, ICON_X1 = 242, ICON_Y1 = 86;
static uint16_t iconBg[(ICON_X1 - ICON_X0) * (ICON_Y1 - ICON_Y0)];
static bool iconBgOk = false;
static void iconBgCopy(bool store) {
  int y0 = max(stripY, ICON_Y0), y1 = min(clipY1, ICON_Y1);
  int x0 = max(clipX0, ICON_X0), x1 = min(clipX1, ICON_X1);
  for (int y = y0; y < y1; y++) {
    uint16_t* a = sbuf + (y - stripY) * W + x0;
    uint16_t* b = iconBg + (y - ICON_Y0) * (ICON_X1 - ICON_X0) + (x0 - ICON_X0);
    if (store) memcpy(b, a, (x1 - x0) * 2); else memcpy(a, b, (x1 - x0) * 2);
  }
}
static bool inIconRect() { return clipX0 >= ICON_X0 && clipX1 <= ICON_X1 && stripY >= ICON_Y0 && clipY1 <= ICON_Y1; }

// Render the current strip region. Particles go on top and are left out of the cache.
static void drawScene(ContentFn content, bool full) {
  bool dash = content == drawDashboard;
  if (dash && !full && iconBgOk && inIconRect()) iconBgCopy(false);
  else drawBackground();
  if (dash && full && clipX0 == 0 && clipX1 == W) iconBgCopy(true);
  if (content) content();
  if (dash && fx != FX_NONE) drawParticles();
}

static void renderFrame(ContentFn content, bool toScreen = true, StripSink sink = nullptr,
                        int x0 = 0, int x1 = W, int y0 = 0, int y1 = H) {
  prepareScene();
  if (content == drawDashboard) prepareLayout();
  bool full = toScreen && x0 == 0 && x1 == W && y0 == 0 && y1 == H;
  clipX0 = x0; clipX1 = x1;
  for (stripY = (y0 / STRIP) * STRIP; stripY < y1; stripY += STRIP) {
    clipY1 = stripY + STRIP;
    drawScene(content, full);
    if (toScreen) {
      if (x0 == 0 && x1 == W) spr.pushSprite(0, stripY);
      else spr.pushSprite(x0, stripY, x0, 0, x1 - x0, STRIP);
    }
    if (sink) sink();
  }
  clipX0 = 0; clipX1 = W;
  if (full && content == drawDashboard) iconBgOk = true;
}

// Send rows [stripY, clipY1) x [x0, x1) of the strip buffer to the display in one go.
static void pushWindow(int x0, int x1) {
  int w = x1 - x0, h = clipY1 - stripY;
  tft.startWrite();
  tft.setAddrWindow(x0, stripY, w, h);
  for (int r = 0; r < h; r++) tft.pushPixels(sbuf + r * W + x0, w);  // buffer is already in display byte order
  tft.endWrite();
}

// Re-render just one rectangle of the current screen (used by animations).
static void renderRect(int x0, int y0, int x1, int y1) {
  x0 = max(0, x0); y0 = max(0, y0); x1 = min(W, x1); y1 = min(H, y1);
  if (x0 >= x1 || y0 >= y1) return;
  ContentFn content = contentFor(mode);
  clipX0 = x0; clipX1 = x1;
  for (stripY = y0; stripY < y1; stripY += STRIP) {
    clipY1 = min(stripY + STRIP, y1);
    drawScene(content, false);
    pushWindow(x0, x1);
  }
  clipX0 = 0; clipX1 = W;
}

static void fxFrame(float dt) {
  struct Box { int16_t x0, y0, x1, y1; } boxes[44];
  float t = millis() / 1000.0f;
  for (int i = 0; i < partCount; i++) {
    Particle& p = parts[i];
    int ax0, ay0, ax1, ay1, bx0, by0, bx1, by1;
    particleBox(p, ax0, ay0, ax1, ay1);
    p.y += p.vy * dt;
    if (fx == FX_RAIN) p.x += p.vx * dt;
    else p.x += sinf(t * 1.3f + p.ph) * 0.45f * dt;
    if (p.y > H + 4 || p.x < -12 || p.x > W + 44) spawnParticle(p, false);
    particleBox(p, bx0, by0, bx1, by1);
    // one rect covering the old and new position when they overlap; otherwise (a respawned
    // particle) just the old one, since the new position starts above the screen
    bool overlap = by0 <= ay1 + 2 && by1 >= ay0 - 2 && bx0 <= ax1 + 2 && bx1 >= ax0 - 2;
    if (overlap) boxes[i] = {(int16_t)min(ax0, bx0), (int16_t)min(ay0, by0), (int16_t)max(ax1, bx1), (int16_t)max(ay1, by1)};
    else boxes[i] = {(int16_t)ax0, (int16_t)ay0, (int16_t)ax1, (int16_t)ay1};
  }
  for (int i = 0; i < partCount; i++) renderRect(boxes[i].x0, boxes[i].y0, boxes[i].x1, boxes[i].y1);
}

// Main weather icon on the home page: sun rays turn, clouds drift, rain falls.
static bool iconAnimated() {
  return cfg.anim && mode == MODE_DASH && page == PAGE_HOME && wx.valid && !(curCode() == 0 && !wx.isDay);
}

// Lightning: a double flash of the backlight (and the LED) every few seconds during storms.
static uint32_t flashStart = 0, nextFlash = 0;
static float lightningLevel(uint32_t t) {
  if (t < 70) return 1;
  if (t < 140) return 0.12f;
  if (t < 200) return 0.85f;
  return 0.85f * expf(-(t - 200) / 110.0f);
}
static void updateLightning() {
  bool storm = wx.valid && curCode() >= 95 && (cfg.anim || cfg.ledOn);
  uint32_t now = millis();
  if (!storm) {
    if (flashStart) { flashStart = 0; flashLevel = 0; applyBrightness(); }
    nextFlash = 0;
    return;
  }
  if (!nextFlash) nextFlash = now + 1500 + rnd() % 4000;
  if (!flashStart && now >= nextFlash) flashStart = now;
  if (!flashStart) return;
  uint32_t t = now - flashStart;
  if (t > 900) {
    flashStart = 0;
    flashLevel = 0;
    nextFlash = now + 3000 + rnd() % 9000;
    applyBrightness();
    return;
  }
  flashLevel = lightningLevel(t);
  if (cfg.anim && mode == MODE_DASH) {
    int base = max<int>(10, activeBrightness()), peak = min(255, base + 170);
    ledcWrite(0, base + (peak - base) * flashLevel);
  }
}

// ---------------------------------------------------------------- ambient LED
// RGB LED on the back (active-low): R = GPIO4, G = GPIO16, B = GPIO17
static const int LED_CH[3] = {2, 3, 4};
static void ledSet(float r, float g, float b) {
  float v[3] = {r, g, b};
  for (int i = 0; i < 3; i++) ledcWrite(LED_CH[i], 255 - (int)(powf(clamp01(v[i]), 2.2f) * 255 + 0.5f));
}
static float breathe(uint32_t periodMs) {
  float x = 0.5f - 0.5f * cosf(phase(periodMs) * TWO_PI);
  return x * x * (3 - 2 * x);
}
static void updateLed() {
  static uint32_t last = 0;
  if (millis() - last < 20) return;
  last = millis();
  if (!cfg.ledOn || !wx.valid) { ledSet(0, 0, 0); return; }
  float k = cfg.ledBright / 100.0f * (isNightDim(currentHour()) ? 0.3f : 1.0f);
  int c = curCode();
  RGB col;  // 0..1 per channel
  if (c >= 95) {  // thunder: flashes of white over a faint blue
    col = mix({0, 0.05f, 0.18f}, {1, 1, 1}, flashLevel);
  } else if (isRainCode(c)) {  // breathing blue
    float b = 0.1f + 0.9f * breathe(4000);
    col = {0, 0.22f * b, b};
  } else if (isSnowCode(c)) {  // slow icy breathing
    float b = 0.25f + 0.75f * breathe(6500);
    col = {0.55f * b, 0.75f * b, b};
  } else if (c >= 2) {  // cloudy / fog: static white
    col = {0.85f, 0.85f, 0.8f};
  } else if (wx.isDay || codeOverride >= 0) {  // sunny: warm orange-yellow
    col = {1, 0.42f, 0};
  } else {  // clear night: soft moonlight
    col = {0.12f, 0.16f, 0.5f};
  }
  ledSet(col.r * k, col.g * k, col.b * k);
}

static void serialSink() { Serial.write((const uint8_t*)spr.getPointer(), W * STRIP * 2); }

static void redraw() {
  if (fxKey != wantedFxKey()) setupFx();  // before drawing, so no stale particles are left behind
  bool shot = screenshotPending;
  screenshotPending = false;
  if (shot) Serial.println("\nSCREENSHOT_BEGIN");
  uint32_t t0 = millis();
  renderFrame(contentFor(mode), true, shot ? serialSink : nullptr);
  if (shot) Serial.println("\nSCREENSHOT_END");
  Serial.printf("Rendered mode %d in %lu ms\n", mode, millis() - t0);
}

static void showMessage(const String& title, const String& l1, const String& l2) {
  msgTitle = title; msgLine1 = l1; msgLine2 = l2;
  mode = MODE_MESSAGE;
  redraw();
}

// ---------------------------------------------------------------- network
static bool httpGet(const String& url, JsonDocument& doc) {
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(url)) return false;
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    DeserializationError err = deserializeJson(doc, http.getString());
    ok = !err;
    if (err) Serial.printf("JSON error: %s\n", err.c_str());
  } else {
    Serial.printf("HTTP %d for %s\n", code, url.c_str());
  }
  http.end();
  return ok;
}

static void lookupLocation() {
  if (!cfg.autoLoc) {
    lat = cfg.lat; lon = cfg.lon;
    haveLocation = true;
    return;
  }
  JsonDocument doc;
  if (httpGet("http://ip-api.com/json/?fields=status,city,lat,lon,offset", doc) && doc["status"] == "success") {
    lat = doc["lat"]; lon = doc["lon"];
    utcOffset = doc["offset"];
    ipCity = doc["city"].as<String>();
    haveLocation = true;
    Serial.printf("Location: %s (%.3f, %.3f) UTC%+ld\n", ipCity.c_str(), lat, lon, utcOffset / 3600);
  }
}

static bool fetchWeather() {
  if (!haveLocation) lookupLocation();
  if (!haveLocation) return false;
  char url[520];
  snprintf(url, sizeof url,
           "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,apparent_temperature,weather_code,is_day"
           "&hourly=temperature_2m,weather_code,is_day,precipitation_probability,precipitation"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset,uv_index_max"
           "&timezone=auto&forecast_days=6&forecast_hours=24&timeformat=unixtime",
           lat, lon);
  JsonDocument doc;
  if (!httpGet(url, doc)) return false;

  utcOffset = doc["utc_offset_seconds"] | utcOffset;
  JsonObject cur = doc["current"];
  wx.temp = cur["temperature_2m"];
  wx.feels = cur["apparent_temperature"];
  wx.code = cur["weather_code"];
  wx.isDay = cur["is_day"] | 1;
  time_t now = time(nullptr);
  JsonObject daily = doc["daily"];
  JsonArray days = daily["time"];
  wx.dayCount = min((int)days.size(), 6);
  for (int i = 0; i < wx.dayCount; i++) {
    wx.dayTime[i] = days[i].as<long>();
    wx.dayCode[i] = daily["weather_code"][i];
    wx.dayMax[i] = daily["temperature_2m_max"][i];
    wx.dayMin[i] = daily["temperature_2m_min"][i];
    wx.dayRain[i] = daily["precipitation_probability_max"][i] | 0;
    wx.dayUv[i] = daily["uv_index_max"][i] | 0.0f;
    wx.sunrise[i] = daily["sunrise"][i].as<long>();
    wx.sunset[i] = daily["sunset"][i].as<long>();
  }
  int today = todayIndex();
  wx.tmax = wx.dayMax[today];
  wx.tmin = wx.dayMin[today];

  JsonObject hourly = doc["hourly"];
  JsonArray times = hourly["time"];
  wx.hourCount = 0;
  wx.rainCount = 0;
  for (size_t i = 0; i < times.size(); i++) {
    time_t t = times[i].as<long>();
    if (t > now && wx.hourCount < 3) {
      wx.hourTime[wx.hourCount] = t;
      wx.hourTemp[wx.hourCount] = hourly["temperature_2m"][i];
      wx.hourCode[wx.hourCount] = hourly["weather_code"][i];
      wx.hourDay[wx.hourCount] = hourly["is_day"][i] | 1;
      wx.hourCount++;
    }
    if (t > now - 3600 && wx.rainCount < 12) {  // from the current hour on
      wx.rainTime[wx.rainCount] = t;
      wx.rainProb[wx.rainCount] = hourly["precipitation_probability"][i] | 0;
      wx.rainMm[wx.rainCount] = hourly["precipitation"][i] | 0.0f;
      wx.rainCount++;
    }
  }
  wx.valid = true;
  wx.fetchedAt = now;
  Serial.printf("Weather: %.1fC, code %d, day %d, hi %.1f lo %.1f\n", wx.temp, wx.code, wx.isDay, wx.tmax, wx.tmin);
  return true;
}

static void fetchAir() {
  char url[400];
  snprintf(url, sizeof url,
           "http://air-quality-api.open-meteo.com/v1/air-quality?latitude=%.4f&longitude=%.4f"
           "&current=european_aqi,us_aqi,pm2_5,pm10,ozone,nitrogen_dioxide,uv_index,"
           "alder_pollen,birch_pollen,grass_pollen,mugwort_pollen,olive_pollen,ragweed_pollen&timezone=auto",
           lat, lon);
  JsonDocument doc;
  if (!httpGet(url, doc)) return;
  JsonObject c = doc["current"];
  // European index inside Europe, US index elsewhere
  wx.aqiEU = lat > 34 && lat < 72 && lon > -32 && lon < 45;
  wx.aqi = (wx.aqiEU ? c["european_aqi"] : c["us_aqi"]) | 0;
  wx.pm25 = c["pm2_5"] | 0.0f;
  wx.pm10 = c["pm10"] | 0.0f;
  wx.ozone = c["ozone"] | 0.0f;
  wx.no2 = c["nitrogen_dioxide"] | 0.0f;
  wx.uv = c["uv_index"] | 0.0f;
  wx.pollenValid = false;
  wx.pollen = 0;
  for (const char* k : {"alder_pollen", "birch_pollen", "grass_pollen", "mugwort_pollen", "olive_pollen", "ragweed_pollen"})
    if (c[k].is<float>()) { wx.pollenValid = true; wx.pollen = fmaxf(wx.pollen, c[k].as<float>()); }
  wx.airValid = true;
  Serial.printf("Air: AQI %d (%s), UV %.1f, pollen %.0f\n", wx.aqi, wx.aqiEU ? "EU" : "US", wx.uv, wx.pollen);
}

static void applyRadio() {
  if (cfg.wifiOn) {
    if (WiFi.getMode() != WIFI_STA) {
      WiFi.mode(WIFI_STA);
      WiFi.begin();  // credentials saved by the setup portal
    }
  } else {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

// ---------------------------------------------------------------- web server
WebServer server(80);

static void sendJson(JsonDocument& doc, int code = 200) {
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

static void settingsToJson(JsonDocument& d) {
  d["use24h"] = cfg.use24h;
  d["fahrenheit"] = cfg.fahrenheit;
  d["brightDay"] = cfg.brightDay;
  d["brightNight"] = cfg.brightNight;
  d["autoDim"] = cfg.autoDim;
  d["dimStart"] = cfg.dimStart;
  d["dimEnd"] = cfg.dimEnd;
  d["autoLoc"] = cfg.autoLoc;
  d["showCity"] = cfg.showCity;
  d["lat"] = cfg.lat;
  d["lon"] = cfg.lon;
  d["city"] = cfg.city;
  d["rotation"] = cfg.rotation;
  d["refreshMin"] = cfg.refreshMin;
  d["wifiOn"] = cfg.wifiOn;
  d["airplane"] = cfg.airplane;
  d["anim"] = cfg.anim;
  d["ledOn"] = cfg.ledOn;
  d["ledBright"] = cfg.ledBright;
  JsonArray bg = d["backgrounds"].to<JsonArray>();
  for (int i = 0; i < 4; i++) bg.add(customBg[i]);
}

static void handleStatus() {
  JsonDocument d;
  d["version"] = FIRMWARE_VERSION;
  d["ip"] = WiFi.localIP().toString();
  d["ssid"] = WiFi.SSID();
  d["rssi"] = WiFi.RSSI();
  d["uptime"] = millis() / 1000;
  d["heap"] = ESP.getFreeHeap();
  d["fsUsed"] = LittleFS.usedBytes();
  d["fsTotal"] = LittleFS.totalBytes();
  d["timeValid"] = timeValid();
  d["epoch"] = (long)time(nullptr);
  d["utcOffset"] = utcOffset;
  d["city"] = cfg.autoLoc ? ipCity : cfg.city;
  d["lat"] = lat;
  d["lon"] = lon;
  JsonObject w = d["weather"].to<JsonObject>();
  w["valid"] = wx.valid;
  if (wx.valid) {
    w["temp"] = wx.temp;
    w["feels"] = wx.feels;
    w["high"] = wx.tmax;
    w["low"] = wx.tmin;
    w["condition"] = conditionName(wx.code);
    w["fetchedAt"] = (long)wx.fetchedAt;
    if (wx.dayCount) {
      int t = todayIndex();
      w["sunrise"] = (long)wx.sunrise[t];
      w["sunset"] = (long)wx.sunset[t];
      w["uvMax"] = wx.dayUv[t];
    }
    if (wx.airValid) {
      w["aqi"] = wx.aqi;
      w["aqiScale"] = wx.aqiEU ? "EU" : "US";
      w["aqiLevel"] = levelFor(wx.aqiEU ? AQI_EU : AQI_US, wx.aqi).name;
      w["uv"] = wx.uv;
    }
  }
  float mp = moonPhase(time(nullptr));
  String mn = moonName(mp);
  mn.replace("\n", " ");
  d["moon"] = mn;
  d["moonLit"] = (int)lroundf(moonIllum(mp) * 100);
  d["page"] = page;
  sendJson(d);
}

static void handleSettingsPost() {
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain"))) { server.send(400, "text/plain", "bad json"); return; }
  bool relocate = false;
  if (in["use24h"].is<bool>()) cfg.use24h = in["use24h"];
  if (in["fahrenheit"].is<bool>()) cfg.fahrenheit = in["fahrenheit"];
  if (in["brightDay"].is<int>()) cfg.brightDay = constrain(in["brightDay"].as<int>(), 10, 255);
  if (in["brightNight"].is<int>()) cfg.brightNight = constrain(in["brightNight"].as<int>(), 10, 255);
  if (in["autoDim"].is<bool>()) cfg.autoDim = in["autoDim"];
  if (in["dimStart"].is<int>()) cfg.dimStart = constrain(in["dimStart"].as<int>(), 0, 23);
  if (in["dimEnd"].is<int>()) cfg.dimEnd = constrain(in["dimEnd"].as<int>(), 0, 23);
  if (in["showCity"].is<bool>()) cfg.showCity = in["showCity"];
  if (in["refreshMin"].is<int>()) cfg.refreshMin = constrain(in["refreshMin"].as<int>(), 5, 240);
  if (in["autoLoc"].is<bool>() && in["autoLoc"] != cfg.autoLoc) { cfg.autoLoc = in["autoLoc"]; relocate = true; }
  if (in["lat"].is<float>() && in["lat"].as<float>() != cfg.lat) { cfg.lat = in["lat"]; relocate = true; }
  if (in["lon"].is<float>() && in["lon"].as<float>() != cfg.lon) { cfg.lon = in["lon"]; relocate = true; }
  if (in["city"].is<const char*>()) cfg.city = in["city"].as<String>();
  if (in["anim"].is<bool>()) cfg.anim = in["anim"];
  if (in["ledOn"].is<bool>()) cfg.ledOn = in["ledOn"];
  if (in["ledBright"].is<int>()) cfg.ledBright = constrain(in["ledBright"].as<int>(), 5, 100);
  if (in["rotation"].is<int>()) {
    cfg.rotation = in["rotation"] == 3 ? 3 : 1;
    tft.setRotation(cfg.rotation);
    ts.setRotation(cfg.rotation);
  }
  saveSettings();
  applyBrightness();
  if (relocate) { haveLocation = false; forceFetch = true; }
  if (mode == MODE_DASH) redraw();
  JsonDocument d;
  settingsToJson(d);
  sendJson(d);
}

static void webSink() {
  uint16_t* buf = (uint16_t*)spr.getPointer();
  for (int i = 0; i < W * STRIP; i++) buf[i] = (buf[i] >> 8) | (buf[i] << 8);  // BMP wants little-endian
  server.sendContent((const char*)buf, W * STRIP * 2);
}

static void handleScreenBmp() {
  static const uint32_t IMG = W * H * 2, OFF = 14 + 40 + 12;
  uint8_t hdr[OFF] = {'B', 'M'};
  auto put32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; i++) hdr[at + i] = v >> (8 * i); };
  put32(2, OFF + IMG); put32(10, OFF);
  put32(14, 40); put32(18, W); put32(22, (uint32_t)(-H));  // negative height = top-down rows
  hdr[26] = 1; hdr[28] = 16; put32(30, 3); put32(34, IMG);  // 16bpp, BI_BITFIELDS
  put32(38, 2835); put32(42, 2835);
  put32(54, 0xF800); put32(58, 0x07E0); put32(62, 0x001F);
  server.setContentLength(OFF + IMG);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "image/bmp", "");
  server.sendContent((const char*)hdr, OFF);
  if (server.hasArg("bg")) {
    // background-only preview of one time-of-day slot
    static const int SLOT_HOUR[] = {8, 14, 19, 23};
    int slot = constrain(server.arg("bg").toInt(), 0, 3);
    int saved = hourOverride;
    hourOverride = SLOT_HOUR[slot];
    renderFrame(nullptr, false, webSink);
    hourOverride = saved;
  } else {
    renderFrame(contentFor(mode), false, webSink);
  }
}

static File upFile;
static size_t upSize = 0;
static int upSlot = -1;
static bool upOk = false;

static void handleBgUpload() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    upSlot = server.hasArg("slot") ? server.arg("slot").toInt() : -1;
    upSize = 0;
    upOk = false;
    if (upSlot >= 0 && upSlot < 4) {
      closeBgFile();
      LittleFS.remove(bgPath(upSlot));  // free the space first
      customBg[upSlot] = false;
      upFile = LittleFS.open("/upload.tmp", "w");
    }
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (upFile) upSize += upFile.write(u.buf, u.currentSize);
  } else if (u.status == UPLOAD_FILE_END) {
    if (upFile) {
      upFile.close();
      if (upSize == (size_t)W * H * 2) upOk = LittleFS.rename("/upload.tmp", bgPath(upSlot));
      else LittleFS.remove("/upload.tmp");
    }
    scanBackgrounds();
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    if (upFile) upFile.close();
    LittleFS.remove("/upload.tmp");
  }
}

static void startPreview(int hour) {
  hourOverride = hour;
  previewUntil = millis() + 20000;
  mode = MODE_DASH;
  redraw();
}

static void showPage(int p) {
  page = constrain(p, 0, PAGE_COUNT - 1);
  lastInteraction = millis();
  mode = MODE_DASH;
  redraw();
}

// Show a weather condition (effects, icon and LED) for 30 seconds; code < 0 ends it.
static void previewWeather(int code) {
  codeOverride = code;
  codeOverrideUntil = code >= 0 ? millis() + 30000 : 0;
  if (mode == MODE_DASH) redraw();
}

static void setupWeb() {
  server.on("/", HTTP_GET, [] {
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html", (const char*)WEB_INDEX_GZ, WEB_INDEX_GZ_LEN);
  });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/settings", HTTP_GET, [] { JsonDocument d; settingsToJson(d); sendJson(d); });
  server.on("/api/settings", HTTP_POST, handleSettingsPost);
  server.on("/screen.bmp", HTTP_GET, handleScreenBmp);
  server.on("/api/bg", HTTP_POST, [] {
    server.send(upOk ? 200 : 400, "text/plain", upOk ? "ok" : "upload failed (expected 153600 bytes)");
    if (upOk && mode == MODE_DASH) redraw();
  }, handleBgUpload);
  server.on("/api/bg", HTTP_DELETE, [] {
    int slot = server.arg("slot").toInt();
    closeBgFile();
    if (slot >= 0 && slot < 4) LittleFS.remove(bgPath(slot));
    scanBackgrounds();
    server.send(200, "text/plain", "ok");
    if (mode == MODE_DASH) redraw();
  });
  server.on("/api/action", HTTP_POST, [] {
    String a = server.arg("do");
    server.send(200, "text/plain", "ok");
    if (a == "refresh") forceFetch = true;
    else if (a == "preview") startPreview(constrain(server.arg("hour").toInt(), 0, 23));
    else if (a == "weather") previewWeather(server.arg("code").toInt());
    else if (a == "page") showPage(server.arg("n").toInt());
    else if (a == "calibrate") { calStep = 0; mode = MODE_CAL; redraw(); }
    else if (a == "reboot") { delay(300); ESP.restart(); }
    else if (a == "forget") { delay(300); WiFiManager wm; wm.resetSettings(); ESP.restart(); }
  });
  server.onNotFound([] { server.send(404, "text/plain", "not found"); });
}

// ---------------------------------------------------------------- Wi-Fi picker logic
static uint32_t scanStarted = 0, nextScanAt = 0, connectStarted = 0, wifiResultAt = 0;
static int scanTries = 0;
static bool resumeAfterScan = false;  // reconnect to the saved network once the scan is done
static String prevSsid, prevPass;

// Scans fail (or finish instantly with nothing) while the radio is busy connecting, so
// connection attempts are paused during the scan and failed scans are retried.
static void startScan() {
  if (!cfg.wifiOn || cfg.airplane) {  // the radio has to be on to look for networks
    cfg.wifiOn = true;
    cfg.airplane = false;
    saveSettings();
    WiFi.mode(WIFI_STA);
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(false);
    resumeAfterScan = true;
  }
  wstate = WS_SCANNING;
  WiFi.scanDelete();
  int r = WiFi.scanNetworks(true);
  scanStarted = millis();
  nextScanAt = 0;
  Serial.printf("Wi-Fi scan %d started: %d (status %d)\n", scanTries + 1, r, WiFi.status());
}
static void retryScan(uint32_t afterMs) {
  WiFi.scanDelete();
  nextScanAt = millis() + afterMs;
}

static void openWifiList() {
  mode = MODE_WIFI;
  netScroll = 0;
  scanTries = 0;
  resumeAfterScan = false;
  lastInteraction = millis();
  startScan();
  redraw();
}

static void collectScan(int n) {
  netCount = 0;
  for (int i = 0; i < n; i++) {
    String id = WiFi.SSID(i);
    if (!id.length()) continue;  // hidden network
    int rssi = WiFi.RSSI(i), j;
    for (j = 0; j < netCount && nets[j].ssid != id; j++) {}
    if (j < netCount) { nets[j].rssi = max(nets[j].rssi, rssi); continue; }  // same network, another access point
    if (netCount == 20) continue;
    nets[netCount++] = {id, rssi, WiFi.encryptionType(i) != WIFI_AUTH_OPEN};
  }
  std::sort(nets, nets + netCount, [](const Net& a, const Net& b) { return a.rssi > b.rssi; });
  WiFi.scanDelete();
}

static void startConnect(const String& ssid, const String& pass) {
  resumeAfterScan = false;
  joinSsid = ssid;
  wifiMsg = "";
  prevSsid = WiFi.SSID();  // so we can go back if the new network doesn't work out
  prevPass = WiFi.psk();
  mode = MODE_WIFI;
  wstate = WS_CONNECTING;
  redraw();
  WiFi.disconnect(false);
  delay(100);
  WiFi.begin(ssid.c_str(), pass.length() ? pass.c_str() : nullptr);
  connectStarted = millis();
}

static void openKeyboard(const String& ssid) {
  joinSsid = ssid;
  kbText = "";
  kbError = "";
  kbLayer = 0;
  kbShift = false;
  kbPressed = -1;
  buildKeys();
  mode = MODE_KEYB;
  lastInteraction = millis();
  redraw();
}

// Called from loop(): scan results, connection progress, timeouts.
static void updateWifiPicker() {
  if (mode != MODE_WIFI && mode != MODE_KEYB) return;
  uint32_t now = millis();
  if (mode == MODE_WIFI && wstate == WS_SCANNING) {
    if (nextScanAt) {
      if (now >= nextScanAt) startScan();
      return;
    }
    int n = WiFi.scanComplete();
    // an empty result that comes back almost at once is a scan that never really ran
    bool bogus = n == 0 && now - scanStarted < 1500;
    if (n > 0 || (n == 0 && !bogus)) {
      Serial.printf("Wi-Fi scan found %d\n", n);
      collectScan(n);
      wstate = WS_LIST;
      if (resumeAfterScan) { WiFi.begin(); resumeAfterScan = false; }
      redraw();
    } else if (bogus || n == WIFI_SCAN_FAILED || now - scanStarted > 12000) {
      if (++scanTries < 6) retryScan(800);
      else {
        Serial.println("Wi-Fi scan gave up");
        wstate = WS_LIST;
        if (resumeAfterScan) { WiFi.begin(); resumeAfterScan = false; }
        redraw();
      }
    }
  } else if (wstate == WS_CONNECTING) {
    wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
      wstate = WS_OK;
      wifiMsg = WiFi.localIP().toString();
      wifiResultAt = now;
      Serial.printf("Joined %s\n", joinSsid.c_str());
      redraw();
    } else if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL || now - connectStarted > 15000) {
      bool secure = false;
      for (int i = 0; i < netCount; i++) if (nets[i].ssid == joinSsid) secure = nets[i].secure;
      Serial.printf("Could not join %s (status %d)\n", joinSsid.c_str(), st);
      if (prevSsid.length() && prevSsid != joinSsid) WiFi.begin(prevSsid.c_str(), prevPass.c_str());  // back to the old network
      if (secure) {
        kbError = "Couldn't join - check the password";
        kbPressed = -1;
        mode = MODE_KEYB;
        wstate = WS_LIST;
      } else {
        wstate = WS_FAIL;
        wifiMsg = "Out of range?";
        wifiResultAt = now;
      }
      redraw();
    }
  } else if ((wstate == WS_OK || wstate == WS_FAIL) && now - wifiResultAt > 2500) {
    if (wstate == WS_OK) { mode = MODE_DASH; forceFetch = true; }
    wstate = WS_LIST;
    redraw();
  }
  if (wstate != WS_CONNECTING && now - lastInteraction > (mode == MODE_KEYB ? 120000UL : 60000UL)) {
    mode = MODE_DASH;  // nobody's using it
    redraw();
  }
}

static int keyAt(int x, int y) {
  int best = -1;
  float bd = 14;  // forgiving: nearest key within 14 px
  for (int i = 0; i < keyCount; i++) {
    const Key& k = keys[i];
    float dx = x < k.x0 ? k.x0 - x : (x > k.x1 ? x - k.x1 : 0), dy = y < k.y0 ? k.y0 - y : (y > k.y1 ? y - k.y1 : 0);
    float d = sqrtf(dx * dx + dy * dy);
    if (d < bd) { bd = d; best = i; }
  }
  return best;
}
static void redrawKey(int i) {
  if (i < 0) return;
  const Key& k = keys[i];
  renderRect(k.x0 - 1, k.y0 - 1, k.x1 + 2, k.y1 + 2);
}
static void redrawField() { renderRect(KB_FIELD.x0 - 1, KB_FIELD.y0 - 1, KB_FIELD.x1 + 2, KB_FIELD.y1 + 2); }

static void pressKey(int i) {
  const Key& k = keys[i];
  bool full = false;
  if (kbError.length()) { kbError = ""; full = true; }
  switch (k.kind) {
    case K_CHAR:
      if (kbText.length() < 63) kbText += (kbLayer == 0 && kbShift) ? (char)toupper(k.ch) : k.ch;
      if (kbShift) { kbShift = false; full = true; }  // shift applies to one letter
      break;
    case K_SPACE: if (kbText.length() < 63) kbText += ' '; break;
    case K_BKSP: if (kbText.length()) kbText.remove(kbText.length() - 1); break;
    case K_SHIFT: kbShift = !kbShift; full = true; break;
    case K_LAYER: kbLayer = kbLayer == 0 ? 1 : 0; full = true; break;
    case K_ALT: kbLayer = kbLayer == 1 ? 2 : 1; full = true; break;
    case K_JOIN:
      if (kbText.length() >= 8) { startConnect(joinSsid, kbText); return; }
      kbError = "Passwords are at least 8 characters";
      full = true;
      break;
  }
  if (full) { buildKeys(); kbPressed = -1; redraw(); }
  else { kbPressed = -1; redrawKey(i); redrawField(); }
}

// ---------------------------------------------------------------- touch handling
struct TouchState {
  bool down = false;
  int sx = 0, sy = 0, x = 0, y = 0, rx = 0, ry = 0;
  uint32_t t0 = 0, last = 0;
  bool handled = false, slider = false;
} tch;

static bool readTouch(int& x, int& y, int& rx, int& ry) {
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint();
  if (p.z < 250) return false;
  rx = p.x; ry = p.y;
  x = constrain(map(p.x, cfg.tsX0, cfg.tsX1, 0, W - 1), 0, W - 1);
  y = constrain(map(p.y, cfg.tsY0, cfg.tsY1, 0, H - 1), 0, H - 1);
  return true;
}

static void openControlCenter() { mode = MODE_CC; ccLastTouch = millis(); redraw(); }
static void closeControlCenter() { mode = MODE_DASH; redraw(); }

static void setSliderFromY(int y) {
  float t = clamp01((CC_SLIDER.y1 - y) / (float)(CC_SLIDER.y1 - CC_SLIDER.y0));
  activeBrightness() = 10 + t * 245;
  applyBrightness();
}
static void redrawSlider() {
  renderFrame(drawControlCenter, true, nullptr, CC_SLIDER.x0 - 2, CC_SLIDER.x1 + 3, CC_SLIDER.y0, CC_SLIDER.y1 + 1);
}

static int scrollStart = 0;
static uint32_t lastRepeat = 0;

static void onTouchStart() {
  lastInteraction = millis();
  scrollStart = netScroll;
  if (mode == MODE_KEYB) {
    kbPressed = keyAt(tch.x, tch.y);
    lastRepeat = millis();
    redrawKey(kbPressed);
  }
  if (mode == MODE_CC) {
    ccLastTouch = millis();
    if (CC_SLIDER.hit(tch.x, tch.y, 2)) { tch.slider = true; setSliderFromY(tch.y); redrawSlider(); }
  }
}

static void onTouchMove() {
  static uint32_t lastSliderDraw = 0;
  int dx = tch.x - tch.sx, dy = tch.y - tch.sy;
  lastInteraction = millis();
  if (mode == MODE_CC && !tch.handled && CC_WIFI.hit(tch.sx, tch.sy) && dx * dx + dy * dy < 15 * 15 && millis() - tch.t0 > 650) {
    tch.handled = true;  // long press on Wi-Fi: choose a network
    openWifiList();
    return;
  }
  if (mode == MODE_WIFI && wstate != WS_CONNECTING && netCount > NET_ROWS && abs(dy) > 12) {
    tch.handled = true;  // drag to scroll, a whole row at a time
    int ns = constrain(scrollStart - (int)lroundf(dy / (float)NET_ROW), 0, netCount - NET_ROWS);
    if (ns != netScroll) { netScroll = ns; redraw(); }
    return;
  }
  if (mode == MODE_KEYB && kbPressed >= 0 && keys[kbPressed].kind == K_BKSP && millis() - tch.t0 > 500 && millis() - lastRepeat > 90) {
    lastRepeat = millis();  // hold backspace to keep deleting
    tch.handled = true;
    if (kbText.length()) { kbText.remove(kbText.length() - 1); redrawField(); }
    return;
  }
  if (mode == MODE_KEYB && kbPressed >= 0 && !tch.handled) {  // finger slid onto another key
    int k = keyAt(tch.x, tch.y);
    if (k != kbPressed) { int old = kbPressed; kbPressed = k; redrawKey(old); redrawKey(k); }
    return;
  }
  if (mode == MODE_DASH && !tch.handled && tch.sx > 200 && tch.sy < 50 && dy > 35) {
    tch.handled = true;
    openControlCenter();
  } else if (mode == MODE_DASH && !tch.handled && abs(dx) > 45 && abs(dx) > 1.6f * abs(dy)) {
    tch.handled = true;  // swipe left = next page, right = previous
    int np = page + (dx < 0 ? 1 : -1);
    if (np >= 0 && np < PAGE_COUNT) showPage(np);
  } else if (mode == MODE_CC) {
    ccLastTouch = millis();
    if (tch.slider) {
      setSliderFromY(tch.y);
      if (millis() - lastSliderDraw > 60) { redrawSlider(); lastSliderDraw = millis(); }
    } else if (!tch.handled && dy < -40 && abs(dy) > abs(dx)) {
      tch.handled = true;
      closeControlCenter();
    }
  }
}

static void onTap(int x, int y) {
  if (mode == MODE_WIFI) {
    if (wstate == WS_CONNECTING) return;
    if (wstate == WS_OK || wstate == WS_FAIL) { wstate = WS_LIST; redraw(); return; }
    if (WL_BACK.hit(x, y, 0)) {
      if (resumeAfterScan) { WiFi.begin(); resumeAfterScan = false; }
      openControlCenter();
      return;
    }
    if (WL_RESCAN.hit(x, y, 0)) { if (wstate != WS_SCANNING) { scanTries = 0; startScan(); redraw(); } return; }
    int r = (y - NET_Y0) / NET_ROW;
    if (y >= NET_Y0 && r < NET_ROWS && netScroll + r < netCount) {
      const Net& n = nets[netScroll + r];
      if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == n.ssid) return;  // already on it
      if (n.secure) openKeyboard(n.ssid);
      else startConnect(n.ssid, "");
    }
    return;
  }
  if (mode == MODE_KEYB) {
    if (KB_CANCEL.hit(x, y, 0)) { mode = MODE_WIFI; wstate = WS_LIST; redraw(); }
    return;
  }
  if (mode == MODE_DASH) {
    if (page != PAGE_HOME) return;
    if (x < TIME_X1 && y > TIME_Y0) { cfg.use24h = !cfg.use24h; saveSettings(); redraw(); }
    else if (x > TEMP_X0 && y < TEMP_Y1 && wx.valid) { cfg.fahrenheit = !cfg.fahrenheit; saveSettings(); redraw(); }
    return;
  }
  if (mode == MODE_CC) {
    ccLastTouch = millis();
    if (CC_AIR.hit(x, y)) {
      cfg.airplane = !cfg.airplane;
      cfg.wifiOn = !cfg.airplane;
      applyRadio();
    } else if (CC_WIFI.hit(x, y)) {
      if (millis() - tch.t0 > 450) return;  // part of a long press, not a tap
      cfg.wifiOn = !cfg.wifiOn;
      applyRadio();
    } else if (CC_24.hit(x, y)) {
      cfg.use24h = !cfg.use24h;
    } else if (CC_UNIT.hit(x, y)) {
      cfg.fahrenheit = !cfg.fahrenheit;
    } else if (CC_NIGHT.hit(x, y)) {
      cfg.autoDim = !cfg.autoDim;
      applyBrightness();
    } else if (CC_REFRESH.hit(x, y)) {
      forceFetch = true;
      refreshing = cfg.wifiOn;
    } else if (!CC_CONN.hit(x, y) && !CC_SLIDER.hit(x, y)) {
      closeControlCenter();
      return;
    } else {
      return;
    }
    saveSettings();
    redraw();
    return;
  }
  if (mode == MODE_CAL) {
    calRaw[calStep][0] = tch.rx;
    calRaw[calStep][1] = tch.ry;
    if (++calStep < 2) { redraw(); return; }
    float sx = (calRaw[1][0] - calRaw[0][0]) / (float)(CAL_PTS[1][0] - CAL_PTS[0][0]);
    float sy = (calRaw[1][1] - calRaw[0][1]) / (float)(CAL_PTS[1][1] - CAL_PTS[0][1]);
    if (fabsf(sx) > 3 && fabsf(sy) > 3) {  // sanity check before saving
      cfg.tsX0 = calRaw[0][0] - CAL_PTS[0][0] * sx;
      cfg.tsX1 = cfg.tsX0 + (W - 1) * sx;
      cfg.tsY0 = calRaw[0][1] - CAL_PTS[0][1] * sy;
      cfg.tsY1 = cfg.tsY0 + (H - 1) * sy;
      saveSettings();
      Serial.printf("Touch calibrated: x %d..%d  y %d..%d\n", cfg.tsX0, cfg.tsX1, cfg.tsY0, cfg.tsY1);
    }
    mode = MODE_DASH;
    redraw();
  }
}

static void onTouchEnd() {
  if (tch.slider) { saveSettings(); redrawSlider(); return; }
  if (mode == MODE_KEYB && kbPressed >= 0) {
    int k = kbPressed;
    if (tch.handled && keys[k].kind == K_BKSP) { kbPressed = -1; redrawKey(k); return; }  // was auto-repeating
    pressKey(k);
    return;
  }
  if (tch.handled) return;
  int dx = tch.x - tch.sx, dy = tch.y - tch.sy;
  if (dx * dx + dy * dy < 18 * 18 && millis() - tch.t0 < 800) onTap(tch.sx, tch.sy);
}

static void pollTouch() {
  int x, y, rx, ry;
  if (readTouch(x, y, rx, ry)) {
    if (!tch.down) {
      tch = TouchState();
      tch.down = true;
      tch.sx = tch.x = x; tch.sy = tch.y = y;
      tch.rx = rx; tch.ry = ry;
      tch.t0 = tch.last = millis();
      Serial.printf("Touch at %d,%d (raw %d,%d)\n", x, y, rx, ry);
      onTouchStart();
    } else {
      tch.x = x; tch.y = y; tch.rx = rx; tch.ry = ry; tch.last = millis();
      onTouchMove();
    }
  } else if (tch.down && millis() - tch.last > 100) {  // debounce brief dropouts
    tch.down = false;
    onTouchEnd();
  }
}

// ---------------------------------------------------------------- animation
static void animate() {
  static uint32_t lastFx = 0, lastIcon = 0, lastStar = 0;
  static int starIdx = 0;
  if (mode != MODE_DASH) return;
  uint32_t now = millis();
  bool hide = timeValid() && time(nullptr) % 2;  // blink the clock's colon
  if (hide != colonHidden) {
    colonHidden = hide;
    if (page == PAGE_HOME && L.colX1) renderRect(L.colX0, L.colY0, L.colX1, L.colY1);
  }
  if (fx != FX_NONE && now - lastFx >= 33) {
    static uint32_t frames = 0, busy = 0, since = 0;
    float dt = lastFx ? constrain((now - lastFx) / 33.0f, 0.5f, 1.25f) : 1;  // slow down rather than jump
    lastFx = now;
    fxFrame(dt);
    frames++;
    busy += millis() - now;
    if (now - since > 10000) {
      if (since) Serial.printf("Effects: %.1f fps, %lu ms per frame\n", frames * 1000.0f / (now - since), busy / max<uint32_t>(frames, 1));
      frames = busy = 0;
      since = now;
    }
  }
  if (iconAnimated() && now - lastIcon >= 90) {
    lastIcon = now;
    renderRect(ICON_X0, ICON_Y0, ICON_X1, ICON_Y1);
  }
  if (starsOn && cfg.anim && now - lastStar >= 60) {  // twinkle a couple of stars at a time
    lastStar = now;
    for (int k = 0; k < 2; k++) {
      const Star& s = stars[starIdx];
      starIdx = (starIdx + 1) % (int)(sizeof stars / sizeof stars[0]);
      renderRect(s.x - 3, s.y - 3, s.x + 4, s.y + 4);
    }
  }
}

// ---------------------------------------------------------------- main
static void onPortal(WiFiManager*) {
  showMessage("WiFi Setup", "On your phone, join WiFi", String("\"") + SETUP_AP_NAME + "\" to connect me");
}

void setup() {
  Serial.begin(115200);
  // RGB LED on the back (active-low), dimmed with PWM
  const int ledPins[3] = {4, 16, 17};
  for (int i = 0; i < 3; i++) {
    ledcSetup(LED_CH[i], 5000, 8);
    ledcAttachPin(ledPins[i], LED_CH[i]);
    ledcWrite(LED_CH[i], 255);
  }
  pinMode(0, INPUT_PULLUP);

  loadSettings();
  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
  scanBackgrounds();

  tft.init();
  tft.setRotation(cfg.rotation);
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL, 0);
  ledcWrite(0, cfg.brightDay);

  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(cfg.rotation);

  loadFonts();
  spr.setColorDepth(16);
  if (!spr.createSprite(W, STRIP)) Serial.println("Sprite allocation failed!");
  sbuf = (uint16_t*)spr.getPointer();
  initBackground();
  setupWeb();

  if (digitalRead(0) == LOW) {
    showMessage("WiFi Reset", "Saved network forgotten", "");
    WiFiManager wm;
    wm.resetSettings();
    cfg.wifiOn = true; cfg.airplane = false;
    saveSettings();
    delay(1000);
  }

  if (cfg.wifiOn) {
    showMessage("Hello!", "Connecting to WiFi...", "");
    WiFiManager wm;
    wm.setAPCallback(onPortal);
    wm.setConnectTimeout(20);
    if (!wm.autoConnect(SETUP_AP_NAME)) ESP.restart();
    Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
    showMessage("Hello!", "Setting the clock...", "");
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    for (int i = 0; i < 100 && !timeValid(); i++) delay(100);
  } else {
    WiFi.mode(WIFI_OFF);
  }
  mode = MODE_DASH;
}

void loop() {
  static int lastMinute = -2;
  static uint32_t lastFetch = 0, nextFetchDelay = 0;
  static bool wasConnected = false;

  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) {
    Serial.printf("Online at http://%s/ (http://%s.local/)\n", WiFi.localIP().toString().c_str(), MDNS_NAME);
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    server.begin();
    MDNS.end();
    if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);
    forceFetch = true;
    if (mode == MODE_CC) redraw();
  } else if (!connected && wasConnected && mode == MODE_CC) {
    redraw();
  }
  wasConnected = connected;
  if (connected) server.handleClient();

  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'S') { screenshotPending = true; redraw(); }
    if (c == 'c') { mode = MODE_CC; ccLastTouch = millis(); }  // open control centre (for testing)
    if (c == 'n') openWifiList();                         // Wi-Fi list (for testing)
    if (c == 'k') { openKeyboard("Example Network"); kbText = "example"; redraw(); }  // keyboard (for testing)
    if (c == 'p') { delay(5); char a = Serial.read(); if (isdigit(a)) { page = constrain(a - '0', 0, PAGE_COUNT - 1); lastInteraction = millis(); mode = MODE_DASH; } }
    if (c == 'w') {  // w61 → preview weather code 61 for 30 s, w-- → real weather
      delay(5);
      char a = Serial.read(), b = Serial.read();
      codeOverride = (isdigit(a) && isdigit(b)) ? (a - '0') * 10 + (b - '0') : -1;
      codeOverrideUntil = codeOverride >= 0 ? millis() + 30000 : 0;
      mode = MODE_DASH;
    }
    if (c == 'h') {  // h07 → preview 07:xx, h-- → back to real time
      delay(5);
      char a = Serial.read(), b = Serial.read();
      if (isdigit(a) && isdigit(b)) startPreview((a - '0') * 10 + (b - '0'));
      else { hourOverride = -1; previewUntil = 0; mode = MODE_DASH; lastMinute = -2; }
    }
  }

  pollTouch();
  if (tch.down) { delay(5); return; }  // stay responsive while a finger is down

  if (connected && (forceFetch || millis() - lastFetch > nextFetchDelay)) {
    forceFetch = false;
    lastFetch = millis();
    bool ok = fetchWeather();
    if (ok) fetchAir();
    nextFetchDelay = ok ? cfg.refreshMin * 60000UL : 60000UL;
    refreshing = false;
    if (mode == MODE_DASH || mode == MODE_CC) redraw();
  } else if (refreshing && !connected) {
    refreshing = false;
    if (mode == MODE_CC) redraw();
  }

  if (previewUntil && millis() > previewUntil) { previewUntil = 0; hourOverride = -1; lastMinute = -2; }

  if (mode == MODE_CC && millis() - ccLastTouch > 20000) closeControlCenter();
  updateWifiPicker();
  if (codeOverrideUntil && millis() > codeOverrideUntil) { codeOverrideUntil = 0; codeOverride = -1; lastMinute = -2; }
  // back to the home page after two minutes on another page
  if (mode == MODE_DASH && page != PAGE_HOME && millis() - lastInteraction > 120000) { page = PAGE_HOME; lastMinute = -2; }
  if (mode == MODE_DASH && fxKey != wantedFxKey()) lastMinute = -2;  // weather changed: full redraw

  int minute = timeValid() ? localNow().tm_min : -1;
  if (minute != lastMinute) {
    lastMinute = minute;
    applyBrightness();
    if (mode == MODE_DASH) redraw();
  }

  updateLightning();
  updateLed();
  animate();
  delay(fx != FX_NONE ? 1 : 10);
}

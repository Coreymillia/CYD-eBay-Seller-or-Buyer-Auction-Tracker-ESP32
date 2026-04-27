// CYDEbayTicker - eBay Auction Monitor for CYD (Cheap Yellow Display)
// Fetches active auctions from up to 3 eBay sellers (or a keyword search)
// and displays them sorted by ending soonest on a 320x240 ILI9341 TFT.
// Updates every 5 minutes. Short press BOOT cycles pages; 3s hold = setup.

#define DEVICE_NAME      "CYDEbayTicker"
#define FIRMWARE_VERSION "1.0.2"
#include "CYDIdentity.h"

#include <Arduino.h>
#include <WiFi.h>

/*******************************************************************************
 * Display setup - CYD (Cheap Yellow Display)
 * ILI9341 320x240 landscape via hardware SPI
 ******************************************************************************/
#include <Arduino_GFX_Library.h>
#include <XPT2046_Touchscreen.h>

#define GFX_BL 21

Arduino_DataBus *bus = new Arduino_HWSPI(
    2  /* DC */,
    15 /* CS */,
    14 /* SCK */,
    13 /* MOSI */,
    12 /* MISO */);

Arduino_GFX *gfx = new Arduino_ILI9341(bus, GFX_NOT_DEFINED /* RST */, 1 /* rotation: landscape */);
/*******************************************************************************
 * End of display setup
 ******************************************************************************/

// Touch controller (XPT2046 on VSPI, CYD standard wiring)
#define TOUCH_CS  33
#define TOUCH_IRQ 36
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
static unsigned long lastTouchMs = 0;

#include "Portal.h"
#include "eBay.h"

// ---------------------------------------------------------------------------
// Layout constants
// ---------------------------------------------------------------------------
#define HEADER_H    14
#define SUBHDR_H     9
#define DIVIDER_Y   24
#define ROWS_Y      26
#define ROW_H       21

#define COLOR_GOLD    0xFEA0   // eBay gold/yellow
#define COLOR_HEADER  0x2000   // dark red-brown
#define COLOR_TEXT    RGB565_WHITE
#define COLOR_DIM     0x8410   // grey
#define COLOR_ENDED   0xF800   // red
#define COLOR_SOON    0xFD20   // orange (< 1 hour)
#define COLOR_OK      0x07E0   // green

// Per-row title colors — cycles through this palette
static const uint16_t TITLE_COLORS[] = {
  0xFFFF,  // white
  0x07FF,  // cyan
  0xFFE0,  // yellow
  0xF81F,  // magenta
  0x07E0,  // green
  0xFD20,  // orange
  0xAFDF,  // sky blue
  0xFBB0,  // peach
  0xBFF7,  // light cyan
  0xFC10,  // amber
};
#define NUM_TITLE_COLORS (sizeof(TITLE_COLORS) / sizeof(TITLE_COLORS[0]))

// Refresh interval: 5 minutes (eBay rate limits; auctions don't change that fast)
#define REFRESH_INTERVAL (5 * 60 * 1000UL)

// ---------------------------------------------------------------------------
// Paging state
// ---------------------------------------------------------------------------
static int currentPage    = 0;
static int totalPages     = 1;
static unsigned long lastRefresh  = 0;
static unsigned long lastTimeTick = 0;
static bool fetchOk = false;

// ---------------------------------------------------------------------------
// Scroll state — one offset (pixels) per visible row; pause counter in ticks
// ---------------------------------------------------------------------------
#define TITLE_AREA_W  183   // px — title column ends here; price starts at 184
#define SCROLL_SPEED    1   // px per tick
#define SCROLL_TICK    30   // ms between ticks (≈ 33px/s = ~2.7 chars/s smooth)
#define SCROLL_PAUSE   50   // ticks to pause at start/wrap (50 * 30ms = 1.5s)

static int  sc_offset[EB_PER_PAGE];
static int  sc_pause [EB_PER_PAGE];
static int  sc_active = -1;          // which row is currently scrolling (-1 = find next)
static unsigned long sc_lastTick = 0;

// Returns the next row index (≥0) that has a title long enough to scroll,
// wrapping after 'current'. Returns -1 if none on this page.
int findNextScrollable(int startIdx, int current, int rowCount) {
  for (int tries = 0; tries < rowCount; tries++) {
    int next = (current + 1 + tries) % rowCount;
    int idx  = startIdx + next;
    if (idx >= eb_item_count) continue;
    EbayItem &item = eb_items[idx];
    if (!item.valid) continue;
    if ((int)strlen(item.title) * 12 - TITLE_AREA_W > 0) return next;
  }
  return -1;
}

void resetScroll() {
  for (int i = 0; i < EB_PER_PAGE; i++) {
    sc_offset[i] = 0;
    sc_pause [i] = SCROLL_PAUSE;
  }
  sc_active = -1;
}

// ---------------------------------------------------------------------------
// Helper: truncate string to maxChars, appending ".." if cut
// ---------------------------------------------------------------------------
void truncate(const char *src, char *dst, int maxChars) {
  int len = strlen(src);
  if (len <= maxChars) {
    strcpy(dst, src);
  } else {
    strncpy(dst, src, maxChars - 2);
    dst[maxChars - 2] = '.';
    dst[maxChars - 1] = '.';
    dst[maxChars]     = '\0';
  }
}

// ---------------------------------------------------------------------------
// Status message in header
// ---------------------------------------------------------------------------
void showStatus(const char *msg) {
  gfx->fillRect(0, 0, gfx->width(), HEADER_H, COLOR_HEADER);
  gfx->setTextColor(COLOR_GOLD);
  gfx->setTextSize(1);
  gfx->setCursor(4, 3);
  gfx->print(msg);
  Serial.println(msg);
}

// ---------------------------------------------------------------------------
// Draw the header bar with title, page indicator, and last-updated time
// ---------------------------------------------------------------------------
void drawHeader() {
  gfx->fillRect(0, 0, gfx->width(), HEADER_H, COLOR_HEADER);
  gfx->setTextSize(1);

  // Title: "eBay Auctions" or sellers list
  gfx->setTextColor(COLOR_GOLD);
  gfx->setCursor(4, 3);
  if (eb_seller1[0] || eb_seller2[0] || eb_seller3[0]) {
    char sellers[72] = "";
    if (eb_seller1[0]) { strncat(sellers, eb_seller1, 20); strncat(sellers, " ", 2); }
    if (eb_seller2[0]) { strncat(sellers, eb_seller2, 20); strncat(sellers, " ", 2); }
    if (eb_seller3[0]) { strncat(sellers, eb_seller3, 20); }
    // trim trailing space
    int sl = strlen(sellers);
    if (sl > 0 && sellers[sl-1] == ' ') sellers[sl-1] = '\0';
    // leave room for page indicator (~8 chars = 48px from right)
    char hdr[68];
    truncate(sellers, hdr, (gfx->width() - 56) / 6);
    gfx->print(hdr);
  } else {
    gfx->print("eBay Auctions");
  }

  // Page indicator (right side)
  char pgBuf[12];
  snprintf(pgBuf, sizeof(pgBuf), "Pg %d/%d", currentPage + 1, totalPages);
  gfx->setTextColor(COLOR_DIM);
  int pgX = gfx->width() - strlen(pgBuf) * 6 - 4;
  gfx->setCursor(pgX, 3);
  gfx->print(pgBuf);

  // Sub-header: auction count (cyan) + last updated time (grey)
  gfx->fillRect(0, HEADER_H, gfx->width(), SUBHDR_H, RGB565_BLACK);
  gfx->setCursor(4, HEADER_H + 1);

  if (eb_item_count > 0) {
    // Count in cyan
    char cntBuf[12];
    snprintf(cntBuf, sizeof(cntBuf), "%d auctions", eb_item_count);
    gfx->setTextColor(0x07FF);  // cyan
    gfx->print(cntBuf);

    // Last successful refresh time in grey right after
    time_t stamp = eb_last_fetch ? eb_last_fetch : time(nullptr);
    struct tm *t = gmtime(&stamp);
    char timeBuf[24];
    snprintf(timeBuf, sizeof(timeBuf),
             eb_refresh_stale ? "  stale %02d:%02d" : "  %02d:%02d UTC",
             t->tm_hour, t->tm_min);
    gfx->setTextColor(COLOR_DIM);
    gfx->print(timeBuf);

    // Total of confirmed bids only (hasBid=true) in gold
    float total = 0.0f;
    for (int i = 0; i < eb_item_count; i++)
      if (eb_items[i].hasBid)
        total += atof(eb_items[i].price);
    char totBuf[20];
    snprintf(totBuf, sizeof(totBuf), "  $%.2f", total);
    gfx->setTextColor(COLOR_GOLD);
    gfx->print(totBuf);
  } else {
    gfx->setTextColor(COLOR_DIM);
    gfx->print("No auctions");
  }

  gfx->drawFastHLine(0, DIVIDER_Y, gfx->width(), COLOR_DIM);
}

// ---------------------------------------------------------------------------
// Color for time-left string
// ---------------------------------------------------------------------------
uint16_t timeColor(const char *tl) {
  if (strncmp(tl, "ENDED", 5) == 0) return COLOR_ENDED;
  // orange if less than 1 hour (no 'd' and no 'h' in string)
  if (strchr(tl, 'd') == nullptr && strchr(tl, 'h') == nullptr) return COLOR_SOON;
  return COLOR_OK;
}

// ---------------------------------------------------------------------------
// Draw a single item row (title at scroll offset, price, time).
// Uses character windowing so the cursor X is never deeply negative —
// large negative cursor positions cause GFX artefacts.
// ---------------------------------------------------------------------------
void drawItemRow(int idx, int rowIdx, int titleOffset) {
  int y = ROWS_Y + rowIdx * ROW_H;

  // Erase the full row first — single clean operation, no intermediate state
  gfx->fillRect(0, y, gfx->width(), ROW_H, RGB565_BLACK);

  if (idx >= eb_item_count) return;
  EbayItem &item = eb_items[idx];
  if (!item.valid) return;

  // ── Title: character-window scrolling ──────────────────────────────────
  // textSize=2 = 12px per char. Never pass a large negative X to the GFX
  // library — it renders incorrectly. Instead, skip to the first partly-
  // visible character and start the cursor at a small sub-character offset.
  const int CHAR_W = 12;
  int titleLen  = (int)strlen(item.title);
  int firstChar = titleOffset / CHAR_W;         // first (partially) visible char
  int partialPx = titleOffset % CHAR_W;         // pixels of it hidden on the left
  if (firstChar >= titleLen) firstChar = titleLen - 1;

  // Copy the visible window: 16 chars is enough to fill TITLE_AREA_W + 1 char overflow
  char winBuf[18];
  int  winLen = titleLen - firstChar;
  if (winLen > 16) winLen = 16;
  if (winLen < 0)  winLen = 0;
  strncpy(winBuf, item.title + firstChar, winLen);
  winBuf[winLen] = '\0';

  gfx->setTextColor(TITLE_COLORS[rowIdx % NUM_TITLE_COLORS]);
  gfx->setTextSize(2);
  // Cursor starts at most -11px (one partial char) — GFX handles this fine
  gfx->setCursor(2 - partialPx, y + 2);
  gfx->print(winBuf);

  // ── Price (right-aligned, ending at x=264, textSize=1) ─────────────────
  char priceBuf[14];
  snprintf(priceBuf, sizeof(priceBuf), "$%s", item.price);
  int priceX = 264 - (int)strlen(priceBuf) * 6;
  if (priceX < 186) priceX = 186;
  gfx->setTextColor(COLOR_GOLD);
  gfx->setTextSize(1);
  gfx->setCursor(priceX, y + 7);
  gfx->print(priceBuf);

  // ── Time left (far right, textSize=1) ───────────────────────────────────
  ebFormatTimeLeft(item.endEpoch, item.timeLeft, sizeof(item.timeLeft));
  int tlX = gfx->width() - strlen(item.timeLeft) * 6 - 2;
  gfx->setTextColor(timeColor(item.timeLeft));
  gfx->setTextSize(1);
  gfx->setCursor(tlX, y + 7);
  gfx->print(item.timeLeft);
}

// ---------------------------------------------------------------------------
// Scroll tick — advances ONE row at a time in round-robin order
// ---------------------------------------------------------------------------
void scrollTick() {
  if (!fetchOk || eb_item_count == 0) return;
  if (millis() - sc_lastTick < SCROLL_TICK) return;
  sc_lastTick = millis();

  int startIdx = currentPage * EB_PER_PAGE;
  int rowCount = 0;
  for (int i = 0; i < EB_PER_PAGE; i++)
    if (startIdx + i < eb_item_count) rowCount++;
  if (rowCount == 0) return;

  // Find active scrolling row if not yet set
  if (sc_active < 0) {
    sc_active = findNextScrollable(startIdx, -1, rowCount);
    if (sc_active < 0) return;  // nothing to scroll on this page
  }

  int idx = startIdx + sc_active;
  if (idx >= eb_item_count) { sc_active = -1; return; }
  EbayItem &item = eb_items[idx];
  if (!item.valid) { sc_active = findNextScrollable(startIdx, sc_active, rowCount); return; }

  int titlePx   = (int)strlen(item.title) * 12;
  int maxOffset = titlePx - TITLE_AREA_W;
  if (maxOffset <= 0) {
    // This row doesn't need scrolling — skip to next
    sc_active = findNextScrollable(startIdx, sc_active, rowCount);
    return;
  }

  if (sc_pause[sc_active] > 0) { sc_pause[sc_active]--; return; }

  sc_offset[sc_active] += SCROLL_SPEED;
  drawItemRow(idx, sc_active, sc_offset[sc_active]);

  if (sc_offset[sc_active] >= maxOffset) {
    // Row finished — reset it, move to next scrollable row
    sc_offset[sc_active] = 0;
    int finished = sc_active;
    sc_active = findNextScrollable(startIdx, sc_active, rowCount);
    if (sc_active >= 0) sc_pause[sc_active] = SCROLL_PAUSE;
    // Redraw finished row back at offset 0
    drawItemRow(startIdx + finished, finished, 0);
  }
}

// ---------------------------------------------------------------------------
// Draw the current page of listings
// ---------------------------------------------------------------------------
void drawListings() {
  gfx->fillRect(0, ROWS_Y, gfx->width(), gfx->height() - ROWS_Y, RGB565_BLACK);
  resetScroll();

  if (!fetchOk || eb_item_count == 0) {
    gfx->setTextColor(COLOR_ENDED);
    gfx->setTextSize(1);
    gfx->setCursor(4, ROWS_Y + 8);
    char errMsg[64];
    snprintf(errMsg, sizeof(errMsg), "Fetch failed: %s", eb_last_error);
    gfx->print(errMsg);
    return;
  }

  int startIdx = currentPage * EB_PER_PAGE;
  for (int i = 0; i < EB_PER_PAGE; i++) {
    drawItemRow(startIdx + i, i, 0);
  }
}

// ---------------------------------------------------------------------------
// Full refresh: fetch + redraw
// ---------------------------------------------------------------------------
void refreshAll() {
  showStatus("Fetching eBay...");
  fetchOk = ebFetchAll();
  totalPages  = max(1, (eb_item_count + EB_PER_PAGE - 1) / EB_PER_PAGE);
  if (currentPage >= totalPages) currentPage = 0;
  drawHeader();
  drawListings();
  lastRefresh  = millis();
  lastTimeTick = millis();
}

// ---------------------------------------------------------------------------
// Redraw time-left column in place (no fetch, just update text)
// ---------------------------------------------------------------------------
void tickTimeLeft() {
  if (!fetchOk || eb_item_count == 0) return;
  int startIdx = currentPage * EB_PER_PAGE;
  for (int i = 0; i < EB_PER_PAGE; i++) {
    int idx = startIdx + i;
    if (idx >= eb_item_count) break;
    // Redraw full row at current scroll offset so title stays in sync
    drawItemRow(idx, i, sc_offset[i]);
  }
}

// ---------------------------------------------------------------------------
// Button: short press = next page, 3s hold = restart into setup
// ---------------------------------------------------------------------------
void checkButton() {
  if (digitalRead(0) != LOW) return;

  unsigned long pressStart = millis();
  while (digitalRead(0) == LOW) {
    if (millis() - pressStart >= 3000) {
      showStatus("Restarting setup...");
      Preferences prefs;
      prefs.begin("cydebay", false);
      prefs.putBool("forceportal", true);
      prefs.end();
      delay(1000);
      ESP.restart();
    }
    delay(20);
  }

  // Short press: cycle page forward
  currentPage = (currentPage + 1) % totalPages;
  drawHeader();
  drawListings();
}

// Touch: left half = prev page, right half = next page
void checkTouch() {
  if (!ts.tirqTouched() || !ts.touched()) return;
  if (millis() - lastTouchMs < 400) return;
  lastTouchMs = millis();
  TS_Point p = ts.getPoint();
  int tx = map(p.x, 200, 3900, 0, 320);
  tx = constrain(tx, 0, 319);
  if (tx < 160) {
    currentPage = (currentPage + totalPages - 1) % totalPages;  // prev
  } else {
    currentPage = (currentPage + 1) % totalPages;               // next
  }
  drawHeader();
  drawListings();
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("CYDEbayTicker - eBay Auction Monitor");

  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
  }
  gfx->fillScreen(RGB565_BLACK);

  pinMode(GFX_BL, OUTPUT);
  ledcSetup(0, 5000, 8);
  ledcAttachPin(GFX_BL, 0);
  ledcWrite(0, 255);

  pinMode(0, INPUT_PULLUP);

  // Touch init (XPT2046 on VSPI — CLK=25, MISO=39, MOSI=32, CS=33, IRQ=36)
  touchSPI.begin(25, 39, 32, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  ebLoadSettings();
  ledcWrite(0, eb_brightness);

  bool showPortal = !eb_has_settings || eb_force_portal;

  if (!showPortal) {
    showStatus("Hold BOOT to change settings...");
    for (int i = 0; i < 30 && !showPortal; i++) {
      if (digitalRead(0) == LOW) showPortal = true;
      delay(100);
    }
  }

  if (showPortal) {
    ebInitPortal();
    while (!portalDone) {
      ebRunPortal();
      delay(5);
    }
    ebClosePortal();
    ledcWrite(0, eb_brightness);
  }

  // Connect WiFi
  gfx->fillScreen(RGB565_BLACK);
  WiFi.mode(WIFI_STA);
  WiFi.begin(eb_wifi_ssid, eb_wifi_pass);

  int dots = 0;
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - wifiStart > 30000) {
      char errMsg[60];
      snprintf(errMsg, sizeof(errMsg), "WiFi failed: \"%s\"", eb_wifi_ssid);
      showStatus(errMsg);
      while (true) delay(1000);
    }
    delay(500);
    char msg[48];
    snprintf(msg, sizeof(msg), "Connecting to WiFi%.*s", (dots % 4) + 1, "....");
    showStatus(msg);
    dots++;
  }

  showStatus("WiFi connected! Syncing time...");
  identityBegin();

  // Sync NTP (UTC) — required for time-remaining calculations
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeInfo;
  int ntpTries = 0;
  while (!getLocalTime(&timeInfo) && ntpTries < 20) {
    delay(500);
    ntpTries++;
  }
  if (ntpTries >= 20) {
    showStatus("NTP sync failed — times may be wrong");
    delay(2000);
  } else {
    Serial.printf("[NTP] Time synced: %02d:%02d:%02d UTC\n",
                  timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
  }

  refreshAll();
}

// ---------------------------------------------------------------------------
// Loop: check button, update time-left, full refresh
// When any auction ends within 1 hour: refresh every 60s, tick every 10s
// Normal mode: refresh every 5 min, tick every 30s
// ---------------------------------------------------------------------------
void loop() {
  identityHandle();
  checkButton();
  checkTouch();
  scrollTick();

  bool soonMode      = ebAnyEndingSoon();
  unsigned long tickInterval    = soonMode ? 10000UL  : 30000UL;
  unsigned long refreshInterval = soonMode ? 60000UL  : REFRESH_INTERVAL;

  if (millis() - lastTimeTick >= tickInterval) {
    tickTimeLeft();
    lastTimeTick = millis();
  }

  if (millis() - lastRefresh >= refreshInterval) {
    refreshAll();
  }
}

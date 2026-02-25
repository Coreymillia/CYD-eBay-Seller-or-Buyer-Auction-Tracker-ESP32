// CYDEbayTicker - eBay Auction Monitor for CYD (Cheap Yellow Display)
// Fetches active auctions from up to 3 eBay sellers (or a keyword search)
// and displays them sorted by ending soonest on a 320x240 ILI9341 TFT.
// Updates every 5 minutes. Short press BOOT cycles pages; 3s hold = setup.

#include <Arduino.h>
#include <WiFi.h>

/*******************************************************************************
 * Display setup - CYD (Cheap Yellow Display)
 * ILI9341 320x240 landscape via hardware SPI
 ******************************************************************************/
#include <Arduino_GFX_Library.h>

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

    // Updated time in grey right after
    time_t now = time(nullptr);
    struct tm *t = gmtime(&now);
    char timeBuf[24];
    snprintf(timeBuf, sizeof(timeBuf), "  %02d:%02d UTC", t->tm_hour, t->tm_min);
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
// Draw the current page of listings
// ---------------------------------------------------------------------------
void drawListings() {
  gfx->fillRect(0, ROWS_Y, gfx->width(), gfx->height() - ROWS_Y, RGB565_BLACK);

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
    int idx = startIdx + i;
    int y   = ROWS_Y + i * ROW_H;

    gfx->fillRect(0, y, gfx->width(), ROW_H, RGB565_BLACK);

    if (idx >= eb_item_count) break;

    EbayItem &item = eb_items[idx];
    if (!item.valid) continue;

    // ── Title (left, truncated) ─────────────────────────────────────────
    // Layout: title 0-182px | price 186-264px | time 270-318px
    const int titleMaxChars = 30;
    char titleBuf[34];
    truncate(item.title, titleBuf, titleMaxChars);

    gfx->setTextColor(COLOR_TEXT);
    gfx->setTextSize(1);
    gfx->setCursor(2, y + 7);
    gfx->print(titleBuf);

    // ── Price (right-aligned, ending at x=264) ──────────────────────────
    char priceBuf[14];
    snprintf(priceBuf, sizeof(priceBuf), "$%s", item.price);
    int priceX = 264 - (int)strlen(priceBuf) * 6;
    if (priceX < 186) priceX = 186;  // don't crowd into title
    gfx->setTextColor(COLOR_GOLD);
    gfx->setCursor(priceX, y + 7);
    gfx->print(priceBuf);

    // ── Time left (far right) ───────────────────────────────────────────
    // Refresh before drawing
    ebFormatTimeLeft(item.endEpoch, item.timeLeft, sizeof(item.timeLeft));
    int tlX = gfx->width() - strlen(item.timeLeft) * 6 - 2;
    gfx->setTextColor(timeColor(item.timeLeft));
    gfx->setCursor(tlX, y + 7);
    gfx->print(item.timeLeft);
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

    EbayItem &item = eb_items[idx];
    if (!item.valid) continue;

    int y   = ROWS_Y + i * ROW_H;
    // Erase old time-left area (rightmost 50px)
    gfx->fillRect(gfx->width() - 50, y, 50, ROW_H, RGB565_BLACK);

    ebFormatTimeLeft(item.endEpoch, item.timeLeft, sizeof(item.timeLeft));
    int tlX = gfx->width() - strlen(item.timeLeft) * 6 - 2;
    gfx->setTextColor(timeColor(item.timeLeft));
    gfx->setTextSize(1);
    gfx->setCursor(tlX, y + 7);
    gfx->print(item.timeLeft);
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

  // Short press: cycle page
  currentPage = (currentPage + 1) % totalPages;
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
  digitalWrite(GFX_BL, HIGH);

  pinMode(0, INPUT_PULLUP);

  ebLoadSettings();

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
  checkButton();

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

  delay(20);
}

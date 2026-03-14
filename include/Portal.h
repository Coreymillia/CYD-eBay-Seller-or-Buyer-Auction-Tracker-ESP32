#pragma once

#include <Arduino_GFX_Library.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// gfx is defined in main.cpp
extern Arduino_GFX *gfx;

// ---------------------------------------------------------------------------
// Persisted settings
// ---------------------------------------------------------------------------
static char eb_wifi_ssid[64]  = "";
static char eb_wifi_pass[64]  = "";
static char eb_appid[128]     = "";  // eBay Production App ID (Client ID)
static char eb_certid[128]    = "";  // eBay Production Cert ID (Client Secret)
static char eb_seller1[64]    = "";  // eBay seller ID #1
static char eb_seller2[64]    = "";  // eBay seller ID #2
static char eb_seller3[64]    = "";  // eBay seller ID #3
static char eb_seller_kw[64]  = "";  // keyword scoped to seller searches (required by Browse API)
static char eb_cat_id[16]     = "";  // eBay category ID (alternative to keyword, e.g. 13473)
static char eb_keyword[64]    = "";  // fallback keyword search (used if no sellers set)
static bool eb_has_settings   = false;
static bool eb_force_portal   = false;
static uint8_t eb_brightness  = 200;   // backlight 10–255

// ---------------------------------------------------------------------------
// Portal state
// ---------------------------------------------------------------------------
static WebServer *portalServer = nullptr;
static DNSServer *portalDNS    = nullptr;
static bool       portalDone   = false;

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------
static void ebLoadSettings() {
  Preferences prefs;
  prefs.begin("cydebay", true);
  String ssid    = prefs.getString("ssid",    "");
  String pass    = prefs.getString("pass",    "");
  String appid   = prefs.getString("appid",   "");
  String certid  = prefs.getString("certid",  "");
  String seller1   = prefs.getString("seller1",   "");
  String seller2   = prefs.getString("seller2",   "");
  String seller3   = prefs.getString("seller3",   "");
  String seller_kw = prefs.getString("seller_kw", "");
  String cat_id    = prefs.getString("cat_id",    "");
  String keyword   = prefs.getString("keyword",   "");
  bool   force   = prefs.getBool("forceportal", false);
  prefs.end();

  if (force) {
    Preferences rw;
    rw.begin("cydebay", false);
    rw.putBool("forceportal", false);
    rw.end();
  }

  ssid.toCharArray(eb_wifi_ssid, sizeof(eb_wifi_ssid));
  pass.toCharArray(eb_wifi_pass, sizeof(eb_wifi_pass));
  appid.toCharArray(eb_appid,    sizeof(eb_appid));
  certid.toCharArray(eb_certid,  sizeof(eb_certid));
  seller1.toCharArray(eb_seller1,    sizeof(eb_seller1));
  seller2.toCharArray(eb_seller2,    sizeof(eb_seller2));
  seller3.toCharArray(eb_seller3,    sizeof(eb_seller3));
  seller_kw.toCharArray(eb_seller_kw, sizeof(eb_seller_kw));
  cat_id.toCharArray(eb_cat_id,       sizeof(eb_cat_id));
  keyword.toCharArray(eb_keyword,     sizeof(eb_keyword));

  bool hasSeller  = (seller1[0] != '\0' || seller2[0] != '\0' || seller3[0] != '\0');
  bool hasKeyword = (keyword[0] != '\0');
  bool hasSellerSearch = (seller_kw[0] != '\0' || cat_id[0] != '\0');
  eb_has_settings = (ssid[0] != '\0' && appid[0] != '\0' && certid[0] != '\0'
                     && (hasKeyword || (hasSeller && hasSellerSearch)));
  eb_force_portal = force;
  eb_brightness   = (uint8_t)prefs.getUChar("bright", 200);
  if (eb_brightness < 10) eb_brightness = 10;
}

static void ebSaveSettings(const char *ssid, const char *pass,
                            const char *appid,  const char *certid,
                            const char *seller1, const char *seller2, const char *seller3,
                            const char *seller_kw, const char *cat_id, const char *keyword,
                            uint8_t brightness) {
  Preferences prefs;
  prefs.begin("cydebay", false);
  prefs.putString("ssid",      ssid);
  prefs.putString("pass",      pass);
  prefs.putString("appid",     appid);
  prefs.putString("certid",    certid);
  prefs.putString("seller1",   seller1);
  prefs.putString("seller2",   seller2);
  prefs.putString("seller3",   seller3);
  prefs.putString("seller_kw", seller_kw);
  prefs.putString("cat_id",    cat_id);
  prefs.putString("keyword",   keyword);
  prefs.putUChar("bright",     brightness);
  prefs.end();

  strncpy(eb_wifi_ssid,  ssid,      sizeof(eb_wifi_ssid)  - 1);
  strncpy(eb_wifi_pass,  pass,      sizeof(eb_wifi_pass)  - 1);
  strncpy(eb_appid,      appid,     sizeof(eb_appid)      - 1);
  strncpy(eb_certid,     certid,    sizeof(eb_certid)     - 1);
  strncpy(eb_seller1,    seller1,   sizeof(eb_seller1)    - 1);
  strncpy(eb_seller2,    seller2,   sizeof(eb_seller2)    - 1);
  strncpy(eb_seller3,    seller3,   sizeof(eb_seller3)    - 1);
  strncpy(eb_seller_kw,  seller_kw, sizeof(eb_seller_kw)  - 1);
  strncpy(eb_cat_id,     cat_id,    sizeof(eb_cat_id)     - 1);
  strncpy(eb_keyword,    keyword,   sizeof(eb_keyword)    - 1);
  eb_brightness   = brightness;
  eb_has_settings = true;
}

// ---------------------------------------------------------------------------
// On-screen setup instructions
// ---------------------------------------------------------------------------
static void ebShowPortalScreen() {
  gfx->fillScreen(RGB565_BLACK);

  gfx->setTextColor(0x07FF);  // cyan
  gfx->setTextSize(2);
  gfx->setCursor(10, 5);
  gfx->print("CYDEbayTicker");

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(60, 26);
  gfx->print("eBay Auction Monitor");

  gfx->setTextColor(0xFFE0);  // yellow
  gfx->setCursor(4, 46);
  gfx->print("1. Connect to WiFi:");
  gfx->setTextColor(0x07FF);
  gfx->setTextSize(2);
  gfx->setCursor(14, 58);
  gfx->print("CYDEbay_Setup");

  gfx->setTextColor(0xFFE0);
  gfx->setTextSize(1);
  gfx->setCursor(4, 82);
  gfx->print("2. Open browser:");
  gfx->setTextColor(0x07FF);
  gfx->setTextSize(2);
  gfx->setCursor(50, 94);
  gfx->print("192.168.4.1");

  gfx->setTextColor(0xFFE0);
  gfx->setTextSize(1);
  gfx->setCursor(4, 118);
  gfx->print("3. Enter WiFi, App ID,");
  gfx->setCursor(4, 130);
  gfx->print("   and seller IDs.");

  if (eb_has_settings) {
    gfx->setTextColor(0x07E0);  // green
    gfx->setCursor(4, 152);
    gfx->print("Existing settings found.");
    gfx->setCursor(4, 164);
    gfx->print("Tap 'No Changes' to keep.");
  }
}

// ---------------------------------------------------------------------------
// Web handlers
// ---------------------------------------------------------------------------
static void ebHandleRoot() {
  String html = "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>CYDEbayTicker Setup</title>"
    "<style>"
    "body{background:#0d0d1a;color:#e6c200;font-family:Arial,sans-serif;"
         "text-align:center;padding:20px;max-width:480px;margin:auto;}"
    "h1{color:#ffdd00;font-size:1.6em;margin-bottom:4px;}"
    "p{color:#aa9900;font-size:0.9em;}"
    "label{display:block;text-align:left;margin:14px 0 4px;color:#ffcc00;font-weight:bold;}"
    "small{display:block;text-align:left;color:#665500;font-size:0.8em;margin-top:2px;}"
    "input{width:100%;box-sizing:border-box;background:#1a1400;color:#ffdd00;"
          "border:2px solid #665500;border-radius:6px;padding:10px;font-size:1em;}"
    ".btn{display:block;width:100%;padding:14px;margin:10px 0;font-size:1.05em;"
         "border-radius:8px;border:none;cursor:pointer;font-weight:bold;}"
    ".btn-save{background:#443300;color:#ffdd00;border:2px solid #aa8800;}"
    ".btn-save:hover{background:#665500;}"
    ".btn-skip{background:#1a1a1a;color:#665500;border:2px solid #333322;}"
    ".btn-skip:hover{background:#222211;color:#aa9900;}"
    ".note{color:#443300;font-size:0.82em;margin-top:16px;}"
    "hr{border:1px solid #332200;margin:20px 0;}"
    ".rng{display:flex;align-items:center;gap:8px;margin:6px 0 14px;}"
    ".rng input[type=range]{flex:1;accent-color:#ffdd00;}"
    ".rng output{min-width:28px;text-align:right;color:#ffcc00;}"
    ".sect{color:#aa7700;font-size:0.85em;text-align:left;margin:18px 0 6px;"
          "border-bottom:1px solid #332200;padding-bottom:4px;}"
    "</style></head><body>"
    "<h1>&#127885; CYDEbayTicker Setup</h1>"
    "<p>Enter your WiFi credentials, eBay App ID, and up to 3 seller IDs.</p>"
    "<form method='post' action='/save'>"
    "<div class='sect'>WiFi</div>"
    "<label>Network Name (SSID):</label>"
    "<input type='text' name='ssid' value='";
  html += String(eb_wifi_ssid);
  html += "' placeholder='Your 2.4 GHz WiFi name' maxlength='63' required>"
    "<label>WiFi Password:</label>"
    "<input type='password' name='pass' value='";
  html += String(eb_wifi_pass);
  html += "' placeholder='Leave blank if open network' maxlength='63'>"
    "<hr>"
    "<div class='sect'>eBay Developer App ID</div>"
    "<label>Production App ID:</label>"
    "<small>Get free from developer.ebay.com &rarr; My Account &rarr; Application Keysets &rarr; Production</small>"
    "<input type='text' name='appid' value='";
  html += String(eb_appid);
  html += "' placeholder='YourName-YourApp-PRD-xxxxxxx-xxxxxxxx' maxlength='127' required>"
    "<label>Production Cert ID (Client Secret):</label>"
    "<small>Found next to the App ID in your eBay developer keyset.</small>"
    "<input type='password' name='certid' value='";
  html += String(eb_certid);
  html += "' placeholder='PRD-xxxxxxxxxxxxxxxx-xxxxxxxxxxxxxxxx' maxlength='127' required>"
    "<hr>"
    "<div class='sect'>Sellers to Track (at least one, or use keyword below)</div>"
    "<label>Seller ID #1:</label>"
    "<input type='text' name='seller1' value='";
  html += String(eb_seller1);
  html += "' placeholder='ebay seller username' maxlength='63'>"
    "<label>Seller ID #2 (optional):</label>"
    "<input type='text' name='seller2' value='";
  html += String(eb_seller2);
  html += "' placeholder='ebay seller username' maxlength='63'>"
    "<label>Seller ID #3 (optional):</label>"
    "<input type='text' name='seller3' value='";
  html += String(eb_seller3);
  html += "' placeholder='ebay seller username' maxlength='63'>"
    "<label>Seller Search Keyword(s) (required if using sellers above):</label>"
    "<small>eBay requires at least one keyword to scope seller searches. Comma-separate multiple words for broader coverage (e.g. <b>coin,silver,lot</b>). Each keyword adds an API call and deduplicates results.</small>"
    "<input type='text' name='seller_kw' value='";
  html += String(eb_seller_kw);
  html += "' placeholder='e.g. coin,silver,lot,new' maxlength='63'>"
    "<label>Category ID (optional — overrides keyword, finds ALL auctions in that category):</label>"
    "<small>Browse to your eBay category and grab the number from the URL: ebay.com/b/Exonumia/<b>13473</b>/... &mdash; enter just the number.</small>"
    "<input type='text' name='cat_id' value='";
  html += String(eb_cat_id);
  html += "' placeholder='e.g. 13473' maxlength='15'>"
    "<hr>"
    "<div class='sect'>Keyword Search (used only if no sellers set)</div>"
    "<label>Item Keyword:</label>"
    "<input type='text' name='keyword' value='";
  html += String(eb_keyword);
  html += "' placeholder='e.g. vintage rolex' maxlength='63'>"
    "<label>Brightness:</label>"
    "<div class='rng'><input type='range' name='bright' min='10' max='255' value='";
  html += String(eb_brightness);
  html += "' oninput='this.nextElementSibling.value=this.value'><output>";
  html += String(eb_brightness);
  html += "</output></div>"
    "<br><button class='btn btn-save' type='submit'>&#128190; Save &amp; Connect</button>"
    "</form>";
  if (eb_has_settings) {
    html += "<hr>"
      "<form method='post' action='/nochange'>"
      "<button class='btn btn-skip' type='submit'>&#10006; No Changes &mdash; Use Current Settings</button>"
      "</form>";
  }
  html += "<p class='note'>&#9888; ESP32 supports 2.4 GHz WiFi only.</p>"
    "</body></html>";

  portalServer->send(200, "text/html", html);
}

static void ebHandleSave() {
  String ssid    = portalServer->hasArg("ssid")    ? portalServer->arg("ssid")    : "";
  String pass    = portalServer->hasArg("pass")    ? portalServer->arg("pass")    : "";
  String appid   = portalServer->hasArg("appid")   ? portalServer->arg("appid")   : "";
  String certid  = portalServer->hasArg("certid")  ? portalServer->arg("certid")  : "";
  String seller1   = portalServer->hasArg("seller1")   ? portalServer->arg("seller1")   : "";
  String seller2   = portalServer->hasArg("seller2")   ? portalServer->arg("seller2")   : "";
  String seller3   = portalServer->hasArg("seller3")   ? portalServer->arg("seller3")   : "";
  String seller_kw = portalServer->hasArg("seller_kw") ? portalServer->arg("seller_kw") : "";
  String cat_id    = portalServer->hasArg("cat_id")    ? portalServer->arg("cat_id")    : "";
  String keyword   = portalServer->hasArg("keyword")   ? portalServer->arg("keyword")   : "";

  auto sendErr = [](WebServer *srv, const char *msg) {
    String page = "<html><body style='background:#0d0d1a;color:#ff5555;font-family:Arial;"
                  "text-align:center;padding:40px'><h2>&#10060; ";
    page += msg;
    page += "</h2><a href='/' style='color:#ffdd00'>&#8592; Go Back</a></body></html>";
    srv->send(400, "text/html", page);
  };

  if (ssid.length() == 0)   { sendErr(portalServer, "WiFi SSID cannot be empty!");  return; }
  if (appid.length() == 0)  { sendErr(portalServer, "App ID cannot be empty!");      return; }
  if (certid.length() == 0) { sendErr(portalServer, "Cert ID cannot be empty!");     return; }
  bool hasSeller  = (seller1.length() > 0 || seller2.length() > 0 || seller3.length() > 0);
  bool hasKeyword = (keyword.length() > 0);
  if (hasSeller && seller_kw.length() == 0 && cat_id.length() == 0) {
    sendErr(portalServer, "Enter a Keyword or Category ID for your seller IDs!");
    return;
  }
  if (!hasSeller && !hasKeyword) {
    sendErr(portalServer, "Enter at least one Seller ID or a Keyword!");
    return;
  }

  ebSaveSettings(ssid.c_str(), pass.c_str(), appid.c_str(), certid.c_str(),
                 seller1.c_str(), seller2.c_str(), seller3.c_str(),
                 seller_kw.c_str(), cat_id.c_str(), keyword.c_str(),
                 portalServer->hasArg("bright") ? (uint8_t)constrain(portalServer->arg("bright").toInt(), 10, 255) : 200);

  String sellers = "";
  if (seller1.length()) sellers += "<b>" + seller1 + "</b> ";
  if (seller2.length()) sellers += "<b>" + seller2 + "</b> ";
  if (seller3.length()) sellers += "<b>" + seller3 + "</b>";

  portalServer->send(200, "text/html",
    "<html><head><meta charset='UTF-8'>"
    "<style>body{background:#0d0d1a;color:#ffdd00;font-family:Arial;"
    "text-align:center;padding:40px;}h2{color:#ffff00;}"
    "p{color:#aa9900;}</style></head><body>"
    "<h2>&#9989; Settings Saved!</h2>"
    "<p>WiFi: <b>" + ssid + "</b></p>"
    "<p>Sellers: " + (sellers.length() ? sellers : "<i>none</i>") + "</p>"
    "<p>You can close this page and disconnect from <b>CYDEbay_Setup</b>.</p>"
    "</body></html>");

  delay(1500);
  portalDone = true;
}

static void ebHandleNoChange() {
  portalServer->send(200, "text/html",
    "<html><head><meta charset='UTF-8'>"
    "<style>body{background:#0d0d1a;color:#ffdd00;font-family:Arial;"
    "text-align:center;padding:40px;}h2{color:#ffff00;}"
    "p{color:#aa9900;}</style></head><body>"
    "<h2>&#128077; No Changes</h2>"
    "<p>Using saved settings. Connecting now.</p>"
    "<p>You can close this page and disconnect from <b>CYDEbay_Setup</b>.</p>"
    "</body></html>");

  delay(1500);
  portalDone = true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
static void ebInitPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("CYDEbay_Setup", "");
  delay(500);

  portalDNS    = new DNSServer();
  portalServer = new WebServer(80);

  portalDNS->start(53, "*", WiFi.softAPIP());

  portalServer->on("/",         ebHandleRoot);
  portalServer->on("/save",     HTTP_POST, ebHandleSave);
  portalServer->on("/nochange", HTTP_POST, ebHandleNoChange);
  portalServer->onNotFound(ebHandleRoot);
  portalServer->begin();

  portalDone = false;
  ebShowPortalScreen();

  Serial.printf("[Portal] AP up — connect to CYDEbay_Setup, open %s\n",
                WiFi.softAPIP().toString().c_str());
}

static void ebRunPortal() {
  portalDNS->processNextRequest();
  portalServer->handleClient();
}

static void ebClosePortal() {
  portalServer->stop();
  portalDNS->stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(300);

  delete portalServer; portalServer = nullptr;
  delete portalDNS;    portalDNS    = nullptr;
}

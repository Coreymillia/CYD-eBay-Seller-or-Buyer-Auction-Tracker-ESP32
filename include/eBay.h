#pragma once

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

#define EB_MAX_ITEMS   60   // max total items stored
#define EB_PER_PAGE    10   // items displayed per screen page
#define EB_PER_SELLER  50   // items fetched per seller/keyword call (Browse API max ~200)

// ---------------------------------------------------------------------------
// Item struct
// ---------------------------------------------------------------------------
struct EbayItem {
  char   itemId[24];   // eBay item ID — used for deduplication
  char   title[64];
  char   price[12];     // "999999.99"
  char   currency[4];   // "USD"
  char   endTime[24];   // "2024-02-23T14:30:00.000Z"
  char   timeLeft[16];  // "2h 15m", "45m 10s", "ENDED", "3d 4h"
  time_t endEpoch;      // parsed for sorting / countdown
  bool   valid;
  bool   hasBid;        // true only if currentBidPrice present (real bid, not just start price)
};

static EbayItem eb_items[EB_MAX_ITEMS];
static int      eb_item_count  = 0;
static char     eb_last_error[64] = "";
static time_t   eb_last_fetch  = 0;

// OAuth token cache (Browse API requires Bearer token)
// eBay tokens can exceed 1000 chars; use generous buffers to avoid truncation
static char   eb_token[2048]    = "";
static char   eb_auth_hdr[2100] = "";   // "Bearer <token>"
static time_t eb_token_expiry   = 0;

// ---------------------------------------------------------------------------
// Base64 encoder — needed for HTTP Basic auth in OAuth token request
// ---------------------------------------------------------------------------
static const char b64chars[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64Encode(const char *src, char *dst, size_t dstLen) {
  int n = strlen(src), o = 0;
  for (int i = 0; i < n && o + 4 < (int)dstLen; i += 3) {
    uint8_t b0 = (uint8_t)src[i];
    uint8_t b1 = (i + 1 < n) ? (uint8_t)src[i + 1] : 0;
    uint8_t b2 = (i + 2 < n) ? (uint8_t)src[i + 2] : 0;
    dst[o++] = b64chars[b0 >> 2];
    dst[o++] = b64chars[((b0 & 3) << 4) | (b1 >> 4)];
    dst[o++] = (i + 1 < n) ? b64chars[((b1 & 0xf) << 2) | (b2 >> 6)] : '=';
    dst[o++] = (i + 2 < n) ? b64chars[b2 & 0x3f] : '=';
  }
  dst[o] = '\0';
}

// ---------------------------------------------------------------------------
// Parse ISO 8601 UTC string "2024-02-23T14:30:00.000Z" -> time_t
// ---------------------------------------------------------------------------
static time_t ebParseTime(const char *iso) {
  struct tm t = {};
  if (sscanf(iso, "%d-%d-%dT%d:%d:%d",
             &t.tm_year, &t.tm_mon, &t.tm_mday,
             &t.tm_hour, &t.tm_min, &t.tm_sec) == 6) {
    t.tm_year -= 1900;
    t.tm_mon  -= 1;
    t.tm_isdst = 0;
    return mktime(&t);  // assumes TZ set to UTC via configTime(0,0,...)
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Format seconds remaining into a human-readable string
// ---------------------------------------------------------------------------
static void ebFormatTimeLeft(time_t endEpoch, char *buf, size_t bufLen) {
  time_t now  = time(nullptr);
  long   secs = (long)(endEpoch - now);

  if (secs <= 0) {
    strncpy(buf, "ENDED", bufLen - 1);
    buf[bufLen - 1] = '\0';
    return;
  }

  long days  = secs / 86400;
  long hours = (secs % 86400) / 3600;
  long mins  = (secs % 3600) / 60;
  long s     = secs % 60;

  if (days >= 1)       snprintf(buf, bufLen, "%ldd %ldh", days, hours);
  else if (hours >= 1) snprintf(buf, bufLen, "%ldh %ldm", hours, mins);
  else                 snprintf(buf, bufLen, "%ldm %lds", mins, s);
}

// ---------------------------------------------------------------------------
// Fetch OAuth application token via Client Credentials grant.
// Caches token; refreshes ~60s before expiry (~2-hour lifetime).
// ---------------------------------------------------------------------------
static bool ebGetToken() {
  time_t now = time(nullptr);
  if (eb_token[0] && now < eb_token_expiry - 60) return true;  // still valid

  if (!eb_appid[0] || !eb_certid[0]) {
    snprintf(eb_last_error, sizeof(eb_last_error), "No App/Cert ID");
    return false;
  }

  // Build Basic auth header: base64(appid:certid)
  char creds[350];
  snprintf(creds, sizeof(creds), "%s:%s", eb_appid, eb_certid);
  char b64[600];
  b64Encode(creds, b64, sizeof(b64));
  char basicHdr[660];
  snprintf(basicHdr, sizeof(basicHdr), "Basic %s", b64);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);

  if (!http.begin(client, "https://api.ebay.com/identity/v1/oauth2/token")) return false;
  http.addHeader("Content-Type",  "application/x-www-form-urlencoded");
  http.addHeader("Authorization", basicHdr);

  int code = http.POST(
    "grant_type=client_credentials"
    "&scope=https%3A%2F%2Fapi.ebay.com%2Foauth%2Fapi_scope");

  if (code != HTTP_CODE_OK) {
    snprintf(eb_last_error, sizeof(eb_last_error), "OAuth %d", code);
    Serial.printf("[eBay] OAuth HTTP %d: %s\n", code, http.getString().substring(0,200).c_str());
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    snprintf(eb_last_error, sizeof(eb_last_error), "OAuth JSON err");
    return false;
  }

  const char *tok = doc["access_token"] | "";
  int expiresIn   = doc["expires_in"]   | 7200;

  if (!tok[0]) {
    snprintf(eb_last_error, sizeof(eb_last_error), "No token");
    return false;
  }

  strncpy(eb_token, tok, sizeof(eb_token) - 1);
  eb_token[sizeof(eb_token) - 1] = '\0';
  snprintf(eb_auth_hdr, sizeof(eb_auth_hdr), "Bearer %s", eb_token);
  eb_token_expiry = now + expiresIn;

  Serial.printf("[eBay] Token OK — length=%d, expires in %ds\n",
                (int)strlen(eb_token), expiresIn);
  return true;
}

// ---------------------------------------------------------------------------
// Deduplication: return true if itemId already in eb_items[]
// ---------------------------------------------------------------------------
static bool ebItemExists(const char *id) {
  for (int i = 0; i < eb_item_count; i++)
    if (strncmp(eb_items[i].itemId, id, sizeof(eb_items[i].itemId) - 1) == 0)
      return true;
  return false;
}

// ---------------------------------------------------------------------------
// Parse a Browse API itemSummaries array into eb_items
// ---------------------------------------------------------------------------
static int ebParseItems(JsonArray items) {
  int added = 0;
  for (JsonObject item : items) {
    if (eb_item_count >= EB_MAX_ITEMS) break;

    // Extract itemId first for deduplication
    const char *id = item["itemId"] | "";
    if (id[0] && ebItemExists(id)) continue;  // skip duplicates

    EbayItem &e = eb_items[eb_item_count];

    strncpy(e.itemId, id, sizeof(e.itemId) - 1);
    e.itemId[sizeof(e.itemId) - 1] = '\0';

    // Title
    const char *title = item["title"] | "";
    strncpy(e.title, title, sizeof(e.title) - 1);
    e.title[sizeof(e.title) - 1] = '\0';

    // Price: prefer currentBidPrice (actual bid), fall back to listing price
    const char *val  = "0.00";
    const char *curr = "USD";
    JsonVariant bid  = item["currentBidPrice"];
    if (!bid.isNull()) {
      val      = bid["value"]    | "0.00";
      curr     = bid["currency"] | "USD";
      e.hasBid = true;
    } else {
      val      = item["price"]["value"]    | "0.00";
      curr     = item["price"]["currency"] | "USD";
      e.hasBid = false;
    }
    strncpy(e.price,    val,  sizeof(e.price)    - 1);
    strncpy(e.currency, curr, sizeof(e.currency) - 1);
    e.price[sizeof(e.price) - 1]       = '\0';
    e.currency[sizeof(e.currency) - 1] = '\0';

    // End time
    const char *et = item["itemEndDate"] | "";
    strncpy(e.endTime, et, sizeof(e.endTime) - 1);
    e.endTime[sizeof(e.endTime) - 1] = '\0';
    e.endEpoch = ebParseTime(et);

    ebFormatTimeLeft(e.endEpoch, e.timeLeft, sizeof(e.timeLeft));
    e.valid = true;
    eb_item_count++;
    added++;
  }
  return added;
}

// ---------------------------------------------------------------------------
// Browse API GET + JSON parse (shared by seller and keyword fetches)
// ---------------------------------------------------------------------------
static int ebBrowseSearch(const char *url) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);

  if (!http.begin(client, url)) return 0;
  http.addHeader("Authorization",           eb_auth_hdr);
  http.addHeader("X-EBAY-C-MARKETPLACE-ID", "EBAY_US");

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    String errBody = http.getString();
    snprintf(eb_last_error, sizeof(eb_last_error), "HTTP %d", code);
    Serial.printf("[eBay] Browse HTTP %d\n  URL: %.120s\n  Body: %.300s\n",
                  code, url, errBody.c_str());
    http.end();
    return 0;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    snprintf(eb_last_error, sizeof(eb_last_error), "JSON error");
    return 0;
  }

  JsonArray items = doc["itemSummaries"].as<JsonArray>();
  if (items.isNull()) {
    Serial.println("[eBay] No itemSummaries in response");
    return 0;
  }

  return ebParseItems(items);
}

// ---------------------------------------------------------------------------
// Fetch auction listings for one seller.
// If eb_cat_id is set: uses category_ids (no q needed — finds ALL auctions).
// Otherwise: iterates comma-separated eb_seller_kw, deduplicates by itemId.
// ---------------------------------------------------------------------------
static int ebFetchSeller(const char *sellerId) {
  if (WiFi.status() != WL_CONNECTED || !ebGetToken()) return 0;

  // Category ID mode: no keyword required, covers full category inventory
  if (eb_cat_id[0]) {
    char url[512];
    snprintf(url, sizeof(url),
      "https://api.ebay.com/buy/browse/v1/item_summary/search"
      "?category_ids=%s&filter=sellers%%3A%%7B%s%%7D%%2CbuyingOptions%%3A%%7BAUCTION%%7D"
      "&sort=endingSoonest&limit=%d",
      eb_cat_id, sellerId, EB_PER_SELLER);
    Serial.printf("[eBay] Seller: %s  cat: %s\n", sellerId, eb_cat_id);
    int added = ebBrowseSearch(url);
    Serial.printf("[eBay] Added %d items from seller %s\n", added, sellerId);
    return added;
  }

  // Keyword mode: split on commas, one call per keyword, deduplicate
  char kwBuf[sizeof(eb_seller_kw)];
  strncpy(kwBuf, eb_seller_kw, sizeof(kwBuf) - 1);
  kwBuf[sizeof(kwBuf) - 1] = '\0';

  int totalAdded = 0;
  char *tok = strtok(kwBuf, ",");
  while (tok) {
    while (*tok == ' ') tok++;
    char *end = tok + strlen(tok) - 1;
    while (end > tok && *end == ' ') *end-- = '\0';

    if (*tok) {
      // URL-encode spaces within a keyword token as '+'
      char encTok[128]; int ei = 0;
      for (int ci = 0; tok[ci] && ei < (int)sizeof(encTok)-1; ci++)
        encTok[ei++] = (tok[ci] == ' ') ? '+' : tok[ci];
      encTok[ei] = '\0';

      char url[512];
      snprintf(url, sizeof(url),
        "https://api.ebay.com/buy/browse/v1/item_summary/search"
        "?q=%s&filter=sellers%%3A%%7B%s%%7D%%2CbuyingOptions%%3A%%7BAUCTION%%7D"
        "&sort=endingSoonest&limit=%d",
        encTok, sellerId, EB_PER_SELLER);
      Serial.printf("[eBay] Seller: %s  kw: %s\n", sellerId, encTok);
      totalAdded += ebBrowseSearch(url);
    }
    tok = strtok(nullptr, ",");
  }

  Serial.printf("[eBay] Added %d items from seller %s\n", totalAdded, sellerId);
  return totalAdded;
}

// ---------------------------------------------------------------------------
// Fetch auction listings by keyword (fallback when no sellers configured)
// ---------------------------------------------------------------------------
static int ebFetchKeyword(const char *keyword) {
  if (WiFi.status() != WL_CONNECTED || !ebGetToken()) return 0;

  char encoded[128]; int j = 0;
  for (int i = 0; keyword[i] && j < (int)sizeof(encoded) - 3; i++)
    encoded[j++] = (keyword[i] == ' ') ? '+' : keyword[i];
  encoded[j] = '\0';

  char url[512];
  snprintf(url, sizeof(url),
    "https://api.ebay.com/buy/browse/v1/item_summary/search"
    "?q=%s&filter=buyingOptions%%3A%%7BAUCTION%%7D"
    "&sort=endingSoonest&limit=%d",
    encoded, EB_MAX_ITEMS);

  Serial.printf("[eBay] Fetching keyword: %s\n", keyword);
  int added = ebBrowseSearch(url);
  Serial.printf("[eBay] Added %d keyword items\n", added);
  return added;
}

// ---------------------------------------------------------------------------
// Sort eb_items[0..eb_item_count-1] by endEpoch ascending (soonest first)
// ---------------------------------------------------------------------------
static void ebSortItems() {
  for (int i = 1; i < eb_item_count; i++) {
    EbayItem key = eb_items[i];
    int j = i - 1;
    while (j >= 0 && eb_items[j].endEpoch > key.endEpoch) {
      eb_items[j + 1] = eb_items[j];
      j--;
    }
    eb_items[j + 1] = key;
  }
}

// ---------------------------------------------------------------------------
// Main fetch: all configured sellers (or keyword), merge + sort
// ---------------------------------------------------------------------------
static bool ebFetchAll() {
  eb_item_count    = 0;
  eb_last_error[0] = '\0';

  bool hasSeller = (eb_seller1[0] || eb_seller2[0] || eb_seller3[0]);

  if (hasSeller) {
    if (eb_seller1[0]) ebFetchSeller(eb_seller1);
    if (eb_seller2[0]) ebFetchSeller(eb_seller2);
    if (eb_seller3[0]) ebFetchSeller(eb_seller3);
  } else if (eb_keyword[0]) {
    ebFetchKeyword(eb_keyword);
  } else {
    snprintf(eb_last_error, sizeof(eb_last_error), "No sellers/keyword");
    return false;
  }

  if (eb_item_count == 0 && eb_last_error[0] == '\0')
    snprintf(eb_last_error, sizeof(eb_last_error), "No auctions found");

  if (eb_item_count > 0) {
    ebSortItems();
    eb_last_fetch = time(nullptr);
    for (int i = 0; i < eb_item_count; i++)
      ebFormatTimeLeft(eb_items[i].endEpoch, eb_items[i].timeLeft,
                       sizeof(eb_items[i].timeLeft));
  }

  Serial.printf("[eBay] Total items: %d\n", eb_item_count);
  return (eb_item_count > 0);
}

// ---------------------------------------------------------------------------
// Returns true if any active auction ends within the next hour
// ---------------------------------------------------------------------------
static bool ebAnyEndingSoon() {
  time_t now = time(nullptr);
  for (int i = 0; i < eb_item_count; i++)
    if (eb_items[i].valid && eb_items[i].endEpoch > now &&
        (long)(eb_items[i].endEpoch - now) <= 3600)
      return true;
  return false;
}
static void ebRefreshTimeLeft() {
  for (int i = 0; i < eb_item_count; i++)
    ebFormatTimeLeft(eb_items[i].endEpoch, eb_items[i].timeLeft,
                     sizeof(eb_items[i].timeLeft));
}

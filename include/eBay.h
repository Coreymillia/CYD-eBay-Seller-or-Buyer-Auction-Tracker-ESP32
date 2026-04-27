#pragma once

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "CYDIdentity.h"

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
static EbayItem eb_items_backup[EB_MAX_ITEMS];
static int      eb_item_count  = 0;
static char     eb_last_error[64] = "";
static time_t   eb_last_fetch  = 0;
static IPAddress eb_last_api_ip;
static char     eb_api_host[20] = "api.ebay.com";
static bool     eb_refresh_stale = false;

// OAuth token cache (Browse API requires Bearer token)
// eBay tokens can exceed 1000 chars; use generous buffers to avoid truncation
static char   eb_token[2048]    = "";
static char   eb_auth_hdr[2100] = "";   // "Bearer <token>"
static time_t eb_token_expiry   = 0;

static void b64Encode(const char *src, char *dst, size_t dstLen);

#define EB_ERRFLAG_OAUTH  0x00000001u
#define EB_ERRFLAG_BROWSE 0x00000002u
#define EB_HTTP_TIMEOUT_S 15
#define EB_HTTP_IDLE_MS   15000UL
#define EB_HTTP_MAX_ATTEMPTS 4

// ---------------------------------------------------------------------------
// Error helpers
// ---------------------------------------------------------------------------
static bool ebShouldRetryHttpCode(int code) {
  return (code < 0 || code >= 500);
}

static unsigned long ebRetryDelayMs(int attempt, int code) {
  if (code == 504) return 2500UL * attempt;
  if (code == 502 || code == 503) return 1500UL * attempt;
  return 750UL * attempt;
}

static const char *ebFallbackApiHost(const char *host) {
  return (strcmp(host, "api.ebay.com") == 0) ? "apiz.ebay.com" : nullptr;
}

static void ebSetHttpError(const char *prefix, int code) {
  String detail = (code < 0) ? HTTPClient::errorToString(code) : String();
  if (detail.length() > 0) {
    snprintf(eb_last_error, sizeof(eb_last_error), "%s %s (%d)",
             prefix, detail.c_str(), code);
  } else {
    snprintf(eb_last_error, sizeof(eb_last_error), "%s HTTP %d", prefix, code);
  }
}

static void ebLogNetContext(const char *prefix) {
  Serial.printf("[eBay] %s net: ip=%s gw=%s dns1=%s dns2=%s rssi=%d\n",
                prefix,
                WiFi.localIP().toString().c_str(),
                WiFi.gatewayIP().toString().c_str(),
                WiFi.dnsIP(0).toString().c_str(),
                WiFi.dnsIP(1).toString().c_str(),
                WiFi.RSSI());
}

static bool ebApplyFallbackDns(const char *prefix) {
  IPAddress local = WiFi.localIP();
  IPAddress gw    = WiFi.gatewayIP();
  IPAddress mask  = WiFi.subnetMask();
  IPAddress dns1(1, 1, 1, 1);
  IPAddress dns2(8, 8, 8, 8);

  if ((uint32_t)local == 0 || (uint32_t)gw == 0 || (uint32_t)mask == 0) {
    Serial.printf("[eBay] %s cannot apply fallback DNS: incomplete network config\n", prefix);
    ebLogNetContext(prefix);
    return false;
  }

  if (WiFi.dnsIP(0) == dns1 && WiFi.dnsIP(1) == dns2) {
    return true;
  }

  bool ok = WiFi.config(local, gw, mask, dns1, dns2);
  Serial.printf("[eBay] %s fallback DNS %s -> dns1=%s dns2=%s\n",
                prefix,
                ok ? "applied" : "failed",
                dns1.toString().c_str(),
                dns2.toString().c_str());
  delay(250);
  ebLogNetContext(prefix);
  return ok;
}

static bool ebResolveApiHost(IPAddress &apiIp) {
  const char *hosts[] = {"api.ebay.com", "apiz.ebay.com"};
  for (size_t i = 0; i < 2; i++) {
    if (WiFi.hostByName(hosts[i], apiIp)) {
      strncpy(eb_api_host, hosts[i], sizeof(eb_api_host) - 1);
      eb_api_host[sizeof(eb_api_host) - 1] = '\0';
      return true;
    }
  }
  return false;
}

static bool ebConnectOfficialApiHost(WiFiClientSecure &client, IPAddress &usedIp) {
  if (WiFi.hostByName("api.ebay.com", usedIp)) {
    if (client.connect(usedIp, 443, "api.ebay.com", nullptr, nullptr, nullptr)) {
      return true;
    }
  }

  const IPAddress bootstrapIps[] = {
    IPAddress(151, 101, 2, 206),
    IPAddress(151, 101, 66, 206),
    IPAddress(151, 101, 130, 206),
    IPAddress(151, 101, 194, 206),
  };

  for (const IPAddress &ip : bootstrapIps) {
    if (client.connect(ip, 443, "api.ebay.com", nullptr, nullptr, nullptr)) {
      usedIp = ip;
      return true;
    }
  }

  return false;
}

static bool ebWaitForData(WiFiClientSecure &client, unsigned long timeoutMs) {
  unsigned long start = millis();
  while (!client.available()) {
    if (!client.connected()) return false;
    if (millis() - start >= timeoutMs) return false;
    delay(10);
  }
  return true;
}

static bool ebReadLine(WiFiClientSecure &client, String &line, unsigned long timeoutMs) {
  line = "";
  unsigned long lastDataMs = millis();
  bool sawData = false;

  while (true) {
    while (client.available()) {
      char c = (char)client.read();
      sawData = true;
      lastDataMs = millis();

      if (c == '\r') continue;
      if (c == '\n') return true;
      line += c;
    }

    if (sawData && !client.connected()) return true;
    if (!sawData && !client.connected()) return false;
    if (millis() - lastDataMs >= timeoutMs) return false;
    delay(10);
  }
}

static bool ebReadHttpResponse(WiFiClientSecure &client, int &statusCode, String &body) {
  String statusLine;
  do {
    if (!ebReadLine(client, statusLine, EB_HTTP_IDLE_MS)) return false;
    statusLine.trim();
  } while (statusLine.length() == 0);

  if (!statusLine.startsWith("HTTP/1.")) {
    return false;
  }

  int sp1 = statusLine.indexOf(' ');
  int sp2 = statusLine.indexOf(' ', sp1 + 1);
  statusCode = (sp1 >= 0) ? statusLine.substring(sp1 + 1, sp2 > sp1 ? sp2 : statusLine.length()).toInt() : 0;

  int contentLength = -1;
  bool chunked = false;
  while (client.connected() || client.available()) {
    String trimmed;
    if (!ebReadLine(client, trimmed, EB_HTTP_IDLE_MS)) return false;
    trimmed.trim();
    if (trimmed.length() == 0) break;

    String headerLower = trimmed;
    headerLower.toLowerCase();

    if (headerLower.startsWith("content-length:")) {
      contentLength = trimmed.substring(trimmed.indexOf(':') + 1).toInt();
    } else if (headerLower.startsWith("transfer-encoding:")) {
      chunked = (headerLower.indexOf("chunked") >= 0);
    }
  }

  body = "";
  if (chunked) {
    while (true) {
      String sizeLine;
      if (!ebReadLine(client, sizeLine, EB_HTTP_IDLE_MS)) return false;
      sizeLine.trim();
      if (sizeLine.length() == 0) continue;

      int semi = sizeLine.indexOf(';');
      if (semi >= 0) sizeLine = sizeLine.substring(0, semi);
      long chunkSize = strtol(sizeLine.c_str(), nullptr, 16);
      if (chunkSize < 0) return false;
      if (chunkSize == 0) {
        while (client.connected() || client.available()) {
          String trailer = client.readStringUntil('\n');
          trailer.trim();
          if (trailer.length() == 0) break;
        }
        break;
      }

      unsigned long lastDataMs = millis();
      while (chunkSize > 0) {
        if (!client.available() && !ebWaitForData(client, EB_HTTP_IDLE_MS)) return false;
        char c = (char)client.read();
        body += c;
        chunkSize--;
        lastDataMs = millis();
      }

      // Consume CRLF after each chunk.
      if (!ebWaitForData(client, EB_HTTP_IDLE_MS)) return false;
      client.read();
      if (!ebWaitForData(client, EB_HTTP_IDLE_MS)) return false;
      client.read();
    }
  } else if (contentLength >= 0) {
    unsigned long lastDataMs = millis();
    while ((int)body.length() < contentLength) {
      if (!client.available() && !ebWaitForData(client, EB_HTTP_IDLE_MS)) return false;
      char c = (char)client.read();
      body += c;
      lastDataMs = millis();
    }
  } else {
    unsigned long lastDataMs = millis();
    while ((client.connected() || client.available()) && millis() - lastDataMs < EB_HTTP_IDLE_MS) {
      while (client.available()) {
        body += (char)client.read();
        lastDataMs = millis();
      }
      delay(10);
    }
  }
  return true;
}

static bool ebExtractUri(const char *url, char *uri, size_t uriLen) {
  const char *scheme = strstr(url, "://");
  const char *path = scheme ? strchr(scheme + 3, '/') : strchr(url, '/');
  if (!path) return false;
  strncpy(uri, path, uriLen - 1);
  uri[uriLen - 1] = '\0';
  return true;
}

static bool ebFetchTokenOfficial(String &body, int &statusCode) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(EB_HTTP_TIMEOUT_S);
  client.setHandshakeTimeout(EB_HTTP_TIMEOUT_S);

  IPAddress usedIp;
  if (!ebConnectOfficialApiHost(client, usedIp)) {
    char sslErr[96] = "";
    int sslCode = client.lastError(sslErr, sizeof(sslErr));
    if (sslCode != 0) {
      snprintf(eb_last_error, sizeof(eb_last_error), "OAuth TLS failed");
      Serial.printf("[eBay] OAuth official host connect failed: %s (%d)\n", sslErr, sslCode);
    } else {
      snprintf(eb_last_error, sizeof(eb_last_error), "OAuth connect failed");
      Serial.println("[eBay] OAuth official host connect failed");
    }
    return false;
  }

  eb_last_api_ip = usedIp;
  strncpy(eb_api_host, "api.ebay.com", sizeof(eb_api_host) - 1);
  eb_api_host[sizeof(eb_api_host) - 1] = '\0';
  Serial.printf("[eBay] OAuth connected to official host via %s\n", usedIp.toString().c_str());

  char creds[350];
  snprintf(creds, sizeof(creds), "%s:%s", eb_appid, eb_certid);
  char b64[600];
  b64Encode(creds, b64, sizeof(b64));

  const char *payload =
    "grant_type=client_credentials"
    "&scope=https%3A%2F%2Fapi.ebay.com%2Foauth%2Fapi_scope";

  String req;
  req.reserve(1024);
  req += "POST /identity/v1/oauth2/token HTTP/1.1\r\n";
  req += "Host: api.ebay.com\r\n";
  req += "User-Agent: CYDEbayTicker/1.0\r\n";
  req += "Authorization: Basic ";
  req += b64;
  req += "\r\n";
  req += "Accept-Encoding: identity\r\n";
  req += "Content-Type: application/x-www-form-urlencoded\r\n";
  req += "Content-Length: ";
  req += strlen(payload);
  req += "\r\n";
  req += "Connection: close\r\n\r\n";
  req += payload;

  if (client.print(req) != (int)req.length()) {
    snprintf(eb_last_error, sizeof(eb_last_error), "OAuth send failed");
    client.stop();
    return false;
  }

  bool ok = ebReadHttpResponse(client, statusCode, body);
  client.stop();
  if (!ok) {
    snprintf(eb_last_error, sizeof(eb_last_error), "OAuth read failed");
    return false;
  }

  return true;
}

static bool ebFetchBrowseOfficial(const char *url, String &body, int &statusCode) {
  char uri[384];
  if (!ebExtractUri(url, uri, sizeof(uri))) {
    snprintf(eb_last_error, sizeof(eb_last_error), "Browse URL err");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(EB_HTTP_TIMEOUT_S);
  client.setHandshakeTimeout(EB_HTTP_TIMEOUT_S);

  IPAddress usedIp;
  if (!ebConnectOfficialApiHost(client, usedIp)) {
    char sslErr[96] = "";
    int sslCode = client.lastError(sslErr, sizeof(sslErr));
    if (sslCode != 0) {
      snprintf(eb_last_error, sizeof(eb_last_error), "Browse TLS failed");
      Serial.printf("[eBay] Browse official host connect failed: %s (%d)\n", sslErr, sslCode);
    } else {
      snprintf(eb_last_error, sizeof(eb_last_error), "Browse connect failed");
      Serial.println("[eBay] Browse official host connect failed");
    }
    return false;
  }

  eb_last_api_ip = usedIp;
  strncpy(eb_api_host, "api.ebay.com", sizeof(eb_api_host) - 1);
  eb_api_host[sizeof(eb_api_host) - 1] = '\0';

  String req;
  req.reserve(1024);
  req += "GET ";
  req += uri;
  req += " HTTP/1.1\r\n";
  req += "Host: api.ebay.com\r\n";
  req += "User-Agent: CYDEbayTicker/1.0\r\n";
  req += "Authorization: ";
  req += eb_auth_hdr;
  req += "\r\n";
  req += "X-EBAY-C-MARKETPLACE-ID: EBAY_US\r\n";
  req += "Accept-Encoding: identity\r\n";
  req += "Connection: close\r\n\r\n";

  if (client.print(req) != (int)req.length()) {
    snprintf(eb_last_error, sizeof(eb_last_error), "Browse send failed");
    client.stop();
    return false;
  }

  bool ok = ebReadHttpResponse(client, statusCode, body);
  client.stop();
  if (!ok) {
    snprintf(eb_last_error, sizeof(eb_last_error), "Browse read failed");
    return false;
  }

  return true;
}

static bool ebDiagnoseApiConnect(const char *prefix) {
  IPAddress apiIp;
  if (!ebResolveApiHost(apiIp)) {
    Serial.printf("[eBay] %s DNS lookup failed for api.ebay.com and apiz.ebay.com\n", prefix);
    ebLogNetContext(prefix);
    if (ebApplyFallbackDns(prefix) && ebResolveApiHost(apiIp)) {
      eb_last_api_ip = apiIp;
      Serial.printf("[eBay] %s DNS recovered via fallback host %s -> %s\n",
                    prefix, eb_api_host, apiIp.toString().c_str());
    } else {
      snprintf(eb_last_error, sizeof(eb_last_error), "%s DNS failed", prefix);
      return false;
    }
  }

  eb_last_api_ip = apiIp;
  Serial.printf("[eBay] %s resolved %s -> %s\n",
                prefix, eb_api_host, apiIp.toString().c_str());

  WiFiClientSecure probe;
  probe.setInsecure();
  probe.setTimeout(15);
  probe.setHandshakeTimeout(15);

  if (!probe.connect(eb_api_host, 443)) {
    char sslErr[96] = "";
    int sslCode = probe.lastError(sslErr, sizeof(sslErr));
    if (sslCode != 0) {
      snprintf(eb_last_error, sizeof(eb_last_error), "%s TLS failed", prefix);
      Serial.printf("[eBay] %s TLS probe failed: %s (%d)\n",
                    prefix, sslErr, sslCode);
    } else {
      snprintf(eb_last_error, sizeof(eb_last_error), "%s connect failed", prefix);
      Serial.printf("[eBay] %s TCP/TLS probe failed with no SSL detail\n", prefix);
    }
    ebLogNetContext(prefix);
    probe.stop();
    return false;
  }

  Serial.printf("[eBay] %s TLS probe OK to %s (%s):443\n",
                prefix, eb_api_host, apiIp.toString().c_str());
  probe.stop();
  return true;
}

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

  for (int attempt = 1; attempt <= EB_HTTP_MAX_ATTEMPTS; attempt++) {
    int code = 0;
    String body;
    if (!ebFetchTokenOfficial(body, code)) {
      Serial.printf("[eBay] OAuth fetch failed (attempt %d/%d): %s\n",
                    attempt, EB_HTTP_MAX_ATTEMPTS, eb_last_error);
      if (attempt < EB_HTTP_MAX_ATTEMPTS) {
        delay(ebRetryDelayMs(attempt, -1));
        continue;
      }
      identity_error_flags |= EB_ERRFLAG_OAUTH;
      return false;
    }

    if (code != HTTP_CODE_OK) {
      ebSetHttpError("OAuth", code);
      Serial.printf("[eBay] OAuth failure (attempt %d/%d): %s\n",
                    attempt, EB_HTTP_MAX_ATTEMPTS, eb_last_error);
      if (body.length()) {
        Serial.printf("[eBay] OAuth body: %.200s\n", body.c_str());
      }
      if (attempt < EB_HTTP_MAX_ATTEMPTS && ebShouldRetryHttpCode(code)) {
        delay(ebRetryDelayMs(attempt, code));
        continue;
      }
      identity_error_flags |= EB_ERRFLAG_OAUTH;
      return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
      snprintf(eb_last_error, sizeof(eb_last_error), "OAuth JSON err");
      Serial.printf("[eBay] OAuth JSON parse failed. Body: %.240s\n", body.c_str());
      identity_error_flags |= EB_ERRFLAG_OAUTH;
      return false;
    }

    const char *tok = doc["access_token"] | "";
    int expiresIn   = doc["expires_in"]   | 7200;

    if (!tok[0]) {
      snprintf(eb_last_error, sizeof(eb_last_error), "No token");
      identity_error_flags |= EB_ERRFLAG_OAUTH;
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

  identity_error_flags |= EB_ERRFLAG_OAUTH;
  return false;
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
  for (int attempt = 1; attempt <= EB_HTTP_MAX_ATTEMPTS; attempt++) {
    int code = 0;
    String payload;
    if (!ebFetchBrowseOfficial(url, payload, code)) {
      Serial.printf("[eBay] Browse fetch failed (attempt %d/%d): %s\n",
                    attempt, EB_HTTP_MAX_ATTEMPTS, eb_last_error);
      if (attempt < EB_HTTP_MAX_ATTEMPTS) {
        delay(ebRetryDelayMs(attempt, -1));
        continue;
      }
      identity_error_flags |= EB_ERRFLAG_BROWSE;
      return 0;
    }

    if (code != HTTP_CODE_OK) {
      ebSetHttpError("Browse", code);
      Serial.printf("[eBay] Browse failure (attempt %d/%d)\n  Error: %s\n  URL: %.120s\n",
                    attempt, EB_HTTP_MAX_ATTEMPTS, eb_last_error, url);
      if (payload.length()) {
        Serial.printf("  Body: %.300s\n", payload.c_str());
      }
      if (attempt < EB_HTTP_MAX_ATTEMPTS && ebShouldRetryHttpCode(code)) {
        delay(ebRetryDelayMs(attempt, code));
        continue;
      }
      identity_error_flags |= EB_ERRFLAG_BROWSE;
      return 0;
    }

    JsonDocument doc;
    if (deserializeJson(doc, payload)) {
      snprintf(eb_last_error, sizeof(eb_last_error), "JSON error");
      Serial.printf("[eBay] Browse JSON parse failed. Body: %.240s\n", payload.c_str());
      identity_error_flags |= EB_ERRFLAG_BROWSE;
      return 0;
    }

    JsonArray items = doc["itemSummaries"].as<JsonArray>();
    if (items.isNull()) {
      Serial.println("[eBay] No itemSummaries in response");
      return 0;
    }

    return ebParseItems(items);
  }

  identity_error_flags |= EB_ERRFLAG_BROWSE;
  return 0;
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
      "https://%s/buy/browse/v1/item_summary/search"
      "?category_ids=%s&filter=sellers%%3A%%7B%s%%7D%%2CbuyingOptions%%3A%%7BAUCTION%%7D"
      "&sort=endingSoonest&limit=%d",
      eb_api_host, eb_cat_id, sellerId, EB_PER_SELLER);
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
        "https://%s/buy/browse/v1/item_summary/search"
        "?q=%s&filter=sellers%%3A%%7B%s%%7D%%2CbuyingOptions%%3A%%7BAUCTION%%7D"
        "&sort=endingSoonest&limit=%d",
        eb_api_host, encTok, sellerId, EB_PER_SELLER);
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
    "https://%s/buy/browse/v1/item_summary/search"
    "?q=%s&filter=buyingOptions%%3A%%7BAUCTION%%7D"
    "&sort=endingSoonest&limit=%d",
    eb_api_host, encoded, EB_MAX_ITEMS);

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
  int prev_item_count = eb_item_count;
  time_t prev_last_fetch = eb_last_fetch;
  unsigned long prev_identity_last_fetch = identity_last_fetch;
  bool had_cached_items = (prev_item_count > 0);
  if (had_cached_items) {
    memcpy(eb_items_backup, eb_items, sizeof(eb_items));
  }

  eb_item_count    = 0;
  eb_last_error[0] = '\0';
  identity_error_flags = 0;
  eb_refresh_stale = false;

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
    identity_last_fetch = (unsigned long)eb_last_fetch;
    for (int i = 0; i < eb_item_count; i++)
      ebFormatTimeLeft(eb_items[i].endEpoch, eb_items[i].timeLeft,
                       sizeof(eb_items[i].timeLeft));
  }

  if (identity_error_flags != 0 && had_cached_items) {
    memcpy(eb_items, eb_items_backup, sizeof(eb_items));
    eb_item_count = prev_item_count;
    eb_last_fetch = prev_last_fetch;
    identity_last_fetch = prev_identity_last_fetch;
    eb_refresh_stale = true;
    Serial.printf("[eBay] Refresh failed (%s) - showing %d cached items from last good fetch\n",
                  eb_last_error[0] ? eb_last_error : "unknown error",
                  eb_item_count);
    return true;
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

#include "net_fetch.h"
#include "net_lock.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

namespace net {
namespace {

struct PsramAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override {
        void* p = ps_malloc(n);
        return p ? p : malloc(n);
    }
    void deallocate(void* p) override { free(p); }
    void* reallocate(void* p, size_t n) override {
        void* q = ps_realloc(p, n);
        return q ? q : realloc(p, n);
    }
};
PsramAllocator psram_alloc;

constexpr size_t BODY_CAP = 700 * 1024;

// Response buffer, allocated once in PSRAM (heap fallback) and reused across
// polls — re-allocating ~700 KB every cycle is pointless churn.
char*  body_buf = nullptr;
size_t body_cap = 0;

// Read the full body of an in-progress GET into the shared buffer.
char* read_body(HTTPClient& http, size_t& out_len) {
    if (!body_buf) {
        body_buf = (char*)ps_malloc(BODY_CAP);
        body_cap = BODY_CAP;
        if (!body_buf) {                        // no PSRAM? small heap fallback
            body_buf = (char*)malloc(48 * 1024);
            body_cap = 48 * 1024;
        }
        if (!body_buf) return nullptr;
    }

    WiFiClient* stream = http.getStreamPtr();
    int   total = http.getSize();               // -1 when no Content-Length
    size_t len = 0;
    uint32_t last_data = millis();

    while (stream->connected() || stream->available()) {
        size_t avail = stream->available();
        if (avail) {
            size_t want = min(avail, body_cap - 1 - len);
            if (want == 0) break;               // cap reached
            len += stream->readBytes(body_buf + len, want);
            last_data = millis();
            if (total > 0 && (int)len >= total) break;
        } else {
            if (millis() - last_data > 8000) break;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    body_buf[len] = '\0';
    out_len = len;
    return body_buf;
}

}  // namespace

ArduinoJson::Allocator* psram_allocator() { return &psram_alloc; }

bool http_get_json(const char* url, JsonDocument& doc, JsonDocument& filter) {
    netlock::Guard one_tls_at_a_time;
    WiFiClientSecure client;
    client.setInsecure();   // public read-only data; CA validation not worth the RAM
    HTTPClient http;
    http.useHTTP10(true);   // no chunked transfer encoding
    http.setConnectTimeout(5000);
    http.setTimeout(8000);
    if (!http.begin(client, url)) return false;

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        log_w("[net] GET %s -> %d", url, code);
        http.end();
        return false;
    }

    size_t len = 0;
    char* body = read_body(http, len);
    http.end();
    if (!body) {
        log_w("[net] body alloc failed");
        return false;
    }

    DeserializationError err =
        deserializeJson(doc, body, len, DeserializationOption::Filter(filter));
    if (err) {
        log_w("[net] JSON parse (%u bytes): %s", (unsigned)len, err.c_str());
        return false;
    }
    return true;
}

bool http_get_text(const char* url, char* out, size_t cap, const char* bearer) {
    netlock::Guard one_tls_at_a_time;
    // Pick transport by scheme — Home Assistant is usually plain http on the LAN.
    WiFiClient      plain;
    WiFiClientSecure secure;
    bool https = strncmp(url, "https:", 6) == 0;
    if (https) secure.setInsecure();

    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(8000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    bool ok = https ? http.begin(secure, url) : http.begin(plain, url);
    if (!ok) return false;

    if (bearer && bearer[0]) {
        String auth = "Bearer ";
        auth += bearer;
        http.addHeader("Authorization", auth);
    }

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        log_w("[net] GET %s -> %d", url, code);
        http.end();
        return false;
    }
    String body = http.getString();
    http.end();
    strlcpy(out, body.c_str(), cap);
    return true;
}

}  // namespace net

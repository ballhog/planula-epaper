// OTA client, shared design with the Coeno knob and the Calyx nodes
// (coeno/hardware/knob-display/src/ota.cpp). Planula-ePaper: health means
// Wi-Fi up and one screen drawn with data.
#include "ota.h"
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "mbedtls/sha256.h"
#include "esp_ota_ops.h"

#ifndef FW_BUILD
#define FW_BUILD 0
#endif

// App-level rollback state. We track "just OTA'd, not yet confirmed" in NVS
// (the bootloader can't do it for us under the Arduino framework).
static const char    *OTA_NVS_NS    = "ota";
static const char    *OTA_NVS_KEY   = "pending";   // bool: image awaiting health confirmation
static const char    *OTA_NVS_TRIES = "tries";     // u8:   unconfirmed boots so far (boot-loop guard)
static const uint32_t OTA_VERIFY_MS = 120000;  // prove network + sensor-cycle health within 120s
static const uint8_t  OTA_MAX_BOOT_TRIES = 3;  // roll back after this many unconfirmed boots
static bool          _verifying       = false;
static uint32_t      _verify_deadline = 0;
static volatile bool _reverting       = false;   // UI shows the revert message when true
static volatile int  _dl_pct          = -1;      // -1 idle; 0-100 while downloading

bool ota_is_reverting() { return _reverting; }
int  ota_download_pct() { return _dl_pct; }

// Centralizes UI download state: any "downloading" status updates the progress
// bar; any other state (failed / bad-checksum / applied) clears it.
static void say(ota_status_cb cb, const char *s, int pct) {
    _dl_pct = (strcmp(s, "downloading") == 0) ? pct : -1;
    if (cb) cb(s, pct);
}

static void _set_pending(bool v) {
    Preferences p;
    if (!p.begin(OTA_NVS_NS, false)) return;
    if (v) p.putBool(OTA_NVS_KEY, true);
    else   p.remove(OTA_NVS_KEY);
    p.end();
}

static uint8_t _get_tries() {
    Preferences p;
    uint8_t t = 0;
    if (p.begin(OTA_NVS_NS, true)) { t = p.getUChar(OTA_NVS_TRIES, 0); p.end(); }
    return t;
}

static void _set_tries(uint8_t t) {
    Preferences p;
    if (!p.begin(OTA_NVS_NS, false)) return;
    if (t) p.putUChar(OTA_NVS_TRIES, t);
    else   p.remove(OTA_NVS_TRIES);
    p.end();
}

// Flip the boot partition back to the previous (non-running) slot and reboot.
// Used by both the boot-loop guard and the WiFi/MQTT health timeout.
static void _rollback(const char *why) {
    _set_pending(false);   // clear so the restored image boots clean (not pending)
    _set_tries(0);
    const esp_partition_t *prev = esp_ota_get_next_update_partition(NULL);  // non-running = old image
    if (prev && esp_ota_set_boot_partition(prev) == ESP_OK) {
        Serial.printf("[ota] %s — rolling back to previous firmware\n", why);
        _reverting = true;     // UI renders "Update failed - reverting" if it's already up
        delay(1800);           // let it show (no-op during early-boot rollback)
        esp_restart();
    } else {
        Serial.printf("[ota] %s but no valid previous slot — staying put\n", why);
        _verifying = false;    // give up; don't loop
    }
}

// Compare a 32-byte digest against a 64-char lowercase-hex string.
static bool digest_matches(const uint8_t *digest, const char *hex) {
    if (!hex || strlen(hex) != 64) return false;
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        char hi = H[(digest[i] >> 4) & 0xF];
        char lo = H[digest[i] & 0xF];
        if (tolower((unsigned char)hex[i * 2]) != hi ||
            tolower((unsigned char)hex[i * 2 + 1]) != lo)
            return false;
    }
    return true;
}

static int  _last_http = 0;
static char _last_reason[48] = "";
int ota_last_http() { return _last_http; }
const char *ota_last_reason() { return _last_reason; }

long ota_check(const char *manifest_url) {
    HTTPClient http;
    _last_http = 0;
    _last_reason[0] = '\0';
    if (!http.begin(manifest_url)) { strlcpy(_last_reason, "bad update source URL", sizeof(_last_reason)); return -1; }
    http.setConnectTimeout(8000);
    http.setTimeout(8000);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        _last_http = code;
        if (code == 404) strlcpy(_last_reason, "no build published there (404)", sizeof(_last_reason));
        else if (code < 0) snprintf(_last_reason, sizeof(_last_reason), "can't connect (%s)", HTTPClient::errorToString(code).c_str());
        else snprintf(_last_reason, sizeof(_last_reason), "server answered HTTP %d", code);
        http.end();
        return -1;
    }
    String body = http.getString();
    http.end();
    JsonDocument doc;
    if (deserializeJson(doc, body)) { strlcpy(_last_reason, "manifest isn't valid JSON", sizeof(_last_reason)); return -1; }
    long        build = doc["build"]  | 0L;
    const char *url   = doc["url"]    | "";
    const char *sha   = doc["sha256"] | "";
    long        size  = doc["size"]   | 0L;
    if (!url[0] || !sha[0] || size <= 0) { strlcpy(_last_reason, "manifest is missing url/sha256/size", sizeof(_last_reason)); return -1; }
    return build > FW_BUILD ? build : 0;
}

bool ota_check_and_apply(const char *manifest_url, ota_status_cb cb) {
    // ── 1. Fetch + parse the manifest ──────────────────────────────────────
    HTTPClient http;
    if (!http.begin(manifest_url)) return false;
    http.setConnectTimeout(8000);
    http.setTimeout(8000);
    int code = http.GET();
    if (code != HTTP_CODE_OK) { http.end(); return false; }  // 404 → no manifest yet
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    long        build = doc["build"]  | 0L;
    const char *url   = doc["url"]    | "";
    const char *sha   = doc["sha256"] | "";
    long        size  = doc["size"]   | 0L;

    if (build <= FW_BUILD) return false;                     // not newer
    if (!url[0] || !sha[0] || size <= 0) return false;       // malformed manifest

    // ── 2. Download into the inactive OTA slot, hashing as we go ───────────
    say(cb, "downloading", 0);
    HTTPClient dl;
    if (!dl.begin(url)) { say(cb, "failed", 0); return false; }
    dl.setConnectTimeout(8000);
    dl.setTimeout(15000);
    code = dl.GET();
    if (code != HTTP_CODE_OK) { dl.end(); say(cb, "failed", 0); return false; }
    int clen = dl.getSize();
    if (clen > 0 && clen != size) { dl.end(); say(cb, "failed", 0); return false; }

    if (!Update.begin(size)) { dl.end(); say(cb, "failed", 0); return false; }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);  // 0 → SHA-256 (not SHA-224)

    WiFiClient *stream = dl.getStreamPtr();
    uint8_t buf[1024];
    long written = 0;
    while (dl.connected() && written < size) {
        size_t avail = stream->available();
        if (avail) {
            size_t want = avail < sizeof(buf) ? avail : sizeof(buf);
            int n = stream->readBytes(buf, want);
            if (n <= 0) break;
            if (Update.write(buf, n) != (size_t)n) {
                Update.abort(); dl.end(); mbedtls_sha256_free(&ctx);
                say(cb, "failed", 0); return false;
            }
            mbedtls_sha256_update(&ctx, buf, n);
            written += n;
            say(cb, "downloading", (int)(100L * written / size));
        } else {
            delay(1);
        }
    }
    dl.end();

    uint8_t digest[32];
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    // ── 3. Verify integrity before committing the boot flip ────────────────
    if (written != size || !digest_matches(digest, sha)) {
        Update.abort();
        say(cb, "bad-checksum", 0);
        return false;
    }
    if (!Update.end(true)) {   // true → set the new slot as boot partition
        say(cb, "failed", 0);
        return false;
    }

    // ── 4. Mark needs-verify + reboot into the new firmware ────────────────
    _set_pending(true);   // the new image must confirm health or we roll back
    _set_tries(0);        // fresh update → start the boot-loop counter from zero
    say(cb, "applied", 100);
    delay(500);
    ESP.restart();
    return true;  // unreachable
}

// ── App-level rollback lifecycle ────────────────────────────────────────────

void ota_begin_boot_check() {
    Preferences p;
    bool pending = false;
    if (p.begin(OTA_NVS_NS, true)) { pending = p.getBool(OTA_NVS_KEY, false); p.end(); }
    if (!pending) return;

    // Boot-loop guard. Bump a persistent counter on EVERY unconfirmed boot —
    // this runs before any crash-prone init (display/LVGL/charts), so a fresh
    // image that hard-crashes on boot still increments it. Once we hit
    // OTA_MAX_BOOT_TRIES boots without confirming health, roll back. This is the
    // piece the 90s timer alone can't provide: a crash before the self-test
    // resets the timer every boot, so without this counter it loops forever.
    // ota_mark_healthy() clears the counter when the new image proves itself.
    uint8_t tries = _get_tries() + 1;
    _set_tries(tries);
    Serial.printf("[ota] pending verify — unconfirmed boot %u of %u\n", tries, OTA_MAX_BOOT_TRIES);
    if (tries >= OTA_MAX_BOOT_TRIES) {
        _rollback("boot-loop: never confirmed healthy");   // reboots; does not return
        return;
    }
    // Survived this far → also arm the connectivity timer for the "boots and
    // runs but never gets healthy" case (no crash, so the counter alone
    // wouldn't fire).
    _verifying = true;
    _verify_deadline = millis() + OTA_VERIFY_MS;
    Serial.println("[ota] confirm network + a sensor cycle in 120s or roll back");
}

void ota_mark_pending() {
    _set_tries(0);
    _set_pending(true);
}

void ota_mark_healthy() {
    if (!_verifying) return;
    _verifying = false;
    _set_pending(false);
    _set_tries(0);     // commit: this image is good, reset the boot-loop counter
    // Courtesy to IDF's OTA state machine; a no-op without bootloader rollback cfg.
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("[ota] new firmware confirmed healthy");
}

void ota_tick() {
    if (!_verifying) return;
    if ((int32_t)(millis() - _verify_deadline) < 0) return;   // window not elapsed
    _verifying = false;
    _rollback("verify timed out (no network or no sensor cycle)");
}

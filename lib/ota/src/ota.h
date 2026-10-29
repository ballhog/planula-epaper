#pragma once
#include <Arduino.h>

// OTA client (same design as the Coeno knob and Calyx nodes) — pull model. Fetches a JSON manifest from the Pi; if it
// advertises a newer FW_BUILD, downloads the .bin, verifies SHA-256, applies it
// via the Update library (writes to the inactive OTA slot, flips boot only on
// success), and reboots.
//
// Manifest shape (served by the Pi at OTA_MANIFEST_URL):
//   { "build": 1234, "version": "v1.2-3-gabc", "url": "http://10.0.0.228:8000/firmware/calyx-sense/calyx-sense-1234.bin",
//     "sha256": "<64 hex>", "size": 1353552 }
//
// Returns:
//   true  → an update was applied (device reboots; call does not return)
//   false → no update available, or any failure (stays on current firmware)
//
// Safe to call when the Pi has no manifest yet: a 404 is treated as "no update".
typedef void (*ota_status_cb)(const char *state, int pct);

bool ota_check_and_apply(const char *manifest_url, ota_status_cb cb);

// Check only: reads the manifest and reports, without downloading.
//   > 0  build number of a newer, well-formed image on the server
//     0  nothing newer
//    -1  server unreachable, no manifest, or malformed manifest
long ota_check(const char *manifest_url);

// Why the last check failed, for the status page: the HTTP status (404 = no
// build published) or an HTTPClient error (< 0, e.g. connection refused), and
// a short text for it. 0 = no failure.
int ota_last_http();
const char *ota_last_reason();

// ── App-level rollback (no bootloader app-rollback under Arduino framework) ──
// After an OTA, the new image must prove it's healthy (network up and one
// full sensor cycle done) or it
// reverts to the previous slot. Two complementary guards, both cleared by
// ota_mark_healthy():
//   1. Boot-loop counter — bumped in ota_begin_boot_check() on every unconfirmed
//      boot, BEFORE any crash-prone init. A fresh image that hard-crashes on
//      boot (e.g. a bad render path) rolls back after OTA_MAX_BOOT_TRIES resets,
//      even though it never reaches the self-test. Closes the old gap where a
//      pre-self-test crash looped forever (each reboot reset the 90s timer).
//   2. Health timeout — if the image boots and runs but never gets the
//      network up and a sensor cycle done within ~120s, ota_tick() reverts.
//
// Wiring in main:
//   setup() (early):    ota_begin_boot_check();   // counts the boot; may roll back now
//   net up + 1st cycle: ota_mark_healthy();       // confirms the new image is good
//   in a loop:          ota_tick();               // reverts if not confirmed in time
void ota_begin_boot_check();
void ota_mark_healthy();
// A new image was written by some other path (network upload from the Mac):
// treat it like a pulled update, so it must prove itself or roll back.
void ota_mark_pending();
void ota_tick();

// True while ota_tick() is about to roll back (set ~1.8s before reboot) so the
// UI task can show "Update failed - reverting" on the boot screen.
bool ota_is_reverting();

// Download progress for the UI: -1 when not downloading, else 0-100. Lets the
// boot screen show "Updating  N%" while a fresh build streams from the Pi.
int ota_download_pct();

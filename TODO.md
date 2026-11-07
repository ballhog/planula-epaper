# Planula-ePaper — to do

Done 2026-11-07 (builds 53-58; item 2 rejoins by BSSID + channel, the DHCP lease is not reused): 1-6 and 8. Still open: 7 and the questions below.

From the code review on 2026-11-07 (after build 52). Order = suggested order;
1, 2, 4 and 5 go together in the next build, none of them changes what the
screen shows.

## Speed

- [x] **1. Draw once per change on the reTerminal.** `display::show()` renders
  the scene three times: to hash it, to write it, and again for
  `writeImageAgain`. Banding (`BandCanvas`, 120-row strips) needs that on the
  Waveshare board, but with PSRAM the whole frame is one band, so render once
  and reuse the buffer for all three. Also log how long render and push take
  (`[display] render N ms, push N ms`) so there are numbers, not guesses.
- [x] **2. Fast Wi-Fi reconnect after a battery sleep.** Every Saver/Daily wake
  joins Wi-Fi from scratch (~2-4 s of radio). Keep the access point's BSSID and
  channel (and the DHCP lease: IP, gateway, DNS) in RTC memory and reconnect
  with them; fall back to a normal join if it fails. Usually under 1 s, and
  Wi-Fi is most of the battery drain.
- [x] **3. Don't rebuild every page for alarms on each redraw.**
  `collectAlarms()` builds all pages (`fetchPage`) every draw. Cache per page
  and rebuild only when readings changed (`readings::newKeys()` / an update
  counter) or a Coeno page arrived.

The e-paper itself (~0.9 s partial, ~1.6 s full refresh) is the floor.

## Clean-up

- [x] **4. Split `src/main.cpp` (~1,100 lines).** Into `Keys.cpp` (reTerminal
  buttons, BOOT button), `Alarms.cpp` (collect, acknowledge, reminders, beeps),
  `Updates.cpp` (check / install / upload), `Screen.cpp` (chrome, draw, page
  rotation). Same behaviour, smaller files.
- [x] **5. Host tests for the logic that has had the bugs.** Move the pure parts
  into `lib/` and test them with real replies saved from Joe's logs:
  - battery redraw decision (`power::stepFor`, compare, the saver / daily /
    hourly rules) and next-wake times (`nextAt`, hourly check, midnight)
  - outlet history and the 12 h window (`outlets::record`, `buildPage`)
  - Apex `/rest/status` parsing (inputs and outputs, °F/°C)
  - Hydros status parsing (`Input`, `Output.valueState`, water change, dosers)
  - signed time differences (the 49-day `sessionStart` bug, build 17)
- [x] **6. Fewer throw-away Strings per redraw.** About 60 `String`s in
  `main.cpp`, mostly the footer/status lines built on every draw. Harmless on
  the reTerminal (PSRAM); on the Waveshare board they fragment the heap over
  days. Fixed `char` buffers for the per-draw ones.
- [x] **7. Remove the one-off Hydros boot dumps** (status layout, outputs
  section, dosing head) once the dose-amount question is answered.
- [x] **8. Refresh the README.** Still describes the old header, the
  single-button layout on the reTerminal and other pre-build-40 details;
  add the power modes, buttons, views, takeover, Outlets page.

## Next round (2026-11-07, after a night on battery)

- [x] **9. History from the controllers, not just what the panel saw.** On
  battery the panel looks every 15 min: trends are coarse, outlet changes land
  late, short doses are missed, and a reflash starts from nothing. Read the
  Apex data log and outlet log (`/rest/ilog`, `/rest/olog`) on each wake for
  true 24 h sparklines and an accurate Outlets timeline. Find whether Hydros's
  cloud API has the same.
- [x] **10. Hydros: don't sign in from scratch on every wake.** Each wake did
  Cognito login, `/user`, `/thing`, GetCredentialsForIdentity, then the IoT
  socket: four or five TLS handshakes. Keep the account details and the AWS
  credentials (good for ~1 h) and reuse them.
- [ ] **11. Measure the battery.** One night on Saver (15 min), % before and
  after. Decides whether Saver is enough or Daily + hourly check is the default.
- [ ] **12. Honest room temperature.** The SHT4x sits by the ESP32 and reads
  warm once awake. Read it first thing on a wake, before Wi-Fi heats the
  board, and add an offset setting.
- [ ] **13. Glance cues.** A trend arrow (↑ ↓) on the Numbers view; a small mark
  on tiles that moved since the last refresh.
- [ ] **14. Product polish.** Host tests on real saved Apex / Hydros replies;
  drop the Hydros boot dumps (item 7); setup that doesn't need the USB log.

## Open questions

- [x] **Hydros dose amounts.** Answered 2026-11-07: the status has no dose
  volume (`remaining`, `reservoir` only); the amounts come from the cloud
  dosing log (`GET /logs`), read every 30 min and shown above the marks.
  Was: Waiting on the `[hydros] a dosing head: …` boot
  line: if the status carries a dose volume, show it above each dose mark on
  the Outlets page (like "2.5" on the Coeno Schedule page).
- [ ] **Battery life.** Measure: Saver every 10 min overnight on battery, note
  the % before and after.

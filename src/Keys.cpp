// Buttons: the BOOT button (Waveshare) and the reTerminal's three keys.
// Split out of main.cpp; shared state in Shared.h.
#include "Shared.h"
#include "Log.h"

namespace ui {

#if !defined(BOARD_RETERMINAL)  // the reTerminal has its own keys, below
// ── button ──────────────────────────────────────────────────────────────
uint32_t pressedAt = 0;
bool wasDown = false, longDone = false;

void button() {
  bool down = digitalRead(PIN_BUTTON) == LOW;
  uint32_t now = millis();
  if (down && !wasDown) {
    pressedAt = now;
    longDone = false;
  }
  if (down && !longDone) {
    uint32_t held = now - pressedAt;
    if (held > 25000) {
      longDone = true;
      settings::factoryReset();
      panel::Chrome c;
      display::show(message("Settings erased", "Restarting into setup mode…", "", c), true);
      delay(500);
      ESP.restart();
    }
  }
  if (!down && wasDown) {
    uint32_t held = now - pressedAt;
    if (held >= 10000 && held <= 25000) {
      net::openSetup("Setup network opened with the button.");
      app::redraw(false);
    } else if (held >= 3000) {
      // Opens the System screen; held again while it's up = check for updates now.
      if (infoUntil && now < infoUntil && coenoLinked()) app::checkUpdateSoon();
      infoUntil = now + 120000;
      app::redraw(false);
    } else if (held >= 40) {
      if (infoUntil && now < infoUntil) {
        // System screen: press = install when there's one (or retry), else close.
        if (availBuild > 0 && coenoLinked()) {
          app::installUpdateSoon();
          infoUntil = now + 120000;
        } else {
          infoUntil = 0;
        }
        app::redraw(false);
      } else if (!alarms.empty() && !acked) app::ackAlarm();
      else app::nextPage();
    }
  }
  wasDown = down;
}

#endif

#if defined(BOARD_RETERMINAL)
// ── reTerminal E1001 keys (Seeed: KEY0 GPIO3 right/green, KEY1 GPIO4 middle,
// KEY2 GPIO5 left; active low) ─────────────────────────────────────────────
//   left / right       previous / next page
//   middle             acknowledge the alarm, else switch the view
//                      (number + trend -> graphs -> numbers only)
//   middle hold 3 s    System screen (hold again there = check for updates)
//   middle hold 10 s   open the setup network
//   left + right 10 s  erase settings
struct Key {
  int pin;
  bool down = false, longDone = false;
  uint32_t at = 0, changed = 0;
};
Key keyRight{3}, keyMid{4}, keyLeft{5};

// Debounced: true when the key changed state this pass.
bool readKey(Key &k, uint32_t now) {
  bool d = digitalRead(k.pin) == LOW;
  if (d == k.down || now - k.changed < 30) return false;
  k.down = d;
  k.changed = now;
  if (d) {
    k.at = now;
    k.longDone = false;
  }
  return true;
}

void keysBegin() {
  pinMode(keyRight.pin, INPUT_PULLUP);
  pinMode(keyMid.pin, INPUT_PULLUP);
  pinMode(keyLeft.pin, INPUT_PULLUP);
}

void keys() {
  uint32_t now = millis();
  bool eR = readKey(keyRight, now), eM = readKey(keyMid, now), eL = readKey(keyLeft, now);
  if (eR || eM || eL) power::touch();
  if (eR || eM || eL)
    plog.printf("[keys] left %s, middle %s, right %s\n", keyLeft.down ? "down" : "up", keyMid.down ? "down" : "up",
                  keyRight.down ? "down" : "up");

  // Left + right held together: erase.
  if (keyLeft.down && keyRight.down) {
    uint32_t both = now - max(keyLeft.at, keyRight.at);
    if (both > 10000) {
      settings::factoryReset();
      panel::Chrome c;
      display::show(message("Settings erased", "Restarting into setup mode…", "", c), true);
      delay(500);
      ESP.restart();
    }
    if (both > 300) keyLeft.longDone = keyRight.longDone = true;  // no page turn after a chord
    return;
  }

  bool infoUp = infoUntil && now < infoUntil;
  // Presses and 3 s holds do what Display → Buttons says. Fixed: middle held
  // 10 s = setup network, left + right held 10 s = erase.
  auto doAction = [&](const String &action) {
    if (action == "prev") app::prevPage();
    else if (action == "next") app::nextPage();
    else if (action == "view") app::cycleView();
    else if (action == "schedule") app::toggleSchedule();
    else if (action.startsWith("page")) app::togglePage(action.substring(4).toInt() - 1);
    else if (action == "refresh") {
      apex::pollNow();
      app::redraw(true);
    } else if (action == "info") {
      infoUntil = now + 120000;
      app::redraw(false);
    }
  };
  auto shortPress = [&](const String &action, bool isMiddle) {
    const Settings &s = settings::get();
    bool ackHere = s.keyAck == "any" || isMiddle;
    if (ackHere && !alarms.empty() && !acked) {
      app::ackAlarm();
      return;
    }
    if (infoUp) {  // on the System screen a press closes it (middle installs)
      if (isMiddle && availBuild > 0 && coenoLinked()) {
        app::installUpdateSoon();
        infoUntil = now + 120000;
      } else {
        infoUntil = 0;
      }
      app::redraw(false);
      return;
    }
    doAction(action);
  };
  auto holdPress = [&](const String &action) {
    if (action == "info" && infoUp) {  // held again on the System screen: check for updates
      if (coenoLinked() || settings::get().githubUpdates) app::checkUpdateSoon();
      infoUntil = now + 120000;
      app::redraw(false);
      return;
    }
    doAction(action);
  };
  const Settings &ks = settings::get();
  if (eL && !keyLeft.down && !keyLeft.longDone) {
    if (now - keyLeft.at >= 3000) holdPress(ks.holdLeft);
    else shortPress(ks.keyLeft, false);
  }
  if (eR && !keyRight.down && !keyRight.longDone) {
    if (now - keyRight.at >= 3000) holdPress(ks.holdRight);
    else shortPress(ks.keyRight, false);
  }
  if (eM && !keyMid.down && !keyMid.longDone) {
    uint32_t held = now - keyMid.at;
    if (held >= 10000) {
      net::openSetup("Setup network opened with the button.");
      app::redraw(false);
    } else if (held >= 3000) {
      holdPress(ks.holdMid);
    } else {
      shortPress(ks.keyMid, true);
    }
  }
}
#endif

}  // namespace ui

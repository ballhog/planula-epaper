// Coeno mode over MQTT (Coeno's broker, plain LAN MQTT like the Calyx nodes).
//
// Coeno → panel (retained):
//   panel_node/<name>/page/<n>   one page, PageJson.h format (n = 0..5)
//   panel_node/<name>/config     {"pages": 3, "rotate_s": 60}
//   panel_node/<name>/cmd/<what> next | page {n} | refresh | identify | update | check | ack
// Panel → Coeno:
//   panel_node/<name>/hello      retained {online, type, build, ip, ...}; LWT {online:false}
//   panel_node/<name>/state      retained every 60 s and on change: page shown, alarm, rssi, mode
//   panel_node/<name>/ack        {"ids": [...]} when the alarm is acknowledged at the panel
//   panel_node/<name>/describe   retained, on connect: Coeno's device standard (who it is,
//                                what Coeno may do); hello carries describe_rev
//   panel_node/<name>/event      answers to set/do: {"kind", "path", "ok", "why"?}
// Coeno → panel, device standard: set/screen/page {"value": n},
//   do/screen/<next|refresh|identify|ack|check_update>
#pragma once
#include <Arduino.h>

#include "Model.h"

namespace coeno {

void loop();      // starts/stops the client to match the settings
void restart();   // after a host/name change
bool connected();
String status();

int pageCount();  // from config, else highest page received + 1
bool getPage(int i, panel::Page &out, uint32_t &receivedMs);
uint32_t lastPageMs();
uint16_t rotateSeconds();  // 0 = panel setting decides

void publishState(const String &json);
void publishAck(const String &json);

// Commands from Coeno, delivered in loop(): leaf = "next", "page", ...
void onCommand(void (*cb)(const String &leaf, const String &payload));

}  // namespace coeno

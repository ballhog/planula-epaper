// The panel's own settings page (http://<name>.local, or http://192.168.4.1 on
// the setup network). Wi-Fi networks, data mode, Coeno host, Apex and Hydros
// logins, gauges and pages for direct mode, display options, updates.
// Optional password (HTTP basic auth, user "admin") once one is set.
//
// API (JSON):
//   GET  /api/status                 live state for the page
//   GET  /api/settings               settings, passwords as {"set": bool}
//   POST /api/settings               partial update; saves and applies
//   GET  /api/wifi/scan              nearby networks
//   POST /api/test/apex   {host,user,pass}   try a login ("" pass = saved one)
//   POST /api/test/hydros {user,pass}
//   GET  /api/readings               every Apex/Hydros value seen (gauge picker)
//   POST /api/page        {n} | {"next":true}
//   POST /api/refresh     {full}      redraw (full = flashing refresh)
//   POST /api/ack                     acknowledge the alarm
//   POST /api/identify                flash the screen
//   POST /api/update/check | /api/update/install
//   POST /api/reboot | /api/factory-reset {"confirm":"erase"}
//   GET  /screen.bmp                  what the panel shows right now
#pragma once

namespace web {
void begin();
void loop();
}  // namespace web

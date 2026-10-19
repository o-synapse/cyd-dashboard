#pragma once

// Factory defaults. Everything here can be changed later from the web page
// (http://<cyd-ip>/ or http://cyd-dashboard.local/) and is saved on the device.

#define DEFAULT_FAHRENHEIT   false
#define DEFAULT_24H          false
#define DEFAULT_ROTATION     1      // 1 or 3 (landscape, flipped 180°)
#define DEFAULT_BRIGHT_DAY   255    // 10-255
#define DEFAULT_BRIGHT_NIGHT 70
#define DEFAULT_DIM_START    22     // night dimming hours
#define DEFAULT_DIM_END      7
#define DEFAULT_REFRESH_MIN  15

// WiFi setup: on first boot the screen shows a hotspot name. Join it from your
// phone and pick your network. Hold the BOOT button while powering on to
// forget the saved WiFi and run setup again.
#define SETUP_AP_NAME        "CYD-Dashboard"
#define MDNS_NAME            "cyd-dashboard"

// Touch controller (XPT2046) wiring on the ESP32-2432S028
#define TOUCH_CS   33
#define TOUCH_IRQ  36
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK  25

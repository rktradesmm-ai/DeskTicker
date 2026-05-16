#pragma once
#include "settings.h"

// Returns true when valid settings have been saved and the device should restart.
bool wifi_setup_run(Settings* s);

bool wifi_connect(const char* ssid, const char* pass, int timeout_ms = 15000);
bool wifi_is_connected();

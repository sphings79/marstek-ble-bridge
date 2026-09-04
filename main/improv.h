#pragma once

#include "esp_err.h"

/**
 * Improv Serial: WiFi credentials handed over the USB cable that just did the flashing.
 *
 * The web installer already has the port open when the firmware first boots, so it can ask for a
 * network and get an answer back - including the address the bridge ended up at. That turns
 * "flash it, then find it" into one uninterrupted flow, and it is the only reason a published
 * build can ship without credentials baked in.
 *
 * Falling back to the bridge's own access point covers everyone who flashed by other means, so
 * this is a convenience rather than the only route. Specification: improv-wifi.com.
 */
esp_err_t improv_start(void);

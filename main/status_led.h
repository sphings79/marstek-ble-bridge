#pragma once

#include "esp_err.h"

/**
 * What the bridge is doing, expressed in blinks.
 *
 * A bridge lives wherever the storage lives - a cellar, a utility room - with no console attached.
 * When it goes quiet there is otherwise no way to tell "it never started" from "it started but
 * cannot reach the network", and those need completely different fixes. The distinction has to be
 * visible from across the room.
 *
 * No blinking at all is itself the most important signal: it means the firmware never got far
 * enough to drive the pin, which points at power rather than configuration.
 */
typedef enum {
    STATUS_LED_NO_WIFI,      /* fast blink - running, not associated */
    STATUS_LED_ONLINE,       /* short flash every two seconds - running and on the network */
    STATUS_LED_AP,           /* double flash - fallback access point is up */
} status_led_state_t;

esp_err_t status_led_init(void);

void status_led_set(status_led_state_t state);

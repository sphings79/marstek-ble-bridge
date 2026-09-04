#pragma once

#include "esp_err.h"

/**
 * Falls back to the previous firmware when a new one cannot make itself reachable.
 *
 * The bootloader's own rollback would be the proper mechanism, but it lives in the bootloader, and
 * no over-the-air update replaces that - enabling it needs the very cable this whole feature
 * exists to avoid. So the check sits here instead.
 *
 * A flag is written at boot and cleared once the bridge is reachable. Finding it still set on the
 * next boot means the previous attempt never got that far, and the other slot is booted instead.
 * That covers the failure that actually matters when updating unattended: firmware that comes up
 * but cannot be talked to, leaving no way to send a correction.
 */
esp_err_t boot_guard_begin(void);

/** Reachable: keep this firmware. Safe to call more than once. */
void boot_guard_mark_healthy(void);

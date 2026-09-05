#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/** Longest values the WiFi driver accepts, plus room for the terminator. */
#define WIFI_SSID_MAX 33
#define WIFI_PASSWORD_MAX 65

/** Hostnames are a DNS label: at most 63 characters, and we are stricter still. */
#define WIFI_HOSTNAME_MAX 32

typedef struct {
    char ssid[WIFI_SSID_MAX];
    int8_t rssi;
    bool secured;
} wifi_scan_entry_t;

/**
 * Bring up WiFi and start joining a network, if one is known.
 *
 * Credentials come from NVS, and fall back to whatever was compiled in. A bridge that knows
 * nothing skips the station side and opens its own access point straight away, which is how a
 * freshly flashed board is reached at all.
 */
esp_err_t wifi_start(void);

/** Whether the bridge currently holds an IP address. */
bool wifi_is_connected(void);

/** Signal strength of the bridge's own WiFi link, in dBm. Zero when not associated. */
int8_t wifi_rssi(void);

/** Whether the fallback access point is currently up. */
bool wifi_fallback_ap_active(void);

/** Whether any credentials are known at all. */
bool wifi_has_credentials(void);

/** The network being joined, or an empty string. */
const char *wifi_ssid(void);

/** Dotted-quad address of the station interface, or an empty string when not associated. */
const char *wifi_ip(void);

/**
 * Try a set of credentials, and keep them if they work.
 *
 * Blocks until the join succeeds or fails, because both the Improv exchange and the setup page
 * have to tell the caller which it was - "saved, good luck" is exactly the answer that leaves
 * someone holding an unreachable board. On failure the previous credentials are restored.
 */
esp_err_t wifi_provision(const char *ssid, const char *password);

/**
 * The name the bridge answers to, without the .local suffix.
 *
 * Settable because one household can have more than one: a second bridge would otherwise be
 * renamed to marstek-bridge-2 by mDNS collision handling, and which one that is depends on the
 * order they happened to boot in.
 */
const char *wifi_hostname(void);

/** Rename the bridge. Lower-case letters, digits and inner hyphens only. */
esp_err_t wifi_set_hostname(const char *name);

/** Forget the stored credentials. Takes effect on the next boot. */
esp_err_t wifi_forget(void);

/** Scan for networks, strongest first. Returns how many entries were written. */
size_t wifi_scan(wifi_scan_entry_t *out, size_t max);

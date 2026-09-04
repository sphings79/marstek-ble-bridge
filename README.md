# Marstek BLE Bridge

ESP32 firmware that puts a Marstek Venus storage on the network. It relays the battery's Bluetooth
LE link over a WebSocket and serves the [Marstek BLE
Control](https://github.com/sphings79/marstek-ble-control) web app from its own flash, so the
browser no longer has to sit within Bluetooth range of the battery.

> **Unofficial project. Not affiliated with, endorsed by, or supported by Marstek.**

> ⚠️ **Work in progress.** Nothing here has been flashed onto real hardware yet.

## Why

Bluetooth reaches a few metres through a wall on a good day, and storages tend to live in cellars
and utility rooms. Existing solutions for the range problem all end in Home Assistant; this one
ends in a browser, and unlike them it can also carry firmware updates.

## How it fits together

```
Browser  ──http──►  ESP32  ──BLE──►  Venus storage
             │        │
             │        └── serves the web app from its own flash
             └── WebSocket carrying raw device bytes
```

The bridge does **not** understand the Marstek protocol. It moves bytes: a WebSocket message
becomes a GATT write, a BLE notification becomes a WebSocket message. All protocol knowledge stays
in the web app, in one place, in one language.

The app is served by the bridge rather than fetched from the hosted site, because a browser on an
`https://` page may not open a `ws://` connection to a device on the LAN. Same origin, plain HTTP,
no certificate warnings, nothing to configure.

## Status

| | |
|---|---|
| Project skeleton, partition layout | ✅ builds for `esp32` and `esp32s3` |
| Web partition, static file serving, `/api/bridge` | ✅ |
| WiFi | ⚠️ credentials compiled in via `menuconfig` - temporary |
| BLE central and the byte relay | ⬜ |
| Authentication, claim window | ⬜ |
| Improv Serial provisioning, SoftAP fallback | ⬜ |
| Web installer | ⬜ |

## Building

Needs ESP-IDF v5.5.x.

```bash
. ~/esp/esp-idf/export.sh
./tools/sync-web.sh ../marstek-ble-control   # fill the web partition with the app bundle
idf.py set-target esp32                      # or esp32s3
idf.py menuconfig                            # Marstek BLE Bridge -> WiFi SSID / password
idf.py build flash monitor
```

`web/` is not kept in the repository - it holds a build artifact of the app repo. `sync-web.sh`
builds the app with a base path of `/`, which is what the bridge needs; the hosted deployment uses
`/marstek/control/`. Only `index.html` differs between the two, the JS bundle is byte-identical.

## Flash layout

4 MB, one app partition and no OTA slots: two app copies plus the web partition do not fit, and the
bridge is updated through the web flasher anyway. Self-update would need an 8 MB module, not a
rearrangement of the table.

| Partition | Offset | Size |
|---|---|---|
| `nvs` | `0x9000` | 24 KB |
| `phy_init` | `0xf000` | 4 KB |
| `factory` | `0x10000` | 2 MB |
| `web` | `0x210000` | 1.9 MB |

## Security, stated plainly

The bridge serves plain HTTP. It has no certificate and deliberately does not fake one: a
self-signed certificate trains users to click through warnings while providing no protection
against anyone actually on the network, and TLS on an ESP32 costs the heap that a firmware
transfer needs.

Login will be challenge-response, so the password never crosses the network. The session token
afterwards does, and anyone who can read it can control the storage. That is a deliberate,
documented limit - it stops a housemate or a guest network, not an attacker already inside your
LAN.

**Do not expose the bridge to the internet.** If you need access from outside, use a VPN. If your
network is not yours alone, put the bridge on its own VLAN.

## Related

- [marstek-ble-control](https://github.com/sphings79/marstek-ble-control) - the web app this serves
- [marstek-firmware-archiv](https://github.com/sphings79/marstek-firmware-archiv) - firmware archive

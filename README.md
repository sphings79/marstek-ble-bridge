<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/sphings79/marstek-ble-bridge/master/docs/img/hero-dark.svg">
  <img alt="Marstek BLE Bridge - your Venus storage on the network, without the cloud" src="https://raw.githubusercontent.com/sphings79/marstek-ble-bridge/master/docs/img/hero-light.svg" width="880">
</picture>

# Marstek BLE Bridge

**Your Marstek Venus storage, on the network — without the cloud, without an app, without standing next to the battery.**

[![Flash it in your browser](https://img.shields.io/badge/⚡_Flash_it_in_your_browser-1976d2?style=for-the-badge)](https://sphings79.github.io/marstek-ble-bridge/)

[![ESP32](https://img.shields.io/badge/ESP32-WROOM--32%20·%20S3-E7352C?style=flat-square)](#hardware)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5-blue?style=flat-square)](#build-it-yourself)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue?style=flat-square)](LICENSE)
[![No cloud](https://img.shields.io/badge/cloud-not%20required-22C55E?style=flat-square)](#what-leaves-your-network)

**English** · [Deutsch](README.de.md)

</div>

> **Unofficial project. Not affiliated with, endorsed by, or supported by Marstek.**

---

## The problem

Bluetooth reaches a few metres through one wall on a good day. Storages live in cellars and
utility rooms. So every time you want to change a work mode, a charge limit, or a schedule, you
walk down there and stand next to the battery holding a phone.

Every other answer to this ends in Home Assistant. This one ends in a browser — and unlike them,
it also carries firmware updates.

## What it is

An ESP32 you leave near the storage. It holds the Bluetooth connection and relays it over your
network, and it serves [Marstek BLE Control](https://github.com/sphings79/marstek-ble-control) —
the entire web interface — out of its own flash.

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/sphings79/marstek-ble-bridge/master/docs/img/architecture-dark.svg">
  <img alt="A browser reaches the storage through an ESP32: WiFi and a WebSocket to the bridge, Bluetooth from the bridge to the battery" src="https://raw.githubusercontent.com/sphings79/marstek-ble-bridge/master/docs/img/architecture-light.svg" width="880">
</picture>

</div>

Open `http://marstek-bridge.local/` from anywhere in the house. That is the whole thing.

<div align="center">

<img alt="The control panel reached through the bridge, on a phone: state of charge, battery and grid power, and the bridge's own WiFi signal beside the Bluetooth one" src="https://raw.githubusercontent.com/sphings79/marstek-ble-bridge/master/docs/img/dashboard-mobile.png" height="620">

</div>

<details>
<summary>The whole panel on a desktop, including the bridge's own cards</summary>

<div align="center">

<img alt="The full control panel served by the bridge, ending in the Bridge Firmware and Bridge Password cards" src="https://raw.githubusercontent.com/sphings79/marstek-ble-bridge/master/docs/img/dashboard-desktop.png" width="100%">

</div>

</details>


## Why it serves the interface itself

A browser on an `https://` page is not allowed to open a `ws://` connection to a device on your
LAN. Serving the interface from the bridge makes it all same-origin over plain HTTP: no
certificate warnings, no mixed content, nothing to configure.

The bridge does **not** understand the Marstek protocol. It moves bytes — a WebSocket message
becomes a Bluetooth write, a notification becomes a WebSocket message. Every reverse-engineered
detail stays in the web app, where it is already maintained, in one place, in one language.

## Getting started

1. **Flash it** from [the web installer](https://sphings79.github.io/marstek-ble-bridge/) — Chrome or Edge,
   nothing to install. It asks for your WiFi over the same cable it just flashed through, then
   hands you the address the bridge ended up at.
2. **Open that address** and set a password. Only possible in the first ten minutes after boot, so
   a bridge left powered on and unclaimed does not stay open indefinitely.
3. **Pick your storage** from the scan and connect.

Flashed some other way, or no serial provisioning? The bridge opens its own access point,
`Marstek-Bridge-XXXX`, and its setup page takes the network from there.

The status LED says where it is without a console:

| Blink | Meaning |
|---|---|
| Short flicker at power-on | Firmware started |
| Flash every two seconds | Online |
| Fast blink | Running, no WiFi |
| Double flash | Its own access point is up — connect and set it up |
| Nothing at all | Never started — check the supply |

## Hardware

Any ESP32 with WiFi, Bluetooth LE and 4 MB of flash:

| | |
|---|---|
| **ESP32-WROOM-32** | The usual NodeMCU / DevKit boards. Recommended. |
| **ESP32-S3** | Also supported. |

A board with a USB port flashes straight from the browser. ESP32-CAM boards work — this was
developed on one — but they are the awkward choice: small antenna, and a 5 V rail that struggles
the moment the radio starts. Which is why transmit power ships capped low; if your supply is
solid, raise `BRIDGE_WIFI_MAX_TX_POWER` and get the range back.

The interface it serves is in **English and German**, following the browser and switchable at any
time.

## Updates

The bridge updates itself over WiFi. New releases show up in the interface with a link to the
changelog, and it downloads and installs them on its own.

Firmware goes into the slot it is not running from and is only booted once it is complete, so a
failed update changes nothing. A firmware that boots but cannot be reached falls back to the
previous one by itself — which is what makes updating a board in a cellar reasonable at all.

## What leaves your network

Nothing. The bridge talks to your storage and to your browser. It reaches the internet for exactly
one thing: checking for and downloading its own updates from GitHub, when you ask it to.

## Security, stated plainly

The bridge serves plain HTTP. It has no certificate and deliberately does not fake one — a
self-signed certificate teaches people to click through warnings while protecting nothing, and TLS
costs the memory a firmware transfer needs.

Login is challenge-response, so **the password itself never crosses your network**. The session
cookie afterwards does, and whoever can read it can control the storage. That is a documented
limit, not an oversight: it stops a guest network or a housemate, not somebody already inside your
LAN.

**Do not expose the bridge to the internet.** Use a VPN for access from outside, and give it its
own VLAN if your network is not only yours.

## Build it yourself

Needs ESP-IDF v5.5.x.

```bash
. ~/esp/esp-idf/export.sh
./tools/sync-web.sh ../marstek-ble-control   # fetch and pack the web interface
idf.py set-target esp32                      # or esp32s3
idf.py menuconfig                            # Marstek BLE Bridge → LED pin, claim window, TX power
idf.py build flash monitor
```

`web/` holds a build artifact of the app repository and is not kept here. `sync-web.sh` builds the
app with a base path of `/`, drops the Web Bluetooth help image a bridge can never need, and gzips
the rest — about 200 KB instead of a megabyte.

Leave the WiFi fields in `menuconfig` empty unless you are flashing by cable and would rather not
provision afterwards. Anything set there is written into NVS on the first successful join, so an
update to a normal build keeps the network.

## Flash layout

4 MB: two app slots so it can update itself, and a web partition beside them.

| Partition | Offset | Size |
|---|---|---|
| `nvs` | `0x9000` | 24 KB |
| `otadata` | `0xf000` | 8 KB |
| `phy_init` | `0x11000` | 4 KB |
| `ota_0` | `0x20000` | 1.5 MB |
| `ota_1` | `0x1A0000` | 1.5 MB |
| `web` | `0x320000` | 896 KB |

## ⭐ Found this useful?

If it got your storage onto the network without a cloud account, a **star** is genuinely
appreciated — it is how other Venus owners find this at all.

## Sponsor this project

These tools are built and maintained in my spare time, and they stay free, open and cloud-free.
If one of them saved you an afternoon, you can buy me a coffee.

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-sphings-FFDD00?style=for-the-badge&logo=buymeacoffee&logoColor=000000)](https://buymeacoffee.com/sphings)

## Related

- [marstek-ble-control](https://github.com/sphings79/marstek-ble-control) — the web interface this serves
- [More tools](https://sphings-dev.de/)

## License

Apache License 2.0 — see [LICENSE](LICENSE).

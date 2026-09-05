<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/sphings79/marstek-ble-bridge/master/docs/img/hero-dark.svg">
  <img alt="Marstek BLE Bridge - dein Venus-Speicher im Netzwerk, ohne Cloud" src="https://raw.githubusercontent.com/sphings79/marstek-ble-bridge/master/docs/img/hero-light.svg" width="880">
</picture>

# Marstek BLE Bridge

**Dein Marstek-Venus-Speicher im Netzwerk — ohne Cloud, ohne App, ohne neben der Batterie zu stehen.**

[![Im Browser flashen](https://img.shields.io/badge/⚡_Im_Browser_flashen-1976d2?style=for-the-badge)](https://sphings79.github.io/marstek-ble-bridge/)

[![ESP32](https://img.shields.io/badge/ESP32-WROOM--32%20·%20S3-E7352C?style=flat-square)](#hardware)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5-blue?style=flat-square)](#selbst-bauen)
[![Lizenz](https://img.shields.io/badge/Lizenz-Apache--2.0-blue?style=flat-square)](LICENSE)
[![Ohne Cloud](https://img.shields.io/badge/Cloud-nicht%20nötig-22C55E?style=flat-square)](#was-das-netzwerk-verlässt)

[English](README.md) · **Deutsch**

</div>

> **Inoffizielles Projekt. Nicht mit Marstek verbunden, nicht von Marstek unterstützt.**

---

## Das Problem

Bluetooth reicht an einem guten Tag ein paar Meter durch eine Wand. Speicher stehen im Keller oder
im Hauswirtschaftsraum. Also läuft man jedes Mal runter und steht mit dem Handy vor der Batterie,
nur um einen Arbeitsmodus, ein Ladelimit oder einen Zeitplan zu ändern.

Jede andere Lösung dafür endet in Home Assistant. Diese endet im Browser — und kann im Gegensatz
dazu auch Firmware-Updates transportieren.

## Was es ist

Ein ESP32, den du beim Speicher lässt. Er hält die Bluetooth-Verbindung, reicht sie über dein
Netzwerk weiter und liefert [Marstek BLE Control](https://github.com/sphings79/marstek-ble-control)
— die komplette Weboberfläche — aus seinem eigenen Flash aus.

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/sphings79/marstek-ble-bridge/master/docs/img/architecture-dark.svg">
  <img alt="Ein Browser erreicht den Speicher über einen ESP32: WLAN und ein WebSocket zur Bridge, Bluetooth von der Bridge zur Batterie" src="https://raw.githubusercontent.com/sphings79/marstek-ble-bridge/master/docs/img/architecture-light.svg" width="880">
</picture>

</div>

`http://marstek-bridge.local/` von überall im Haus öffnen. Das ist alles.

## Warum er die Oberfläche selbst ausliefert

Ein Browser auf einer `https://`-Seite darf keine `ws://`-Verbindung zu einem Gerät im LAN
aufbauen. Wenn die Bridge die Oberfläche selbst ausliefert, ist alles same-origin über einfaches
HTTP: keine Zertifikatswarnung, kein Mixed Content, nichts zu konfigurieren.

Die Bridge **versteht das Marstek-Protokoll nicht**. Sie schiebt Bytes — aus einer
WebSocket-Nachricht wird ein Bluetooth-Write, aus einer Notification eine WebSocket-Nachricht.
Alles Reverse-Engineerte bleibt in der Web-App, wo es ohnehin gepflegt wird: an einer Stelle, in
einer Sprache.

## Loslegen

1. **Flashen** über den [Web-Installer](https://sphings79.github.io/marstek-ble-bridge/) — Chrome oder Edge,
   nichts zu installieren. Er fragt das WLAN über dasselbe Kabel ab, über das er gerade geflasht
   hat, und nennt dir danach die Adresse, unter der die Bridge gelandet ist.
2. **Adresse öffnen** und ein Passwort setzen. Das geht nur in den ersten zehn Minuten nach dem
   Start, damit eine laufende, nicht beanspruchte Bridge nicht dauerhaft offensteht.
3. **Speicher auswählen** und verbinden.

Anders geflasht oder keine serielle Provisionierung? Dann macht die Bridge einen eigenen Accesspoint
auf, `Marstek-Bridge-XXXX`, und die Setup-Seite dort nimmt das Netzwerk entgegen.

Die Status-LED sagt ohne Konsole, wo sie steht:

| Blinken | Bedeutung |
|---|---|
| Kurzes Flackern beim Einschalten | Firmware läuft los |
| Blitz alle zwei Sekunden | Online |
| Schnelles Blinken | Läuft, aber kein WLAN |
| Doppelblitz | Eigener Accesspoint ist offen — verbinden und einrichten |
| Gar nichts | Nie gestartet — Netzteil prüfen |

## Hardware

Jeder ESP32 mit WLAN, Bluetooth LE und 4 MB Flash:

| | |
|---|---|
| **ESP32-WROOM-32** | Die üblichen NodeMCU-/DevKit-Boards. Empfohlen. |
| **ESP32-S3** | Läuft ebenfalls. |

Ein Board mit USB-Anschluss lässt sich direkt aus dem Browser flashen. ESP32-CAM-Boards gehen —
entwickelt wurde auf einem — sind aber die unbequeme Wahl: kleine Antenne, und eine 5-V-Versorgung,
die einbricht, sobald das Funkmodul anläuft. Deshalb ist die Sendeleistung ab Werk niedrig
gedeckelt; bei solidem Netzteil `BRIDGE_WIFI_MAX_TX_POWER` hochsetzen und die Reichweite
zurückholen.

## Updates

Die Bridge aktualisiert sich selbst über WLAN. Neue Releases erscheinen in der Oberfläche mit Link
zum Changelog, und sie lädt und installiert sie eigenständig.

Firmware landet in dem Slot, aus dem sie gerade nicht läuft, und wird erst gestartet, wenn sie
vollständig ist — ein abgebrochenes Update ändert also nichts. Eine Firmware, die zwar startet,
aber nicht erreichbar wird, fällt von selbst auf die vorherige zurück. Genau das macht das
Aktualisieren eines Boards im Keller überhaupt vertretbar.

## Was das Netzwerk verlässt

Nichts. Die Bridge redet mit deinem Speicher und mit deinem Browser. Ins Internet geht sie für
genau eine Sache: ihre eigenen Updates bei GitHub prüfen und holen, wenn du es anstößt.

## Sicherheit, ungeschönt

Die Bridge liefert einfaches HTTP aus. Sie hat kein Zertifikat und tut bewusst auch nicht so — ein
selbstsigniertes Zertifikat erzieht nur dazu, Warnungen wegzuklicken, und schützt dabei nichts.
Außerdem kostet TLS genau den Speicher, den eine Firmware-Übertragung braucht.

Der Login läuft als Challenge-Response, **das Passwort geht also nie über das Netz**. Der
Session-Cookie danach schon, und wer ihn mitlesen kann, kann den Speicher steuern. Das ist eine
dokumentierte Grenze, kein Versehen: Es hält ein Gastnetz oder einen Mitbewohner ab, nicht jemanden,
der schon in deinem LAN sitzt.

**Die Bridge gehört nicht ins offene Internet.** Für Zugriff von außen ein VPN nehmen, und ein
eigenes VLAN, wenn das Netzwerk nicht nur dir gehört.

## Selbst bauen

Braucht ESP-IDF v5.5.x.

```bash
. ~/esp/esp-idf/export.sh
./tools/sync-web.sh ../marstek-ble-control   # Weboberfläche bauen und packen
idf.py set-target esp32                      # oder esp32s3
idf.py menuconfig                            # Marstek BLE Bridge → LED-Pin, Claim-Fenster, Sendeleistung
idf.py build flash monitor
```

`web/` enthält ein Build-Artefakt des App-Repos und liegt nicht hier. `sync-web.sh` baut die App mit
Basispfad `/`, wirft das Web-Bluetooth-Hilfebild raus, das eine Bridge nie braucht, und gzippt den
Rest — rund 200 KB statt einem Megabyte.

Die WLAN-Felder in `menuconfig` leer lassen, außer du flashst per Kabel und willst dir das
Einrichten danach sparen. Was dort steht, wird beim ersten erfolgreichen Verbinden ins NVS
geschrieben — ein Update auf einen normalen Build behält das Netzwerk also.

## Flash-Aufteilung

4 MB: zwei App-Slots, damit sie sich selbst aktualisieren kann, und daneben die Web-Partition.

| Partition | Offset | Größe |
|---|---|---|
| `nvs` | `0x9000` | 24 KB |
| `otadata` | `0xf000` | 8 KB |
| `phy_init` | `0x11000` | 4 KB |
| `ota_0` | `0x20000` | 1,5 MB |
| `ota_1` | `0x1A0000` | 1,5 MB |
| `web` | `0x320000` | 896 KB |

## ⭐ Nützlich gefunden?

Wenn dein Speicher damit ohne Cloud-Konto ins Netzwerk gekommen ist: Ein **Stern** freut mich
ehrlich — so finden andere Venus-Besitzer das hier überhaupt.

## Projekt unterstützen

Diese Tools entstehen und wachsen in meiner Freizeit, und sie bleiben kostenlos, offen und
cloudfrei. Wenn dir eines davon einen Nachmittag gespart hat, kannst du mir einen Kaffee ausgeben.

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-sphings-FFDD00?style=for-the-badge&logo=buymeacoffee&logoColor=000000)](https://buymeacoffee.com/sphings)

## Verwandt

- [marstek-ble-control](https://github.com/sphings79/marstek-ble-control) — die Oberfläche, die hier ausgeliefert wird
- [Weitere Tools](https://sphings-dev.de/)

## Lizenz

Apache License 2.0 — siehe [LICENSE](LICENSE).

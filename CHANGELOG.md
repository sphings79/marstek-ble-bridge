# Changelog

All notable changes to the Marstek BLE Bridge. The interface links here from its update card;
each release also carries its full notes on the
[GitHub releases page](https://github.com/sphings79/marstek-ble-bridge/releases).

Versions apply to the bridge firmware and the web interface it serves, which are released together.

## v1.3.14 — 2026-10-01
- New "Pick from archive" button on the firmware update card: lists the images for the connected model
  (Control, BMS, Micro) straight from the firmware archive on GitHub, newest first with release notes,
  downloads the chosen one, verifies its SHA-256 and runs it through the same checks as a file you pick
  yourself. Venus A, D and E 3.0.
- "Schaltet die Notstromsteckdose an/aus" wording fix.
- Internal cleanup: the connection provider no longer reads a ref while rendering, and a few lint
  findings are gone. No behaviour change intended.
  (Web interface only; the firmware is unchanged from v1.3.9.)

## v1.3.13 — 2026-10-01
- Plainer wording throughout: "SoC" instead of "State of Charge/Ladezustand", clearer descriptions for the
  backup power, surplus feed-in, LED and Bluetooth switches, and the BLE command numbers are gone from the
  notes. The legal-regulations hint is now a single sentence.
- The device-info versions name every component (EMS, BMS, VNS, MPPT).
- The device time card explains where the battery gets its time (cloud, or the Marstek Offline Endpoint)
  and links to it.
- The bridge-connected firmware-update warning and the bridge-password explainer are removed; the
  bridge firmware card shows firmware and interface versions as chips and tucks hand-built uploads
  into a collapsible "Manual update".
- Footer and phone menu: a thank-you to Hypfer, links to the Modbus Suite and the Offline Endpoint,
  and on a phone the project links sit behind a collapsible "More projects" entry.
  (Web interface only; the firmware is unchanged from v1.3.9.)

## v1.3.12 — 2026-10-01
- The Bluetooth reading in the header doubles as the link indicator: dBm while connected, "connecting…"
  in orange while the link comes up, "lost" in red when it is gone. On a phone it sits in the pinned
  section bar, so a dropped connection is visible even after scrolling down.
  (Web interface only; the firmware is unchanged from v1.3.9.)

## v1.3.11 — 2026-10-01
- The header shows the Bluetooth symbol instead of the cellular bars, and Bluetooth and WiFi are just
  the dBm value; the tooltip says which link it is.
- On a phone the device header scrolls away and only the section bar stays pinned. It now carries the
  Bluetooth and WiFi signal readings, so they remain visible.
  (Web interface only; the firmware is unchanged from v1.3.9.)

## v1.3.10 — 2026-10-01
- Firmware update: the technical notes about the Micro/Inverter component are gone, and starting an
  update always asks one short "really start?" question. After a finished update the loaded file is
  cleared so the start button cannot be pressed twice, and the log follows the newest line.
- Power limits are now a "Power" card with 0 to max sliders (50 W steps) for charge and discharge;
  the device power class keeps its buttons.
- The schedule hint explains where the battery gets its time (cloud, or the Marstek Offline Endpoint)
  and links to it.
- On a phone the section bar with the menu button stays visible below the header while scrolling.
  (Web interface only; the firmware is unchanged from v1.3.9.)

## v1.3.9 — 2026-09-14
- The fallback setup access point `Marstek-Bridge-XXXX` is now an open network instead of WPA2 with
  a fixed, undocumented password, so provisioning over it is no longer a dead end. The setup page,
  and the bridge password set from it, remain the real gate. (Firmware only; the web interface is
  unchanged from v1.3.8.)

## v1.3.8 — 2026-09-13
- EMS, BMS and Micro-Inverter (VNS) updates are hardware-confirmed on a Venus D and a Venus E 3.0,
  so the VNS firmware-update note is a plain info line instead of a "not confirmed on real hardware"
  warning. MPPT keeps the caution, only because no MPPT image is in the firmware archive to flash yet.

## v1.3.7 — 2026-09-13
- The offered update shows a real progress bar: the bridge streams its download-and-flash progress
  as it runs, so the bar fills with a percentage instead of an indeterminate one.
- After a web-interface update the page reloads itself, so the freshly installed interface takes
  over instead of the old one lingering in the tab.

## v1.3.6 — 2026-09-12
- Settings hold the value you applied until the next poll lands, instead of flashing the device's
  old value for a moment (self-consumption offset and local-API fields).
- The self-consumption offset now notes that it is capped by the power limits — a positive value by
  the charge limit, a negative one by the discharge limit — so a positive offset lands on 0 when the
  charge limit is 0.

## v1.3.5 — 2026-09-12
- The self-consumption offset is read back and shown, instead of resetting to 0 on every reload.
- The install progress shows an indeterminate "installing…" while the bridge fetches an offered
  update itself, rather than sitting at 0 %. Hand-uploaded files keep their real percentage bar.

## v1.3.4 — 2026-09-12
- Each model's device power class offers the values its firmware actually accepts: Venus A
  800/1200/1500 W, Venus D 800/2200/2500 W, Venus E 3.0 600/800/2500 W (Venus A previously showed
  2200/2500 W, which it ignores, and hid its real 1200/1500 W).
- The Venus E 3.0 gains its 600 W class (for countries capped at 600 W, with a note) and drops the
  free discharge-limit control, which its firmware governs through the power class anyway.

## v1.3.3 — 2026-09-12
- Venus E 3.0 (VNSE3) support. It answers the status poll with a shorter frame that used to crash
  the parser and leave the whole panel spinning; its own layout is now read — state of charge,
  battery and grid power, charge/discharge limits, CT, work mode, depth of discharge, and the LED
  and Bluetooth switches. The local-API card shows its real on/off and port too.

## v1.3.2 — 2026-09-06
- Module States works again with seven battery packs: it reads as many module entries as the frame
  actually holds, instead of trusting a count that overruns the buffer.

## v1.3.1 — 2026-09-05
- Retries a reconnect the storage refuses.

## v1.3.0 — 2026-09-05
- Survives a page reload without losing the stored bridge, and says so when a connect fails.

## v1.2.0 — 2026-09-05
- The interface speaks German too.

## v1.1.0 — 2026-09-05
- The bridge is renameable, and its password can be changed.

## v1.0.0 — 2026-09-04
- First release.

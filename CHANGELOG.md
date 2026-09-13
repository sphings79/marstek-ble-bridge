# Changelog

All notable changes to the Marstek BLE Bridge. The interface links here from its update card;
each release also carries its full notes on the
[GitHub releases page](https://github.com/sphings79/marstek-ble-bridge/releases).

Versions apply to the bridge firmware and the web interface it serves, which are released together.

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

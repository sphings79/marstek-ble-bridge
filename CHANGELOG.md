# Changelog

All notable changes to the Marstek BLE Bridge. The interface links here from its update card;
each release also carries its full notes on the
[GitHub releases page](https://github.com/sphings79/marstek-ble-bridge/releases).

Versions apply to the bridge firmware and the web interface it serves, which are released together.

## v1.4.0-beta.31 — 2026-11-21
**Beta.** Bridge and web interface only work together. Flash firmware and web image.
- **More settings over REST.** Besides peak shaving, the same API token now sets the maximum discharge
  and charge power, the power class, the depth of discharge, the self-consumption offset, backup power,
  Bluetooth, the LED and the work mode: `/api/discharge-limit`, `/api/charge-limit`, `/api/power-class`,
  `/api/depth-of-discharge`, `/api/power-offset`, `/api/backup-power`, `/api/bluetooth`, `/api/led`,
  `/api/work-mode`. `POST` sends, `GET` shows what was sent last (the storage does not report these back).
  See the README. Untested on real storages so far - please report what a storage does with 0 W.
- The release also carries complete 4 MB images (`marstek-ble-bridge-esp32-full.bin`,
  `marstek-ble-bridge-esp32s3-full.bin`) for flashing with esptool at 0x0.

## v1.4.0-beta.30 — 2026-11-20
**Beta.** Bridge and web interface only work together. Flash firmware and web image.
- Merges the stable line up to v1.3.25 into the beta.
- **Fix: a battery module below 0 °C no longer shows about 6552 °C** (web interface).
- **From v1.3.22 / v1.3.23:** the ESP32-S3 setup access point is open again (it still asked for the old
  password), and the Improv command numbers are fixed, so the web installer offers the WiFi setup after
  flashing. The bridge also listens on the ESP32-S3's own USB port for the WiFi setup.

## v1.4.0-beta.29 — 2026-11-16
**Beta.** Bridge and web interface only work together. Flash firmware and web image.
- **The beta switch is back in the update card.** The version drop-down lists stable releases only until
  "Include beta versions" is turned on, so nobody ends up on a beta by accident. Turning it on asks
  first and says plainly that betas are for testing, can contain bugs and may not be easy to leave again;
  while it is on, a warning stays visible. A bridge that already runs a beta has it on.
- The firmware is unchanged from beta.28.

## v1.4.0-beta.28 — 2026-11-16
**Beta.** Bridge and web interface only work together. Flash firmware and web image.
- **Choose the version in the update card.** A drop-down lists every release, newest first, betas marked,
  instead of the switch for betas: pick one and install it - also an older one, to go back. The newest
  stable release is preselected, and the newest of all while a beta is running. Going between beta and
  stable still shows its warning.
- **ESP32-S3.** The firmware reports its chip and the card installs the image built for it; this beta
  carries `marstek-ble-bridge-esp32s3.bin` next to the ESP32 image. (Same as v1.3.21 on the stable line.)
- Merged with v1.3.20 and v1.3.21 (the Venus D connection fix, which this line already had).

## v1.4.0-beta.27 — 2026-11-16
**Beta.** Bridge and web interface only work together. Flash firmware and web image.
- **Cleanup after the Venus D fix.** The Venus D stays connected: the cause was the missing GATT server
  (see beta.26), which stays. Everything that was built to hunt for it is gone: `/api/experiment` with
  all its switches (random address, MTU 512, replay of the vendor app's session, 6-byte polling, small
  data length), the planned reconnect, the silence check with its reconnect, the read of the module's
  third characteristic and the log entries that went with them.
- Detection of a lost connection is unchanged: a link that ends is reported at once, a kept link that
  stops answering is cycled by the keepalive, and a link nobody used for 45 s is checked before it is
  trusted again. `/api/log`, `/api/diag` and `/api/status` stay for testing from Home Assistant.

## v1.4.0-beta.26 — 2026-11-16
**Beta.** Bridge and web interface only work together. Flash firmware and web image.
- **Likely cause of the Venus D going silent after ~28 s found and fixed.** The Venus D sends an ATT
  "Exchange MTU" request of its own right after connecting (the capture of the vendor app shows it ~20 ms
  after link-up, answered by the phone within a millisecond). The bridge was built as a pure Bluetooth
  central without the GATT server, and NimBLE only handles that request when the server is compiled in:
  without it the request is dropped unanswered. A Bluetooth ATT request that is not answered within 30 s
  times out, and from then on the link carries no more ATT traffic - the device stops answering and
  stops notifying while the link itself stays up. That is the 26-29 s seen on every central tried
  (ESP32, ESP32-S3, NimBLE and Bluedroid; the test builds had the same missing piece). The firmware now
  builds with the peripheral role and the GATT server (no service is registered, nothing advertises); the
  firmware is about 16 KB larger.
- The planned reconnect of beta.25 is off by default: it is the workaround this fix should make
  unnecessary. `/api/experiment?set=128` turns it back on (values up to 255 are accepted now).
  The reconnect after 3 s without an answer stays as a backup. If `/api/log` shows no
  `mute-recovery step 0` for several minutes, the fix works.
- The write characteristic (0xFF01) is no longer subscribed for a Venus D recognised by the name from
  the binding (the vendor app only subscribes to 0xFF02). The log no longer reads 0xFF06 one second
  after connecting. Log lines of the mute recovery are no longer cut off.

## v1.4.0-beta.25 — 2026-11-16
**Beta.** Bridge and web interface only work together.
- **Venus D: the link is replaced before it falls silent.** The log of beta.24 shows the Venus D going
  mute 26-29 s after each connection. The bridge now ends the link itself after 18 s at a quiet moment
  (nothing waiting for an answer for a second), and after 23 s whatever is going on, and connects
  again 150 ms later, without the usual 5 s wait and without telling the browser that the link ended.
  The log shows `mute-recovery step 8 (planned reconnect)`. The reconnect after a detected silence
  (3 s without an answer) stays as a backup.
- A firmware transfer is left alone: neither the planned reconnect nor the silence check touches a
  link the browser has written to without waiting for replies in the last 30 s (beta.24 could have
  cut such a transfer short).
- The Venus D is recognised by the name from the binding or the last scan; the name the device reports
  itself (not the advertised one) and the third characteristic only count when no other name is known.

## v1.4.0-beta.24 — 2026-11-16
**Beta.** Bridge and web interface only work together.
- **Venus D: a new connection as soon as it falls silent.** The log of beta.23 shows what the mute is:
  the Venus D stops answering about 26-29 s after connecting, and none of the wake-up steps helps
  (descriptor written again, notifications off and on, connection update). A read of the module's third
  characteristic 0xFF06 is still answered with "not permitted" (status 258) during the mute, so the
  module's Bluetooth stack is alive and the data path behind it is dead. A new connection always
  works at once. So the ladder is gone: a request unanswered for 3 s now drops the link, and the usual
  reconnect brings it back within a few seconds. The log shows `mute-recovery step 6 (reconnect)`.
- The data path through REST works the same way: it connects on demand and is not affected.

## v1.4.0-beta.23 — 2026-11-16
**Beta.** Diagnostic build; bridge and web interface only work together.
- **Fix: the mute recovery of beta.22 never started.** It looked for "VNSD" in the name the device
  reports over Bluetooth, which is not the advertised name, so no Venus D was recognised (the log of
  beta.22 shows no `ff06 read` and no `mute-recovery` line). A Venus D is now recognised by any of: the
  name from the binding, the name the device reports, the name from the last scan, or the presence of
  the module's third characteristic 0xFF06. A `mute-recovery` line "step 7 (supervision active)" is
  logged when a link is being watched.

## v1.4.0-beta.22 — 2026-11-16
**Beta.** Diagnostic build; bridge and web interface only work together. For the Venus D, no switch to set.
- **Mute recovery.** The Venus D stops answering about 28 s after connecting while the link stays up.
  Neither the capture of the vendor app nor its firmware explains it, so the bridge now reacts to it: a
  request that stays unanswered for 4 s starts a ladder, one step every 3 s, each followed by the app's
  own state request as a probe: (1) the probe alone, (2) the notification descriptor written again,
  (3) notifications switched off and on, (4) a connection parameter update, (5) the third characteristic
  read, (6) a new connection. The log (`/api/log`) shows `mute-recovery` lines: the step that is
  followed by "ANSWERED AFTER THIS" is what wakes the device. If none does, the link is dropped and the
  usual reconnect takes over.
- **Third characteristic 0xFF06.** The module's firmware has a characteristic besides 0xFF01/0xFF02 that
  nothing here has touched. It is found on connect, its properties are logged (`ff06 found`) and its
  value is read one second after the bridge is ready and again when the mute is detected (`ff06 read`).

## v1.4.0-beta.21 — 2026-11-16
**Beta.** Diagnostic build; bridge and web interface only work together.
- `/api/experiment` bit 64 (`set=64`): keeps the Bluetooth data length at the default 27 bytes. A capture
  of the vendor app shows that its connection never extends the data length (no Data Length Change
  event, no command for it), while this bridge's controller negotiates up to 251 bytes by itself right
  after connecting (gap event 34 in the log) - the one difference to the app's link that had not been
  tried. `connect-start` now also shows the suggested data length; `gap-event type 34` shows what was
  agreed, as tx octets * 256 + rx octets. Values go up to 127.
- The experiment bit 32 of beta.19/20 (read remote features/version through an internal NimBLE call) is
  gone: it made the host handle the connect event over and over.

## v1.4.0-beta.20 — 2026-11-16
**Beta.** Diagnostic build; bridge and web interface only work together.
- **Fix for `set=32` of beta.19**, which did not work: the two link-layer requests (Read Remote
  Features, Read Remote Version) were sent from inside the connect event, and the call waits for the
  controller's answer, which made the host handle that event over and over until the link was dropped.
  They are now sent from a timer 70 ms and 780 ms after connecting, as in the capture. The earlier result
  for `set=32` is therefore not valid; test it again.

## v1.4.0-beta.19 — 2026-11-16
**Beta.** Diagnostic build; bridge and web interface only work together.
- `/api/experiment` bit 32 (`set=32`): right after connecting, the bridge sends the two link-layer
  requests the vendor app's phone sends according to the capture: LE Read Remote Features and Read Remote
  Version Information. Values go up to 63.
- What the last runs showed: with `set=8`, `15` and `16` (the vendor app's whole session replayed, with the
  web app's commands dropped) the Venus D still stopped answering 28.8-28.9 s after connecting, to the
  tenth of a second. So it is not the commands; something in the connection itself differs from the
  vendor app's.

## v1.4.0-beta.18 — 2026-11-16
**Beta.** Diagnostic build; bridge and web interface only work together.
- `/api/experiment` bit 16 (`set=16`): the bridge replays the vendor app's whole session from a Bluetooth
  capture of it - every command, in the same order and with the same timing, then the state and the
  setting every 3 s - and drops the commands of the web app, so that only the app's traffic reaches the
  storage. The capture shows what the app really sends: `0x03` and `0x0a` as 6-byte frames with payload
  `01`, no `0x04`, and `0x42` only once. If the Venus D keeps answering this way, the cause is what this
  bridge and the web app send; if it still stops after about 28 s, it is the link. Values go up to 31.

## v1.4.0-beta.17 — 2026-11-16
**Beta.** Diagnostic build; bridge and web interface only work together.
- `/api/experiment` has a new bit, 8: the bridge sends the vendor app's steady polling itself, `0x03` and
  `0x0a` as 6-byte frames with the payload byte `01`, one every 3 s. A capture of the vendor app shows
  it uses only that form after its first seconds, while this bridge and the web app ask with a 5-byte
  frame without payload, and a Venus D stops answering about 28 s after connecting. `set=1`, `2` and `4`
  (random address, MTU 512, the app's first reads) were tried and did not help. Values go up to 15.

## v1.4.0-beta.16 — 2026-11-16
**Beta.** Diagnostic build; bridge and web interface only work together.
- **`/api/experiment` (needs a login):** a debug switch for the Venus D that stops answering about 28 s
  after connecting. `/api/experiment?set=N` applies to the next connection and survives a restart; `set=0`
  is the normal behaviour. Bits: 1 = random own Bluetooth address (as the vendor app's phone uses),
  2 = ATT MTU 512 (as the Venus D requests itself), 4 = the three read-only requests the vendor app
  sends in its first seconds (0x11, 0x24, 0x51). `/api/log` shows the value on every `connect-start`.

## v1.4.0-beta.15 — 2026-11-16
**Beta.** Bridge and web interface only work together. A test: it is not yet known whether it helps.
- **Connects the way the vendor app does.** A Bluetooth capture of the vendor app shows a Venus D
  answering for minutes, while with this bridge it stops answering about 28 s after connecting (also
  with firmware 1.3.19). The bridge now asks for the same connection parameters (30-50 ms interval,
  5 s supervision timeout instead of NimBLE's 2.56 s), briefly requests a 7.5 ms interval right after
  connecting and about a second later 48.75 ms, as the app does, and no longer sends its own MTU
  request at once: the Venus D sends one itself, and the bridge only asks when nothing was agreed by
  the time the service has been found.

## v1.4.0-beta.14 — 2026-11-15
**Beta.** Bridge and web interface only work together. A test for the storages that stop answering.
- **One command at a time per storage.** The event log showed that a storage drops a command that
  arrives while it is still busy with the previous one (two commands 5 ms apart: only the first was
  answered). The web app already pauses between writes, but over WiFi two of them can reach the bridge
  in the same instant. Ordinary commands now wait in a short queue per slot: the next one goes out
  when the previous one has been answered (or after 2 s) plus 100 ms, as other integrations do.
  Firmware transfers are not affected. REST commands use the same queue.

## v1.4.0-beta.13 — 2026-11-15
**Beta.** Bridge and web interface only work together. A targeted test, not yet confirmed to fix anything.
- **Venus D: the bridge no longer subscribes to the write characteristic's notifications.** The event
  log showed that a Venus D stops answering about 25 s after connecting, also with a single storage,
  and the only difference to a browser's Web Bluetooth connection (which subscribes to the notify
  characteristic only) is that extra subscription. It is skipped for storages named `MST_VNSD...`;
  all other models are handled as before. `/api/log` shows `write (skipped)`.
- The bridge keeps the device name across a disconnect (it is needed before the device is asked).

## v1.4.0-beta.12 — 2026-11-15
**Beta.** Diagnostic build; bridge and web interface only work together.
- `/api/log` records more of what happens on a link: the connection parameters at link-up and
  whenever they change, every Bluetooth event the bridge does not handle itself (by its number), the
  result of both notification subscriptions, and the handle each reply arrives on.
  Meant to find out why a Venus D stops answering about 30 s after it connects.

## v1.4.0-beta.11 — 2026-11-15
**Beta.** Web interface only; the firmware is unchanged from v1.4.0-beta.10.
- **Fix: the update card did not offer the newest beta.** GitHub lists releases by tag name as text,
  not by date, so `v1.4.0-beta.9` came before `v1.4.0-beta.10` and the card offered the older one.
  The card now offers the most recently published release. A bridge that runs an older web interface
  cannot see this release in the card: install it once by hand (upload the two images from the release
  page), after that the card finds new betas again.

## v1.4.0-beta.10 — 2026-11-15
**Beta.** Bridge and web interface only work together.
- **Fix: the storages stopped answering 30 s after connecting.** The event log pinned it down: every
  connection went silent at exactly the moment its pairing timed out. The bridge started a pairing on
  every new link, the Venus D never answers it, and when the pairing gives up the storage stops
  answering on that link. The bridge no longer starts a pairing. (The 4 s keepalive of beta.9 treated
  a symptom and is gone.)
- A kept slot is watched only while no browser is on the page: after 15 s without a reply the bridge
  asks for the state, and cycles the link after three unanswered requests in a row. Its own request
  no longer resets the miss counter, and a link is never cycled while a browser, and with it
  possibly a firmware transfer, is on the page.

## v1.4.0-beta.9 — 2026-11-15
**Beta.** Bridge and web interface only work together.
- **Fix: connected slots no longer go silent.** The event log of beta.8 showed it: a storage answers
  every request within a fraction of a second, but on a link nobody talks on it goes mute, and the
  keepalive of beta.7 (every 30 s) came too late. The web app only polls the slot on screen, so the
  other slot went mute even with the browser open. Now the bridge asks for the state whenever a slot
  has been quiet for 4 s: always for a slot that is kept connected, and for the others while a browser
  is watching. A kept slot that stays silent for three requests in a row is cycled and reconnected.
  Traffic of any kind counts, so a firmware transfer never gets a stray command pushed into it.
- The event log keeps 320 entries and leaves the bridge's own requests out, so it stays readable.

## v1.4.0-beta.8 — 2026-11-15
**Beta.** Diagnostic build; bridge and web interface only work together.
- New `/api/log` (needs a login): the newest 160 link events with timestamps - connects, drops, every
  write and every reply (bytes and command), keepalive results and link cycles. To find out why a
  storage that is connected answers only a few requests.
- `/api/status` can now also be opened in a logged-in browser; the API token is only needed for other
  clients such as Home Assistant.

## v1.4.0-beta.7 — 2026-11-15
**Beta.** Bridge and web interface only work together.
- **Fix: a kept-connected slot no longer ends up connected but silent.** A link nobody talks on goes
  mute - writes are accepted, nothing comes back - so without a browser the bridge now asks a kept slot
  for its state every 30 s and cycles the link (and reconnects) when two answers in a row stay away.
- **Opening the page no longer drops healthy links.** A link that has answered recently is kept; only
  a link that has been quiet for 45 s is refreshed. This also covers a phone that was closed: its
  half-dead connection used to count as a watching browser, so the stale link was never refreshed.
- The bridge now detects such dead browser connections itself (TCP keepalive, about 30 s) and stops
  counting them.
- `/api/status` shows `lastReplySeconds` per slot: seconds since the storage last sent anything.
- The slots are named *Device 1-3* instead of *Storage 1-3* in the interface.

## v1.4.0-beta.6 — 2026-11-15
**Beta.** Web interface only; the firmware is unchanged from v1.4.0-beta.3.
- *Star on GitHub* in the web interface now links to this repository instead of the Control repository.

## v1.4.0-beta.5 — 2026-11-15
**Beta.** Web interface only; the firmware is unchanged from v1.4.0-beta.3.
- Going back from a beta to stable no longer ends in a dead end: with older firmware than the web
  interface, the page offers login and the update card, and the update card warns when it offers a
  switch between beta and stable.

## v1.4.0-beta.4 — 2026-11-15
**Beta.** Web interface only; the firmware is unchanged from v1.4.0-beta.3.
- The update card has a switch, *Include beta versions* (off by default): the card then offers the
  newest release including betas instead of only the latest stable one.

## v1.4.0-beta.3 — 2026-11-15
**Beta.** Bridge and web interface only work together.
- **Keep a slot connected permanently** (switch per slot under *Connection*): the bridge connects it by
  itself after a restart and reconnects it after a drop (5, 10, 30 s, then every 60 s). The hold time
  does not apply to such a slot. While the bridge is connected, the manufacturer's app cannot connect -
  the interface says so next to the switch. `/api/status` shows `keepConnected` and
  `nextAttemptSeconds` per slot.
- A link that is up but still being set up is no longer mistaken for a missing one.

## v1.4.0-beta.2 — 2026-11-15
**Beta.** Same pairing rule as beta.1: bridge and web interface only work together.
- **Hold time is adjustable** (5-3600 s, default 15 s): how long the storages stay connected after the
  last browser or the last REST command. Set it in the interface under *Connection*.
- **Testing from Home Assistant:** `GET /api/status` (version, uptime, WiFi, heap, slots, last REST
  command) and `POST /api/connection` to connect or release a storage without sending a command.
  README has a ready-made `rest` sensor.
- Slots, binding and removing a device are locked while a firmware update is running.

## v1.4.0-beta.1 — 2026-11-15
**Beta.** Needs the matching web interface; older web apps do not work with this firmware and the
other way round. Not yet tested against real storages in several slots.
- **Three storages per bridge.** Each sits in its own slot; the interface has a slot bar. An existing
  binding moves into slot 1 by itself. The control protocol is now version 2.
- **REST interface for Home Assistant:** `POST /api/peak-shaving` switches peak shaving on or off
  per slot, secured by an API token created in the interface (stored as a hash, shown once).
  If nothing holds the storage, the bridge connects for the command and releases it again shortly
  after. See the README.
- `/api/diag` now lists every slot and reports the free heap, to judge how many connections fit.

## v1.3.25 — 2026-11-20
- **Fix: a battery module below 0 °C showed about 6552 °C.** The module temperature was read as an unsigned
  value, so a reading such as -0.9 °C came out as 6552.7 °C. It is now read as signed. (Web interface only;
  the firmware is unchanged from v1.3.23.)

## v1.3.24 — 2026-11-19
- **The peak shaving card now warns about its effect on the self-consumption offset.** On the Venus A, D
  and E 3.0 the firmware stores the threshold as the self-consumption offset too when peak shaving is
  enabled, and resets the offset to 0 when it is disabled. With a house load below the threshold the
  storage can then charge from the grid. (Web interface only; the firmware is unchanged from v1.3.23.)

## v1.3.23 — 2026-11-19
- **Fix: the web installer now offers the WiFi setup after flashing.** The bridge answered the installer's
  Improv requests with the wrong command numbers (one too high), so the installer never recognised it
  and skipped the WiFi dialog - on every board, not only on the S3. The numbers now match.
- **WiFi setup over the ESP32-S3's own USB port.** The bridge now also listens on the chip's built-in
  USB port (USB Serial/JTAG), so boards that have only that port can enter their WiFi from the
  installer as well. The access point `Marstek-Bridge-XXXX` remains the fallback.

## v1.3.22 — 2026-11-19
- **Fix: the ESP32-S3 image asked for a password on its setup access point.** The S3 build still carried
  the pre-v1.3.9 access point password, so `Marstek-Bridge-XXXX` was protected instead of open. It is open
  again, like on the classic ESP32, and needs no password to join.
- Otherwise the firmware is unchanged from v1.3.21.

## v1.3.21 — 2026-11-16
- **ESP32-S3 in the web installer.** The installer now offers an image for the ESP32-S3 as well as for the
  classic ESP32 and picks the right one by itself. The release carries it as
  `marstek-ble-bridge-esp32s3.bin`; the web image is shared. The firmware reports its chip, and the update
  card installs the image that fits it. Boards with USB through the chip's own port (no separate USB-serial
  chip) can be flashed from the browser, but the WiFi setup over the cable (Improv) needs a UART: join the
  `Marstek-Bridge-XXXX` access point instead.
- Otherwise the firmware is unchanged from v1.3.20.

## v1.3.20 — 2026-11-16
- **Fix: the Venus D no longer goes silent about 28 s after connecting.** Right after connecting, the
  Venus D sends an "Exchange MTU" request of its own. The firmware was built as a pure Bluetooth
  central without the GATT server, and NimBLE only handles that request when the server is built in:
  the request was dropped unanswered, and an unanswered Bluetooth request times out after 30 s, after
  which the link carries nothing more - the device stopped answering and stopped notifying while the
  connection itself stayed up. The firmware is now built with the GATT server (no service is registered,
  nothing advertises; about 16 KB larger).
- The bridge no longer starts pairing after connecting. The vendor app never pairs with a storage, and
  the Venus D does not answer a pairing request.
  (The web interface changes only in that the message of an earlier failed connection attempt no longer
  stays on screen once a later attempt has connected.)

## v1.3.19 — 2026-11-15
- The version is shown in the footer and at the bottom of the mobile menu.
- Firmware update: the transfer is isolated and a fresh connection is required before the next
  transfer or any normal command. After a successful transfer the page says the device needs about
  1 minute to finish installing, then to reconnect and check the version; the duplicated wording
  between the progress line and the hint is gone.
  (Web interface only; the firmware is unchanged from v1.3.9.)

## v1.3.18 — 2026-11-14
- Firmware upload: replies are now checked for the right frame after reassembly, so a stray or split
  frame can no longer be mistaken for the acknowledgement of a block (fix by Ruben J. Jongejan).
  (Web interface only; the firmware is unchanged from v1.3.9.)

## v1.3.17 — 2026-11-13
- Venus E v1/v2 (`MST_ACCP_xxxx`, device type HMG-50) gets its own view. **Not tested on real
  hardware**: it is based on reading the Control firmware only. Work mode including economy/AI, power
  class (800 / 2500 W), charge and discharge limits, backup power, depth of discharge (30–90 %,
  firmware 156+), Bluetooth switch (156+), CT mode and readings, local API and set time.
- Not offered for it because the firmware has no such command, or it could not be confirmed: peak
  shaving, self-control offset, surplus feed-in, battery modules, OTA, factory reset, CT meter type,
  LED switch.
- Firmware files named `ac_app_…` are no longer taken for a Venus E 3.0; Venus E v1/v2 control images
  are recognised by their `MST_ACCP` tag.
  (Web interface only; the firmware is unchanged from v1.3.9.)

## v1.3.16 — 2026-10-25
- The web interface now detects the bridge when it is served over https, for example behind a TLS
  reverse proxy such as Caddy. Before, it always fell back to Web Bluetooth there, and showed
  "Browser not supported" in Safari and Firefox.
  (Web interface only; the firmware is unchanged from v1.3.9.)

## v1.3.15 — 2026-10-01
- The firmware archive picker groups the images by component in collapsible sections, marks the
  version that is installed on the device, and uses the card styling of the rest of the page.
- Test builds show as 149.2 (Beta) instead of 1492, in the archive picker and in the device info.
- Factory reset: the buttons sit at the top of the card, and the second one says it deletes all data.
  (Web interface only; the firmware is unchanged from v1.3.9.)

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

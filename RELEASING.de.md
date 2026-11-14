# Release-Ablauf

> 🇬🇧 English version: [RELEASING.md](RELEASING.md)

Die Bridge bündelt die Web-App ([marstek-ble-control](https://github.com/sphings79/marstek-ble-control)),
dieselbe App läuft zusätzlich auf einem Webserver. **Diese drei Stellen müssen immer denselben Stand
haben:** Control `master`, das neueste Bridge-Release und die gehostete App. Nur der Beta-Build der
gehosteten App darf abweichen.

## Ein Befehl

```bash
tools/release.sh 1.3.19
```

Vorher den Abschnitt `## v1.3.19` in `CHANGELOG.md` schreiben und auf `master` committen. Control muss
auf `master` stehen, sauber sein und `origin/master` entsprechen. Das Skript:

1. setzt den Tag, bündelt den Control-Build (`sync-web.sh`) und baut die Firmware,
2. kopiert die Images, setzt `docs/manifest.json` hoch und trägt den Control-Commit in
   `docs/firmware/build-info.txt` ein, dann Commit,
3. fragt nach, pusht dann `master` und den Tag und legt das GitHub-Release mit beiden Images an,
4. fragt nach, sichert dann die gehostete App und ersetzt sie (nur `control/`, nie `control-beta/`),
5. startet `tools/check-sync.sh`.

Jeder Schritt, der den Rechner verlässt, zeigt vorher, was er tut, und wartet auf `yes` (oder `--yes`).

## Nur prüfen

```bash
tools/check-sync.sh
```

Nur lesend. Vergleicht Control `origin/master`, den Control-Commit im neuesten Bridge-Release und das
Bundle, das die gehostete App gerade ausliefert. Endet mit 1, wenn sie abweichen. Nach jeder Änderung
an Control ausführen und immer, wenn man unsicher ist.

## Eine Control-Änderung ausliefern

Eine Änderung an Control ist erst fertig, wenn Bridge und gehostete App sie ebenfalls tragen. Jede
Control-Änderung, die raus soll, bedeutet also ein Bridge-Release, auch wenn die Firmware gleich
bleibt (im Changelog steht dann „nur Weboberfläche").

## Einstellungen

Pfade und Zugang zum Hosting stehen in `tools/release.local.env`, die von Git ignoriert wird:

| Variable | Bedeutung |
|---|---|
| `CONTROL_DIR` | Checkout von marstek-ble-control |
| `IDF_EXPORT` | `export.sh` von ESP-IDF |
| `HOSTING_SSH` | `user@host` des Webservers (leer lassen, um den Hosting-Schritt zu überspringen) |
| `HOSTING_WEBROOT` | Verzeichnis auf dem Server mit `control/` und `control-beta/` |
| `HOSTING_OWNER` | Besitzer der Dateien dort, `user:group` |
| `HOSTING_BACKUP_DIR` | Ablage der serverseitigen Sicherung von `control/` |
| `HOSTING_URL` | öffentliche URL der gehosteten App |
| `HOSTING_BASE` | URL-Pfad, unter dem die App liegt, zum Beispiel `/marstek/control/` |

Ohne `HOSTING_SSH` entfällt der Hosting-Schritt. Den gehosteten Build dann von Hand hochladen und
`tools/check-sync.sh` ausführen. Der gehostete Build ist `npx vite build --base=<HOSTING_BASE>` im
Control-Checkout.

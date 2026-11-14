# Releasing

> 🇩🇪 Deutsche Version: [RELEASING.de.md](RELEASING.de.md)

The bridge bundles the web app ([marstek-ble-control](https://github.com/sphings79/marstek-ble-control)),
and the same app is also hosted on a web server. **These three places always have to be the same
state:** Control `master`, the latest bridge release, and the hosted app. Only the beta build of the
hosted app may differ.

## One command

```bash
tools/release.sh 1.3.19
```

Before running it, write the `## v1.3.19` section in `CHANGELOG.md` and commit it on `master`. Control
must be on `master`, clean and equal to `origin/master`. The script then:

1. tags the release, bundles the Control build (`sync-web.sh`) and builds the firmware,
2. copies the images, bumps `docs/manifest.json` and records the Control commit in
   `docs/firmware/build-info.txt`, then commits,
3. asks, then pushes `master` and the tag and creates the GitHub release with both images,
4. asks, then backs up and replaces the hosted app (`control/` only, never `control-beta/`),
5. runs `tools/check-sync.sh`.

Every step that leaves the machine shows what it will do and waits for `yes` (or `--yes`).

## Check only

```bash
tools/check-sync.sh
```

Read-only. Compares Control `origin/master`, the Control commit built into the latest bridge release
and the bundle the hosted app serves right now. Exits with 1 if they differ. Run it after any change
to Control, and whenever you are unsure.

## Release a Control change

A change in Control is not released until the bridge and the hosted app carry it too. So every Control
change that should go out means a bridge release, even if the firmware itself is unchanged (those
releases say "web interface only" in the changelog).

## Settings

Paths and hosting access go into `tools/release.local.env`, which is git-ignored:

| Variable | Meaning |
|---|---|
| `CONTROL_DIR` | checkout of marstek-ble-control |
| `IDF_EXPORT` | ESP-IDF `export.sh` |
| `HOSTING_SSH` | `user@host` of the web server (leave unset to skip the hosting step) |
| `HOSTING_WEBROOT` | directory on the server that holds `control/` and `control-beta/` |
| `HOSTING_OWNER` | owner of the files there, `user:group` |
| `HOSTING_BACKUP_DIR` | where the server-side backup of `control/` goes |
| `HOSTING_URL` | public URL of the hosted app |
| `HOSTING_BASE` | URL path the app is served from, for example `/marstek/control/` |

Without `HOSTING_SSH` the hosting step is skipped; upload the hosted build by hand and run
`tools/check-sync.sh`. The hosted build is `npx vite build --base=<HOSTING_BASE>` in the Control
checkout.

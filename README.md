# PS5Save+

**Smarter backups. Fewer interruptions.**

PS5Save+ is a lightweight PS5 save-backup payload with native console notifications, persistent USB diagnostics and an optional web dashboard. It is a fork of [PS5Save](https://github.com/heni0xyz/PS5Save) by **heni0xyz** and contributors, based on upstream v3 and the subsequent notification-redesign work.

> **Development branch — not a tested console release.** Do not replace your working backup payload with this build until the PS5-specific functionality has been validated.

## What is implemented on this branch

- The existing PS4/PS5 decrypted saves, original encrypted saves, trophy data, optional emulator paths and daily snapshots remain supported.
- All data is grouped under **`/mnt/usb0/ps5save-plus/`**. At startup the payload migrates the old root-level `backup.conf`, `backup/` and `backup_daily/` directories through same-filesystem renames, preserving the data and rebasing the change index so unchanged files do not need to be copied again. If the configuration is truly absent it is created from the built-in example and a **Debug notification** confirms its creation. Existing user settings are never overwritten.
- Live backups and dated snapshots now have **separate, independently navigable panels** on the Backups tab, each with a file inventory; opening a snapshot never replaces the live backup view.
- Normal passes no longer notify for every scan, empty configured directory or unchanged backup. An initial quiet pass confirms that backup protection is active; copies produce completion notifications. Progress is sampled after 10 seconds of copying, then every 10–30 seconds according to estimated remaining work; native toasts show the current sampled percentage, not fixed 25/50/75 milestones, and do not repeat an unchanged percentage.
- The rich Sony-style notification transport and the original `all`, `errors`, `off` modes are preserved; the example configuration avoids toasts during games.
- A small **HTTP dashboard** runs on TCP **8755**, independently of backup operations. It shows the recent pass, available USB space, backup directory categories, dated snapshots, an on-demand read-only directory explorer, per-snapshot file/directory counts and occupied space, diagnostic history and the current `backup.conf`.
- **Browser-based configuration editing** is available after pairing with a six-digit code displayed in a native PS5 notification. Changes are validated and saved through an atomic replacement, with the previous version retained. A short-lived session and per-write version check prevent unauthorized or stale writes.
- Native PS5 notifications automatically detect Portuguese (Portugal/Brazil), Spanish, French, German and Italian; other console languages fall back to English. Unknown or untranslated messages also use their original English text.
- A previously reported error can produce one recovery confirmation once the affected problem has been resolved, without duplicate completion notices.
- The Backups tab can navigate the complete directory structure in 64-item pages, showing filenames, sizes and file modification dates. On selecting a snapshot it also calculates a recursive inventory (file count, directory count and total bytes), capped at 30,000 entries or two seconds to keep the payload responsive; incomplete scans are explicitly marked. This is a filesystem inventory, **not a hash-based integrity verification** or proof that all source saves were included. A snapshot preserves the live backup available at its creation time.
- A bounded `events.log` records diagnostics to USB; `history.tsv` records meaningful backup passes. Rotation prevents unlimited file growth.
- No external web service, database, Node.js or other runtime is required for the dashboard.

**Not yet validated on hardware:** real-console backup operations, automatic language detection, dashboard editing/pairing, standby and the optional home-screen launcher. SDK cross-compilation is now part of CI, but a successful cross-build does not prove runtime compatibility.

## Installation (development/testing)

1. Connect an exFAT USB drive. **You do not need to create `backup.conf` manually.** A fresh installation generates `ps5save-plus/backup.conf` with the published defaults and announces this through a Debug notification. If a legacy `backup.conf` and backup folders are already in the USB root, the payload relocates them without discarding saves or changing the existing options. Review the resulting configuration before relying on its defaults.
2. Compile with the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk), then execute `backup.elf` using your usual PS5 payload loader.
3. Open `http://<PS5-IP>:8755` from a PC or phone on the same trusted network; for a PS5 on `192.168.1.91`, use `http://192.168.1.91:8755`.
4. To edit settings, choose **Unlock editing on PS5**, read the six-digit code on the console and confirm it in the dashboard. Then use the simple settings form, or open the advanced text editor for custom paths. The previous file is retained as `backup.conf.bak`.
5. The backup service still works if TCP 8755 is occupied, the browser is closed or dashboard authentication is unavailable.

**Security:** the dashboard uses unencrypted HTTP. Read-only information and configuration details are visible to devices on the trusted local network; editing requires a physical-console pairing code, a short-lived in-memory session and matching request origin. Codes are one-use, expire after 2 minutes, and repeated requests/failed attempts are limited. Authorization sessions expire after 1 hour. **Never expose port 8755 to the public internet, route it through a public proxy or use it on an untrusted network.** The physical PS5 must be able to display native notifications to pair. Pairing notifications are explicitly user-triggered even when ordinary backup notifications are disabled.

The server does not automatically open the PS5 browser or install an app icon. The dashboard is optional and is not required for backups to run.

## Optional PS5 home-screen launcher (development only)

A separate `launcher-installer.elf` provides an **opt-in** PS5 home-screen icon that opens `http://127.0.0.1:8755/`.

- Build it with `make PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk launcher-installer.elf`.
- The installer is independent of the backup ELF; it is **never executed automatically**.
- It uses dedicated title ID `P5SV00001` and writes only its own metadata under `/user/app/P5SV00001`. If that folder exists without the PS5Save+ ownership marker, installation is refused.
- The installer uses the title-specific `sceAppInstUtilAppInstallTitleDir()` API from the official SDK sample. It checks `sceAppInstUtilAppExists()` before writing or registering anything. If its own tile is already registered it exits without reinstalling; a foreign title or folder with the same ID is left untouched.
- It uses cumulative **Debug notifications** for installation, registration and error codes, unlike the native backup notification transport. A failed attempt can be retried without a global application scan.
- The tile opens the browser only; **it does not start the backup payload**. Execute `backup.elf` first.
- The launcher currently reuses the existing 1024x1024 notification artwork; a distinct PS5Save+ icon can be prepared later.

**The installer still needs a supervised PS5 test.** The first version did not create a visible icon. This revision adds Debug error reporting, initializes the PS5 user/network services, checks registered titles and uses the SDK's direct registration function with full app metadata. Cross-compilation alone does not establish which earlier step failed, nor prove the icon will now appear. No one-click uninstall or rollback is available, so a controlled test and removal procedure remain necessary. The installer stays separate from `backup.elf`; merging them is a later phase.

## Configuration

The [example backup.conf](backup.conf) enables PS4/PS5 backups while leaving emulator-specific paths commented out. Review these choices before using it with real data.

```ini
backup_interval_seconds=30
backup_notifications=all
backup_notifications_in_game=false
backup_ps4_saves=true
backup_ps5_saves=true
backup_encrypted_saves=true
backup_ps4_trophies=true
backup_ps5_trophies=true
max_title_size_mb=4096
daily_snapshots_to_keep=3
```

The file remains the only source of backup configuration. Browser edits reject invalid numeric values, unknown options, unsafe USB or parent-directory backup paths, and files larger than 16 KiB. An edit is refused if the configuration has changed since the browser loaded it. Settings take effect on the next backup pass, without interrupting the current one.

For consistent copies of encrypted saves and trophy data, close the relevant game before the pass; decrypted saves from active or suspended titles are deferred until closure.

## Files on USB

| Path | Purpose |
| --- | --- |
| `/mnt/usb0/ps5save-plus/backup.conf` | Main editable configuration; created automatically if missing |
| `/mnt/usb0/ps5save-plus/backup.conf.bak` | Previous configuration, preserved on dashboard edits |
| `/mnt/usb0/ps5save-plus/backup/` | Live backup data |
| `/mnt/usb0/ps5save-plus/backup_daily/YYYY-MM-DD/` | Optional dated snapshots |
| `/mnt/usb0/ps5save-plus/events.log` | Rotating diagnostic log, up to 256 KiB |
| `/mnt/usb0/ps5save-plus/events.log.1` | Previous log segment |
| `/mnt/usb0/ps5save-plus/history.tsv` | Rotating history of meaningful backup passes, up to 128 KiB |
| `/mnt/usb0/ps5save-plus/history.tsv.1` | Previous history segment |

**Migration safety:** the move occurs before the first backup and uses `rename()` within the same USB volume, not a new data copy. The existing change-index paths are translated to their new locations. If both old and new backup directories exist, the payload **stops instead of merging, replacing or deleting either directory**; resolve that conflict manually after reviewing both copies. If both configuration files exist, the one under `ps5save-plus/` takes precedence and the old file is preserved. The previous `backup.conf.bak` is moved if the new location is unoccupied.

Log and history writes are best-effort. Failure to write diagnostics does not stop a backup.

## Build and tests

```sh
make PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make test
make PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk launcher-installer.elf
```

The build embeds `dashboard/index.html` in the ELF and links the PS5 System Service library to read the console language. Host tests cover backup/notification logic, HTTP endpoints, one-time pairing, unauthorized writes, configuration validation, version conflicts, localisation and recovery. Cross-compilation is verified with official SDK v0.50 in GitHub Actions; real save mounts, PS5 ShellUI notifications, browser compatibility, rest mode and launcher registration still require physical-console validation.

## Credits and licence

Based on [PS5Save](https://github.com/heni0xyz/PS5Save) by heni0xyz and original contributors, including code and research credited there. Thanks also to the [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) project for the rich notification transport and [PS5 Payload Template](https://github.com/ghost1372/PS5-Payload-Template) for the reference approach to embedded HTTP dashboards.

Distributed under **GPL-3.0**. See [LICENSE](LICENSE).

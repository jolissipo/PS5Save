# PS5Save+

**Smarter backups. Fewer interruptions.**

PS5Save+ is a lightweight PS5 save-backup payload with native console notifications, persistent USB diagnostics and an optional web dashboard. It is a fork of [PS5Save](https://github.com/heni0xyz/PS5Save) by **heni0xyz** and contributors, based on upstream v3 and the subsequent notification-redesign work.

> **Development branch — not a tested console release.** Do not replace your working backup payload with this build until the PS5-specific functionality has been validated.

## What is implemented on this branch

- The existing PS4/PS5 decrypted saves, original encrypted saves, trophy data, optional emulator paths and daily snapshots remain supported.
- Normal passes no longer notify for every scan, empty configured directory or unchanged backup. An initial quiet pass confirms that backup protection is active; copies produce completion notifications. Progress checks use a 60-second interval by default, rather than repetitive 30-second checks.
- The rich Sony-style notification transport and the original `all`, `errors`, `off` modes are preserved; the example configuration avoids toasts during games.
- A small **HTTP dashboard** runs on TCP **8755**, independently of backup operations. It shows the recent pass, available USB space, backup directory categories, dated snapshots, diagnostic history and the current `backup.conf`.
- **Browser-based configuration editing** is available after pairing with a six-digit code displayed in a native PS5 notification. Changes are validated and saved through an atomic replacement, with the previous version retained. A short-lived session and per-write version check prevent unauthorized or stale writes.
- Native PS5 notifications automatically detect Portuguese (Portugal/Brazil), Spanish, French, German and Italian; other console languages fall back to English. Unknown or untranslated messages also use their original English text.
- A previously reported error can produce one recovery confirmation once the affected problem has been resolved, without duplicate completion notices.
- A bounded `events.log` records diagnostics to USB; `history.tsv` records meaningful backup passes. Rotation prevents unlimited file growth.
- No external web service, database, Node.js or other runtime is required for the dashboard.

**Not yet implemented:** the optional PS5 home-screen launcher. PS5 SDK compilation and real-console validation are also pending. The new editor, language selection and recovery notifications have passed host tests but have not been tested in ShellUI.

## Installation (development/testing)

1. Copy [backup.conf](backup.conf) to the root of an exFAT USB device; edit the enabled backup sources to match your needs.
2. Compile with the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk), then execute `backup.elf` using your usual PS5 payload loader.
3. Open `http://<PS5-IP>:8755` from a PC or phone on the same trusted network; for a PS5 on `192.168.1.91`, use `http://192.168.1.91:8755`.
4. To edit settings, choose **Unlock editing on PS5**, read the six-digit code on the console and confirm it in the dashboard. Then use the simple settings form, or open the advanced text editor for custom paths. The previous file is retained as `backup.conf.bak`.
5. The backup service still works if TCP 8755 is occupied, the browser is closed or dashboard authentication is unavailable.

**Security:** the dashboard uses unencrypted HTTP. Read-only information and configuration details are visible to devices on the trusted local network; editing requires a physical-console pairing code, a short-lived in-memory session and matching request origin. Codes are one-use, expire after 2 minutes, and repeated requests/failed attempts are limited. Authorization sessions expire after 1 hour. **Never expose port 8755 to the public internet, route it through a public proxy or use it on an untrusted network.** The physical PS5 must be able to display native notifications to pair. Pairing notifications are explicitly user-triggered even when ordinary backup notifications are disabled.

The server does not automatically open the PS5 browser or install an app icon. The dashboard is optional and is not required for backups to run.

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
| `/mnt/usb0/backup.conf` | Main editable configuration |
| `/mnt/usb0/backup.conf.bak` | Previous configuration, preserved on dashboard edits |
| `/mnt/usb0/backup/` | Live backup data |
| `/mnt/usb0/backup_daily/YYYY-MM-DD/` | Optional dated snapshots |
| `/mnt/usb0/ps5save-plus/events.log` | Rotating diagnostic log, up to 256 KiB |
| `/mnt/usb0/ps5save-plus/events.log.1` | Previous log segment |
| `/mnt/usb0/ps5save-plus/history.tsv` | Rotating history of meaningful backup passes, up to 128 KiB |
| `/mnt/usb0/ps5save-plus/history.tsv.1` | Previous history segment |

Log and history writes are best-effort. Failure to write diagnostics does not stop a backup.

## Build and tests

```sh
make PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make test
```

The build embeds `dashboard/index.html` in the ELF and links the PS5 System Service library to read the console language. Host tests cover backup/notification logic, HTTP endpoints, one-time pairing, unauthorized writes, configuration validation, version conflicts, localisation and recovery. Real save mounts, PS5 ShellUI notifications, browser compatibility, rest mode and actual PS5 SDK linking still require console/build validation.

## Credits and licence

Based on [PS5Save](https://github.com/heni0xyz/PS5Save) by heni0xyz and original contributors, including code and research credited there. Thanks also to the [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) project for the rich notification transport and [PS5 Payload Template](https://github.com/ghost1372/PS5-Payload-Template) for the reference approach to embedded HTTP dashboards.

Distributed under **GPL-3.0**. See [LICENSE](LICENSE).

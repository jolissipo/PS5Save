# PS5Save+

**Smarter backups. Fewer interruptions.**

PS5Save+ is a lightweight PS5 save-backup payload with native console notifications, persistent USB diagnostics and an optional web dashboard. It is a fork of [PS5Save](https://github.com/heni0xyz/PS5Save) by **heni0xyz** and contributors, based on upstream v3 and the subsequent notification-redesign work.

> **Development branch — not a tested console release.** Do not replace your working backup payload with this build until the PS5-specific functionality has been validated.

## What is implemented on this branch

- The existing PS4/PS5 decrypted saves, original encrypted saves, trophy data, optional emulator paths and daily snapshots remain supported.
- Normal passes no longer notify for every scan, empty configured directory or unchanged backup. An initial quiet pass confirms that backup protection is active; copies produce completion notifications. Progress checks use a 60-second interval by default, rather than repetitive 30-second checks.
- The rich Sony-style notification transport and the original `all`, `errors`, `off` modes are preserved; the example configuration avoids toasts during games.
- A small **read-only HTTP dashboard** runs on TCP **8755**, independently of backup operations. It shows the recent pass, available USB space, backup directory categories, dated snapshots, diagnostic history and the current `backup.conf`.
- A bounded `events.log` records diagnostics to USB; `history.tsv` records meaningful backup passes. Rotation prevents unlimited file growth.
- No external web service, database, Node.js or other runtime is required for the dashboard.

**Not yet implemented:** authenticated browser-based editing of `backup.conf`, automatic notification translation, recovery alerts and an optional PS5 home-screen launcher. These require separate validation. The dashboard intentionally rejects non-GET requests until safe editing is available.

## Installation (development/testing)

1. Copy [backup.conf](backup.conf) to the root of an exFAT USB device; edit the enabled backup sources to match your needs.
2. Compile with the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk), then execute `backup.elf` using your usual PS5 payload loader.
3. If the console IP is `192.168.1.91`, open `http://192.168.1.91:8755` on a PC or phone on the same trusted network.
4. The PS5 backup service works normally even if TCP 8755 is already occupied or the dashboard is not accessible.

**Security:** the dashboard currently serves read-only status, backups metadata, history and configuration through unencrypted HTTP. Restrict access to your trusted local network, do not forward port 8755 to the internet, and do not connect it to a public proxy. Browser-based configuration writes are disabled until authentication and save-safety mechanisms are implemented.

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

The file remains the only source of backup configuration. For consistent copies of encrypted saves and trophy data, close the relevant game before the pass; decrypted saves from active or suspended titles are deferred until closure.

## Files on USB

| Path | Purpose |
| --- | --- |
| `/mnt/usb0/backup.conf` | Main editable configuration |
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

The build embeds `dashboard/index.html` in the ELF. Host tests exercise the backup/notification logic and the dashboard HTTP routes; native PS5 notifications, save mounts, the console browser and rest-mode behavior require physical-console tests.

## Credits and licence

Based on [PS5Save](https://github.com/heni0xyz/PS5Save) by heni0xyz and original contributors, including code and research credited there. Thanks also to the [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) project for the rich notification transport and [PS5 Payload Template](https://github.com/ghost1372/PS5-Payload-Template) for the reference approach to embedded HTTP dashboards.

Distributed under **GPL-3.0**. See [LICENSE](LICENSE).

# PS5Save+

PS5Save+ is a PS5 payload for automatic save-data backups to external USB storage. It extends [PS5Save](https://github.com/heni0xyz/PS5Save) with incremental backups, daily snapshots, console notifications, a local web dashboard and verified USB migration.

**Status:** alpha. Tested on PS5 firmware 11.60. Recovery after rest mode and interrupted USB transfers must be verified on the console before relying on them for critical data.

## Features

- **Incremental backups:** checks for changed files at the configured interval, initially on launch. Supports decrypted PS4/PS5 saves, original encrypted saves, PS4/PS5 trophy data and additional source paths.
- **Game-aware protection:** defers save-image operations associated with running, suspended or otherwise active titles until the data can be accessed safely.
- **Daily snapshots:** date-stamped copies under `backup_daily/YYYY-MM-DD/`, with configurable retention (0–10 days).
- **Notifications:** PS5 native notifications for meaningful backup events, progress, failures and USB migration. Supported console languages: English, Portuguese (Portugal/Brazil), Spanish, French, German and Italian. Other languages fall back to English. Routine unchanged passes do not produce notifications.
- **USB selection:** detects existing installations before choosing a drive. Existing data takes priority over free space. When multiple installations exist, a Debug notification and `events.log` identify the selected USB; other installations remain untouched.
- **Verified USB migration:** stages the complete backup directory on another mounted USB, checks files byte-for-byte and only then switches the active location. A recognized interrupted stage is reused after individual files are verified; partial or mismatched files are recopied. Existing destination installations are never overwritten.
- **Web dashboard:** backup status, history, current files, daily snapshots, storage selection, transfer progress and diagnostics. Overview, Backups and logs remain available during migration; Settings is temporarily disabled.
- **Protected configuration editing:** a six-digit code displayed on the console authorizes each browser independently. Codes expire after two minutes and may be resent without extending their lifetime (minimum five seconds between resends). Up to eight one-hour editing sessions can coexist. Stale configuration writes are rejected.
- **Rest-mode recovery:** the HTTP listener is recreated after a console resume signal or socket failure. USB migrations can retry from verified staged files once the required volumes become available. During active copies, the payload refreshes the system's *idle* timer; explicit rest commands, including HDMI-CEC, are not blocked.
- **Home-screen shortcut:** installs the dashboard launcher if absent. One ELF provides the backup service and dashboard.

## Install and use

1. Connect a writable **exFAT USB** drive with sufficient capacity.
2. Download `ps5saveplus_v0.1.0-alpha.8.elf` from [Releases](https://github.com/jolissipo/PS5Save/releases).
3. Run the ELF with a compatible PS5 payload loader. The first backup check runs immediately, and the dashboard service starts on TCP port **8755**.
4. Open the installed PS5Save+ shortcut, or visit `http://<PS5-IP>:8755/` from a browser on the same trusted network.
5. To edit settings, open **Settings → Unlock editing on PS5** and enter the six-digit code shown on the console.

The dashboard shortcut opens the current service; it does **not** automatically launch the payload after a full console reboot. The payload must be launched again after a reboot unless the loader is configured to autoload it.

### USB directory

```text
/mnt/usbN/ps5save-plus/
├── backup.conf
├── backup/
├── backup_daily/YYYY-MM-DD/
├── events.log
└── history.tsv
```

An interrupted transfer may leave `.ps5save-plus-transfer.part` on the destination USB. Do not remove or edit this directory while recovery is possible. The original backup remains the source until the destination has passed full verification and been committed.

## Configuration

`backup.conf` is created on the selected USB only if it is missing. Existing configurations and custom source paths are retained.

| Key | Default | Purpose |
| --- | --- | --- |
| `backup_usb_device` | `auto` | Existing-backup-first selection, or `/mnt/usb0`–`/mnt/usb7` |
| `backup_interval_seconds` | `30` | Seconds between regular checks (30–3600) |
| `backup_notifications` | `all` | `all`, `errors`, `off` |
| `backup_notifications_in_game` | `false` | Show ordinary backup notifications during gameplay |
| `backup_ps4_saves`, `backup_ps5_saves` | `true` | Decrypted save data |
| `backup_encrypted_saves` | `true` | Original encrypted save files |
| `backup_ps4_trophies`, `backup_ps5_trophies` | `true` | Trophy data |
| `max_title_size_mb` | `4096` | Maximum save-image size per title (decimal MB) |
| `daily_snapshots_to_keep` | `3` | Retained daily snapshots (0–10) |

Custom emulator paths can be added through the advanced editor or directly to `backup.conf`. See the [default configuration](backup.conf) for examples.

## Safety and limitations

- Backups are not a substitute for an independently verified copy of important saves.
- Migration verifies **copied files against the source**. It does not establish that the source save data is valid or restorable.
- A suspended process may survive rest mode while open sockets and USB file handles do not. Recovery is best-effort and requires the payload process to remain alive and both USB devices to mount again. A full reboot requires launching the payload again.
- Power-timer maintenance prevents only automatic idle standby during active copying. HDMI-CEC power commands and manual rest mode remain effective.
- Configuration editing requires console-side pairing and is restricted to a trusted local network. Do not expose TCP **8755** to the internet.
- The dashboard's USB file browser is read-only; it does not restore saves.

## Build and tests

Requires the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk).

```sh
make PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
sh tests/run-host-tests.sh
```

GitHub Actions runs host regression tests and cross-compiles the PS5 ELF. Its release workflow publishes validated alpha binaries.

## Credits and licence

Based on [heni0xyz/PS5Save](https://github.com/heni0xyz/PS5Save) and the credited upstream projects. Dashboard resume recovery follows the signal-and-listener approach used by [itsPLK/ps5-payload-manager](https://github.com/itsPLK/ps5-payload-manager) and [itsPLK/ps5-pkg-manager](https://github.com/itsPLK/ps5-pkg-manager). Idle-timer maintenance uses the PS5 SystemService API as demonstrated by [mortyzip/KeepAwake](https://github.com/mortyzip/KeepAwake).

Licensed under [GPL-3.0](LICENSE).

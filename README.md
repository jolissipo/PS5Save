# PS5Save+

Automatic, incremental save-data backups for jailbroken PlayStation 5 consoles. PS5Save+ extends [heni0xyz/PS5Save](https://github.com/heni0xyz/PS5Save) with a web dashboard, USB migration, daily snapshots and console notifications.

> **Alpha release.** The backup engine and USB migration have been tested on PS5 firmware 11.60. Standby recovery and the idle-sleep timer require additional console validation. Maintain an independent copy of important saves.

## Features

- **Incremental backups:** decrypted PS4/PS5 saves, original encrypted save files, trophy data and optional custom directories (including emulator saves). Unchanged files are skipped.
- **Game-aware protection:** backup of saves belonging to an active or suspended title is postponed until the title closes. Other eligible data can still be copied.
- **Daily snapshots:** dated copies of the live backup with configurable retention.
- **USB selection and migration:** detects existing installations across mounted USB drives before selecting a destination. A new installation prefers the writable USB with the most available space. Transfers use a staging directory and byte-for-byte verification before replacing the active location; pre-existing destination backups are never overwritten.
- **Web dashboard:** status, backup inventory, file browsing, snapshots, history, diagnostic logs and configuration editing.
- **Pairing security:** each browser requires a six-digit code shown on the console. Codes expire after two minutes and may be resent without extending their validity (minimum five seconds between replays). Up to eight independent one-hour editing sessions are supported. Concurrent edits are protected against stale configuration versions.
- **PS5 notifications:** configurable informational, warning and error messages; adaptive transfer progress; supported console-language translations. Routine unchanged passes remain silent.
- **Home-screen shortcut:** automatically installed when absent. Opens the dashboard without requiring a separate web application.
- **Standby handling:** retries the dashboard HTTP listener after a console wake, network change or socket failure. During active data copying, the payload requests resets of the idle power timer; this does **not** prevent manual standby or an HDMI-CEC power-off request.

## Installation

1. Connect an **exFAT USB drive** with sufficient free space.
2. Download the latest `ps5saveplus_*.elf` from [Releases](https://github.com/jolissipo/PS5Save/releases) and launch it with a PS5 ELF loader.
3. The payload selects an eligible USB, creates the configuration if missing, performs the initial backup check and installs the dashboard shortcut.
4. Open the shortcut on the console, or visit `http://<PS5-IP>:8755/` on a device connected to the same trusted network.

The backup engine remains active while the payload process runs. **After a full console reboot, the ELF must be launched again.** The home-screen shortcut opens the dashboard; it does not launch the backup service.

To edit settings, select **Settings → Unlock editing on PS5** and enter the code displayed on the console. The code is single-use; repeated requests during its validity show the same code. Editing is unavailable while USB migration is active, but backup browsing and logs remain accessible.

## USB data layout

```text
/mnt/usbN/ps5save-plus/
├── backup.conf
├── backup/                    # Current incremental backup
├── backup_daily/YYYY-MM-DD/   # Dated snapshots
├── events.log                 # Diagnostics (rotates to events.log.1)
└── history.tsv                # Backup activity
```

Existing PS5Save and PS5Save+ backups take priority over empty drives. If multiple USB devices contain installations, a Debug notification and `events.log` identify the detected drives and chosen destination; no other installation is changed. Switching drives copies and verifies the entire data directory. If verification fails, the original remains on the source USB.

## Configuration

Options in [`backup.conf`](backup.conf) can be changed through the dashboard or directly on the USB.

| Option | Default | Description |
| --- | --- | --- |
| `backup_usb_device` | `auto` | Automatic selection, or `/mnt/usb0`–`/mnt/usb7` |
| `backup_interval_seconds` | `30` | Interval between regular checks (seconds) |
| `backup_notifications` | `all` | `all`, `errors` or `off` |
| `backup_notifications_in_game` | `false` | Show notifications during gameplay |
| `backup_ps4_saves` / `backup_ps5_saves` | `true` | Decrypted game saves |
| `backup_encrypted_saves` | `true` | Original encrypted saves |
| `backup_ps4_trophies` / `backup_ps5_trophies` | `true` | Trophy data |
| `daily_snapshots_to_keep` | `3` | Number of daily snapshots (0–10) |
| `max_title_size_mb` | `4096` | Maximum combined save-image size per title |

Custom absolute source paths may be added with the advanced editor. The web interface is deliberately restricted to the local network; **do not expose port 8755 directly to the internet**.

## Rest mode and HDMI-CEC

The dashboard attempts to restore its HTTP listener after the console resumes. A process still listed in PLDMGR does not, by itself, confirm that its HTTP service is reachable. The listener recovery needs validation on real hardware.

The automatic idle timer is refreshed during active copy and verification operations, using `sceSystemServicePowerTick()`. This is a best-effort idle-sleep measure, not a standby lock. To avoid standby when the TV turns off, disable **Settings → System → HDMI → Enable Power Off Link** on the PS5. **Enable One-Touch Play** can remain on.

Do not unplug USB drives or intentionally enter Rest Mode during a migration. A failed or interrupted migration retains the original data; verify its completion before changing drives.

## Build and tests

Requires the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) and its compiler toolchain.

```sh
make PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
sh tests/run-host-tests.sh
```

The build produces `ps5saveplus.elf`. GitHub Actions runs host regression tests and a PS5 SDK cross-compilation for release candidates.

## Licence and attribution

GPL-3.0. Based on [PS5Save](https://github.com/heni0xyz/PS5Save), which incorporates research and code from the PlayStation 5 save-mounting ecosystem. PS5Save+ uses the PS5 system-service idle timer approach documented by [KeepAwake](https://github.com/mortyzip/KeepAwake) (MIT). See [LICENSE](LICENSE) and source comments for attribution.

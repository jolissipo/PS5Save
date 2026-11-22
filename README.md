<img src="notification.png" width="128" height="128" alt="PS5Save+ icon" />

# PS5Save+

**Automatic save backups for jailbroken PS5 consoles — with a dashboard to keep everything under control.**

PS5Save+ builds on [PS5Save](https://github.com/heni0xyz/PS5Save). It retains the original project's automatic incremental backups, daily snapshots and game-aware protection, and adds easier management, smarter notifications and support for multiple USB drives.

## What it adds

- **A web dashboard.** Check the latest backup, browse saved files and daily snapshots, view free space and read logs from the PS5 or any browser on the local network.
- **Less intrusive notifications.** Meaningful progress, completion and error alerts, with console-language support and options to reduce interruptions during games.
- **Multi-USB management.** Detect existing backups automatically, select a drive and transfer a complete backup to another USB. Transfers are checked byte-for-byte; interrupted transfers can reuse verified files instead of starting from zero.
- **Settings without editing files.** Change backup options in the dashboard after entering a pairing code shown on the console. Each browser has its own temporary editing session.
- **Rest-mode recovery.** The dashboard and backup service recover after the console wakes, provided the payload process remains active. Automatic idle standby is inhibited during file copies; explicit rest commands remain possible.
- **One payload, one shortcut.** The ELF runs the backup service and dashboard and installs a PS5 home-screen shortcut if needed.

Backups cover decrypted PS4/PS5 saves, original encrypted save files, trophies and optional emulator folders. Saves belonging to active games are deferred until they can be copied safely.

## Get started

1. Connect a writable **exFAT USB drive** with enough space for your saves.
2. Download **[PS5Save+ v0.1.1-alpha](https://github.com/jolissipo/PS5Save/releases/tag/v0.1.1-alpha)** and load the ELF with a compatible PS5 payload loader.
3. Open the **PS5Save+** shortcut on the console, or visit `http://<PS5-IP>:8755/` from your local network.
4. To change options, select **Settings → Unlock editing on PS5** and enter the six-digit code shown on the console.

Backups start automatically when the payload launches, then run at the configured interval (30 seconds by default). The dashboard stays available during backups and USB transfers, while **Settings is temporarily disabled** until the operation finishes.

## Your backups and settings

Everything is stored on the selected USB drive:

```text
/mnt/usbN/ps5save-plus/
├── backup.conf             # Configuration
├── backup/                 # Current backups
├── backup_daily/           # Daily snapshots
├── events.log              # Diagnostics
└── history.tsv             # Backup history
```

### Default configuration

PS5Save+ creates `backup.conf` automatically on the selected USB drive if it does not already exist. Common options can be changed through **Settings** in the dashboard, without editing the file directly.

<details>
<summary>Preview the default backup.conf</summary>

```ini
# Destination and backup frequency
backup_usb_device=auto
backup_interval_seconds=30

# Console notifications
backup_notifications=all
backup_notifications_in_game=false

# Saved data
backup_ps4_saves=true
backup_ps5_saves=true
backup_encrypted_saves=true
backup_ps4_trophies=true
backup_ps5_trophies=true

# Size limit and daily snapshots
max_title_size_mb=4096
daily_snapshots_to_keep=3

# Optional emulator paths (remove # to enable)
#/data/PCSX2/Mcd001.ps2
#/data/PCSX2/Mcd002.ps2
```

</details>

The complete file, including examples for other emulators, is available in [`backup.conf`](backup.conf). The key settings are:

- **USB drive:** `auto` prefers an existing backup installation. Alternatively, specify a mounted drive such as `/mnt/usb1`; changing drives starts a verified transfer.
- **Check interval:** `backup_interval_seconds` accepts **30–3600 seconds**. The first check runs when the payload starts.
- **Notifications:** choose `all`, `errors` or `off`. Set `backup_notifications_in_game=true` to allow routine notifications during gameplay.
- **What to back up:** each PS4/PS5 saves, encrypted saves and trophies option accepts `true` or `false`.
- **Snapshots and file size:** `daily_snapshots_to_keep` accepts **0–10** (0 disables snapshots). `max_title_size_mb` is the per-title save-image limit in decimal MB.
- **Custom emulator files:** add one absolute path per line, without a setting name. Lines beginning with `#` are ignored; remove it to enable a listed path.

### Advanced editing

**Settings → Advanced: edit backup.conf directly** opens the complete configuration file. This is intended mainly for custom emulator paths or settings not exposed in the standard controls.

**Be careful:** **Save entire configuration** saves the whole edited file, not just one option. Preserve the existing settings and custom paths, use the correct `key=value` format for named options, and check paths before saving. Invalid settings or paths are rejected; after a successful save, the previous configuration is kept as `backup.conf.bak`. For ordinary changes, use the standard dashboard controls instead.

A transfer to another USB drive preserves the original data until the destination has been verified. If the transfer stops, the next attempt checks the temporary files before reusing or replacing them. An existing backup installation on the destination is never overwritten.

## Important notes

- **Compatibility:** tested on PS5 firmware **11.60**, including recovery after rest mode. Other firmware versions have not been verified here.
- **Power:** HDMI-CEC or a manual command can still put the console into rest mode. After a full reboot, the ELF must be launched again unless your loader handles autoload.
- **Access:** the dashboard uses HTTP on port **8755**. Keep it on a trusted local network; do not expose it to the internet.
- **Restore:** the dashboard lets you browse backups but does not restore saves. File verification during USB transfer compares the two copies; it does not prove that an original save is valid.

## Build

Requires the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk).

```sh
make PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
sh tests/run-host-tests.sh
```

GitHub Actions runs the host tests, compiles the PS5 ELF and publishes validated releases.

## Credits and licence

Based on **[PS5Save by heni0xyz](https://github.com/heni0xyz/PS5Save)** and its upstream contributors. Rest-mode HTTP recovery follows techniques used by [PLDMGR](https://github.com/itsPLK/ps5-payload-manager) and [PKG Manager](https://github.com/itsPLK/ps5-pkg-manager); idle-timer maintenance follows the approach in [KeepAwake](https://github.com/mortyzip/KeepAwake).

Licensed under [GPL-3.0](LICENSE).

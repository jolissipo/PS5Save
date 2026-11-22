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
2. Download **[PS5Save+ v0.1.0-alpha](https://github.com/jolissipo/PS5Save/releases/tag/v0.1.0-alpha)** and load the ELF with a compatible PS5 payload loader.
3. Open the **PS5Save+** shortcut on the console, or visit `http://<PS5-IP>:8755/` from your local network.
4. To change options, select **Settings → Unlock editing on PS5** and enter the six-digit code shown on the console.

Backups start automatically when the payload launches, then run at the configured interval (30 seconds by default). The dashboard is available while the payload is active.

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

Settings can be changed in the dashboard or directly in [`backup.conf`](backup.conf). The default configuration includes automatic USB selection, three retained daily snapshots and controls for backup types, intervals and notifications.

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

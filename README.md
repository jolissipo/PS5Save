# PS5Save+

**Automatic PS5 save backups, with clearer notifications and a built-in web dashboard.**

PS5Save+ is our fork of [PS5Save](https://github.com/heni0xyz/PS5Save). We created it to keep the original project's straightforward, reliable backup approach while improving the everyday experience: fewer interruptions, more visibility into saved data, simpler configuration and one payload for the entire workflow.

> **Alpha software.** The backup engine, dashboard and launcher have been tested on PS5 firmware 11.60. Multi-USB selection and cross-drive transfers are new in alpha.3 and still require console validation. Keep a separate copy of important saves.

## Features

- **Automatic incremental backups:** decrypted PS4/PS5 saves, original encrypted save files, trophy data and optional custom directories for emulators.
- **Daily snapshots:** dated copies of the live backup, with configurable retention.
- **Thoughtful notifications:** meaningful completion and error alerts, adaptive progress updates for longer operations, completion notifications sent immediately after the copy (on-screen timing depends on PS5 ShellUI), and automatic translation for selected console languages.
- **Web dashboard:** backup status, file explorer, separate live backups and snapshots, storage selection, history and diagnostic logs.
- **USB selection:** automatically preserves existing backups or chooses the writable USB with most free space on a fresh installation. Switch to another mounted USB from the dashboard or `backup.conf`; files are staged and compared byte-for-byte before the source is removed.
- **Easy settings:** edit `backup.conf` from the browser after authorising the session with a code shown on the PS5.
- **Home-screen shortcut:** automatically installs the PS5Save+ dashboard tile if missing. Subsequent launches do not reinstall it.
- **One ELF:** no separate launcher, database or web service required.

The backup service runs in the background while the payload remains active. Saves belonging to a running or suspended game are deferred until that title closes; for consistent original encrypted saves and trophy copies, close the game before the backup pass.

## Getting started

1. Connect an **exFAT USB drive** with enough free space for your backups and snapshots.
2. Download **`ps5saveplus_v0.1.0-alpha.4.elf`** from [Releases](https://github.com/jolissipo/PS5Save/releases) and launch it with your preferred PS5 payload loader.
3. On first launch, PS5Save+ selects a mounted USB, creates `/mnt/usbN/ps5save-plus/backup.conf` if missing, checks for changes immediately and installs the dashboard shortcut if needed.
4. Open the PS5Save+ shortcut, or use `http://<PS5-IP>:8755/` from a device on the same trusted network.

To change settings, open **Settings → Unlock editing on PS5**, then enter the temporary code shown on the console. In **Backup USB drive**, select another mounted USB; the transfer starts after the current backup check finishes. The dashboard shows the active USB and available space.

The shortcut opens the web dashboard; it **does not automatically restart the payload** after a console reboot.

### USB layout

```text
ps5save-plus/
├── backup.conf
├── backup/                    # Current backup
├── backup_daily/YYYY-MM-DD/   # Dated snapshots
├── events.log                 # Rotating diagnostics
└── history.tsv                # Backup activity
```

If an existing PS5Save+ or legacy PS5Save backup is connected, it takes priority over a blank USB, regardless of free space. Legacy root-level files are relocated without recopying them. For transfers **between two USB drives**, PS5Save+ copies the full directory to a staging folder, compares the files byte-for-byte and only then switches destination. An existing destination folder is never overwritten; failed transfers preserve the original. Keep an independent backup before testing cross-drive migration.

### Configuration

The [default backup.conf](backup.conf) can also be edited directly on the USB. Essential options:

| Setting | Default | Purpose |
| --- | --- | --- |
| `backup_usb_device` | `auto` | Auto-select or choose `/mnt/usb0`–`/mnt/usb7` |
| `backup_interval_seconds` | `30` | Interval between regular checks |
| `backup_notifications` | `all` | `all`, `errors` or `off` |
| `backup_notifications_in_game` | `false` | Suppress notifications while playing |
| `backup_ps4_saves` / `backup_ps5_saves` | `true` | Decrypted saves |
| `backup_encrypted_saves` | `true` | Original save files |
| `backup_ps4_trophies` / `backup_ps5_trophies` | `true` | Trophy data |
| `daily_snapshots_to_keep` | `3` | Number of daily snapshots retained |

Optional emulator paths can be enabled in `backup.conf` or added through the advanced editor.

## Build

Requires the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) and its Linux toolchain.

```sh
make PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
sh tests/run-host-tests.sh
```

The output is `ps5saveplus.elf`. Release downloads use a versioned filename. GitHub Actions runs host regression tests and cross-compiles the PS5 executable.

## Notes and credits

- **Security:** the dashboard uses HTTP on port **8755**. Keep it on a trusted local network; never expose it directly to the internet. Editing requires a temporary code displayed on the PS5.
- **Backup verification:** the dashboard inventories files present on USB; it does not verify every original save cryptographically. Cross-USB migrations do compare copied file contents, but do not independently validate the original saves.
- **Project origin:** based on PS5Save by [heni0xyz and contributors](https://github.com/heni0xyz/PS5Save). The fork retains and builds on their backup implementation. Additional research and components are credited in the source.
- **Licence:** [GPL-3.0](LICENSE).

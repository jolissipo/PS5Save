# PS5Save · jolissipo fork

PS5Save is a USB backup utility for jailbroken PS5 consoles. The original project was created by **heni0xyz** and developed with the upstream contributors. This fork is based on PS5Save v3. [Original project](https://github.com/heni0xyz/PS5Save) · [Upstream v3](https://github.com/heni0xyz/PS5Save/releases/tag/v3)

This fork was created to make automatic backups quieter. It adds notification modes, gameplay muting, less frequent progress updates and fewer repeated alerts. PS5Save supports PS4 and PS5 saves, trophies, emulator files and daily snapshots.

## Install

1. Download the `.elf` asset from a release tagged `jolissipo-v*`. [Fork releases](https://github.com/jolissipo/PS5Save/releases)
2. Copy the example configuration to `backup.conf` at the root of an exFAT USB drive. Enable the backup types you need and keep only the file paths you use. [Example configuration](backup.conf)
3. Connect the USB drive and load the ELF with your payload loader. PS5Save reads the configuration from `/mnt/usb0/backup.conf` and writes backups under `/mnt/usb0/backup`.

Decrypted saves from active or suspended titles are deferred until the title closes. Close the game before copying encrypted saves or trophy data for a consistent backup.

## Notifications

The example enables all save and trophy backup options and shows notifications during gameplay.

```ini
backup_notifications=all
backup_notifications_in_game=true
```

| Setting | Values | Default when omitted |
| --- | --- | --- |
| `backup_notifications` | `all` for status, progress, warnings and errors. `errors` for warnings and errors. `off` for visual silence. | `all` |
| `backup_notifications_in_game` | `true` follows the selected mode. `false` mutes toasts during possible game or app activity. | `true` |

In `all` mode, each pass shows **Checking for backup changes**. An unchanged source set reports **No changes. Backup not needed.** A pass with new data shows **Changes found. Backing up.** when copying starts and **Backup complete** on success. A snapshot-only pass has its own status. Empty configured folders report their name. Unreadable sources and skipped work report separate messages.

Progress shows up to three milestones at 25%, 50% and 75%. The first progress toast can appear after 30 seconds of copying, and additional progress toasts are at least 30 seconds apart. When several milestones are reached together, only the latest is shown. The previous `backup_progress_interval_seconds` setting is retired. Warnings and errors remain in the logs in every mode.

The PS5 controls pop-up duration through **Settings → Notifications → Display Time**. How this setting affects PS5Save's custom toasts still needs console validation. [PlayStation support](https://www.playstation.com/en-gb/support/account/manage-notifications-ps5/#settings)

The activity check can also mute toasts for suspended titles or when the activity state is unknown. The policy reloads on backup passes and stays in effect if the USB becomes unavailable. A first startup with an unreadable configuration uses the defaults.

## Build and test

With Clang/LLVM 18 and PS5 Payload SDK v0.45 installed on Linux, run the following from the repository root. [PS5 Payload SDK v0.45](https://github.com/ps5-payload-dev/sdk/releases/tag/v0.45)

```sh
make PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk LLVM_CONFIG="$(command -v llvm-config-18)"
```

The output is `backup.elf`. Run `tests/run-host-tests.sh` for the Linux notification tests. Toast rendering and activity detection still need console validation.

## Credits and license

- **heni0xyz** and upstream contributors for PS5Save and its backup engine. [Upstream repository](https://github.com/heni0xyz/PS5Save)
- **n0llptr** for PlayStation 5 Save Mounter. [Project](https://github.com/n0llptr/Playstation-5-Save-Mounter)
- **earthonion** for game title loading and save creation code. [Garlic SaveMgr](https://git.etawen.dev/earthonion/garlic-savemgr)
- **cow** for save mounting research using the PS5 `sceFs*` functions.
- **drakmor** and the ShadowMountPlus contributors for the rich notification transport. [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)

Distributed under the GNU General Public License v3. [License](LICENSE)

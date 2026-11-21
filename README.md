<img width="128" height="128" alt="PS5Save" src="notification.png" />

# PS5Save - Automatic game save and file backups!
Easy to use USB save backup utility for jailbroken PS5s that helps you prevent data loss!  
- Fully automatic with daily snapshots, supports PS5 saves, PS4 saves, emulator saves and other files
<img width="1280" height="720" alt="PS5Save notification screenshot" src="https://github.com/user-attachments/assets/cbef0c27-b218-4da1-943c-f6c0da6e0ef4" />


# Guide
- Get a fast USB storage device that is big enough to fit all of your saves (formatted as exFAT)
- Create a `backup.conf` file on the root of your USB
- Fill `backup.conf` with your personalized values (look at Backup Configuration section)
- Plug in into your PS5
- Push the backup.elf payload to your PS5 using a network loader or an autoloader of your choice

PS4/PS5 saves will backed up while in-game except for the game you're currently playing it will be backed up after you exit the title *(since version v2)*

# Backup Configuration
Example: 
```
# Delay between backup passes in seconds (int, min 30 max 3600)
backup_interval_seconds=30

# Show only warnings and errors, with game activity muted
backup_notifications=errors
backup_notifications_in_game=false

# Progress sampling interval used in all mode
backup_progress_interval_seconds=30

# Enable decrypted PS4 save backups (false, true)
backup_ps4_saves=true

# Enable decrypted PS5 save backups (false, true)
backup_ps5_saves=true

# Copy original encrypted PS4 and PS5 saves, including PS4 companion keys
backup_encrypted_saves=false

# Copy PS4 and PS5 trophy progress and catalogs to USB
backup_ps4_trophies=false
backup_ps5_trophies=false

# Maximum combined save-image size per title in MB (int)
max_title_size_mb=4096

# Number of dated snapshots retained, values 0 thru 10 are valid; 0 means disabled (int)
daily_snapshots_to_keep=2

# File backup section with most popular emulators listed

# PS5SX2
/data/PCSX2/Mcd001.ps2
/data/PCSX2/Mcd002.ps2

# PS5SX3
/data/homebrew/PPSA99303/rpcs3/dev_hdd0/home/

# Porpoise
/data/porpoise/saves/

# ProsperoEden
/data/prosperoeden/user/nand

# PS5X360
/data/homebrew/PPSA50011/saves
```

Encrypted saves and trophies default to off. Each setting works independently of decrypted save backups.

- Encrypted PS4 and PS5 saves keep their original filenames and directory structure under `/backup/user/home/<USER_ID>/savedata/` and `/backup/user/home/<USER_ID>/savedata_prospero/` on the USB. These copies include PS4 `.bin` companion files. The decrypted per-title size limit does not apply to these copies.
- Trophy backups copy the per-user `trophy` and `trophy2` directories and the shared catalogs under `/user/trophy` and `/user/trophy2` when present. The USB copies preserve those paths under `/backup`.
- These options copy files individually on regular backup passes. For a consistent copy of a game's save and trophy files, close the game before running a pass.

## Notifications

| Setting | Values | Default |
| --- | --- | --- |
| `backup_notifications` | `all` shows status, progress, warnings and errors. `errors` shows warnings and errors. `off` disables visual notifications. | `all` |
| `backup_notifications_in_game` | `true` follows the global mode in any context. `false` mutes visual notifications when game or app activity is possible. | `true` |
| `backup_progress_interval_seconds` | `5`–`3600` seconds between progress checks, or `0` to disable progress toasts. Used in `all` mode. | `30` |

For quiet automatic backups, use `backup_notifications=errors` and `backup_notifications_in_game=false`, as in the included `backup.conf`. Use `backup_notifications=off` for complete visual silence. The daemon reads the settings again on subsequent passes. The last valid value for a key in the current file wins. Invalid values are logged and the setting keeps its previous valid value from that read or its default. If the USB configuration becomes unavailable at runtime, the daemon uses the last policy it read. At cold start with an unreadable configuration it uses the defaults to report the startup error.

Passes with no changes stay quiet apart from startup status and problems that need attention. When there is work, progress can generate up to three toasts per pass as it crosses 25%, 50% and 75%. If one interval spans several milestones, a single toast shows the latest sampled percentage. Successful passes that copied data can show a completion message. The progress setting controls visual updates. Backup scheduling and copy behavior are independent of it.

Warnings and errors are logged even when visual notifications are disabled. Repeated observations of the same displayed problem are suppressed using a history of up to 256 entries. A regular pass that can fully evaluate the work clears old problems that no longer occur. Muted notifications are discarded. A persistent issue can still alert on a later pass if it has never been shown and notifications are allowed then.

With `backup_notifications_in_game=false`, PS5Save mutes toasts when it sees a non-system application process, an application sandbox mount, or an inconclusive activity scan. This can include a suspended game on the home screen. The foreground-related SDK symbols lack a documented call contract, so PS5Save checks for possible activity at send time. This check is independent of the save-protection state. [PS5 Payload SDK state exports](https://github.com/ps5-payload-dev/sdk/blob/v0.45/sce_stubs/libSceSystemService.c#L1482-L1496)

PS5Save keeps the rich JSON toast and embedded icon. The SDK shows how to send a toast with an ID and provides no documented update operation for an existing toast. [SDK notification example](https://github.com/ps5-payload-dev/sdk/blob/v0.45/samples/notify/main.c#L25-L85)

## Credits
- n0llptr — [Playstation 5 Save Mounter](https://github.com/n0llptr/Playstation-5-Save-Mounter)
- earthonion — loading game titles code and save creation: [Garlic SaveMgr](https://git.etawen.dev/earthonion/garlic-savemgr)
- cow — save mounting research using internal PS5 `sceFs*` functions

## Building

Requires Linux (for example Ubuntu 24.04) and the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk).

Install the build tools

```sh
sudo apt update
sudo apt install make clang-18 lld-18 llvm-18 wget unzip
```

Download and extract the SDK

```sh
wget https://github.com/ps5-payload-dev/sdk/releases/download/v0.45/ps5-payload-sdk.zip
unzip ps5-payload-sdk.zip
export PS5_PAYLOAD_SDK="$(pwd)/ps5-payload-sdk"
export LLVM_CONFIG="$(command -v llvm-config-18)"
```

Open the PS5Save source directory, then run:

```sh
make
```

The compiled payload is `backup.elf`.

To rebuild after changing the source:

```sh
make clean
make
```

Run the notification tests on Linux with `tests/run-host-tests.sh`. They use a host C compiler, simulated title activity and a stub for the console notification service. Visual rendering and foreground behavior need testing on a PS5.

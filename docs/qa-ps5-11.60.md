# PS5Save+ — PS5 console verification

This checklist complements the [main README](../README.md). The unified payload, dashboard, backups and home-screen shortcut were tested on **PS5 firmware 11.60** for the first public alpha.

## Backup and storage

- On a test USB drive, start `ps5saveplus.elf` and confirm that missing `ps5save-plus/backup.conf` is generated once, with an English Debug notification. An existing configuration must never be overwritten.
- If upgrading from the original PS5Save, verify that existing `backup.conf`, `backup/`, `backup_daily/` and the backup index migrate without overwriting data. Conflicting old/new backup directories must halt the process safely.
- Verify that only changed files are copied, saves of an active title are deferred until the title closes, and dated snapshots preserve their existing contents.
- Disconnect and reconnect a test USB and verify that backups resume without corrupting existing files.

## Dashboard and launcher

- The integrated launcher installs the `PSSV00001` shortcut once. Re-running the ELF must not reinstall it. The shortcut persists after reboot but does not start the backup service by itself.
- Open `http://<PS5-IP>:8755/` and check the live-backup and daily-snapshot explorers independently.
- Pair the web settings editor using the code displayed on the PS5; confirm the previous config remains available as `backup.conf.bak`.
- Keep the dashboard on a trusted LAN. Do not forward TCP 8755 to the public internet.

## Notifications

- Backups shorter than 10 seconds normally display only completion. Longer backups display sampled percentages at adaptive intervals.
- **New in v0.1.0-alpha.2:** after a percentage such as 99%, confirm that the completion toast appears immediately, without a 10-second display gap. It may replace the preceding progress toast. Warning and error messages must not be replaced.
- Confirm that notification controls (`all`, `errors`, `off` and in-game suppression) still work, and that failures produce useful diagnostics.

The host tests confirm emission timing and the Sony toast parameters. **Actual on-screen replacement timing requires the last console check.** File inventories are not cryptographic verification of backed-up saves.

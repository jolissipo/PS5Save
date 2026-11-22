# PS5Save+ — PS5 firmware 11.60 validation checklist

**Development-only build.** This plan is for a controlled physical-console test, not a public release.

## Gate 1 — Non-destructive smoke test

Prerequisites: PS5 on the supported exploit/payload-loader setup, a **spare USB drive** that does not contain valuable backups, and a PC/phone on the same trusted LAN.

1. Save the currently working PS5Save ELF and `backup.conf` separately. **Remove the normal backup USB device** before this test.
2. Use a spare USB device formatted in a PS5-readable filesystem. Create **`ps5save-plus/backup.conf`** with this minimal configuration to prevent the initial smoke test from copying saves:

   ```ini
   backup_interval_seconds=300
   backup_notifications=all
   backup_notifications_in_game=false
   backup_ps4_saves=false
   backup_ps5_saves=false
   backup_encrypted_saves=false
   backup_ps4_trophies=false
   backup_ps5_trophies=false
   daily_snapshots_to_keep=0
   ```

   No source paths are configured, so this stage does not intentionally copy saves. On a separate blank test USB, omit the file to verify automatic creation and the **Debug** notification. Do not perform that test with valuable existing data.
3. Obtain **`backup.elf` only** from the latest successful PS5 SDK GitHub Actions build. Do **not** execute `launcher-installer.elf` during this gate.
4. Launch `backup.elf` normally. **Important:** PS5Save+ uses the same singleton lock (loopback port 9055) as PS5Save; launching it will replace an existing PS5Save instance. It does not restore the old instance automatically.
5. Verify the PS5 displays exactly one startup confirmation, not a repeated notification each five minutes.
6. From the PC, open `http://<PS5-LAN-IP>:8755/`. Verify the dashboard responds, displays the USB state, and shows `backup.conf`.
7. Check the spare USB device contains `ps5save-plus/events.log` after normal activity. A no-change pass need not create a history entry.
8. Click **Unlock editing on PS5**. Verify the pairing toast appears on the console, confirm the six-digit code, and change only `backup_interval_seconds` from 300 to 120.
9. Check that the USB's `ps5save-plus/backup.conf` now contains 120 and `ps5save-plus/backup.conf.bak` contains the previous 300. Verify the displayed interval changes after the next scheduled pass. Restore 300 via the dashboard.
10. Confirm attempts to edit without a valid pairing code are rejected. Do not expose port 8755 outside the trusted LAN.

**Abort and restore the previous ELF** if the console freezes, the USB changes unexpectedly, configuration is corrupted, the dashboard becomes unreliable, or console notifications fail. Keep the original payload and its USB backup configuration intact.

## Gate 2 — Backup correctness and migration (only after Gate 1 passes)

- Test a **copy of a legacy USB layout**: place `backup.conf`, `backup/` (with its existing `.ps5save-index`) and `backup_daily/` in the test USB root. Confirm the first run moves them to `ps5save-plus/` without data loss, rewrites the change index and does not recopy unchanged files.
- Simulate an interrupted migration on a spare drive; confirm it can resume and that **simultaneous legacy and new backup directories cause an explicit startup failure** without replacing files.
- Verify that a missing configuration is created once from the built-in defaults, with a **Debug** notification; a subsequent launch must neither recreate nor overwrite it.
- Check that the **live backup** panel and the **daily snapshots** panel each show their own directories. Opening files in one must leave the other panel unchanged.

- With a separate source **containing dispensable test data**, verify the initial copy, unchanged pass, changed-file copy and preservation of original data.
- Exercise snapshots with `daily_snapshots_to_keep=2`, verify that completed dated folders do not change within the same day, and verify retention on later dates.
- In **Backups**, open the live folder tree and a dated snapshot. Check nested filenames, individual sizes, modification dates, and navigation through more than 64 items using **Show more files**.
- Open each snapshot's inventory; compare counted files and bytes with the actual USB folder. Confirm that any limited or interrupted scan explicitly reports an incomplete inventory, and does not claim all source saves were included.
- Test a deliberately long backup: after approximately 10 seconds the native toast should show the actual current percent (not 25/50/75 milestones). For longer copies, verify that notification cadence adapts between 10 and 30 seconds, never repeats an unchanged percent, and that quick backups show only completion. Progress is work-unit based, not a byte-accurate throughput measurement.
- Test low USB free space, a missing source and a write/read failure in a controlled environment. Confirm the notifier gives a useful cause without bursts; verify the persistent log retains details.
- Verify a previously alerted problem generates only one recovery notice after resolution.
- With **test-game saves**, verify active/suspended-title deferral, resumption after game exit, decrypted backup, original encrypted backup and expected file consistency.
- Verify USB disconnect/reconnect and PS5 rest mode do not crash the backup daemon and do not corrupt the configuration or backups.

## Gate 3 — Optional home-screen launcher (do not run yet)

The separate installer changes console application metadata. Before running it:

- Review the chosen title ID `P5SV00001` for collisions with installed content.
- Confirm the direct SDK registration API and title existence query on firmware 11.60. The separate installer now emits **Debug** notifications for the current step, relevant error codes and successful registration; note the actual output before changing anything else.
- Confirm the icon opens `http://127.0.0.1:8755/` and that it stays functional after console reboot and rest mode. A second installer execution must report an existing registration and avoid reinstalling.
- Design and validate **safe uninstall/rollback** using Sony's app-management interface. No automatic uninstall exists in this branch.
- Only then test the dedicated `launcher-installer.elf` with a backup of relevant console state.

## Validation status

- [x] Host regression scenarios pass in GitHub Actions.
- [x] Embedded dashboard JavaScript passes syntax checks.
- [x] PS5 `backup.elf` cross-compiles with official SDK v0.50.
- [x] Optional launcher-installer cross-compiles with official SDK v0.50.
- [ ] Gate 1 completed on a physical PS5 (FW 11.60).
- [ ] Gate 2 completed on a physical PS5 (FW 11.60).
- [ ] Gate 3 completed on a physical PS5 (FW 11.60).

Do not merge the development PR or mark a version as stable based on cross-compilation alone.

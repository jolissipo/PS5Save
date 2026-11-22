"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");

const html = fs.readFileSync("dashboard/index.html", "utf8");
const configuration = fs.readFileSync("backup.conf", "utf8");

function extractFunction(name, next) {
  const start = html.indexOf("  function " + name + "(");
  const end = html.indexOf("  function " + next + "(", start);
  assert(start >= 0 && end > start,
    "Cannot find dashboard function " + name);
  return html.slice(start, end);
}

// Execute the actual dashboard functions rather than duplicating their
// regular expressions in the tests.
const context = {};
vm.createContext(context);
vm.runInContext(
  extractFunction("parseConfigurationValues", "usbOptions") +
  extractFunction("replaceKey", "saveConfiguration"),
  context,
  {filename: "dashboard-inline.js"}
);

const values = context.parseConfigurationValues(configuration);
assert.equal(values.backup_usb_device, "auto");
assert.equal(values.backup_interval_seconds, "30");
assert.equal(values.backup_notifications, "all");
assert.equal(values.backup_notifications_in_game, "false");
assert.equal(values.backup_ps4_saves, "true");
assert.equal(values.backup_ps5_saves, "true");
assert.equal(values.backup_encrypted_saves, "true");
assert.equal(values.backup_ps4_trophies, "true");
assert.equal(values.backup_ps5_trophies, "true");
assert.equal(values.daily_snapshots_to_keep, "3");
assert.equal(values.max_title_size_mb, "4096");

const unchanged = context.replaceKey(configuration,
  "backup_usb_device", "auto");
assert.equal(unchanged, configuration);

const moved = context.replaceKey(configuration,
  "backup_usb_device", "/mnt/usb1");
assert.equal(moved,
  configuration.replace("backup_usb_device=auto",
                        "backup_usb_device=/mnt/usb1"));
assert.equal(context.parseConfigurationValues(moved).backup_usb_device,
  "/mnt/usb1");
assert.equal(context.parseConfigurationValues(moved).backup_ps5_saves,
  "true");

const custom = "# Custom source\n/data/my-emulator/saves/\n" +
  "backup_usb_device=auto\nmax_title_size_mb=2048\n";
const changed = context.replaceKey(custom, "backup_usb_device", "/mnt/usb2");
assert.equal(changed,
  "# Custom source\n/data/my-emulator/saves/\n" +
  "backup_usb_device=/mnt/usb2\nmax_title_size_mb=2048\n");

// Reject accidental regressions that submit an uninitialised form.
assert.match(html, /name="max_title_size_mb"[^>]*\brequired\b/);
assert.match(html, /if\(!form\.checkValidity\(\)\)/);

console.log("PASS dashboard loads existing USB/backup options and preserves them when changing USB");

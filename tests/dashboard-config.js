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


/* Test the actual busy-state logic for both backup checks and USB transfers,
 * including an already-open Settings tab and a paired browser. */
const preferenceControls = [{disabled: false}, {disabled: false}];
const pairControls = [{disabled: false}, {disabled: false}];
const sections = {
  settings: {hidden: false}, overview: {hidden: true},
  preferences: {querySelectorAll: () => preferenceControls},
  pairForm: {querySelectorAll: () => pairControls},
  rawConfig: {disabled: false},
  saveRaw: {disabled: false},
  requestPair: {disabled: false}
};
function tab(name, pressed) {
  return {
    disabled: false,
    attrs: {"data-tab": name, "aria-pressed": pressed},
    getAttribute(key) { return this.attrs[key]; },
    setAttribute(key, value) { this.attrs[key] = value; }
  };
}
const overviewTab = tab("overview", "false");
const settingsTab = tab("settings", "true");
context.byId = id => sections[id];
context.document = {
  querySelector(selector) {
    assert.equal(selector, 'nav button[data-tab="settings"]');
    return settingsTab;
  },
  querySelectorAll(selector) {
    assert.equal(selector, "nav button");
    return [overviewTab, settingsTab];
  }
};
context.sessionToken="";
context.settingsBusy=false;
vm.runInContext(
  extractFunction("syncBusyEditing", "status"),
  context, {filename: "dashboard-tabs.js"}
);
context.syncBusyEditing(true);
assert.equal(settingsTab.disabled, true);
assert.equal(sections.settings.hidden, true);
assert.equal(sections.overview.hidden, false);
assert.equal(settingsTab.getAttribute("aria-pressed"), "false");
assert.equal(overviewTab.getAttribute("aria-pressed"), "true");
assert.ok(preferenceControls.every(control => control.disabled));
assert.ok(pairControls.every(control => control.disabled));
assert.equal(sections.rawConfig.disabled, true);
assert.equal(sections.saveRaw.disabled, true);
assert.equal(sections.requestPair.disabled, true);
context.syncBusyEditing(false);
assert.equal(settingsTab.disabled, false);
assert.ok(preferenceControls.every(control => !control.disabled));
assert.ok(pairControls.every(control => !control.disabled));
assert.equal(sections.rawConfig.disabled, false);
assert.equal(sections.saveRaw.disabled, false);
assert.equal(sections.requestPair.disabled, false);
context.sessionToken="paired-browser";
context.syncBusyEditing(false);
assert.equal(sections.requestPair.disabled, true, "pairing remains independent");
context.sessionToken="";
sections.settings.hidden=false;
sections.overview.hidden=true;
context.syncBusyEditing(true); /* Same lock for USB transfer. */
assert.equal(settingsTab.disabled, true);
context.syncBusyEditing(false);
assert.match(html, /if\(button\.disabled\)return;/);
assert.match(html, /busy=!!\(x\.usbMoving \|\| x\.running\)/);
assert.match(html, /Backup in progress\. You can view the dashboard and logs; Settings is temporarily disabled\./);
assert.match(html, /USB backup transfer in progress/);
assert.match(html, /setInterval\(status,3000\)/);
console.log("PASS Settings lock during backup checks and USB transfers");

/* The editing indicator belongs to this page's token, not a global console
 * flag. Another browser starts locked and can authorize independently. */
const editorNodes={
  requestPair:{disabled:false,textContent:"Unlock editing on PS5"},
  pairForm:{hidden:false},settingsForm:{hidden:true},advanced:{hidden:true}
};
context.byId = id => editorNodes[id];
context.settingsBusy = false;
vm.runInContext(extractFunction("setEditingState","verifyEditorSession"),context,
  {filename:"dashboard-editor-state.js"});
context.setEditingState(true);
assert.equal(editorNodes.requestPair.disabled,true);
assert.equal(editorNodes.requestPair.textContent,"Editing already enabled");
assert.equal(editorNodes.pairForm.hidden,true);
assert.equal(editorNodes.settingsForm.hidden,false);
assert.equal(editorNodes.advanced.hidden,false);
context.setEditingState(false);
assert.equal(editorNodes.requestPair.disabled,false);
assert.equal(editorNodes.requestPair.textContent,"Unlock editing on PS5");
assert.equal(editorNodes.pairForm.hidden,true);
assert.equal(editorNodes.settingsForm.hidden,true);
assert.equal(editorNodes.advanced.hidden,true);
assert.match(html,/if\(sessionToken\)\{\s*text\("pairMessage","Editing already enabled in this browser\."\)/);
assert.match(html,/post\("\/api\/session","",\{"X-PS5Save-Session":checked\}\)/);
assert.match(html,/error\.status!==403/);
console.log("PASS editing button state and browser-local token validation");

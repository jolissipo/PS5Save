/* PS5Save+ USB selection. Only mounted /mnt/usb0..7 are eligible.
 * Never use internal PS5 storage as the destination for backup data. */
#ifndef PS5SAVE_USB_STORAGE_H
#define PS5SAVE_USB_STORAGE_H
#define STORAGE_USB_SLOTS 8
#ifndef STORAGE_USB_PREFIX
#define STORAGE_USB_PREFIX "/mnt/usb"
#endif
#define STORAGE_ACTIVE_MARKER ".ps5save-active"

static char storage_requested_usb[32] = "";
static atomic_int storage_migrating;
static pthread_mutex_t storage_change_lock = PTHREAD_MUTEX_INITIALIZER;
static char storage_move_error[256] = "";

static int storage_usb_number(const char *path) {
  size_t prefix = strlen(STORAGE_USB_PREFIX);
  if(!path || strncmp(path, STORAGE_USB_PREFIX, prefix) ||
     path[prefix] < '0' || path[prefix] > '7' ||
     path[prefix + 1] != '\0') return -1;
  return path[prefix] - '0';
}

static void storage_set_paths(const char *root) {
  snprintf(storage_usb_path, sizeof storage_usb_path, "%s", root);
  snprintf(storage_root_path, sizeof storage_root_path, "%s/ps5save-plus", root);
  snprintf(storage_conf_path, sizeof storage_conf_path, "%s/backup.conf", STORAGE_ROOT);
  snprintf(storage_backup_path, sizeof storage_backup_path, "%s/backup", STORAGE_ROOT);
  snprintf(storage_daily_path, sizeof storage_daily_path, "%s/backup_daily", STORAGE_ROOT);
  snprintf(storage_legacy_conf, sizeof storage_legacy_conf, "%s/backup.conf", root);
  snprintf(storage_legacy_bak, sizeof storage_legacy_bak, "%s/backup.conf.bak", root);
  snprintf(storage_legacy_backup, sizeof storage_legacy_backup, "%s/backup", root);
  snprintf(storage_legacy_daily, sizeof storage_legacy_daily, "%s/backup_daily", root);
  snprintf(storage_conf_bak, sizeof storage_conf_bak, "%s/backup.conf.bak", STORAGE_ROOT);
  snprintf(storage_conf_part, sizeof storage_conf_part, "%s/backup.conf.part", STORAGE_ROOT);
  snprintf(storage_log_path, sizeof storage_log_path, "%s/events.log", STORAGE_ROOT);
  snprintf(storage_log_rotated, sizeof storage_log_rotated, "%s/events.log.1", STORAGE_ROOT);
  snprintf(storage_history_path, sizeof storage_history_path, "%s/history.tsv", STORAGE_ROOT);
  snprintf(storage_history_rotated, sizeof storage_history_rotated, "%s/history.tsv.1", STORAGE_ROOT);
  snprintf(storage_index_path, sizeof storage_index_path, "%s/.ps5save-index", BACKUP_ROOT);
  snprintf(storage_index_part, sizeof storage_index_part, "%s/.ps5save-index.part", BACKUP_ROOT);
  snprintf(storage_installer_log, sizeof storage_installer_log, "%s/launcher-installer.log", STORAGE_ROOT);
}

static int storage_usb_online(const char *root, uint64_t *free_bytes,
                              uint64_t *total_bytes) {
  if(storage_usb_number(root) < 0) return 0;
  struct stat st;
  struct statfs fs;
  if(lstat(root, &st) || !S_ISDIR(st.st_mode) ||
     statfs(root, &fs)) return 0;
#ifndef PS5SAVE_USB_TEST
  /* /mnt/usbN without a mounted drive can be an internal placeholder. */
  if(strcmp(fs.f_mntonname, root) || (fs.f_flags & MNT_RDONLY)) return 0;
#endif
  if(free_bytes) *free_bytes = (uint64_t)fs.f_bavail * (uint64_t)fs.f_bsize;
  if(total_bytes) *total_bytes = (uint64_t)fs.f_blocks * (uint64_t)fs.f_bsize;
  return 1;
}

/* A moved or existing library outranks free-space selection. An active marker
 * identifies our selected volume even when the console remaps USB numbers.
 * A config that names its own current mount path wins over stale copies.
 */
static int storage_priority(const char *root) {
  char path[PATH_MAX], wanted[48];
  snprintf(path, sizeof path, "%s/ps5save-plus/backup.conf", root);
  FILE *f = fopen(path, "r");
  if(f) {
    int matches = 0;
    char line[256];
    while(fgets(line, sizeof line, f)) {
      if(sscanf(line, "backup_usb_device=%47s", wanted) == 1 &&
         !strcmp(wanted, root)) matches = 1;
    }
    fclose(f);
    if(matches) return 5;
  }
  snprintf(path, sizeof path, "%s/ps5save-plus/%s", root, STORAGE_ACTIVE_MARKER);
  struct stat st;
  if(!lstat(path, &st) && S_ISREG(st.st_mode)) return 4;
  snprintf(path, sizeof path, "%s/ps5save-plus/backup.conf", root);
  if(!stat(path, &st) && S_ISREG(st.st_mode)) return 3;
  snprintf(path, sizeof path, "%s/backup.conf", root);
  if(!stat(path, &st) && S_ISREG(st.st_mode)) return 2;
  snprintf(path, sizeof path, "%s/ps5save-plus/backup", root);
  if(!stat(path, &st) && S_ISDIR(st.st_mode)) return 1;
  return 0;
}

static int storage_choose_initial_usb(void) {
  int selected = -1, highest = -1;
  uint64_t best_free = 0;
  for(int i = 0; i < STORAGE_USB_SLOTS; i++) {
    char mount[PATH_MAX];
    snprintf(mount, sizeof mount, "%s%d", STORAGE_USB_PREFIX, i);
    uint64_t available = 0;
    if(!storage_usb_online(mount, &available, NULL)) continue;
    int rank = storage_priority(mount);
    if(selected < 0 || rank > highest ||
       (rank == highest && available > best_free)) {
      selected = i; highest = rank; best_free = available;
    }
  }
  if(selected < 0) return -1;
  char root[PATH_MAX];
  snprintf(root, sizeof root, "%s%d", STORAGE_USB_PREFIX, selected);
  storage_set_paths(root);
  return 0;
}

/* This marker is not backup content. Never remove or overwrite any other
 * drive's data to establish selection. */
static int storage_mark_active(void) {
  char mark[PATH_MAX];
  if(snprintf(mark, sizeof mark, "%s/%s", STORAGE_ROOT,
              STORAGE_ACTIVE_MARKER) >= (int)sizeof mark) return -1;
  struct stat existing;
  if(!lstat(mark, &existing))
    return S_ISREG(existing.st_mode) ? 0 : -1;
  if(errno != ENOENT) return -1;
  int fd = open(mark, O_CREAT | O_WRONLY | O_EXCL, 0600);
  if(fd < 0) return -1;
  char data[48];
  int n = snprintf(data, sizeof data, "%s\n", USB_ROOT);
  int rc = n > 0 && (size_t)n < sizeof data &&
           write(fd, data, (size_t)n) == n && !fsync(fd) ? 0 : -1;
  if(close(fd)) rc = -1;
  return rc;
}

#endif

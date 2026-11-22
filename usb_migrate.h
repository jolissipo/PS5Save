/* Explicit cross-volume migration. Never writes backup data to internal storage.
 * The destination is staged, compared byte-for-byte and committed atomically
 * on its own USB volume before any source data may be deleted.
 */
#ifndef PS5SAVE_USB_MIGRATE_H
#define PS5SAVE_USB_MIGRATE_H

#define USB_STAGE_NAME ".ps5save-plus-transfer.part"
#define USB_STAGE_MARK ".ps5save-owned-transfer"
#define USB_STAGE_MAGIC "PS5Save+ USB transfer v1\n"
#define USB_MIGRATION_DEPTH 48

static int usb_transfer_file(const char *source, const char *target,
                             const struct stat *source_st) {
  if(copy_file_keep_time(source, target, source_st)) return -1;
  struct stat after;
  if(lstat(source, &after) || after.st_size != source_st->st_size ||
     after.st_mtime != source_st->st_mtime ||
     !S_ISREG(after.st_mode)) return -1;
  return 0;
}

static int usb_transfer_tree(const char *source, const char *target,
                             unsigned depth) {
  if(depth >= USB_MIGRATION_DEPTH) return -1;
  if(mkdir(target, 0700) && errno != EEXIST) return -1;
  DIR *d = opendir(source);
  if(!d) return -1;
  int result = 0;
  struct dirent *entry;
  errno = 0;
  while(!result && (entry = readdir(d))) {
    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    /* The selection marker is recreated on the new volume after commit. */
    if(!strcmp(entry->d_name, STORAGE_ACTIVE_MARKER)) continue;
    char from[PATH_MAX], to[PATH_MAX];
    struct stat st;
    if(snprintf(from, sizeof from, "%s/%s", source, entry->d_name) >= (int)sizeof from ||
       snprintf(to, sizeof to, "%s/%s", target, entry->d_name) >= (int)sizeof to ||
       lstat(from, &st)) {result = -1; break;}
    if(S_ISDIR(st.st_mode)) result = usb_transfer_tree(from, to, depth + 1);
    else if(S_ISREG(st.st_mode)) result = usb_transfer_file(from, to, &st);
    else result = -1; /* Refuse symlinks and unsupported nodes. */
    errno = 0;
  }
  if(!entry && errno) result = -1;
  closedir(d);
  return result;
}

static int usb_compare_file(const char *from, const char *to) {
  int f = open(from, O_RDONLY | O_NOFOLLOW);
  if(f < 0) return -1;
  int g = open(to, O_RDONLY | O_NOFOLLOW);
  if(g < 0) {close(f);return -1;}
  unsigned char a[32768], b[32768];
  int result = 0;
  for(;;) {
    ssize_t n = read(f, a, sizeof a);
    if(n < 0 && errno == EINTR) continue;
    if(n < 0) {result = -1;break;}
    if(n == 0) break;
    size_t used = 0;
    while(used < (size_t)n) {
      ssize_t m = read(g, b + used, (size_t)n - used);
      if(m < 0 && errno == EINTR) continue;
      if(m <= 0) {result = -1;break;}
      used += (size_t)m;
    }
    if(result || memcmp(a, b, (size_t)n)) {result = -1;break;}
  }
  if(!result) {
    unsigned char tail;
    ssize_t n = read(g, &tail, 1);
    if(n != 0) result = -1;
  }
  if(close(f) || close(g)) result = -1;
  return result;
}

static int usb_verify_tree(const char *source, const char *target,
                            unsigned depth) {
  if(depth >= USB_MIGRATION_DEPTH) return -1;
  DIR *d = opendir(source);
  if(!d) return -1;
  int result = 0;
  struct dirent *entry;
  errno = 0;
  while(!result && (entry = readdir(d))) {
    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
       !strcmp(entry->d_name, STORAGE_ACTIVE_MARKER)) continue;
    char from[PATH_MAX], to[PATH_MAX];
    struct stat src, dst;
    if(snprintf(from, sizeof from, "%s/%s", source, entry->d_name) >= (int)sizeof from ||
       snprintf(to, sizeof to, "%s/%s", target, entry->d_name) >= (int)sizeof to ||
       lstat(from, &src) || lstat(to, &dst)) {result = -1;break;}
    if(S_ISDIR(src.st_mode) && S_ISDIR(dst.st_mode))
      result = usb_verify_tree(from, to, depth + 1);
    else if(S_ISREG(src.st_mode) && S_ISREG(dst.st_mode) &&
            src.st_size == dst.st_size)
      result = usb_compare_file(from, to);
    else result = -1;
    errno = 0;
  }
  if(!entry && errno) result = -1;
  closedir(d);
  return result;
}

static int usb_target_ready(const char *target) {
  uint64_t free_bytes;
  if(!storage_usb_online(target, &free_bytes, NULL)) return 0;
  struct stat from, to;
  if(stat(USB_ROOT, &from) || stat(target, &to) ||
     from.st_dev == to.st_dev || !S_ISDIR(to.st_mode)) return 0;
  uint64_t needed = dir_size(STORAGE_ROOT) + SPACE_MARGIN;
  return free_bytes > needed;
}

static int usb_migrate_to(const char *target) {
  if(storage_usb_number(target) < 0 || !strcmp(target, USB_ROOT)) return 0;
  if(!storage_usb_online(USB_ROOT, NULL, NULL) ||
     !usb_target_ready(target)) {
    snprintf(storage_move_error, sizeof storage_move_error,
             "Destination USB unavailable or has insufficient free space.");
    return -1;
  }
  char source[PATH_MAX], destination[PATH_MAX], stage[PATH_MAX];
  snprintf(source, sizeof source, "%s", STORAGE_ROOT);
  if(snprintf(destination, sizeof destination, "%s/ps5save-plus", target) >= (int)sizeof destination ||
     snprintf(stage, sizeof stage, "%s/%s", target, USB_STAGE_NAME) >= (int)sizeof stage)
    return -1;

  struct stat st;
  if(!lstat(destination, &st)) {
    snprintf(storage_move_error, sizeof storage_move_error,
             "Destination contains ps5save-plus already. Refusing to overwrite.");
    return -1;
  }
  if(errno != ENOENT) return -1;

  if(!lstat(stage, &st)) {
    /* Never delete an unrecognised folder from someone else's USB. */
    char marker[PATH_MAX], contents[64] = "";
    snprintf(marker, sizeof marker, "%s/%s", stage, USB_STAGE_MARK);
    FILE *f = fopen(marker, "r");
    if(f) {fgets(contents, sizeof contents, f);fclose(f);}
    if(strcmp(contents, USB_STAGE_MAGIC) || remove_tree(stage)) {
      snprintf(storage_move_error, sizeof storage_move_error,
               "Unexpected previous transfer directory; refusing to delete.");
      return -1;
    }
  } else if(errno != ENOENT) return -1;

  atomic_store(&storage_migrating, 1);
  int ok = 0;
  if(!mkdir(stage, 0700)) {
    char marker[PATH_MAX];
    snprintf(marker, sizeof marker, "%s/%s", stage, USB_STAGE_MARK);
    FILE *f = fopen(marker, "w");
    if(f) {
      int written = fputs(USB_STAGE_MAGIC, f) >= 0 && !fflush(f) && !fsync(fileno(f));
      if(fclose(f)) written = 0;
      if(written && !usb_transfer_tree(source, stage, 0) &&
         !usb_verify_tree(source, stage, 0)) {
        sync();
        if(!rename(stage, destination)) ok = 1;
      }
    }
  }

  if(!ok) {
    snprintf(storage_move_error, sizeof storage_move_error,
             "USB transfer or byte-for-byte verification failed. Original preserved.");
    atomic_store(&storage_migrating, 0);
    return -1; /* The marked staging directory is recoverable on next try. */
  }

  /* All source data is still present here. Commit the new mount selection. */
  char old_backup[PATH_MAX];
  snprintf(old_backup, sizeof old_backup, "%s", BACKUP_ROOT);
  storage_set_paths(target);
  char stage_marker[PATH_MAX], active[PATH_MAX];
  snprintf(stage_marker, sizeof stage_marker, "%s/%s", STORAGE_ROOT, USB_STAGE_MARK);
  unlink(stage_marker);
  snprintf(active, sizeof active, "%s/%s", STORAGE_ROOT, STORAGE_ACTIVE_MARKER);
  unlink(active); /* old marker content named the previous USB mount */
  index_rebase_usb(old_backup, BACKUP_ROOT);
  index_save();
  if(storage_mark_active()) {
    /* The fully copied target remains intact even if the marker fails. */
    logq("USB migration: could not save selection marker.");
  }
  /* Delete originals only after verified destination data is committed. */
  if(remove_tree(source)) {
    logq("USB migration completed, but old backup could not be deleted: %s", source);
  }
  storage_move_error[0] = 0;
  atomic_store(&storage_migrating, 0);
  logq("Backup storage transferred to %s", USB_ROOT);
  notify_info("Backup storage moved to %s.", USB_ROOT);
  return 0;
}
#endif

/* Persistent metadata cache. No file contents are read to check for changes.
 * A missing/invalid cache causes a safe recopy, never a content-hash shortcut. */
#ifndef CHANGE_INDEX_H
#define CHANGE_INDEX_H
#ifdef PS5SAVE_DYNAMIC_STORAGE
#define INDEX_PATH storage_index_path
#else
#define INDEX_PATH BACKUP_ROOT "/.ps5save-index"
#endif
#define INDEX_LIMIT 100000
#define INDEX_BUCKETS 4096

typedef struct file_signature {
  int64_t size, mtime, mtime_ns, ctime, ctime_ns;
  uint64_t inode, device;
} file_signature_t;

typedef struct change_record {
  struct change_record *next;
  char *path;
  file_signature_t source, target;
} change_record_t;
static change_record_t *change_index[INDEX_BUCKETS];
static unsigned index_count;
static int index_dirty;

static file_signature_t
file_signature(const struct stat *st) {
  file_signature_t sig;
  memset(&sig, 0, sizeof sig);
  sig.size = st->st_size;
  sig.mtime = st->st_mtimespec.tv_sec;
  sig.mtime_ns = st->st_mtimespec.tv_nsec;
  sig.ctime = st->st_ctimespec.tv_sec;
  sig.ctime_ns = st->st_ctimespec.tv_nsec;
  sig.inode = st->st_ino;
  sig.device = st->st_dev;
  return sig;
}

static int
source_same(const struct stat *a, const struct stat *b) {
  file_signature_t sa = file_signature(a), sb = file_signature(b);
  return !memcmp(&sa, &sb, sizeof sa);
}

static unsigned
index_bucket(const char *path) {
  uint64_t h = 14695981039346656037ULL;
  for(const unsigned char *p = (const unsigned char *)path; *p; p++)
    h = (h ^ *p) * 1099511628211ULL;
  return h % INDEX_BUCKETS;
}

static change_record_t *
index_record(const char *path, int create) {
  unsigned bucket = index_bucket(path);
  for(change_record_t *r = change_index[bucket]; r; r = r->next)
    if(!strcmp(r->path, path)) return r;
  if(!create || index_count == INDEX_LIMIT) return NULL;
  change_record_t *r = calloc(1, sizeof *r);
  if(!r) return NULL;
  r->path = strdup(path);
  if(!r->path) {free(r); return NULL;}
  r->next = change_index[bucket]; change_index[bucket] = r; index_count++;
  return r;
}

static void
index_clear(void) {
  for(unsigned i = 0; i < INDEX_BUCKETS; i++) {
    while(change_index[i]) {
      change_record_t *r = change_index[i];
      change_index[i] = r->next; free(r->path); free(r);
    }
  }
  index_count = 0; index_dirty = 0;
}

/* After a verified cross-USB copy, rebase cached destination paths and
 * refresh target filesystem signatures. Original source signatures are
 * retained so unchanged saves do not need to be copied again. */
#ifdef PS5SAVE_DYNAMIC_STORAGE
static void index_rebase_usb(const char *from, const char *to) {
  change_record_t *items = NULL;
  for(unsigned bucket = 0; bucket < INDEX_BUCKETS; bucket++) {
    change_record_t *r = change_index[bucket];
    change_index[bucket] = NULL;
    while(r) {
      change_record_t *next = r->next;
      r->next = items;
      items = r;
      r = next;
    }
  }
  size_t prefix = strlen(from);
  index_count = 0;
  for(change_record_t *r = items; r;) {
    change_record_t *next = r->next;
    char newpath[PATH_MAX];
    struct stat st;
    if(strncmp(r->path, from, prefix) || r->path[prefix] != '/' ||
       snprintf(newpath, sizeof newpath, "%s%s", to, r->path + prefix) >= (int)sizeof newpath ||
       lstat(newpath, &st) || !S_ISREG(st.st_mode)) {
      free(r->path);
      free(r);
    } else {
      char *updated = strdup(newpath);
      if(!updated) {free(r->path); free(r);}
      else {
        free(r->path);
        r->path = updated;
        r->target = file_signature(&st);
        unsigned bucket = index_bucket(r->path);
        r->next = change_index[bucket];
        change_index[bucket] = r;
        index_count++;
      }
    }
    r = next;
  }
  index_dirty = 1;
}
#endif

static int
file_current(const char *dst, const struct stat *src) {
  change_record_t *r = index_record(dst, 0);
  struct stat target;
  if(!r || stat(dst, &target) || !S_ISREG(target.st_mode)) return 0;
  file_signature_t s = file_signature(src), t = file_signature(&target);
  return !memcmp(&s, &r->source, sizeof s) && !memcmp(&t, &r->target, sizeof t);
}

static void
index_update(const char *dst, const struct stat *src) {
  struct stat target;
  if(stat(dst, &target)) return;
  change_record_t *r = index_record(dst, 1);
  if(!r) {logq("Change cache full/unavailable; file will be checked again: %s", dst); return;}
  r->source = file_signature(src); r->target = file_signature(&target);
  index_dirty = 1;
}

static void
index_load(void) {
  index_clear();
  FILE *f = fopen(INDEX_PATH, "rb");
  if(!f) return;
  char magic[8];
  int valid = fread(magic, 1, 8, f) == 8 && !memcmp(magic, "PS5IDX2\n", 8);
  while(valid) {
    uint32_t len;
    size_t n = fread(&len, 1, sizeof len, f);
    if(!n && !ferror(f)) break;
    if(n != sizeof len || !len || len >= PATH_MAX || index_count == INDEX_LIMIT) {valid = 0; break;}
    char path[PATH_MAX];
    file_signature_t source, target;
    if(fread(path, 1, len, f) != len ||
       fread(&source, 1, sizeof source, f) != sizeof source ||
       fread(&target, 1, sizeof target, f) != sizeof target) {valid = 0; break;}
    path[len] = 0;
    if(strlen(path) != len) {valid = 0; break;}
    /* Old USB backups were moved by rename(), so their contents and source
     * signatures are still valid. Upgrade only the path prefix inside the
     * persistent cache to avoid re-copying gigabytes of unchanged saves. */
    const size_t old_prefix = strlen(LEGACY_BACKUP_ROOT);
    if(!strncmp(path, LEGACY_BACKUP_ROOT, old_prefix) && path[old_prefix] == '/') {
      char migrated[PATH_MAX];
      int length = snprintf(migrated, sizeof migrated, "%s%s",
                            BACKUP_ROOT, path + old_prefix);
      if(length < 0 || (size_t)length >= sizeof migrated) {valid = 0; break;}
      strcpy(path, migrated);
      index_dirty = 1;  /* Rewrite as the new layout on the next pass. */
    }
    size_t new_prefix = strlen(BACKUP_ROOT);
    if(strncmp(path, BACKUP_ROOT, new_prefix) || path[new_prefix] != '/') {valid = 0; break;}
    change_record_t *r = index_record(path, 1);
    if(!r) {valid = 0; break;}
    r->source = source; r->target = target;
  }
  fclose(f);
  if(!valid) {index_clear(); logq("Change cache invalid; using safe recopy");}
}

static void
index_save(void) {
  if(!index_dirty) return;
#ifdef PS5SAVE_DYNAMIC_STORAGE
  const char *partial = storage_index_part;
#else
  const char *partial = INDEX_PATH ".part";
#endif
  FILE *f = fopen(partial, "wb");
  if(!f) {
    notify_problem(NOTICE_WARNING, "index-save", NULL, "Could not save the backup index. Files may be copied again.");
    return;
  }
  int ok = fwrite("PS5IDX2\n", 1, 8, f) == 8;
  for(unsigned i = 0; i < INDEX_BUCKETS && ok; i++) {
    for(change_record_t *r = change_index[i]; r && ok; r = r->next) {
      uint32_t len = (uint32_t)strlen(r->path);
      ok = fwrite(&len, 1, sizeof len, f) == sizeof len &&
           fwrite(r->path, 1, len, f) == len &&
           fwrite(&r->source, 1, sizeof r->source, f) == sizeof r->source &&
           fwrite(&r->target, 1, sizeof r->target, f) == sizeof r->target;
    }
  }
  if(fflush(f) || fsync(fileno(f))) ok = 0;
  if(fclose(f)) ok = 0;
  if(!ok || rename(partial, INDEX_PATH)) {
    unlink(partial);
    notify_problem(NOTICE_WARNING, "index-save", NULL, "Could not save the backup index. Files may be copied again.");
  } else index_dirty = 0;
}
#endif

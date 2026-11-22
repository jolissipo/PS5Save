/* Read-only USB file explorer for the PS5Save+ dashboard.
 * No filesystem write operations or arbitrary paths are exposed. All traversal
 * is limited to BACKUP_ROOT and dated directories inside DAILY_ROOT. Page
 * listings are capped and stable in lexical order; recursive summaries use
 * time and entry limits and explicitly mark partial results.
 */
#ifndef PS5SAVE_PLUS_BROWSER_H
#define PS5SAVE_PLUS_BROWSER_H
#define BROWSER_PAGE_SIZE 64
#define BROWSER_MAX_ENTRIES 30000
#define BROWSER_MAX_SECONDS 2.0
#define BROWSER_MAX_DEPTH 40

typedef struct {
  char name[256];
  int is_dir;
  uint64_t size;
  time_t mtime;
} browser_item_t;

static int browser_hex(char c) {
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  if(c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int browser_decode(const char *src, size_t n, char *dst, size_t cap) {
  size_t used = 0;
  for(size_t i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)src[i];
    if(ch == '%') {
      if(i + 2 >= n) return 0;
      int hi = browser_hex(src[++i]);
      int lo = browser_hex(src[++i]);
      if(hi < 0 || lo < 0) return 0;
      ch = (unsigned char)((hi << 4) | lo);
    }
    if(ch == 0 || ch < 32 || ch == 127 || used + 1 >= cap) return 0;
    dst[used++] = (char)ch;
  }
  dst[used] = 0;
  return 1;
}

/* Return -1 on duplicated or invalid query values, 0 if absent, 1 if set. */
static int browser_param(const char *query, const char *name,
                         char *out, size_t cap) {
  int matches = 0;
  size_t n = strlen(name);
  out[0] = 0;
  if(!query || !*query) return 0;
  const char *p = query;
  while(*p) {
    const char *end = strchr(p, '&');
    if(!end) end = p + strlen(p);
    if((size_t)(end - p) > n && !strncmp(p, name, n) && p[n] == '=') {
      if(++matches > 1 || !browser_decode(p + n + 1,
                                    (size_t)(end - p - n - 1), out, cap))
        return -1;
    }
    p = *end ? end + 1 : end;
  }
  return matches;
}

static int browser_root(const char *query, char *root, size_t cap) {
  char kind[24], date[20];
  if(browser_param(query, "root", kind, sizeof kind) != 1) return 0;
  if(!strcmp(kind, "live")) {
    if(browser_param(query, "date", date, sizeof date) != 0) return 0;
    return snprintf(root, cap, "%s", BACKUP_ROOT) < (int)cap;
  }
  if(strcmp(kind, "snapshot") ||
     browser_param(query, "date", date, sizeof date) != 1 ||
     !dashboard_snapshot_name(date)) return 0;
  return snprintf(root, cap, "%s/%s", DAILY_ROOT, date) < (int)cap;
}

/* Resolve path component by component with lstat; symlinks are never followed,
 * including the last component. Disallow parent references and empty segments.
 */
static int browser_directory(const char *root, const char *relative,
                             char *absolute, size_t cap) {
  struct stat st;
  if(lstat(root, &st) || !S_ISDIR(st.st_mode)) return 0;
  size_t used = strlen(root);
  if(used >= cap) return 0;
  memcpy(absolute, root, used + 1);
  if(!*relative) return 1;
  if(relative[0] == '/' || relative[strlen(relative)-1] == '/') return 0;
  const char *p = relative;
  unsigned depth = 0;
  while(*p) {
    const char *end = strchr(p, '/');
    if(!end) end = p + strlen(p);
    size_t n = (size_t)(end - p);
    if(!n || n > 255 || (n == 1 && p[0] == '.') ||
       (n == 2 && p[0] == '.' && p[1] == '.') ||
       ++depth > BROWSER_MAX_DEPTH ||
       used + n + 2 > cap) return 0;
    absolute[used++] = '/';
    memcpy(absolute + used, p, n);
    used += n;
    absolute[used] = 0;
    if(lstat(absolute, &st) || !S_ISDIR(st.st_mode)) return 0;
    p = *end ? end + 1 : end;
  }
  return 1;
}

static int browser_item_compare(const browser_item_t *a, const browser_item_t *b) {
  if(a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
  return strcmp(a->name, b->name);
}
static void browser_item_key(const browser_item_t *item, char out[258]) {
  out[0] = item->is_dir ? 'D' : 'F';
  snprintf(out + 1, 257, "%s", item->name);
}
static int browser_after(const browser_item_t *item, const char *after) {
  if(!*after) return 1;
  char key[258];
  browser_item_key(item, key);
  return strcmp(key, after) > 0;
}

/* Tiny insertion sort across at most 64 entries; no directory-wide arrays. */
static void browser_insert(browser_item_t *items, unsigned *count,
                           const browser_item_t *item) {
  if(*count == BROWSER_PAGE_SIZE &&
     browser_item_compare(item, &items[*count-1]) >= 0) return;
  unsigned i = *count < BROWSER_PAGE_SIZE ? (*count)++ : *count - 1;
  while(i && browser_item_compare(item, &items[i-1]) < 0) {
    items[i] = items[i-1];
    i--;
  }
  items[i] = *item;
}

static int browser_skip_metadata(const char *name) {
  size_t n = strlen(name);
  return !strcmp(name, ".ps5save-index") ||
         !strcmp(name, ".image_stamp") ||
         (n > 5 && !strcmp(name + n - 5, ".part"));
}

static void browser_serve_tree(int fd, const char *query) {
  char root[PATH_MAX], rel[PATH_MAX], path[PATH_MAX], after[258];
  if(!browser_root(query, root, sizeof root) ||
     browser_param(query, "path", rel, sizeof rel) < 0 ||
     browser_param(query, "after", after, sizeof after) < 0 ||
     (*after && (*after != 'D' && *after != 'F')) ||
     !browser_directory(root, rel, path, sizeof path)) {
    dashboard_text(fd, 400, "Invalid or unavailable backup directory");
    return;
  }

  DIR *d = opendir(path);
  if(!d) { dashboard_text(fd, 503, "Cannot open backup directory"); return; }
  browser_item_t items[BROWSER_PAGE_SIZE];
  unsigned count = 0, matches = 0, visits = 0;
  double deadline = now_seconds() + BROWSER_MAX_SECONDS;
  int incomplete = 0;
  struct dirent *entry;
  errno = 0;
  while((entry = readdir(d))) {
    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
       browser_skip_metadata(entry->d_name)) continue;
    if(++visits > BROWSER_MAX_ENTRIES ||
       (visits % 64 == 0 && now_seconds() > deadline)) {
      incomplete = 1; break;
    }
    char full[PATH_MAX];
    if(snprintf(full, sizeof full, "%s/%s", path, entry->d_name) >= (int)sizeof full) continue;
    struct stat st;
    if(lstat(full, &st) || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) continue;
    browser_item_t item = {0};
    snprintf(item.name, sizeof item.name, "%s", entry->d_name);
    item.is_dir = S_ISDIR(st.st_mode);
    item.size = S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0;
    item.mtime = st.st_mtime;
    if(!browser_after(&item, after)) continue;
    matches++;
    browser_insert(items, &count, &item);
  }
  int read_error = !entry && errno;
  closedir(d);
  if(incomplete || read_error) {
    dashboard_text(fd, 503, "Directory listing incomplete; try again later");
    return;
  }

  /* Explicit bounds prevent untrusted file names from filling the server. */
  char *result = malloc(160000);
  if(!result) { dashboard_text(fd, 503, "Out of memory"); return; }
  size_t used = 0, capacity = 160000;
  int n = snprintf(result, capacity, "{\"entries\":[");
  if(n < 0 || (size_t)n >= capacity) { free(result); return; }
  used = (size_t)n;
  for(unsigned i = 0; i < count; i++) {
    char safe[2048];
    if(!notification_json_escape(safe, sizeof safe, items[i].name)) continue;
    n = snprintf(result + used, capacity - used,
      "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%llu,\"mtime\":%lld}",
      i ? "," : "", safe, items[i].is_dir ? "true" : "false",
      (unsigned long long)items[i].size, (long long)items[i].mtime);
    if(n < 0 || (size_t)n >= capacity - used) { free(result); return; }
    used += (size_t)n;
  }
  char next[258] = "";
  if(matches > BROWSER_PAGE_SIZE && count)
    browser_item_key(&items[count-1], next);
  char escaped[1024];
  if(!notification_json_escape(escaped, sizeof escaped, next)) {
    free(result); dashboard_text(fd, 503, "Cannot encode page cursor"); return;
  }
  n = snprintf(result + used, capacity - used,
    "],\"next\":%s%s%s}", *next ? "\"" : "null",
    *next ? escaped : "", *next ? "\"" : "");
  if(n < 0 || (size_t)n >= capacity - used) { free(result); return; }
  used += (size_t)n;
  dashboard_respond(fd, 200, "application/json; charset=utf-8", result, used);
  free(result);
}

typedef struct {
  uint64_t bytes;
  unsigned files, dirs, visited;
  int complete;
  double deadline;
} browser_summary_t;

static void browser_walk(const char *path, unsigned depth, browser_summary_t *sum) {
  if(!sum->complete) return;
  if(depth >= BROWSER_MAX_DEPTH || now_seconds() > sum->deadline) {
    sum->complete = 0; return;
  }
  DIR *d = opendir(path);
  if(!d) {sum->complete = 0; return;}
  struct dirent *entry;
  errno = 0;
  while(sum->complete && (entry = readdir(d))) {
    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
       browser_skip_metadata(entry->d_name)) continue;
    if(++sum->visited > BROWSER_MAX_ENTRIES ||
       (sum->visited % 64 == 0 && now_seconds() > sum->deadline)) {
      sum->complete = 0; break;
    }
    char full[PATH_MAX];
    struct stat st;
    if(snprintf(full, sizeof full, "%s/%s", path, entry->d_name) >= (int)sizeof full ||
       lstat(full, &st)) {sum->complete = 0; break;}
    if(S_ISREG(st.st_mode)) {
      sum->files++;
      sum->bytes += (uint64_t)st.st_size;
    } else if(S_ISDIR(st.st_mode)) {
      sum->dirs++;
      browser_walk(full, depth + 1, sum);
    } else {
      sum->complete = 0; /* no claims of completeness if links are skipped */
    }
    errno = 0;
  }
  if(!entry && errno) sum->complete = 0;
  closedir(d);
}

static void browser_serve_summary(int fd, const char *query) {
  char root[PATH_MAX];
  if(!browser_root(query, root, sizeof root)) {
    dashboard_text(fd, 400, "Invalid backup scope");
    return;
  }
  char path[PATH_MAX];
  if(!browser_directory(root, "", path, sizeof path)) {
    dashboard_text(fd, 404, "Backup unavailable");
    return;
  }
  browser_summary_t sum = {.complete = 1, .deadline = now_seconds() + BROWSER_MAX_SECONDS};
  browser_walk(path, 0, &sum);
  char buf[256];
  int n = snprintf(buf, sizeof buf,
    "{\"files\":%u,\"dirs\":%u,\"bytes\":%llu,\"complete\":%s}",
    sum.files, sum.dirs, (unsigned long long)sum.bytes,
    sum.complete ? "true" : "false");
  if(n < 0 || (size_t)n >= sizeof buf) {
    dashboard_text(fd, 503, "Cannot summarize backup");
    return;
  }
  dashboard_respond(fd, 200, "application/json; charset=utf-8", buf, (size_t)n);
}
#endif

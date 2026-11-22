/* PS5Save+ lightweight, read-only web dashboard.
 * The web server is advisory: it must never influence backup execution.
 * HTTP endpoints deliberately accept GET only. Configuration writes require a
 * separate authenticated design; do not add unauthenticated POST handlers.
 */
#ifndef PS5SAVE_PLUS_DASHBOARD_H
#define PS5SAVE_PLUS_DASHBOARD_H
#ifndef BACKUP_HOST_TEST

#define DASHBOARD_PORT 8755
#define DASHBOARD_DATA_DIR USB_ROOT "/ps5save-plus"
#define DASHBOARD_HISTORY DASHBOARD_DATA_DIR "/history.tsv"
#define DASHBOARD_HISTORY_ROTATED DASHBOARD_DATA_DIR "/history.tsv.1"
#define DASHBOARD_HISTORY_MAX (128 * 1024)
#define DASHBOARD_REQUEST_MAX 4096
#define DASHBOARD_FILE_MAX (64 * 1024)

__asm__(".section .rodata\n"
        ".global dashboard_html\n"
        ".global dashboard_html_end\n"
        ".global dashboard_html_size\n"
        ".align 16\n"
        "dashboard_html:\n"
        ".incbin \"dashboard/index.html\"\n"
        "dashboard_html_end:\n"
        "dashboard_html_size:\n"
        ".quad dashboard_html_end - dashboard_html\n"
        ".previous");
extern const unsigned char dashboard_html[];
extern const size_t dashboard_html_size;

typedef struct {
  time_t last_run;
  uint64_t bytes;
  unsigned copied;
  unsigned failed;
  unsigned skipped;
  int deferred;
  int snapshot;
  int running;
  int has_run;
  long interval;
  int notification_mode;
  int notification_in_game;
} dashboard_state_t;

static pthread_mutex_t dashboard_mutex = PTHREAD_MUTEX_INITIALIZER;
static dashboard_state_t dashboard_state;

static void dashboard_set_running(int value) {
  pthread_mutex_lock(&dashboard_mutex);
  dashboard_state.running = value;
  pthread_mutex_unlock(&dashboard_mutex);
}

/* Persist only meaningful passes. The backup engine remains functional if
 * the USB is absent or a history write fails. A bounded .1 rotation prevents
 * unbounded growth; the history is diagnostic, not a second backup index.
 */
static void dashboard_history_append(const backup_stats_t *stats, int deferred,
                                     int snapshot, int errored) {
  if(!stats->copied && !stats->skipped && !stats->failed &&
     !deferred && !snapshot && !errored) return;
  if(access(CONF_PATH, R_OK) || mkdir_p(DASHBOARD_DATA_DIR)) return;
  struct stat st;
  if(!stat(DASHBOARD_HISTORY, &st) && st.st_size >= DASHBOARD_HISTORY_MAX) {
    unlink(DASHBOARD_HISTORY_ROTATED);
    rename(DASHBOARD_HISTORY, DASHBOARD_HISTORY_ROTATED);
  }
  FILE *f = fopen(DASHBOARD_HISTORY, "a");
  if(!f) return;
  time_t t = time(NULL);
  struct tm tm;
  char date[32] = "";
  if(gmtime_r(&t, &tm)) strftime(date, sizeof date, "%Y-%m-%d %H:%M UTC", &tm);
  const char *result = errored || stats->failed ? "error" :
                       deferred ? "deferred" :
                       stats->skipped || snapshot < 0 ? "partial" : "success";
  fprintf(f, "%s\t%s\t%llu\t%u\t%u\t%u\t%d\n",
          date, result, (unsigned long long)stats->bytes, stats->copied,
          stats->failed, stats->skipped, snapshot);
  fclose(f);
}

static void dashboard_cycle_done(const backup_stats_t *stats, int deferred,
                                 int snapshot, int errored) {
  pthread_mutex_lock(&dashboard_mutex);
  dashboard_state.last_run = time(NULL);
  dashboard_state.bytes = stats->bytes;
  dashboard_state.copied = stats->copied;
  dashboard_state.failed = stats->failed + (errored && !stats->failed ? 1u : 0u);
  dashboard_state.skipped = stats->skipped;
  dashboard_state.deferred = deferred;
  dashboard_state.snapshot = snapshot;
  dashboard_state.interval = g_interval;
  dashboard_state.notification_mode = (int)notification_settings.mode;
  dashboard_state.notification_in_game = notification_settings.in_game;
  dashboard_state.running = 0;
  dashboard_state.has_run = 1;
  pthread_mutex_unlock(&dashboard_mutex);
  dashboard_history_append(stats, deferred, snapshot, errored);
}

/* No dynamic inputs are interpolated into HTML. Reply lengths are explicit. */
static int dashboard_send_all(int fd, const char *buf, size_t len) {
  while(len) {
    ssize_t n = send(fd, buf, len, 0);
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) return -1;
    buf += (size_t)n;
    len -= (size_t)n;
  }
  return 0;
}

static void dashboard_respond(int fd, int status, const char *type,
                              const char *body, size_t len) {
  char header[512];
  const char *reason = status == 200 ? "OK" :
                       status == 404 ? "Not Found" :
                       status == 405 ? "Method Not Allowed" :
                       status == 413 ? "Payload Too Large" : "Unavailable";
  int n = snprintf(header, sizeof header,
      "HTTP/1.1 %d %s\r\n"
      "Content-Type: %s\r\n"
      "Content-Length: %zu\r\n"
      "Connection: close\r\n"
      "Cache-Control: no-store\r\n"
      "X-Content-Type-Options: nosniff\r\n"
      "X-Frame-Options: DENY\r\n"
      "Content-Security-Policy: default-src 'self'; "
      "script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
      "connect-src 'self'; object-src 'none'; frame-ancestors 'none'\r\n\r\n",
      status, reason, type, len);
  if(n <= 0 || (size_t)n >= sizeof header) return;
  if(dashboard_send_all(fd, header, (size_t)n)) return;
  dashboard_send_all(fd, body, len);
}

static void dashboard_text(int fd, int status, const char *msg) {
  dashboard_respond(fd, status, "text/plain; charset=utf-8", msg, strlen(msg));
}

static void dashboard_serve_status(int fd) {
  dashboard_state_t state;
  struct statfs sf;
  unsigned long long free_bytes = 0, total_bytes = 0;
  int usb_online = !access(CONF_PATH, R_OK) && !statfs(USB_ROOT, &sf);
  if(usb_online) {
    free_bytes = (unsigned long long)sf.f_bavail * sf.f_bsize;
    total_bytes = (unsigned long long)sf.f_blocks * sf.f_bsize;
  }
  pthread_mutex_lock(&dashboard_mutex);
  state = dashboard_state;
  pthread_mutex_unlock(&dashboard_mutex);
  char buf[1024];
  int n = snprintf(buf, sizeof buf,
      "{\"version\":\"PS5Save+ development\","
      "\"running\":%s,\"hasRun\":%s,\"lastRun\":%lld,"
      "\"bytes\":%llu,\"copied\":%u,\"failed\":%u,"
      "\"skipped\":%u,\"deferred\":%s,\"snapshot\":%d,"
      "\"interval\":%ld,\"notifications\":%d,"
      "\"inGame\":%s,\"usbOnline\":%s,"
      "\"usbFree\":%llu,\"usbTotal\":%llu}",
      state.running ? "true" : "false",
      state.has_run ? "true" : "false",
      (long long)state.last_run,
      (unsigned long long)state.bytes, state.copied,
      state.failed, state.skipped,
      state.deferred ? "true" : "false", state.snapshot,
      state.interval, state.notification_mode,
      state.notification_in_game ? "true" : "false",
      usb_online ? "true" : "false", free_bytes, total_bytes);
  if(n < 0 || (size_t)n >= sizeof buf) {
    dashboard_text(fd, 503, "Status unavailable");
    return;
  }
  dashboard_respond(fd, 200, "application/json; charset=utf-8", buf, (size_t)n);
}

/* The browser cannot access an arbitrary path: only a fixed allowlisted file. */
static void dashboard_serve_file(int fd, const char *path) {
  struct stat st;
  if(stat(path, &st) || !S_ISREG(st.st_mode)) {
    dashboard_text(fd, 404, "File unavailable");
    return;
  }
  if(st.st_size < 0 || st.st_size > DASHBOARD_FILE_MAX) {
    dashboard_text(fd, 413, "File too large");
    return;
  }
  FILE *f = fopen(path, "rb");
  if(!f) {
    dashboard_text(fd, 503, "Cannot read file");
    return;
  }
  char *contents = malloc((size_t)st.st_size + 1);
  if(!contents) {
    fclose(f);
    dashboard_text(fd, 503, "Out of memory");
    return;
  }
  size_t len = fread(contents, 1, (size_t)st.st_size, f);
  int failed = ferror(f);
  fclose(f);
  if(failed) dashboard_text(fd, 503, "Cannot read file");
  else dashboard_respond(fd, 200, "text/plain; charset=utf-8", contents, len);
  free(contents);
}


/* Return only the tail: large diagnostic logs must never block UI requests or
 * allocate memory proportional to the log file size. */
static void dashboard_serve_log_tail(int fd) {
  FILE *f = fopen(DASHBOARD_DATA_DIR "/events.log", "rb");
  if(!f) {
    dashboard_text(fd, 404, "No diagnostic logs yet");
    return;
  }
  if(fseek(f, 0, SEEK_END)) {
    fclose(f);
    dashboard_text(fd, 503, "Cannot seek log");
    return;
  }
  long len = ftell(f);
  if(len < 0) {
    fclose(f);
    dashboard_text(fd, 503, "Cannot read log");
    return;
  }
  long start = len > 16384 ? len - 16384 : 0;
  if(fseek(f, start, SEEK_SET)) {
    fclose(f);
    dashboard_text(fd, 503, "Cannot seek log");
    return;
  }
  char buf[16385];
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  int error = ferror(f);
  fclose(f);
  if(error) {
    dashboard_text(fd, 503, "Cannot read log");
    return;
  }
  buf[n] = 0;
  /* Skip the first partial line if the tail starts mid-record. */
  char *body = buf;
  if(start > 0) {
    char *line = strchr(buf, '\n');
    if(line) body = line + 1;
  }
  dashboard_respond(fd, 200, "text/plain; charset=utf-8",
                    body, strlen(body));
}

static int dashboard_snapshot_name(const char *s) {
  if(strlen(s) != 10) return 0;
  for(int i = 0; i < 10; i++) {
    if(i == 4 || i == 7) {
      if(s[i] != '-') return 0;
    } else if(s[i] < '0' || s[i] > '9') return 0;
  }
  return 1;
}

static void dashboard_serve_snapshot_list(int fd) {
  char result[2048] = "";
  size_t pos = 0;
  DIR *dir = opendir(DAILY_ROOT);
  if(dir) {
    struct dirent *entry;
    while((entry = readdir(dir))) {
      if(!dashboard_snapshot_name(entry->d_name)) continue;
      char full[PATH_MAX];
      struct stat st;
      if(snprintf(full, sizeof full, "%s/%s", DAILY_ROOT, entry->d_name) >= (int)sizeof full ||
         stat(full, &st) || !S_ISDIR(st.st_mode)) continue;
      size_t n = strlen(entry->d_name);
      if(pos + n + 2 >= sizeof result) break;
      memcpy(result + pos, entry->d_name, n);
      pos += n;
      result[pos++] = '\n';
      result[pos] = 0;
    }
    closedir(dir);
  }
  dashboard_respond(fd, 200, "text/plain; charset=utf-8", result, pos);
}

static void dashboard_serve_backup_roots(int fd) {
  char result[2048] = "";
  size_t pos = 0;
  DIR *dir = opendir(BACKUP_ROOT);
  if(dir) {
    struct dirent *entry;
    while((entry = readdir(dir))) {
      if(entry->d_name[0] == '.') continue;
      size_t n = strlen(entry->d_name);
      if(!n || n > 200 || pos + n + 2 >= sizeof result) continue;
      int safe = 1;
      for(size_t i = 0; i < n; i++)
        if((unsigned char)entry->d_name[i] < 32) safe = 0;
      if(!safe) continue;
      char full[PATH_MAX];
      struct stat st;
      if(snprintf(full, sizeof full, "%s/%s", BACKUP_ROOT, entry->d_name) >= (int)sizeof full ||
         stat(full, &st) || !S_ISDIR(st.st_mode)) continue;
      memcpy(result + pos, entry->d_name, n);
      pos += n;
      result[pos++] = '\n';
      result[pos] = 0;
    }
    closedir(dir);
  }
  dashboard_respond(fd, 200, "text/plain; charset=utf-8", result, pos);
}

static void dashboard_request(int fd) {
  char req[DASHBOARD_REQUEST_MAX + 1];
  size_t len = 0;
  while(len < DASHBOARD_REQUEST_MAX) {
    ssize_t n = recv(fd, req + len, DASHBOARD_REQUEST_MAX - len, 0);
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) return;
    len += (size_t)n;
    req[len] = 0;
    if(strstr(req, "\r\n\r\n")) break;
  }
  if(len >= DASHBOARD_REQUEST_MAX && !strstr(req, "\r\n\r\n")) {
    dashboard_text(fd, 413, "Request too large");
    return;
  }
  char method[8], path[128], version[16];
  if(sscanf(req, "%7s %127s %15s", method, path, version) != 3 ||
     strncmp(version, "HTTP/1.", 7)) {
    dashboard_text(fd, 404, "Bad request");
    return;
  }
  if(strcmp(method, "GET")) {
    dashboard_text(fd, 405, "Read-only dashboard");
    return;
  }
  if(!strcmp(path, "/") || !strcmp(path, "/index.html")) {
    dashboard_respond(fd, 200, "text/html; charset=utf-8",
                      (const char *)dashboard_html, dashboard_html_size);
  } else if(!strcmp(path, "/api/status")) dashboard_serve_status(fd);
  else if(!strcmp(path, "/api/snapshots")) dashboard_serve_snapshot_list(fd);
  else if(!strcmp(path, "/api/backups")) dashboard_serve_backup_roots(fd);
  else if(!strcmp(path, "/api/config")) dashboard_serve_file(fd, CONF_PATH);
  else if(!strcmp(path, "/api/history")) dashboard_serve_file(fd, DASHBOARD_HISTORY);
  else if(!strcmp(path, "/api/logs")) dashboard_serve_log_tail(fd);
  else dashboard_text(fd, 404, "Not found");
}

static void *dashboard_worker(void *arg) {
  int server = (int)(intptr_t)arg;
  for(;;) {
    int client = accept(server, NULL, NULL);
    if(client < 0) {
      if(errno == EINTR) continue;
      break;
    }
    struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    dashboard_request(client);
    close(client);
  }
  close(server);
  return NULL;
}

static void dashboard_start(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if(fd < 0) return;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(DASHBOARD_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if(bind(fd, (struct sockaddr *)&addr, sizeof addr) ||
     listen(fd, 8)) {
    logq("PS5Save+ dashboard unavailable on TCP %d: %s",
         DASHBOARD_PORT, strerror(errno));
    close(fd);
    return;
  }
  pthread_t thread;
  int rc = pthread_create(&thread, NULL, dashboard_worker,
                          (void *)(intptr_t)fd);
  if(rc) {
    logq("PS5Save+ dashboard thread unavailable: %s", strerror(rc));
    close(fd);
    return;
  }
  pthread_detach(thread);
  logq("PS5Save+ dashboard listening on TCP %d (read-only)", DASHBOARD_PORT);
}
#endif /* !BACKUP_HOST_TEST */
#endif /* PS5SAVE_PLUS_DASHBOARD_H */

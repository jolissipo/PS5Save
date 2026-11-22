/* PS5Save+ lightweight web dashboard.
 * The backup engine remains independent from the HTTP server. Config editing
 * requires an explicit one-time code displayed on the physical PS5.
 */
#ifndef PS5SAVE_PLUS_DASHBOARD_H
#define PS5SAVE_PLUS_DASHBOARD_H
#ifndef BACKUP_HOST_TEST

#define DASHBOARD_PORT 8755
#ifdef PS5SAVE_DYNAMIC_STORAGE
#define DASHBOARD_DATA_DIR STORAGE_ROOT
#define DASHBOARD_HISTORY storage_history_path
#define DASHBOARD_HISTORY_ROTATED storage_history_rotated
#else
#define DASHBOARD_DATA_DIR USB_ROOT "/ps5save-plus"
#define DASHBOARD_HISTORY DASHBOARD_DATA_DIR "/history.tsv"
#define DASHBOARD_HISTORY_ROTATED DASHBOARD_DATA_DIR "/history.tsv.1"
#endif
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
                       status == 400 ? "Bad Request" :
                       status == 403 ? "Forbidden" :
                       status == 404 ? "Not Found" :
                       status == 405 ? "Method Not Allowed" :
                       status == 409 ? "Conflict" :
                       status == 413 ? "Payload Too Large" :
                       status == 422 ? "Unprocessable Content" :
                       status == 429 ? "Too Many Requests" : "Unavailable";
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

#include "dashboard_editor.h"

static void dashboard_usb_list(int fd) {
  char output[2000];
  size_t used = 0;
  int n = snprintf(output, sizeof output,
    "{\"current\":\"%s\",\"moving\":%s,\"drives\":[",
    USB_ROOT,
#ifdef PS5SAVE_DYNAMIC_STORAGE
    atomic_load(&storage_migrating) ? "true" : "false"
#else
    "false"
#endif
  );
  if(n < 0 || (size_t)n >= sizeof output) {
    dashboard_text(fd, 503, "USB information unavailable");
    return;
  }
  used = (size_t)n;
#ifdef PS5SAVE_DYNAMIC_STORAGE
  unsigned count = 0;
  for(int i = 0; i < STORAGE_USB_SLOTS; i++) {
    char mount[32];
    uint64_t free_bytes, total_bytes;
    snprintf(mount, sizeof mount, "/mnt/usb%d", i);
    if(!storage_usb_online(mount, &free_bytes, &total_bytes)) continue;
    n = snprintf(output + used, sizeof output - used,
       "%s{\"path\":\"%s\",\"free\":%llu,\"total\":%llu}",
       count++ ? "," : "", mount, (unsigned long long)free_bytes,
       (unsigned long long)total_bytes);
    if(n < 0 || (size_t)n >= sizeof output - used) {
      dashboard_text(fd, 503, "Too many USB drives"); return;
    }
    used += (size_t)n;
  }
#else
  int count = snprintf(output + used, sizeof output - used,
        "{\"path\":\"%s\",\"free\":0,\"total\":0}", USB_ROOT);
  if(count < 0 || (size_t)count >= sizeof output - used) return;
  used += (size_t)count;
#endif
  n = snprintf(output + used, sizeof output - used, "]}");
  if(n < 0 || (size_t)n >= sizeof output - used) return;
  used += (size_t)n;
  dashboard_respond(fd, 200, "application/json; charset=utf-8", output, used);
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
      "\"usbFree\":%llu,\"usbTotal\":%llu,"
      "\"usbDevice\":\"%s\",\"usbMoving\":%s,\"usbPercent\":%u}",
      state.running ? "true" : "false",
      state.has_run ? "true" : "false",
      (long long)state.last_run,
      (unsigned long long)state.bytes, state.copied,
      state.failed, state.skipped,
      state.deferred ? "true" : "false", state.snapshot,
      state.interval, state.notification_mode,
      state.notification_in_game ? "true" : "false",
      usb_online ? "true" : "false", free_bytes, total_bytes,
      USB_ROOT,
#ifdef PS5SAVE_DYNAMIC_STORAGE
      atomic_load(&storage_migrating) ? "true" : "false",
      atomic_load(&storage_migration_percent)
#else
      "false", 0u
#endif
  );
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
static void dashboard_serve_log_tail_file(int fd, const char *path) {
  FILE *f = fopen(path, "rb");
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

static void dashboard_serve_log_tail(int fd) {
#ifdef PS5SAVE_DYNAMIC_STORAGE
  dashboard_serve_log_tail_file(fd, storage_log_path);
#else
  dashboard_serve_log_tail_file(fd, DASHBOARD_DATA_DIR "/events.log");
#endif
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

#include "dashboard_browser.h"

static void dashboard_request(int fd) {
  /* Headers: 4 KiB maximum. Config body: another 16 KiB maximum.
   * Reject chunked requests and duplicate Content-Length fields. */
  char req[DASHBOARD_REQUEST_MAX + EDITOR_CONFIG_MAX + 1];
  size_t len = 0;
  size_t header_end = 0;
  while(len < DASHBOARD_REQUEST_MAX) {
    ssize_t n = recv(fd, req + len, DASHBOARD_REQUEST_MAX - len, 0);
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) return;
    len += (size_t)n;
    req[len] = 0;
    char *boundary = strstr(req, "\r\n\r\n");
    if(boundary) {
      header_end = (size_t)(boundary - req) + 4;
      break;
    }
  }
  if(!header_end || header_end > DASHBOARD_REQUEST_MAX ||
     memchr(req, 0, header_end)) {
    dashboard_text(fd, 400, "Invalid HTTP headers.");
    return;
  }
  char method[8], path[2048], version[16], host[96];
  if(sscanf(req, "%7s %2047s %15s", method, path, version) != 3 ||
     strncmp(version, "HTTP/1.", 7) ||
     editor_header(req, "Host", host, sizeof host) != 1 ||
     !editor_valid_host(host)) {
    dashboard_text(fd, 400, "Invalid request host or path.");
    return;
  }
  char *query = strchr(path, '?');
  if(query) *query++ = 0;
  /* Read-only dashboard routes remain reachable throughout USB migration.
   * POST /api/config is separately blocked/serialized in editor_save().
   * A file read at the instant of drive switch may need one retry; neither
   * the home page nor diagnostics should be hidden behind an HTTP error. */
  if(!strcmp(method, "GET")) {
    if(!strcmp(path, "/") || !strcmp(path, "/index.html")) {
      dashboard_respond(fd, 200, "text/html; charset=utf-8",
                        (const char *)dashboard_html, dashboard_html_size);
    } else if(!strcmp(path, "/api/status")) dashboard_serve_status(fd);
    else if(!strcmp(path, "/api/usb-devices")) dashboard_usb_list(fd);
    else if(!strcmp(path, "/api/snapshots")) dashboard_serve_snapshot_list(fd);
    else if(!strcmp(path, "/api/backups")) dashboard_serve_backup_roots(fd);
    else if(!strcmp(path, "/api/tree")) browser_serve_tree(fd, query);
    else if(!strcmp(path, "/api/summary")) browser_serve_summary(fd, query);
    else if(!strcmp(path, "/api/config")) dashboard_serve_file(fd, CONF_PATH);
    else if(!strcmp(path, "/api/config-version")) editor_serve_version(fd);
    else if(!strcmp(path, "/api/history")) dashboard_serve_file(fd, DASHBOARD_HISTORY);
    else if(!strcmp(path, "/api/logs")) dashboard_serve_log_tail(fd);
    else dashboard_text(fd, 404, "Not found");
    return;
  }
  if(strcmp(method, "POST") ||
     (strcmp(path, "/api/pair") && strcmp(path, "/api/unlock") &&
      strcmp(path, "/api/session") && strcmp(path, "/api/config"))) {
    dashboard_text(fd, 405, "Method not allowed");
    return;
  }
  if(!editor_request_origin(req)) {
    dashboard_text(fd, 403, "Rejected request origin or missing authorization header.");
    return;
  }
  char content_length[24];
  if(editor_header(req, "Content-Length", content_length, sizeof content_length) != 1 ||
     !editor_integer(content_length, 0, EDITOR_CONFIG_MAX)) {
    dashboard_text(fd, 400, "Missing or invalid Content-Length.");
    return;
  }
  size_t needed = (size_t)strtoul(content_length, NULL, 10);
  while(len < header_end + needed) {
    ssize_t n = recv(fd, req + len, sizeof req - 1 - len, 0);
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) return;
    len += (size_t)n;
  }
  const char *body = req + header_end;
  if(memchr(body, 0, needed)) {
    dashboard_text(fd, 400, "NUL bytes are not accepted.");
    return;
  }
  if(!strcmp(path, "/api/pair")) {
    if(needed) dashboard_text(fd, 400, "Pairing request must be empty.");
    else editor_start_pair(fd, req);
  } else if(!strcmp(path, "/api/session")) {
    if(needed) dashboard_text(fd, 400, "Session check must be empty.");
    else editor_session_status(fd, req);
  } else if(!strcmp(path, "/api/unlock")) {
    char code[7] = "";
    if(needed != 6) {
      dashboard_text(fd, 400, "Pairing code must be six digits.");
      return;
    }
    memcpy(code, body, 6);
    editor_unlock(fd, code, 6);
  } else if(!strcmp(path, "/api/config")) {
    editor_save(fd, req, body, needed);
  }
}

/* A process can survive PS5 standby while its TCP listen socket does not.
 * SIGCONT only sets a flag: socket operations and logging stay in this worker.
 * A network-address change and accept/poll failures also force a fresh bind.
 * Backup checks and USB migration never depend on the dashboard listener. */
static volatile sig_atomic_t dashboard_resume_pending;

static void dashboard_sigcont(int sig) {
  (void)sig;
  dashboard_resume_pending=1;
}

static uint64_t dashboard_ipv4_signature(void) {
  struct ifaddrs *ifas=NULL;
  if(getifaddrs(&ifas)) return UINT64_MAX; /* Don't mistake an API failure for a network event. */
  uint64_t result=0;
  for(struct ifaddrs *p=ifas;p;p=p->ifa_next) {
    if(!p->ifa_addr || p->ifa_addr->sa_family!=AF_INET)continue;
    uint32_t ip=ntohl(((struct sockaddr_in*)p->ifa_addr)->sin_addr.s_addr);
    if((ip>>24)==127)continue; /* Localhost must not trigger restarts. */
    result+=(uint64_t)ip*UINT64_C(0x9e3779b185ebca87);
  }
  freeifaddrs(ifas);
  return result;
}

static int dashboard_open_listener(void) {
  int fd=socket(AF_INET, SOCK_STREAM, 0);
  if(fd<0)return -1;
  int one=1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr={0};
  addr.sin_family=AF_INET;
  addr.sin_port=htons(DASHBOARD_PORT);
  addr.sin_addr.s_addr=htonl(INADDR_ANY);
  if(bind(fd,(struct sockaddr*)&addr,sizeof addr) || listen(fd,8)) {
    int err=errno;
    close(fd);
    errno=err;
    return -1;
  }
  return fd;
}

static void *dashboard_worker(void *arg) {
  int server=(int)(intptr_t)arg;
  uint64_t last_network=dashboard_ipv4_signature();
  double next_network_check=now_seconds()+5;
  unsigned failures=0;
  for(;;) {
    int restart=0;
    if(dashboard_resume_pending) {
      dashboard_resume_pending=0;
      logq("Dashboard: console resumed; reopening HTTP listener on TCP %d",DASHBOARD_PORT);
      restart=1;
    }
    double now=now_seconds();
    if(now>=next_network_check) {
      next_network_check=now+5;
      uint64_t current=dashboard_ipv4_signature();
      if(current!=UINT64_MAX && last_network!=UINT64_MAX && current!=last_network) {
        logq("Dashboard: network changed; reopening HTTP listener on TCP %d",DASHBOARD_PORT);
        restart=1;
      }
      if(current!=UINT64_MAX)last_network=current;
    }
    if(restart && server>=0) {
      close(server);
      server=-1;
    }
    if(server<0) {
      server=dashboard_open_listener();
      if(server<0) {
        /* Retry without killing the resident payload or disabling backups. */
        if(++failures==1 || failures%30==0)
          logq("Dashboard: HTTP listener recovery pending on TCP %d: %s",
               DASHBOARD_PORT,strerror(errno));
        poll(NULL,0,1000);
        continue;
      }
      failures=0;
      logq("Dashboard: HTTP listener available again on TCP %d",DASHBOARD_PORT);
    }

    struct pollfd pfd={.fd=server,.events=POLLIN};
    int ready=poll(&pfd,1,1000);
    if(ready<0 && errno==EINTR)continue;
    if(ready<0 || (ready>0 && (pfd.revents&(POLLERR|POLLHUP|POLLNVAL)))) {
      logq("Dashboard: listener error; reopening TCP %d",DASHBOARD_PORT);
      close(server);
      server=-1;
      continue;
    }
    if(ready<=0 || !(pfd.revents&POLLIN))continue;
    int client=accept(server,NULL,NULL);
    if(client<0) {
      if(errno==EINTR || errno==EAGAIN)continue;
      logq("Dashboard: accept failed; reopening HTTP listener: %s",strerror(errno));
      close(server);
      server=-1;
      continue;
    }
    struct timeval timeout={.tv_sec=2,.tv_usec=0};
    setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
    setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
    dashboard_request(client);
    close(client);
  }
  return NULL;
}

static void dashboard_start(void) {
  int fd=dashboard_open_listener();
  if(fd<0) {
    logq("PS5Save+ dashboard unavailable on TCP %d: %s",
         DASHBOARD_PORT,strerror(errno));
    return;
  }
  struct sigaction action={0};
  action.sa_handler=dashboard_sigcont;
  sigemptyset(&action.sa_mask);
  if(sigaction(SIGCONT,&action,NULL))
    logq("Dashboard: SIGCONT handler unavailable: %s",strerror(errno));
  pthread_t thread;
  int rc=pthread_create(&thread,NULL,dashboard_worker,(void*)(intptr_t)fd);
  if(rc) {
    logq("PS5Save+ dashboard thread unavailable: %s",strerror(rc));
    close(fd);
    return;
  }
  pthread_detach(thread);
  logq("PS5Save+ dashboard listening on TCP %d (read-only)",DASHBOARD_PORT);
}
#endif /* !BACKUP_HOST_TEST */
#endif /* PS5SAVE_PLUS_DASHBOARD_H */

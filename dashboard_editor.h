/* PS5Save+ optional edit capability. Physical-console pairing is required.
 * Independent short-lived browser sessions are stored only in process memory.
 * No passwords or tokens are stored on USB, exposed in logs, or accepted in query strings.
 */
#ifndef PS5SAVE_PLUS_EDITOR_H
#define PS5SAVE_PLUS_EDITOR_H

#define EDITOR_CONFIG_MAX 16384
#define EDITOR_PIN_SECONDS 120.0
#define EDITOR_SESSION_SECONDS 3600.0
#define EDITOR_PAIR_COOLDOWN_SECONDS 60.0
#define EDITOR_PAIR_RESEND_SECONDS 5.0
#define EDITOR_MAX_SESSIONS 8

typedef struct {
  char token[33];
  double expiry;
} editor_session_t;

static char editor_pin[7];
static editor_session_t editor_sessions[EDITOR_MAX_SESSIONS];
static double editor_pin_expiry;
static double editor_last_pair = -999999.0;
static double editor_last_notice = -999999.0;
static double editor_locked_until;
static unsigned editor_attempts;

/* Refuse to enable edits without proper entropy. Never fall back to time(),
 * PID, or a predictable random generator for authorization tokens. */
static int editor_random(void *bytes, size_t n) {
  int fd = open("/dev/urandom", O_RDONLY);
  if(fd < 0) return 0;
  unsigned char *b = bytes;
  size_t used = 0;
  while(used < n) {
    ssize_t count = read(fd, b + used, n - used);
    if(count < 0 && errno == EINTR) continue;
    if(count <= 0) { close(fd); return 0; }
    used += (size_t)count;
  }
  close(fd);
  return 1;
}

static int editor_equals(const char *a, const char *b, size_t n) {
  unsigned int diff = 0;
  for(size_t i = 0; i < n; i++) diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
  return diff == 0;
}

static int editor_code_valid(const char *code) {
  if(strlen(code) != 6) return 0;
  for(int i = 0; i < 6; i++) if(code[i] < '0' || code[i] > '9') return 0;
  return 1;
}

/* Protect against DNS rebinding: allow only numeric IPv4 addresses assigned
 * to this host, plus loopback. A malicious host name is never accepted. */
static int editor_valid_host(const char *value) {
  char host[64];
  const char *colon = strchr(value, ':');
  if(!colon || strcmp(colon, ":8755")) return 0;
  size_t n = (size_t)(colon - value);
  if(!n || n >= sizeof host) return 0;
  memcpy(host, value, n);
  host[n] = 0;
  if(!strcmp(host, "localhost") || !strcmp(host, "127.0.0.1")) return 1;
  struct in_addr address;
  if(inet_pton(AF_INET, host, &address) != 1) return 0;
  struct ifaddrs *interfaces;
  if(getifaddrs(&interfaces)) return 0;
  int found = 0;
  for(struct ifaddrs *p = interfaces; p; p = p->ifa_next) {
    if(p->ifa_addr && p->ifa_addr->sa_family == AF_INET &&
       ((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr == address.s_addr) {
      found = 1;
      break;
    }
  }
  freeifaddrs(interfaces);
  return found;
}

/* Reject duplicate header names and values that exceed the buffer. */
static int editor_header(const char *request, const char *key,
                         char *out, size_t out_size) {
  const char *line = strstr(request, "\r\n");
  if(!line) return -1;
  line += 2;
  int count = 0;
  size_t k = strlen(key);
  out[0] = 0;
  while(line[0] && !(line[0] == '\r' && line[1] == '\n')) {
    const char *end = strstr(line, "\r\n");
    if(!end) return -1;
    if((size_t)(end - line) > k && !strncasecmp(line, key, k) && line[k] == ':') {
      const char *value = line + k + 1;
      while(value < end && (*value == ' ' || *value == '\t')) value++;
      while(end > value && (end[-1] == ' ' || end[-1] == '\t')) end--;
      size_t n = (size_t)(end - value);
      if(++count > 1 || n >= out_size) return -1;
      memcpy(out, value, n);
      out[n] = 0;
    }
    line = strstr(line, "\r\n");
    if(!line) return -1;
    line += 2;
  }
  return count;
}

static int editor_request_origin(const char *headers) {
  char host[96], origin[128], marker[32], transfer[32];
  if(editor_header(headers, "Host", host, sizeof host) != 1 ||
     editor_header(headers, "Origin", origin, sizeof origin) != 1 ||
     editor_header(headers, "X-PS5Save-Request", marker, sizeof marker) != 1 ||
     editor_header(headers, "Transfer-Encoding", transfer, sizeof transfer) != 0)
    return 0;
  if(!editor_valid_host(host) || strcmp(marker, "1")) return 0;
  char expected[128];
  int n = snprintf(expected, sizeof expected, "http://%s", host);
  return n > 0 && (size_t)n < sizeof expected && !strcmp(origin, expected);
}

/* Authorization is scoped to this browser's token, never to a global flag.
 * A second successful pairing must not revoke an active editing session. */
static int editor_session_valid(const char *token) {
  if(!token || strlen(token) != 32) return 0;
  double now = now_seconds();
  for(int i = 0; i < EDITOR_MAX_SESSIONS; i++) {
    editor_session_t *session = &editor_sessions[i];
    if(session->token[0] && now >= session->expiry) {
      memset(session, 0, sizeof *session);
      continue;
    }
    if(session->token[0] && editor_equals(token, session->token, 32))
      return 1;
  }
  return 0;
}

static int editor_authorized(const char *headers) {
  char token[48];
  return editor_header(headers, "X-PS5Save-Session", token, sizeof token) == 1 &&
         editor_session_valid(token);
}

static int editor_free_session_slot(void) {
  double now = now_seconds();
  for(int i = 0; i < EDITOR_MAX_SESSIONS; i++) {
    editor_session_t *session = &editor_sessions[i];
    if(!session->token[0] || now >= session->expiry) {
      memset(session, 0, sizeof *session);
      return i;
    }
  }
  return -1; /* Never evict another browser's active authorization. */
}

static void editor_session_status(int fd, const char *headers) {
  if(editor_authorized(headers))
    dashboard_text(fd, 200, "Editing already enabled in this browser.");
  else
    dashboard_text(fd, 403, "Editing session expired. Unlock editing again.");
}

static void editor_start_pair(int fd, const char *headers) {
  double now = now_seconds();
  if(editor_authorized(headers)) {
    dashboard_text(fd, 409, "Editing already enabled in this browser.");
    return;
  }
  if(now < editor_locked_until) {
    dashboard_text(fd, 429, "Pairing temporarily locked.");
    return;
  }
  const int reuse = editor_pin[0] && now < editor_pin_expiry;
  if(reuse && now < editor_last_notice + EDITOR_PAIR_RESEND_SECONDS) {
    dashboard_text(fd, 429, "Wait briefly before resending the active pairing code.");
    return;
  }
  if(!reuse && now < editor_last_pair + EDITOR_PAIR_COOLDOWN_SECONDS) {
    dashboard_text(fd, 429, "Wait before requesting another pairing code.");
    return;
  }
  char code[7];
  if(reuse) {
    strcpy(code, editor_pin);
  } else {
    uint32_t random;
    if(!editor_random(&random, sizeof random)) {
      dashboard_text(fd, 503, "Secure randomness unavailable.");
      return;
    }
    snprintf(code, sizeof code, "%06u", (unsigned)(random % 1000000u));
  }
  char msg[120];
  if(reuse) {
    unsigned remaining = (unsigned)(editor_pin_expiry - now) + 1;
    snprintf(msg, sizeof msg, "PS5Save+ pairing code: %s (expires in %u sec).",
             code, remaining);
  } else {
    snprintf(msg, sizeof msg, "PS5Save+ pairing code: %s (valid for 2 min).", code);
  }
  pthread_mutex_lock(&notification_mutex);
  int sent = send_notification(msg); /* Explicit user request, even in off mode. */
  pthread_mutex_unlock(&notification_mutex);
  if(!sent) {
    dashboard_text(fd, 503, "Could not display the pairing code on PS5.");
    return;
  }
  editor_last_notice = now;
  if(!reuse) {
    strcpy(editor_pin, code);
    editor_pin_expiry = now + EDITOR_PIN_SECONDS;
    editor_last_pair = now;
    editor_attempts = 0;
  }
  dashboard_text(fd, 200, reuse ?
    "The same pairing code was sent again; its original expiry is unchanged." :
    "A six-digit pairing code was sent to the PS5.");
}

static void editor_unlock(int fd, const char *body, size_t len) {
  double now = now_seconds();
  if(now < editor_locked_until) {
    dashboard_text(fd, 429, "Too many attempts. Try later.");
    return;
  }
  if(!editor_pin[0] || now >= editor_pin_expiry) {
    dashboard_text(fd, 403, "Pairing code expired. Request a new code.");
    return;
  }
  int good = len == 6 && editor_code_valid(body) && editor_equals(editor_pin, body, 6);
  if(!good) {
    if(++editor_attempts >= 5) {
      editor_locked_until = now + 300;
      editor_pin[0] = 0;
    }
    dashboard_text(fd, 403, "Incorrect pairing code.");
    return;
  }
  int slot = editor_free_session_slot();
  if(slot < 0) {
    dashboard_text(fd, 429, "Too many active editing sessions. Try again later.");
    return;
  }
  unsigned char random[16];
  if(!editor_random(random, sizeof random)) {
    dashboard_text(fd, 503, "Secure randomness unavailable.");
    return;
  }
  static const char hex[] = "0123456789abcdef";
  editor_session_t *session = &editor_sessions[slot];
  for(size_t i = 0; i < sizeof random; i++) {
    session->token[2*i] = hex[random[i] >> 4];
    session->token[2*i+1] = hex[random[i] & 15];
  }
  session->token[32] = 0;
  session->expiry = now + EDITOR_SESSION_SECONDS;
  editor_pin[0] = 0; /* Single-use code; earlier browser sessions stay active. */
  editor_attempts = 0;
  dashboard_respond(fd, 200, "text/plain; charset=utf-8", session->token, 32);
}

static uint64_t editor_hash(const char *content, size_t n) {
  uint64_t hash = UINT64_C(14695981039346656037);
  for(size_t i = 0; i < n; i++) {
    hash ^= (unsigned char)content[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static int editor_read_config(char **text, size_t *length) {
  *text = NULL;
  *length = 0;
  struct stat st;
  if(stat(CONF_PATH, &st) || !S_ISREG(st.st_mode) ||
     st.st_size < 1 || st.st_size > EDITOR_CONFIG_MAX) return 0;
  FILE *f = fopen(CONF_PATH, "rb");
  if(!f) return 0;
  char *buffer = malloc((size_t)st.st_size + 1);
  if(!buffer) { fclose(f); return 0; }
  size_t n = fread(buffer, 1, (size_t)st.st_size, f);
  int error = ferror(f);
  fclose(f);
  if(error || n != (size_t)st.st_size || memchr(buffer, 0, n)) {
    free(buffer);
    return 0;
  }
  buffer[n] = 0;
  *text = buffer;
  *length = n;
  return 1;
}

static void editor_serve_version(int fd) {
  char *buffer;
  size_t len;
  if(!editor_read_config(&buffer, &len)) {
    dashboard_text(fd, 503, "Configuration unavailable");
    return;
  }
  char hash[17];
  snprintf(hash, sizeof hash, "%016llx",
           (unsigned long long)editor_hash(buffer, len));
  free(buffer);
  dashboard_respond(fd, 200, "text/plain", hash, 16);
}

static int editor_yesno(const char *s) {
  return !strcmp(s, "true") || !strcmp(s, "false");
}

static int editor_integer(const char *s, long min, long max) {
  if(!*s) return 0;
  for(const unsigned char *p=(const unsigned char *)s; *p; p++)
    if(*p < '0' || *p > '9') return 0;
  errno = 0;
  char *end = NULL;
  long x = strtol(s, &end, 10);
  return !errno && end && *end == 0 && x >= min && x <= max;
}

/* Validate every line, not just settings rendered by the simplified UI.
 * Unknown options are refused instead of silently producing wrong backups.
 * Never accept filesystem-root, USB, or parent traversal as a source.
 */
static int editor_validate(const char *text, size_t length) {
  if(!length || length > EDITOR_CONFIG_MAX || memchr(text, 0, length)) return 0;
  char *copy = malloc(length + 1);
  if(!copy) return 0;
  memcpy(copy, text, length);
  copy[length] = 0;
  int ok = 1;
  char *cursor = copy;
  while(ok && *cursor) {
    char *newline = strchr(cursor, '\n');
    if(newline) *newline = 0;
    size_t n = strlen(cursor);
    if(n && cursor[n-1] == '\r') cursor[--n] = 0;
    while(n && (cursor[n-1] == ' ' || cursor[n-1] == '\t'))
      cursor[--n] = 0;
    char *line = cursor;
    while(*line == ' ' || *line == '\t') line++;
    n = strlen(line);
    if(n >= PATH_MAX || n >= 4096) { ok = 0; break; }
    if(*line && *line != '#') {
      char *equal = strchr(line, '=');
      if(equal) {
        *equal++ = 0;
        char *key = line;
        while(*key == ' ' || *key == '\t') key++;
        size_t klen = strlen(key);
        while(klen && (key[klen-1] == ' ' || key[klen-1] == '\t')) key[--klen] = 0;
        while(*equal == ' ' || *equal == '\t') equal++;
        if(!strcmp(key, "backup_interval_seconds") || !strcmp(key, "interval"))
          ok = editor_integer(equal, 30, 3600);
        else if(!strcmp(key, "max_title_size_mb") || !strcmp(key, "max_game_mb"))
          ok = editor_integer(equal, 64, 65536);
        else if(!strcmp(key, "daily_snapshots_to_keep") || !strcmp(key, "dailybackups"))
          ok = editor_integer(equal, 0, 10);
        else if(!strcmp(key, "backup_notifications"))
          ok = !strcmp(equal, "all") || !strcmp(equal, "errors") || !strcmp(equal, "off");
        else if(!strcmp(key, "backup_usb_device"))
          ok = !strcmp(equal, "auto") ||
               (strlen(equal) == 9 && !strncmp(equal, "/mnt/usb", 8) &&
                equal[8] >= '0' && equal[8] <= '7');
        else if(!strcmp(key, "backup_notifications_in_game") ||
                !strcmp(key, "backup_ps4_saves") ||
                !strcmp(key, "backup_ps5_saves") ||
                !strcmp(key, "backup_encrypted_saves") ||
                !strcmp(key, "backup_ps4_trophies") ||
                !strcmp(key, "backup_ps5_trophies") ||
                !strcmp(key, "ps4saves") || !strcmp(key, "ps5saves"))
          ok = editor_yesno(equal);
        else ok = 0;
      } else if(line[0] == '/') {
        size_t usb_len = strlen(USB_ROOT);
        if(!strcmp(line, "/") || !strncmp(line, USB_ROOT, usb_len) ||
           strstr(line, "/../") || !strcmp(line + (n >= 3 ? n - 3 : 0), "/.."))
          ok = 0;
        for(size_t i=0; ok && i<n; i++) {
          unsigned char ch = (unsigned char)line[i];
          if(ch < 32 || ch == 127) ok = 0;
          if(ch == '*' && i != n-1) ok = 0;
        }
      } else ok = 0;
    }
    cursor = newline ? newline + 1 : cursor + strlen(cursor);
  }
  free(copy);
  return ok;
}

static int editor_write_exact(int fd, const char *data, size_t len) {
  while(len) {
    ssize_t n = write(fd, data, len);
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) return 0;
    data += (size_t)n;
    len -= (size_t)n;
  }
  return 1;
}

static int editor_atomic_file(const char *target, const char *suffix,
                              const char *data, size_t len) {
  char tmp[PATH_MAX];
  if(snprintf(tmp, sizeof tmp, "%s%s", target, suffix) >= (int)sizeof tmp)
    return 0;
  unlink(tmp); /* fixed file path, not user controlled */
  int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if(fd < 0) return 0;
  int ok = editor_write_exact(fd, data, len) && !fsync(fd);
  if(close(fd)) ok = 0;
  if(ok) ok = rename(tmp, target) == 0;
  if(!ok) unlink(tmp);
  return ok;
}

static void editor_save_unlocked(int fd, const char *headers,
                                 const char *body, size_t len) {
  if(!editor_authorized(headers)) {
    dashboard_text(fd, 403, "Pair this browser with the PS5 before editing.");
    return;
  }
#ifdef PS5SAVE_DYNAMIC_STORAGE
  if(atomic_load(&storage_migrating)) {
    dashboard_text(fd, 409, "USB storage transfer in progress. Try again after completion.");
    return;
  }
#endif
  char version[32];
  if(editor_header(headers, "X-PS5Save-Version", version, sizeof version) != 1 ||
     strlen(version) != 16) {
    dashboard_text(fd, 400, "Missing or invalid configuration version.");
    return;
  }
  char *old;
  size_t old_len;
  if(!editor_read_config(&old, &old_len)) {
    dashboard_text(fd, 503, "Configuration unavailable");
    return;
  }
  char current_version[17];
  snprintf(current_version, sizeof current_version, "%016llx",
           (unsigned long long)editor_hash(old, old_len));
  if(!editor_equals(current_version, version, 16)) {
    free(old);
    dashboard_text(fd, 409, "Configuration was changed elsewhere. Reload first.");
    return;
  }
  if(!editor_validate(body, len)) {
    free(old);
    dashboard_text(fd, 422, "Configuration is invalid. No changes were saved.");
    return;
  }
#ifdef PS5SAVE_DYNAMIC_STORAGE
  /* Explicit destinations must be mounted writable USB devices. Never
   * accept a path within /data or any other internal filesystem. */
  char *draft = malloc(len + 1);
  if(!draft) {free(old);dashboard_text(fd, 503, "Insufficient memory.");return;}
  memcpy(draft, body, len);draft[len] = 0;
  char *saveptr = NULL;
  for(char *line = strtok_r(draft, "\n", &saveptr);line;
      line = strtok_r(NULL, "\n", &saveptr)) {
    char *key = strstr(line, "backup_usb_device");
    if(!key) continue;
    if(key != line) {
      while(line < key && (*line == ' ' || *line == '\t')) line++;
      if(line != key) continue;
    }
    char *eq = strchr(key, '=');
    if(!eq) continue;
    eq++;
    while(*eq == ' ' || *eq == '\t') eq++;
    char *end = eq + strlen(eq);
    while(end > eq && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
      *--end = 0;
    if(strcmp(eq, "auto") && !storage_usb_online(eq, NULL, NULL)) {
      free(draft);free(old);
      dashboard_text(fd, 422, "Selected USB is not mounted or writable.");
      return;
    }
  }
  free(draft);
#endif
  /* Retain one previous version on the same USB. Writes are completed to
   * separate temp files and atomically renamed. Readers never see half a file.
   */
#ifdef PS5SAVE_DYNAMIC_STORAGE
  const char *backup_path = storage_conf_bak;
#else
  const char *backup_path = CONF_PATH ".bak";
#endif
  int backup_saved = editor_atomic_file(backup_path, ".part", old, old_len);
  free(old);
  if(!backup_saved ||
     !editor_atomic_file(CONF_PATH, ".part", body, len)) {
    dashboard_text(fd, 503, "Could not save safely; original configuration preserved.");
    return;
  }
  logq("PS5Save+ dashboard: backup.conf updated (previous saved as backup.conf.bak)");
  dashboard_text(fd, 200, "Configuration saved. Applies on the next backup check.");
}

static void editor_save(int fd, const char *headers,
                        const char *body, size_t len) {
#ifdef PS5SAVE_DYNAMIC_STORAGE
  if(atomic_load(&storage_migrating)) {
    dashboard_text(fd, 409, "USB transfer in progress; try again.");
    return;
  }
  pthread_mutex_lock(&storage_change_lock);
#endif
  editor_save_unlocked(fd, headers, body, len);
#ifdef PS5SAVE_DYNAMIC_STORAGE
  pthread_mutex_unlock(&storage_change_lock);
#endif
}

#endif

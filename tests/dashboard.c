/* Native-host smoke tests for the isolated dashboard component.
 * Run from repository root; dashboard/index.html is embedded by assembler.
 */
#include <assert.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

#define USB_ROOT TEST_ROOT "/usb"
#define CONF_PATH USB_ROOT "/backup.conf"
#define BACKUP_ROOT USB_ROOT "/backup"
#define DAILY_ROOT USB_ROOT "/backup_daily"

typedef struct backup_stats {
  uint64_t bytes;
  unsigned copied, unchanged, skipped, empty_sources, failed;
} backup_stats_t;

typedef enum { NOTICES_ALL, NOTICES_ERRORS, NOTICES_OFF } notice_mode_t;
typedef struct {
  notice_mode_t mode;
  int in_game;
} notification_settings_t;

static notification_settings_t notification_settings = { NOTICES_ALL, 0 };
static long g_interval = 300;
static pthread_mutex_t notification_mutex = PTHREAD_MUTEX_INITIALIZER;
static char delivered_pair_message[160];
static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}
static int send_notification(const char *message) {
  snprintf(delivered_pair_message, sizeof delivered_pair_message, "%s", message);
  return 1;
}

static int mkdir_p(const char *path) {
  char b[2048];
  size_t n = snprintf(b, sizeof b, "%s", path);
  if(!n || n >= sizeof b) return -1;
  for(char *p = b + 1; *p; p++) {
    if(*p != '/') continue;
    *p = 0;
    if(mkdir(b, 0700) && errno != EEXIST) return -1;
    *p = '/';
  }
  if(mkdir(b, 0700) && errno != EEXIST) return -1;
  return 0;
}

static void logq(const char *fmt, ...) { (void)fmt; }

#include "../dashboard.h"

static void write_file(const char *path, const char *contents) {
  FILE *f = fopen(path, "wb");
  assert(f && fwrite(contents, 1, strlen(contents), f) == strlen(contents));
  assert(!fclose(f));
}

static void make_request(const char *request, const char *expected) {
  int sockets[2];
  assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
  assert(send(sockets[1], request, strlen(request), 0) == (ssize_t)strlen(request));
  assert(!shutdown(sockets[1], SHUT_WR));
  dashboard_request(sockets[0]);
  close(sockets[0]);
  char response[32768];
  size_t n = 0;
  ssize_t got;
  while(n < sizeof response - 1 &&
        (got = recv(sockets[1], response + n, sizeof response - 1 - n, 0)) > 0)
    n += (size_t)got;
  close(sockets[1]);
  response[n] = 0;
  if(!strstr(response, expected)) {
    fprintf(stderr, "Expected %s, received: %.800s\n", expected, response);
    abort();
  }
}


static void test_config_editor(void) {
  const char *pair =
    "POST /api/pair HTTP/1.1\r\n"
    "Host: localhost:8755\r\n"
    "Origin: http://localhost:8755\r\n"
    "X-PS5Save-Request: 1\r\n"
    "Content-Length: 0\r\n\r\n";
  make_request(pair, "200 OK");
  assert(strstr(delivered_pair_message, "pairing code: "));
  char code[7];
  memcpy(code, strstr(delivered_pair_message, "pairing code: ") + strlen("pairing code: "), 6);
  code[6] = 0;
  assert(editor_code_valid(code));

  char req[21000];
  int n = snprintf(req, sizeof req,
    "POST /api/unlock HTTP/1.1\r\n"
    "Host: localhost:8755\r\n"
    "Origin: http://localhost:8755\r\n"
    "X-PS5Save-Request: 1\r\n"
    "Content-Length: 6\r\n\r\n"
    "000000");
  assert(n > 0 && (size_t)n < sizeof req);
  /* A wrong code is not accepted. */
  if(strcmp(code, "000000")) make_request(req, "403 Forbidden");
  n = snprintf(req, sizeof req,
    "POST /api/unlock HTTP/1.1\r\n"
    "Host: localhost:8755\r\n"
    "Origin: http://localhost:8755\r\n"
    "X-PS5Save-Request: 1\r\n"
    "Content-Length: 6\r\n\r\n%s", code);
  assert(n > 0 && (size_t)n < sizeof req);
  make_request(req, "200 OK");
  assert(strlen(editor_session) == 32);
  make_request(req, "403 Forbidden"); /* Pairing code is one-use. */

  const char *original = "backup_interval_seconds=300\nbackup_notifications=all\n";
  char version[17];
  snprintf(version, sizeof version, "%016llx",
           (unsigned long long)editor_hash(original, strlen(original)));
  make_request("GET /api/config-version HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               version);
  const char *bad = "backup_interval_seconds=60\n" USB_ROOT "/backup\n";
  n = snprintf(req, sizeof req,
    "POST /api/config HTTP/1.1\r\n"
    "Host: localhost:8755\r\n"
    "Origin: http://localhost:8755\r\n"
    "X-PS5Save-Request: 1\r\n"
    "X-PS5Save-Session: %s\r\n"
    "X-PS5Save-Version: %s\r\n"
    "Content-Length: %zu\r\n\r\n%s",
    editor_session, version, strlen(bad), bad);
  assert(n > 0 && (size_t)n < sizeof req);
  make_request(req, "422 Unprocessable Content");
  FILE *verify = fopen(CONF_PATH, "rb");
  assert(verify);
  char contents[256];
  size_t read = fread(contents, 1, sizeof contents - 1, verify);
  assert(!fclose(verify));
  contents[read] = 0;
  assert(!strcmp(contents, original));

  const char *updated = "backup_interval_seconds=60\nbackup_notifications=errors\n";
  n = snprintf(req, sizeof req,
    "POST /api/config HTTP/1.1\r\n"
    "Host: localhost:8755\r\n"
    "Origin: http://localhost:8755\r\n"
    "X-PS5Save-Request: 1\r\n"
    "X-PS5Save-Session: 00000000000000000000000000000000\r\n"
    "X-PS5Save-Version: %s\r\n"
    "Content-Length: %zu\r\n\r\n%s",
    version, strlen(updated), updated);
  assert(n > 0 && (size_t)n < sizeof req);
  make_request(req, "403 Forbidden");

  n = snprintf(req, sizeof req,
    "POST /api/config HTTP/1.1\r\n"
    "Host: localhost:8755\r\n"
    "Origin: http://localhost:8755\r\n"
    "X-PS5Save-Request: 1\r\n"
    "X-PS5Save-Session: %s\r\n"
    "X-PS5Save-Version: %s\r\n"
    "Content-Length: %zu\r\n\r\n%s",
    editor_session, version, strlen(updated), updated);
  assert(n > 0 && (size_t)n < sizeof req);
  make_request(req, "200 OK");
  verify = fopen(CONF_PATH, "rb");
  assert(verify);
  read = fread(contents, 1, sizeof contents - 1, verify);
  assert(!fclose(verify));
  contents[read] = 0;
  assert(!strcmp(contents, updated));

  verify = fopen(CONF_PATH ".bak", "rb");
  assert(verify);
  read = fread(contents, 1, sizeof contents - 1, verify);
  assert(!fclose(verify));
  contents[read] = 0;
  assert(!strcmp(contents, original));
  make_request(req, "409 Conflict"); /* Old version must not overwrite edits. */

  n = snprintf(req, sizeof req,
    "POST /api/config HTTP/1.1\r\n"
    "Host: attacker.example:8755\r\n"
    "Origin: http://attacker.example:8755\r\n"
    "X-PS5Save-Request: 1\r\n"
    "X-PS5Save-Session: %s\r\n"
    "Content-Length: 0\r\n\r\n", editor_session);
  assert(n > 0 && (size_t)n < sizeof req);
  make_request(req, "400 Bad Request"); /* DNS rebinding protection. */
  puts("PASS dashboard pairing, validation, atomic save and write protection");
}

int main(void) {
  (void)dashboard_start; /* Production starts the socket; tests use socketpairs. */
  assert(!mkdir_p(USB_ROOT));
  assert(!mkdir_p(BACKUP_ROOT "/saves"));
  assert(!mkdir_p(DAILY_ROOT "/2026-11-22"));
  write_file(CONF_PATH, "backup_interval_seconds=300\nbackup_notifications=all\n");
  assert(!mkdir_p(DASHBOARD_DATA_DIR));
  write_file(DASHBOARD_DATA_DIR "/events.log", "2026-11-22T13:00:00Z backup started\n");

  backup_stats_t stats = { .bytes = 12000, .copied = 2 };
  dashboard_set_running(1);
  dashboard_cycle_done(&stats, 0, 1, 0);

  struct stat st;
  assert(!stat(DASHBOARD_HISTORY, &st) && st.st_size > 0);
  make_request("GET / HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "PS5Save+");
  make_request("GET /api/status HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "\"copied\":2");
  make_request("GET /api/history HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "\tsuccess\t12000\t2\t0\t0\t1");
  make_request("GET /api/snapshots HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "2026-11-22");
  make_request("GET /api/backups HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "saves");
  make_request("GET /api/config HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "backup_notifications=all");
  make_request("GET /api/logs HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "backup started");
  make_request("POST /api/config HTTP/1.1\r\nHost: localhost:8755\r\nContent-Length: 0\r\n\r\n",
               "403 Forbidden");
  make_request("GET /../../etc/passwd HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "404 Not Found");
  test_config_editor();
  puts("PASS dashboard endpoints, persisted history and write security");
  return 0;
}

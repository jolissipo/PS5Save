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
#define STORAGE_ROOT USB_ROOT "/ps5save-plus"
#define CONF_PATH STORAGE_ROOT "/backup.conf"
#define BACKUP_ROOT STORAGE_ROOT "/backup"
#define DAILY_ROOT STORAGE_ROOT "/backup_daily"

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
static int notification_json_escape(char *dst, size_t cap, const char *src) {
  size_t used=0;
  const char hex[]="0123456789abcdef";
  for(const unsigned char *p=(const unsigned char*)src; *p; p++) {
    if(*p<32) {
      if(cap-used<7) return 0;
      dst[used++]='\\';dst[used++]='u';dst[used++]='0';dst[used++]='0';
      dst[used++]=hex[*p>>4];dst[used++]=hex[*p&15];
    } else {
      if((*p=='"' || *p=='\\') && cap-used<3) return 0;
      if(*p=='"' || *p=='\\')dst[used++]='\\';
      if(cap-used<2)return 0;
      dst[used++]=(char)*p;
    }
  }
  if(used>=cap) return 0;
  dst[used]=0;
  return 1;
}

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



static void test_file_browser(void) {
  assert(!mkdir_p(BACKUP_ROOT "/saves/ps5"));
  write_file(BACKUP_ROOT "/saves/ps5/PPSA12345", "TEST");
  write_file(DAILY_ROOT "/2026-11-22/snapshot.dat", "copy");
  make_request("GET /api/tree?root=live HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "\"name\":\"saves\"");
  make_request("GET /api/tree?root=live&path=saves%2Fps5 HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "\"name\":\"PPSA12345\"");
  make_request("GET /api/summary?root=live HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "\"files\":1");
  make_request("GET /api/summary?root=snapshot&date=2026-11-22 HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "\"bytes\":4");
  make_request("GET /api/tree?root=snapshot&date=2026-11-22 HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "\"name\":\"snapshot.dat\"");
  make_request("GET /api/tree?root=snapshot&date=2026-11-22&path=.. HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "400 Bad Request");
  make_request("GET /api/tree?root=live&path=saves%2F.. HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "400 Bad Request");
  make_request("GET /api/tree?root=live&path=%00 HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "400 Bad Request");
  make_request("GET /api/tree?root=live&path=%2fetc HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "400 Bad Request");
  assert(!symlink("/etc", BACKUP_ROOT "/outside"));
  make_request("GET /api/tree?root=live&path=outside HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "400 Bad Request");

  /* A large directory must be browsable beyond the initial 64 results. */
  for(int i=0;i<72;i++) {
    char path[PATH_MAX];
    assert(snprintf(path,sizeof path,"%s/item%03d",BACKUP_ROOT,i) < (int)sizeof path);
    write_file(path,"x");
  }
  make_request("GET /api/tree?root=live HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "\"next\":\"Fitem062\"");
  make_request("GET /api/tree?root=live&after=Fitem062 HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "\"name\":\"item071\"");
  make_request("GET /api/tree?root=live&after=Fitem062 HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "\"next\":null");
  puts("PASS dashboard tree pagination, snapshot inventory and path confinement");
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
  test_file_browser();
  puts("PASS dashboard endpoints, persisted history and write security");
  return 0;
}

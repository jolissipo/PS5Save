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

#define DASHBOARD_PORT 18755 /* Keep test traffic away from real service. */
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
static unsigned delivered_pair_count;
static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}
static int send_notification(const char *message) {
  snprintf(delivered_pair_message, sizeof delivered_pair_message, "%s", message);
  delivered_pair_count++;
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

static void test_usb_config_validation(void) {
  const char *good_auto = "backup_usb_device=auto\n";
  const char *good_usb = "backup_usb_device=/mnt/usb7\n";
  const char *bad_internal = "backup_usb_device=/data\n";
  const char *bad_outside = "backup_usb_device=/mnt/usb8\n";
  assert(editor_validate(good_auto, strlen(good_auto)));
  assert(editor_validate(good_usb, strlen(good_usb)));
  assert(!editor_validate(bad_internal, strlen(bad_internal)));
  assert(!editor_validate(bad_outside, strlen(bad_outside)));
  make_request("GET /api/usb-devices HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "\"current\":");
  puts("PASS USB configuration accepts only automatic or PS5 USB slots");
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
  double original_expiry=editor_pin_expiry;
  unsigned initial_notifications=delivered_pair_count;
  make_request(pair, "Wait briefly before resending the active pairing code.");
  assert(delivered_pair_count==initial_notifications);
  editor_last_notice-=EDITOR_PAIR_RESEND_SECONDS;
  make_request(pair, "The same pairing code was sent again");
  assert(editor_pin_expiry==original_expiry);
  assert(strstr(delivered_pair_message, code));
  assert(strstr(delivered_pair_message, "expires in"));

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
  if(strcmp(code, "000000")){
    make_request(req, "403 Forbidden");
    assert(editor_attempts==1);
  }
  unsigned attempts_before_resend=editor_attempts;
  editor_last_notice-=EDITOR_PAIR_RESEND_SECONDS;
  make_request(pair, "The same pairing code was sent again");
  assert(editor_attempts==attempts_before_resend);
  assert(editor_pin_expiry==original_expiry);
  n = snprintf(req, sizeof req,
    "POST /api/unlock HTTP/1.1\r\n"
    "Host: localhost:8755\r\n"
    "Origin: http://localhost:8755\r\n"
    "X-PS5Save-Request: 1\r\n"
    "Content-Length: 6\r\n\r\n%s", code);
  assert(n > 0 && (size_t)n < sizeof req);
  make_request(req, "200 OK");
  assert(strlen(editor_sessions[0].token) == 32);
  make_request(req, "403 Forbidden"); /* Pairing code is one-use. */
  char first_token[33];
  strcpy(first_token, editor_sessions[0].token);
  assert(editor_session_valid(first_token));

  /* Already-paired browser is refused server-side; another browser is not. */
  n=snprintf(req,sizeof req,
    "POST /api/pair HTTP/1.1\r\n"
    "Host: localhost:8755\r\nOrigin: http://localhost:8755\r\n"
    "X-PS5Save-Request: 1\r\nX-PS5Save-Session: %s\r\n"
    "Content-Length: 0\r\n\r\n",first_token);
  assert(n>0 && (size_t)n<sizeof req);
  unsigned before_rejected_pair=delivered_pair_count;
  make_request(req,"409 Conflict");
  assert(delivered_pair_count==before_rejected_pair);

  /* A fresh browser is allowed to pair while the first remains authorized.
   * Advance the rate-limit timestamp instead of sleeping for a full minute. */
  make_request(pair, "Wait before requesting another pairing code.");
  editor_last_pair-=EDITOR_PAIR_COOLDOWN_SECONDS;
  make_request(pair, "200 OK");
  char second_code[7];
  memcpy(second_code,strstr(delivered_pair_message,"pairing code: ")+strlen("pairing code: "),6);
  second_code[6]=0;
  n=snprintf(req,sizeof req,
    "POST /api/unlock HTTP/1.1\r\n"
    "Host: localhost:8755\r\nOrigin: http://localhost:8755\r\n"
    "X-PS5Save-Request: 1\r\nContent-Length: 6\r\n\r\n%s",second_code);
  assert(n>0 && (size_t)n<sizeof req);
  make_request(req,"200 OK");
  char second_token[33];
  strcpy(second_token,editor_sessions[1].token);
  assert(editor_session_valid(first_token) && editor_session_valid(second_token));
  assert(strcmp(first_token,second_token));

  n=snprintf(req,sizeof req,
    "POST /api/session HTTP/1.1\r\n"
    "Host: localhost:8755\r\nOrigin: http://localhost:8755\r\n"
    "X-PS5Save-Request: 1\r\nX-PS5Save-Session: %s\r\n"
    "Content-Length: 0\r\n\r\n",first_token);
  assert(n>0 && (size_t)n<sizeof req);
  make_request(req,"Editing already enabled in this browser.");
  n=snprintf(req,sizeof req,
    "POST /api/session HTTP/1.1\r\n"
    "Host: localhost:8755\r\nOrigin: http://localhost:8755\r\n"
    "X-PS5Save-Request: 1\r\nX-PS5Save-Session: %s\r\n"
    "Content-Length: 0\r\n\r\n",second_token);
  assert(n>0 && (size_t)n<sizeof req);
  make_request(req,"200 OK");
  /* A real token is required; no information is disclosed to random clients. */
  make_request("POST /api/session HTTP/1.1\r\n"
    "Host: localhost:8755\r\nOrigin: http://localhost:8755\r\n"
    "X-PS5Save-Request: 1\r\nContent-Length: 0\r\n\r\n", "403 Forbidden");

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
    editor_sessions[0].token, version, strlen(bad), bad);
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
    editor_sessions[0].token, version, strlen(updated), updated);
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

  /* The second browser can edit too, but its stale configuration is rejected. */
  n=snprintf(req,sizeof req,
    "POST /api/config HTTP/1.1\r\n"
    "Host: localhost:8755\r\nOrigin: http://localhost:8755\r\n"
    "X-PS5Save-Request: 1\r\nX-PS5Save-Session: %s\r\n"
    "X-PS5Save-Version: %s\r\nContent-Length: %zu\r\n\r\n%s",
    second_token,version,strlen(updated),updated);
  assert(n>0 && (size_t)n<sizeof req);
  make_request(req,"409 Conflict");
  char newest_version[17];
  snprintf(newest_version,sizeof newest_version,"%016llx",
    (unsigned long long)editor_hash(updated,strlen(updated)));
  const char *other_update="backup_interval_seconds=120\nbackup_notifications=errors\n";
  n=snprintf(req,sizeof req,
    "POST /api/config HTTP/1.1\r\n"
    "Host: localhost:8755\r\nOrigin: http://localhost:8755\r\n"
    "X-PS5Save-Request: 1\r\nX-PS5Save-Session: %s\r\n"
    "X-PS5Save-Version: %s\r\nContent-Length: %zu\r\n\r\n%s",
    second_token,newest_version,strlen(other_update),other_update);
  assert(n>0 && (size_t)n<sizeof req);
  make_request(req,"200 OK");
  assert(editor_session_valid(first_token) && editor_session_valid(second_token));

  n = snprintf(req, sizeof req,
    "POST /api/config HTTP/1.1\r\n"
    "Host: attacker.example:8755\r\n"
    "Origin: http://attacker.example:8755\r\n"
    "X-PS5Save-Request: 1\r\n"
    "X-PS5Save-Session: %s\r\n"
    "Content-Length: 0\r\n\r\n", editor_sessions[0].token);
  assert(n > 0 && (size_t)n < sizeof req);
  make_request(req, "400 Bad Request"); /* DNS rebinding protection. */

  /* Expiring one session cannot revoke the other. */
  editor_sessions[0].expiry=now_seconds()-1;
  assert(!editor_session_valid(first_token));
  assert(editor_session_valid(second_token));

  /* An expired PIN is replaced, but it cannot be refreshed by resending. */
  editor_last_pair-=EDITOR_PAIR_COOLDOWN_SECONDS;
  make_request(pair,"200 OK");
  editor_pin_expiry=now_seconds()-1;
  editor_last_pair-=EDITOR_PAIR_COOLDOWN_SECONDS;
  make_request(pair,"200 OK");
  assert(editor_pin_expiry>now_seconds());
  assert(editor_attempts==0);

  /* Five incorrect codes lock subsequent resends/new PINs. */
  char invalid[7]="000000";
  if(!strcmp(invalid,editor_pin))strcpy(invalid,"999999");
  for(int i=0;i<5;i++){
    n=snprintf(req,sizeof req,
      "POST /api/unlock HTTP/1.1\r\n"
      "Host: localhost:8755\r\nOrigin: http://localhost:8755\r\n"
      "X-PS5Save-Request: 1\r\nContent-Length: 6\r\n\r\n%s", invalid);
    assert(n>0 && (size_t)n<sizeof req);
    make_request(req,"403 Forbidden");
  }
  assert(editor_locked_until>now_seconds());
  editor_last_notice-=EDITOR_PAIR_RESEND_SECONDS;
  make_request(pair,"Pairing temporarily locked.");
  assert(editor_session_valid(second_token));

  puts("PASS browser-isolated pairing, PIN reuse, security lockout, config version and atomic writes");
}

/* Exercise real socket rebind after a simulated SIGCONT, not only helpers. */
static void test_resume_http(void) {
  dashboard_start();
  /* The initial listener is created synchronously in dashboard_start. */
  assert(!atomic_load(&dashboard_listener_restores));
  dashboard_on_continue(SIGCONT);
  for(int n=0; n<40 && !atomic_load(&dashboard_listener_restores); n++)
    usleep(200000);
  assert(atomic_load(&dashboard_listener_restores)>0);
  int fd=socket(AF_INET,SOCK_STREAM,0);
  assert(fd>=0);
  struct sockaddr_in address={0};
  address.sin_family=AF_INET;
  address.sin_port=htons(DASHBOARD_PORT);
  assert(inet_pton(AF_INET,"127.0.0.1",&address.sin_addr)==1);
  assert(!connect(fd,(struct sockaddr*)&address,sizeof address));
  const char request[]="GET /api/status HTTP/1.1\r\nHost: localhost:8755\r\n\r\n";
  assert(send(fd,request,strlen(request),0)==(ssize_t)strlen(request));
  char answer[1024]={0};
  struct timeval timeout={.tv_sec=3,.tv_usec=0};
  setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
  ssize_t got=recv(fd,answer,sizeof answer-1,0);
  assert(got>0 && strstr(answer,"200 OK"));
  close(fd);
  puts("PASS dashboard HTTP listener reopened after simulated resume");
}

int main(void) {
  assert(!mkdir_p(USB_ROOT));
  assert(!mkdir_p(BACKUP_ROOT "/saves"));
  assert(!mkdir_p(DAILY_ROOT "/2026-11-22"));
  write_file(CONF_PATH, "backup_interval_seconds=300\nbackup_notifications=all\n");
  assert(!mkdir_p(DASHBOARD_DATA_DIR));
  write_file(DASHBOARD_DATA_DIR "/events.log",
             "2026-11-22T13:00:00Z backup started\n"
             "2026-11-22T13:00:01Z [Launcher] registration accepted\n");

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
  make_request("GET /api/logs HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "[Launcher] registration accepted");
  make_request("GET /api/installer-log HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "404 Not Found");
  make_request("POST /api/config HTTP/1.1\r\nHost: localhost:8755\r\nContent-Length: 0\r\n\r\n",
               "403 Forbidden");
  make_request("GET /../../etc/passwd HTTP/1.1\r\nHost: localhost:8755\r\n\r\n",
               "404 Not Found");
  test_usb_config_validation();
  test_config_editor();
  test_file_browser();
  test_resume_http();
  puts("PASS dashboard endpoints, persisted history and write security");
  return 0;
}

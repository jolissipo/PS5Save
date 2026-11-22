#include <assert.h>
#include <dirent.h>
#include <stdatomic.h>
#include <stdlib.h>

#define BACKUP_HOST_TEST
#define USB_ROOT TEST_ROOT "/usb"
#define HOME_ROOT TEST_ROOT "/home"
#define TMP_DIR TEST_ROOT "/tmp"
#define PFS_ROOT TEST_ROOT "/pfs"
#define PFSMGR_DEV TEST_ROOT "/unavailable-pfsmgr"
#define NOTIFICATION_ICON_DIR TEST_ROOT "/icons"
#define LOCK_PORT 0

static int fail_copy_buffer;
static void *host_malloc(size_t size);
static struct dirent *host_readdir(DIR *dir);

#define malloc host_malloc
#define main daemon_main
#define readdir host_readdir
#include "../main.c"
#undef readdir
#undef main
#undef malloc

#define SOURCE TEST_ROOT "/save.dat"
#define OVERSIZED TEST_ROOT "/oversized.dat"
#define MISSING TEST_ROOT "/missing.dat"
#define EMPTY TEST_ROOT "/empty"
#define NESTED TEST_ROOT "/nested"
#define ACTIVITY_PATH TEST_ROOT "/activity"
#define CHECKING "Checking for backup changes."
#define COPYING "Changes found. Backing up."
#define NO_CHANGES "No changes. Backup not needed."
#define LOG_CAPACITY 256

static pthread_mutex_t capture_mutex = PTHREAD_MUTEX_INITIALIZER;
static char toasts[LOG_CAPACITY][1024], logs[LOG_CAPACITY][512];
static double toast_times[LOG_CAPACITY];
static unsigned sent, logged;
static int fail_notification_send;
static int test_console_language = 1;
int sceSystemServiceParamGetInt(int param_id, int *value) {
  assert(param_id == 1);
  *value = test_console_language;
  return 0;
}
static const char *fail_readdir_path;
static atomic_uint sending, progress_attempts;

static void *
host_malloc(size_t size) {
  if(fail_copy_buffer && size == COPY_BUF_SZ) return NULL;
  return malloc(size);
}

static struct dirent *
host_readdir(DIR *dir) {
  struct stat actual, selected;
  int previous_errno = errno;
  if(fail_readdir_path && !stat(fail_readdir_path, &selected) &&
     !fstat(dirfd(dir), &actual) && actual.st_dev == selected.st_dev &&
     actual.st_ino == selected.st_ino) {
    errno = EIO;
    return NULL;
  }
  errno = previous_errno;
  return readdir(dir);
}

intptr_t kernel_get_root_vnode(void) { return 0; }
intptr_t kernel_get_proc_jaildir(pid_t pid) { (void)pid; return 0; }
int32_t kernel_set_proc_rootdir(pid_t pid, intptr_t vnode) { (void)pid; (void)vnode; return 0; }
int32_t kernel_set_proc_jaildir(pid_t pid, intptr_t vnode) { (void)pid; (void)vnode; return 0; }
uint64_t kernel_get_ucred_authid(pid_t pid) { (void)pid; return 0; }
int32_t kernel_set_ucred_authid(pid_t pid, uint64_t authid) { (void)pid; (void)authid; return 0; }
int32_t kernel_get_ucred_caps(pid_t pid, uint8_t caps[16]) { (void)pid; memset(caps, 0, 16); return 0; }
int32_t kernel_set_ucred_caps(pid_t pid, const uint8_t caps[16]) { (void)pid; (void)caps; return 0; }
int sceFsInitMountSaveDataOpt(MountOpt *opt) { (void)opt; return 0; }
int sceFsMountSaveData(MountOpt *opt, const char *path, const char *mount, uint8_t *key) {
  (void)opt; (void)path; (void)mount; (void)key;
  abort(); /* Real mounts must never be reached in a host scenario. */
}
int sceFsInitUmountSaveDataOpt(UmountOpt *opt) { (void)opt; return 0; }
int sceFsUmountSaveData(UmountOpt *opt, const char *mount, int handle, int ignore) {
  (void)opt; (void)mount; (void)handle; (void)ignore;
  return -1;
}

int
klog_printf(const char *fmt, ...) {
  pthread_mutex_lock(&capture_mutex);
  assert(logged < LOG_CAPACITY);
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(logs[logged], sizeof logs[logged], fmt, ap);
  va_end(ap);
  logged++;
  pthread_mutex_unlock(&capture_mutex);
  return len;
}

int
sceNotificationSend(int user_id, bool is_logged, const char *payload) {
  assert(atomic_fetch_add(&sending, 1) == 0);
  assert(user_id == NOTIFICATION_USER_ID && is_logged);
  assert(strstr(payload, NOTIFICATION_ICON_PATH));
  assert(strstr(payload, "\"bundleName\":\"PS5Save+\""));
  assert(strstr(payload, "\"localNotificationId\":\"ps5save-"));
  if(fail_notification_send) {
    assert(atomic_fetch_sub(&sending, 1) == 1);
    return -1;
  }
  pthread_mutex_lock(&capture_mutex);
  assert(sent < LOG_CAPACITY && strlen(payload) < sizeof toasts[sent]);
  toast_times[sent] = now_seconds();
  strcpy(toasts[sent++], payload);
  pthread_mutex_unlock(&capture_mutex);
  assert(atomic_fetch_sub(&sending, 1) == 1);
  return 0;
}

static unsigned
toast_count(const char *text) {
  unsigned count = 0;
  pthread_mutex_lock(&capture_mutex);
  for(unsigned i = 0; i < sent; i++)
    if(!text || strstr(toasts[i], text)) count++;
  pthread_mutex_unlock(&capture_mutex);
  return count;
}

static void
expect_toast(unsigned index, const char *text) {
  pthread_mutex_lock(&capture_mutex);
  assert(index < sent && strstr(toasts[index], text));
  pthread_mutex_unlock(&capture_mutex);
}

static int
log_contains(const char *text) {
  for(unsigned i = 0; i < logged; i++)
    if(strstr(logs[i], text)) return 1;
  return 0;
}

static void
write_text(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  assert(f && fputs(text, f) >= 0 && !fclose(f));
}

static void
create_sparse_file(const char *path, off_t bytes) {
  int fd = open(path, O_CREAT | O_WRONLY, 0600);
  assert(fd >= 0 && !ftruncate(fd, bytes) && !close(fd));
}

static void
expect_file(const char *path, const char *text) {
  char contents[64];
  FILE *f = fopen(path, "r");
  assert(f && fgets(contents, sizeof contents, f) && !fclose(f));
  assert(!strcmp(contents, text));
}

static void
cycle_with_config(const char *config) {
  write_text(CONF_PATH, config);
  assert(run_cycle(0) == 0);
}

static void
test_legacy(void) {
  write_text(SOURCE, "original");
  write_text(ACTIVITY_PATH, "PPSA12345\n");
  cycle_with_config("  interval = 120 \r\n  max_game_mb = 64 \n"
                    "dailybackups=0\nps4saves=false\nps5saves=false\n"
                    "backup_notifications_in_game=true\n" SOURCE "\n");
  assert(g_interval == 120 && g_max_game == 64 * BYTES_PER_MB);
  assert(toast_count(NULL) == 1);
  expect_toast(0, "Backup complete");
  expect_file(BACKUP_ROOT SOURCE, "original");
  assert(run_cycle(0) == 0);
  assert(toast_count(NULL) == 1); /* No-change passes stay silent. */
}

static void
test_settings(void) {
  cycle_with_config("backup_notifications=invalid\n"
                    "backup_notifications_in_game=invalid\n"
                    "backup_progress_interval_seconds=retired-value\n");
  assert(log_contains("Invalid backup_notifications") && log_contains("Invalid boolean"));
  assert(!log_contains("retired-value"));
  assert(notification_settings.mode == NOTICES_ALL && !notification_settings.in_game);
  cycle_with_config(" backup_notifications = errors \t\r\n"
                    " backup_notifications_in_game = false \n"
                    "backup_notifications_in_game=invalid\n");
  assert(notification_settings.mode == NOTICES_ERRORS && !notification_settings.in_game);
  assert(!notification_allowed(NOTICE_INFO) && notification_allowed(NOTICE_WARNING));
  cycle_with_config("backup_notifications=off\nbackup_notifications=INVALID\n");
  unsigned before = toast_count(NULL);
  notify_info("Must stay muted");
  assert(toast_count(NULL) == before);
  cycle_with_config("");
  assert(notification_settings.mode == NOTICES_ALL && !notification_settings.in_game);
  assert(notification_allowed(NOTICE_INFO));
}

static void
test_errors(void) {
  write_text(SOURCE, "first version");
  const char *config = SOURCE "\n" OVERSIZED "\nbackup_notifications=errors\n";
  create_sparse_file(OVERSIZED, MAX_FILE_SZ + 1);
  cycle_with_config(config);
  expect_file(BACKUP_ROOT SOURCE, "first version");
  assert(toast_count("oversized.dat") == 1);
  assert(log_contains("WARNING: File too large"));
  assert(!toast_count(CHECKING) && !toast_count(COPYING) && !toast_count("Backup complete"));

  write_text(SOURCE, "second, longer version");
  assert(!setenv("TEST_USB_FREE", "0", 1));
  assert(run_cycle(0) == 0);
  expect_file(BACKUP_ROOT SOURCE, "first version");
  assert(toast_count("USB space") == 1 && log_contains("ERROR: Not enough USB space"));
  unsigned before = toast_count(NULL);
  cycle_with_config(SOURCE "\n" OVERSIZED "\n" MISSING "\nbackup_notifications=errors\n");
  before++;
  assert(toast_count(NULL) == before && toast_count("missing.dat") == 1);
  assert(run_cycle(0) == 0);
  assert(toast_count(NULL) == before);

  assert(!setenv("TEST_USB_FREE", "1073741824", 1));
  assert(run_cycle(0) == 0);
  expect_file(BACKUP_ROOT SOURCE, "second, longer version");
  assert(toast_count(NULL) == before);
  assert(!setenv("TEST_USB_FREE", "0", 1));
  write_text(SOURCE, "third version after recovery");
  assert(run_cycle(0) == 0);
  assert(toast_count("USB space") == 2);

  cycle_with_config(SOURCE "\nbackup_notifications=all\n");
  cycle_with_config(OVERSIZED "\nbackup_notifications=all\n");
  assert(!toast_count(CHECKING) && !toast_count(NO_CHANGES));
}

static void
test_off(void) {
  write_text(SOURCE, "first version");
  write_text(CONF_PATH, SOURCE "\nbackup_notifications=off\n");
  assert(!setenv("TEST_CYCLES", "1", 1));
  fail_copy_buffer = 1;
  assert(daemon_main() == EXIT_FAILURE);
  assert(!toast_count(NULL) && log_contains("not enough memory"));
  assert(access(NOTIFICATION_ICON_PATH, F_OK) != 0);
  fail_copy_buffer = 0;
  assert(daemon_main() == EXIT_SUCCESS);
  expect_file(BACKUP_ROOT SOURCE, "first version");
  assert(!toast_count(NULL) && access(NOTIFICATION_ICON_PATH, F_OK) != 0);
  notify_problem(NOTICE_ERROR, "test-off", NULL, "Muted test error");
  assert(!toast_count(NULL) && log_contains("Muted test error"));
  assert(!unlink(CONF_PATH));
  int result = run_cycle(0);
  assert(result == -1);
  report_config_error(result);
  assert(!toast_count(NULL) && log_contains("Could not open backup.conf"));
}

static void
test_gameplay(void) {
  write_text(SOURCE, "backed up during activity");
  write_text(ACTIVITY_PATH, "PPSA12345\n");
  cycle_with_config(SOURCE "\nbackup_notifications=all\nbackup_notifications_in_game=false\n");
  expect_file(BACKUP_ROOT SOURCE, "backed up during activity");
  assert(!toast_count(NULL));
  notify_info("Muted game info");
  notify_problem(NOTICE_ERROR, "game-error", NULL, "Muted game error");
  assert(!toast_count(NULL) && log_contains("Muted game error"));

  write_text(ACTIVITY_PATH, "/mnt/sandbox/PPSA12345_000\n");
  notify_info("Muted mounted app info");
  write_text(ACTIVITY_PATH, "UNKNOWN\n");
  notify_problem(NOTICE_WARNING, "unknown-warning", NULL, "Muted unknown warning");
  assert(!toast_count(NULL) && log_contains("Muted unknown warning"));

  write_text(ACTIVITY_PATH, "NPXS12345\n");
  notify_info("Fresh shell info");
  assert(toast_count(NULL) == 1 && toast_count("Fresh shell info") == 1);
  cycle_with_config("backup_notifications=errors\nbackup_notifications_in_game=true\n");
  write_text(ACTIVITY_PATH, "PPSA12345\n");
  notify_info("Muted informational event");
  notify_problem(NOTICE_WARNING, "new-warning", NULL, "Visible game warning");
  assert(toast_count(NULL) == 2 && toast_count("Visible game warning") == 1);
}

static void
test_protection(void) {
  const char *title = "PPSA12345";
  assert(!mkdir_p(HOME_ROOT "/1/savedata_prospero/PPSA12345"));
  create_sparse_file(HOME_ROOT "/1/savedata_prospero/PPSA12345/save", 4096);
  write_text(ACTIVITY_PATH, "PPSA12345\n");
  cycle_with_config("backup_ps5_saves=true\nbackup_notifications_in_game=false\n");
  assert(pending_find("1", title, 1));
  assert(!toast_count(NULL) && activity.count == 1);
  cycle_with_config("backup_ps5_saves=true\nbackup_notifications_in_game=true\n");
  assert(!toast_count(CHECKING) && !toast_count(NO_CHANGES));
  double checked = activity.checked;

  write_text(ACTIVITY_PATH, "");
  notify_info("Shell activity after game exit");
  assert(toast_count("Shell activity after game exit") == 1);
  assert(activity.count == 1 && activity.checked == checked);
}

static void
test_empty(void) {
  assert(!mkdir_p(EMPTY) && !mkdir_p(NESTED "/subdir"));
  cycle_with_config(EMPTY "\n" NESTED "/\nbackup_notifications=all\n");
  assert(!toast_count("No files to back up in") && log_contains("No files to back up"));
  assert(!toast_count(NO_CHANGES) && !toast_count(COPYING));

  assert(run_cycle(0) == 0);
  assert(!toast_count("No files to back up in") && !toast_count(NO_CHANGES));

  write_text(EMPTY "/save", "save in previously empty folder");
  write_text(NESTED "/subdir/save", "save in nested folder");
  assert(run_cycle(0) == 0);
  expect_file(BACKUP_ROOT EMPTY "/save", "save in previously empty folder");
  expect_file(BACKUP_ROOT NESTED "/subdir/save", "save in nested folder");

  assert(run_cycle(0) == 0);
  assert(!toast_count(NO_CHANGES) && toast_count("Backup complete") == 1);

  assert(!unlink(EMPTY "/save"));
  assert(run_cycle(0) == 0);
  assert(!toast_count("No files to back up in") && !toast_count(NO_CHANGES));
  expect_file(BACKUP_ROOT EMPTY "/save", "save in previously empty folder");
}

static void
test_read_error(void) {
  const char *directory = TEST_ROOT "/unreadable";
  assert(!mkdir_p(directory));
  write_text(TEST_ROOT "/unreadable/save", "directory save");
  fail_readdir_path = directory;
  cycle_with_config(TEST_ROOT "/unreadable\nbackup_notifications=all\n");
  assert(toast_count("Backup cannot read") == 1 && log_contains("Backup incomplete"));
  assert(!toast_count("No files to back up in") && !toast_count(NO_CHANGES));
  assert(access(BACKUP_ROOT TEST_ROOT "/unreadable/save", F_OK) != 0);
  fail_readdir_path = NULL;
  assert(run_cycle(0) == 0);
  expect_file(BACKUP_ROOT TEST_ROOT "/unreadable/save", "directory save");
}

static void
test_snapshot(void) {
  const char *config = SOURCE "\ndaily_snapshots_to_keep=2\nbackup_notifications=errors\n";
  write_text(TEST_ROOT "/now", "1784563200\n");
  assert(!setenv("TEST_NOW_FILE", TEST_ROOT "/now", 1));
  write_text(SOURCE, "snapshot version");
  fail_readdir_path = BACKUP_ROOT;
  cycle_with_config(config);
  clock_info_t clock;
  local_clock(wall_clock(), &clock);
  char snapshot[PATH_MAX], saved_file[PATH_MAX];
  snprintf(snapshot, sizeof snapshot, "%s/%s", DAILY_ROOT, clock.date);
  snprintf(saved_file, sizeof saved_file, "%s%s", snapshot, SOURCE);
  assert(access(snapshot, F_OK) != 0 && toast_count("snapshot could not be saved") == 1);
  expect_file(BACKUP_ROOT SOURCE, "snapshot version");
  fail_readdir_path = NULL;
  cycle_with_config(SOURCE "\ndaily_snapshots_to_keep=2\nbackup_notifications=all\n");
  assert(toast_count(NULL) == 2);
  expect_toast(1, "Daily snapshot complete.");
  assert(!toast_count(NO_CHANGES) && !toast_count(COPYING));
  expect_file(saved_file, "snapshot version");

  write_text(SOURCE, "new live version");
  cycle_with_config(config);
  expect_file(BACKUP_ROOT SOURCE, "new live version");
  expect_file(saved_file, "snapshot version");
  assert(toast_count(NULL) == 2 && log_contains("created"));
}

static void
record_progress(unsigned percent) {
  notify_info("Test progress (%u%%)", percent);
  atomic_fetch_add(&progress_attempts, 1);
}

static void
wait_for_progress(unsigned count) {
  double deadline = now_seconds() + 5;
  struct timespec pause = {0, 10000000};
  while(atomic_load(&progress_attempts) < count && now_seconds() < deadline)
    nanosleep(&pause, NULL);
  assert(atomic_load(&progress_attempts) == count);
}

static void
test_recovery(void) {
  write_text(SOURCE, "needs backup");
  const char *config = SOURCE "\nbackup_notifications=errors\nbackup_notifications_in_game=true\n";
  assert(!setenv("TEST_USB_FREE", "0", 1));
  cycle_with_config(config);
  assert(toast_count("Not enough USB space for backups.") == 1);
  assert(!toast_count("Backup protection restored."));
  assert(!setenv("TEST_USB_FREE", "1073741824", 1));
  assert(run_cycle(0) == 0);
  expect_file(BACKUP_ROOT SOURCE, "needs backup");
  assert(toast_count("Backup protection restored.") == 1);
  assert(run_cycle(0) == 0);
  assert(toast_count("Backup protection restored.") == 1);

  /* A repeated problem may escalate from a warning to an error. */
  notification_issues_begin(0);
  notify_problem(NOTICE_WARNING, "escalation", "disk", "First warning");
  notify_problem(NOTICE_ERROR, "escalation", "disk", "Escalated error");
  assert(toast_count("First warning") == 1);
  assert(toast_count("Escalated error") == 1);
}

static void
test_languages(void) {
  char buffer[2048];
  const char *original = "Backup complete (12MB).";
  test_console_language = 7;
  assert(!strcmp(ps5_localize_notification(original, buffer, sizeof buffer),
                 "Cópia de segurança concluída (12MB)."));
  test_console_language = 17;
  assert(!strcmp(ps5_localize_notification("Backup protection active.", buffer, sizeof buffer),
                 "Backup ativo."));
  test_console_language = 3;
  assert(!strcmp(ps5_localize_notification(original, buffer, sizeof buffer),
                 "Copia de seguridad completada (12MB)."));
  test_console_language = 2;
  assert(!strcmp(ps5_localize_notification(original, buffer, sizeof buffer),
                 "Sauvegarde terminée (12MB)."));
  test_console_language = 0; /* Japanese currently falls back to English. */
  assert(!strcmp(ps5_localize_notification(original, buffer, sizeof buffer), original));
}

static void
test_progress(void) {
  /* Change cadence with estimated remaining work, not fixed 25% steps. */
  assert(progress_interval_for(0, 10) == PROGRESS_INTERVAL_SECONDS);
  assert(progress_interval_for(90, 20) == PROGRESS_INTERVAL_SECONDS);
  assert(progress_interval_for(10, 1.5) == 2 * PROGRESS_INTERVAL_SECONDS);
  assert(progress_interval_for(5, 30) == 3 * PROGRESS_INTERVAL_SECONDS);

  write_text(ACTIVITY_PATH, "PPSA12345\n");
  cycle_with_config("backup_notifications_in_game=false\n");
  progress_reset();
  progress_plan(1);
  progress_begin(100);
  progress_advance(35);
  double started = now_seconds();
  progress_start(record_progress);
  wait_for_progress(1);
  assert(now_seconds() - started >= 0.95 && !toast_count(NULL) && log_contains("35%"));
  write_text(ACTIVITY_PATH, "");
  progress_advance(30);
  wait_for_progress(2);
  assert(toast_count(NULL) == 1 && toast_count("65%") == 1);
  assert(toast_times[0] - started >= 1.95);
  progress_advance(20);
  wait_for_progress(3);
  assert(toast_count(NULL) == 2 && toast_count("85%") == 1);
  assert(toast_times[1] - toast_times[0] >= 0.95);
  /* Unchanged percent must not produce another toast. */
  struct timespec wait = {.tv_sec=1, .tv_nsec=200000000};
  nanosleep(&wait, NULL);
  assert(atomic_load(&progress_attempts) == 3);
  progress_finish();
  progress_stop();
  assert(!progress_live && !toast_count("100%"));
}

static void *
send_concurrently(void *arg) {
  const unsigned *worker = arg;
  for(unsigned i = 0; i < 4; i++) {
    notify_problem(NOTICE_ERROR, "concurrent-error", "disk", "Repeated test error");
    notify_info("Worker %u update %u", *worker, i);
  }
  return NULL;
}

static void
test_concurrent(void) {
  pthread_t workers[4];
  unsigned ids[4];
  for(unsigned i = 0; i < 4; i++) {
    ids[i] = i;
    assert(!pthread_create(&workers[i], NULL, send_concurrently, &ids[i]));
  }
  for(unsigned i = 0; i < 4; i++) assert(!pthread_join(workers[i], NULL));
  assert(toast_count("Repeated test error") == 1);
  assert(toast_count("Worker") == 16);
  for(unsigned i = 0; i < sent; i++)
    for(unsigned j = 0; j < i; j++)
      assert(strcmp(strstr(toasts[i], "\"localNotificationId\""),
                    strstr(toasts[j], "\"localNotificationId\"")));

  fail_notification_send = 1;
  notify_problem(NOTICE_ERROR, "transport-error", NULL, "Transport delivery test");
  assert(!toast_count("Transport delivery test") && log_contains("sceNotificationSend failed"));
  fail_notification_send = 0;
  notify_problem(NOTICE_ERROR, "transport-error", NULL, "Transport delivery test");
  assert(toast_count("Transport delivery test") == 1);
}

int
main(int argc, char **argv) {
  assert(argc == 2);
  assert(!mkdir_p(USB_ROOT) && !mkdir_p(HOME_ROOT));
  assert(!setenv("TEST_USB_FREE", "1073741824", 1));
  assert(!setenv("TEST_DATA_FREE", "1073741824", 1));
  assert(!setenv("TEST_ACTIVE_TITLES", ACTIVITY_PATH, 1));
  write_text(ACTIVITY_PATH, "");
  if(strcmp(argv[1], "off")) assert((g_buf = malloc(COPY_BUF_SZ)) != NULL);

  if(!strcmp(argv[1], "legacy")) test_legacy();
  else if(!strcmp(argv[1], "settings")) test_settings();
  else if(!strcmp(argv[1], "errors")) test_errors();
  else if(!strcmp(argv[1], "off")) test_off();
  else if(!strcmp(argv[1], "gameplay")) test_gameplay();
  else if(!strcmp(argv[1], "protection")) test_protection();
  else if(!strcmp(argv[1], "empty")) test_empty();
  else if(!strcmp(argv[1], "read-error")) test_read_error();
  else if(!strcmp(argv[1], "snapshot")) test_snapshot();
  else if(!strcmp(argv[1], "progress")) test_progress();
  else if(!strcmp(argv[1], "language")) test_languages();
  else if(!strcmp(argv[1], "recovery")) test_recovery();
  else if(!strcmp(argv[1], "concurrent")) test_concurrent();
  else abort();

  index_clear();
  free(g_buf);
  return 0;
}

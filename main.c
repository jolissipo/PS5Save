/* Copyright (C) 2026

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

/**
 * Save backup daemon for PS5.
 *
 * Reads /mnt/usb0/backup.conf and copies the listed files to
 * /mnt/usb0/backup/<original path>:
 *
 *   /data/PCSX2/Mcd001.ps2        one file
 *   /data/porpoise/saves/<star>   every file and folder below, recursively
 *   backup_interval_seconds=60      seconds between backups (default 60)
 *   backup_notifications=all       all, errors or off (default all)
 *   backup_notifications_in_game=true  allow toasts during possible game activity
 *   backup_ps4_saves=true           enable decrypted PS4 saves (default false)
 *   backup_ps5_saves=true           enable decrypted PS5 saves (default false)
 *   backup_encrypted_saves=true     copy original PS4 and PS5 save files
 *   backup_ps4_trophies=true        copy PS4 trophy data and catalogs
 *   backup_ps5_trophies=true        copy PS5 trophy data and catalogs
 *   max_title_size_mb=4096          size limit for all save images of one title
 *                                  in decimal MB; default 4096 = 4.096GB
 *   daily_snapshots_to_keep=3       keep a dated snapshot (a copy of the live
 *                                 backup) of the newest N days in
 *                                 /mnt/usb0/backup_daily/YYYY-MM-DD, using the
 *                                 console's local date. 0..10, default 0 = off.
 *                                 The first run on a date creates its snapshot
 *                                 and it is never overwritten that day; it is
 *                                 checked at every payload start, wake-up and
 *                                 at local midnight.
 *
 * Plain files larger than 100MB are skipped. A game's saves are backed up
 * completely or not at all: the size of the whole title id folder is checked
 * against max_title_size_mb, and the USB drive and internal storage are checked for
 * free space, before anything is mounted. The payload keeps running in the
 * background and only copies what changed. Starting it again replaces the
 * instance that is already running.
 *
 * PS4/PS5 saves are encrypted images. For each image that changed, a COPY of
 * the image is mounted (the original is only ever read), the decrypted files
 * are copied to /mnt/usb0/backup/saves/..., and the image is unmounted again.
 * A title is deferred while that same title is running, suspended or still
 * sandbox-mounted. Other titles may be backed up during gameplay. Activity is
 * checked before mounting and during copying; unknown activity fails closed.
 * Deferred titles are retried after closure, independently of the interval.
 *
 * Save mounting is based on the Playstation-5-Save-Mounter payload by n0llptr
 * (GPLv3), which in turn builds on research by cow and the garlic-savemgr
 * project by earthonion.
 **/

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "notification_icon.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef BACKUP_HOST_TEST
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/user.h>
#include <sys/sysctl.h>
#else
#include <sys/statfs.h>
#endif

#include <ps5/kernel.h>
#include <ps5/klog.h>


#ifndef USB_ROOT
#define USB_ROOT      "/mnt/usb0"
#endif
#ifndef HOME_ROOT
#define HOME_ROOT     "/user/home"
#endif
#ifndef TMP_DIR
#define TMP_DIR       "/data/save_files"
#endif
#ifndef PFS_ROOT
#define PFS_ROOT      "/mnt/pfs"
#endif
#ifndef PFSMGR_DEV
#define PFSMGR_DEV    "/dev/pfsmgr"
#endif

#define CONF_PATH     USB_ROOT "/backup.conf"
#define BACKUP_ROOT   USB_ROOT "/backup"
#define DAILY_ROOT    USB_ROOT "/backup_daily"
#define MAX_DAILY     10      /* highest value for daily_snapshots_to_keep= */
#define MAX_DAILY_DIRS 1024   /* date folders looked at when pruning */
#define TMP_IMG       TMP_DIR "/_backup_tmp_image"
#define MOUNT_POINT   PFS_ROOT "/savebackup_tmp"
#define BYTES_PER_MB  1000000ULL /* decimal megabytes */
#define MAX_FILE_SZ   (100ULL * BYTES_PER_MB)
#define DEF_GAME_MB   4096    /* default limit: all saves of one title id, in MB */
#ifndef MIN_GAME_MB
#define MIN_GAME_MB   64
#endif
#define MAX_GAME_MB   65536
#define MAX_IMAGES    512     /* save images per title id */
#define SPACE_MARGIN  (64ULL * 1024ULL * 1024ULL)
#define COPY_BUF_SZ   (1024 * 1024)

#define DEF_INTERVAL  60      /* seconds between backups */
#ifndef MIN_INTERVAL
#define MIN_INTERVAL  30
#endif
#define MAX_INTERVAL  3600
#ifndef SLICE_SEC
#define SLICE_SEC     10      /* wake-up granularity while waiting */
#endif
#ifndef LOCK_PORT
#define LOCK_PORT     9055    /* loopback port used to detect/replace a running instance */
#endif
#define MISSING_TRIES 6       /* conf missing this many slices in a row -> notify */

#ifndef GAME_CHECK_SEC
#define GAME_CHECK_SEC 1.0    /* how often the game check is repeated while mounted */
#endif
#define SAVE_AUTHID   0x4800000000000010ULL


/* Rich notification transport follows ShadowMountPlus src/sm_log.c (GPLv3):
 * https://github.com/drakmor/ShadowMountPlus/blob/
 * 98c524890176c7b474fcb03387bec88c4a559861/src/sm_log.c
 * sceKernelSendNotificationRequest is the legacy debug-popup mechanism;
 * sceNotificationSend accepts the shell's rich JSON notification model. */
int sceNotificationSend(int user_id, bool is_logged, const char *payload);
#define NOTIFICATION_USER_ID 0xfe /* same local/broadcast user as SMP */
#define NOTIFICATION_JSON_SIZE 16384

#ifndef NOTIFICATION_ICON_DIR
#define NOTIFICATION_ICON_DIR "/user/data/ps5save"
#endif
#define PS5SAVE_RELEASE "dev"
#define NOTIFICATION_ICON_PATH NOTIFICATION_ICON_DIR "/notification-v3.png"

static const char PS5SAVE_VERSION[] = PS5SAVE_RELEASE;

static int mkdir_p(const char *path);

/* Install the embedded, unmodified user artwork where ShellUI can read it.
 * No USB icon file or network service is required. Try again on a later
 * notification if installation fails. Never fall back to legacy debug popups. */
static int
prepare_notification_icon(void) {
  static int ready;
  const char *tmp = NOTIFICATION_ICON_PATH ".part";
  int fd;
  size_t pos = 0;

  if(ready && !access(NOTIFICATION_ICON_PATH, R_OK)) {
    return 1;
  }
  ready = 0;
  if(mkdir_p(NOTIFICATION_ICON_DIR)) {
    return 0;
  }
  if((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
    return 0;
  }
  while(pos < sizeof notification_icon_png) {
    ssize_t n = write(fd, notification_icon_png + pos,
                      sizeof notification_icon_png - pos);
    if(n < 0 && errno == EINTR) {
      continue;
    }
    if(n <= 0) {
      close(fd);
      unlink(tmp);
      return 0;
    }
    pos += (size_t)n;
  }
  int ok = fchmod(fd, 0644) == 0 && fsync(fd) == 0;
  if(close(fd)) {
    ok = 0;
  }
  if(!ok || rename(tmp, NOTIFICATION_ICON_PATH)) {
    unlink(tmp);
    return 0;
  }
  ready = 1;
  return 1;
}



/* App-info ABI used by kstuff-lite and ShadowMountPlus. The title starts at
 * 0x10: inserting an app_type field here shifts every title by four bytes.
 * https://github.com/EchoStretch/kstuff-lite/blob/main/ps5-kstuff-ldr/src/shellui_patch.c */
typedef struct app_info {
  uint32_t app_id;
  uint64_t unknown1;
  char     title_id[14];
  char     unknown2[0x3c];
} app_info_t;
_Static_assert(offsetof(app_info_t, title_id) == 0x10, "Incorrect app-info title offset");

int sceKernelGetAppInfo(pid_t pid, app_info_t *info);


/* save data mounting, from libSceFsInternalForVsh (see Save Mounter) */
typedef struct { uint8_t reserved; char *budgetid; } MountOpt;
typedef struct { uint8_t dummy; } UmountOpt;

int sceFsInitMountSaveDataOpt(MountOpt *opt);
int sceFsMountSaveData(MountOpt *opt, const char *path, const char *mount,
                       uint8_t *key);
int sceFsInitUmountSaveDataOpt(UmountOpt *opt);
int sceFsUmountSaveData(UmountOpt *opt, const char *mount, int handle,
                        int ignore);


typedef struct backup_stats {
  uint64_t bytes;
  unsigned copied;
  unsigned unchanged;
  unsigned skipped;
  unsigned empty_sources;
  unsigned failed;
} backup_stats_t;

typedef struct priv_state {
  uint64_t authid;
  uint8_t  caps[16];
  intptr_t jaildir;
} priv_state_t;

typedef enum { SV_OK, SV_SKIP, SV_FAILED, SV_GAME } sv_result_t;


static char *g_buf;
static long g_interval = DEF_INTERVAL;

static uint64_t g_max_game = DEF_GAME_MB * BYTES_PER_MB;
static uint64_t g_file_cap = MAX_FILE_SZ;   /* per-file limit, 0 = none */
static int g_daily = 0;                     /* daily_snapshots_to_keep=N, 0 = off */
static int g_ps4saves = 0;
static int g_ps5saves = 0;
static int g_encrypted_saves = 0;
static int g_ps4trophies = 0;
static int g_ps5trophies = 0;
static int g_need_cleanup = 1;
static int g_mount_stuck = 0;
static int g_watch_game = 0;      /* protect the currently mounted title */
static int g_game_abort = 0;      /* abort this save image only */
static int g_backup_busy = 0;     /* one activity warning per copying pass */
static double g_last_watch = 0;
static char g_abort_why[192];


static void logq(const char *fmt, ...);
static int notification_game_muted(void);

typedef enum { NOTICES_ALL, NOTICES_ERRORS, NOTICES_OFF } notice_mode_t;
typedef enum { NOTICE_INFO, NOTICE_WARNING, NOTICE_ERROR } notice_level_t;
typedef struct {
  notice_mode_t mode;
  int in_game;
} notification_settings_t;

#define DEFAULT_NOTIFICATION_SETTINGS {NOTICES_ALL, 0}
static const notification_settings_t notification_defaults = DEFAULT_NOTIFICATION_SETTINGS;
static notification_settings_t notification_settings = DEFAULT_NOTIFICATION_SETTINGS;

/* Settings are published between passes with the worker joined. The sender
 * mutex serializes rich toasts, icon installation and issue history updates. */
static pthread_mutex_t notification_mutex = PTHREAD_MUTEX_INITIALIZER;
#define MAX_NOTIFICATION_ISSUES 256
static struct {
  const char *problem;
  char *subject;
  int observed;
  notice_level_t level;
} notification_issues[MAX_NOTIFICATION_ISSUES];
static size_t next_notification_issue;
static int notification_cycle_failed;
static int notification_recovered;

/* Escape every JSON control character and reject overflow rather than emit
 * truncated or malformed JSON. UTF-8 bytes are preserved. */
static int
notification_json_escape(char *dst, size_t size, const char *src) {
  static const char hex[] = "0123456789abcdef";
  size_t pos = 0;

  if(!size) {
    return 0;
  }
  for(const unsigned char *p = (const unsigned char *)src; *p; p++) {
    size_t need = *p < 0x20 ? 6 : (*p == '"' || *p == '\\' ? 2 : 1);
    if(need >= size - pos) {
      dst[0] = 0;
      return 0;
    }
    if(*p < 0x20) {
      dst[pos++] = '\\';
      dst[pos++] = 'u';
      dst[pos++] = '0';
      dst[pos++] = '0';
      dst[pos++] = hex[*p >> 4];
      dst[pos++] = hex[*p & 15];
    } else {
      if(*p == '"' || *p == '\\') {
        dst[pos++] = '\\';
      }
      dst[pos++] = (char)*p;
    }
  }
  dst[pos] = 0;
  return 1;
}

/* Standard informative notification: status message, PS5Save label and image.
 * Use a plain local image path, as in the SMP rich notification transport.
 * No interactive actions, alternate views or legacy debug popups. */
#include "ps5save_i18n.h"

static int
build_notification_payload(const char *message, const char *created_at,
                           const char *id, char *payload, size_t size) {
  size_t capacity = strlen(message) * 6 + 1;
  if(capacity > 2048 * 6 + 1) return 0;
  char *escaped = malloc(capacity);
  if(!escaped) return 0;
  if(!notification_json_escape(escaped, capacity, message)) {
    free(escaped);
    return 0;
  }
  int n = snprintf(payload, size,
    "{\"rawData\":{"
      "\"viewTemplateType\":\"ToastTemplateB\","
      "\"channelType\":\"SystemFeedback\","
      "\"bundleName\":\"PS5Save+\",\"useCaseId\":\"IDC\","
      "\"soundEffect\":\"none\",\"toastOverwriteType\":\"No\","
      "\"isImmediate\":true,\"priority\":100,"
      "\"viewData\":{"
        "\"icon\":{\"type\":\"Url\",\"parameters\":{"
          "\"url\":\"" NOTIFICATION_ICON_PATH "\"}},"
        "\"message\":{\"body\":\"%s\"},"
        "\"subMessage\":{\"body\":\"PS5Save+ | %s\"}}},"
      "\"createdDateTime\":\"%s\",\"localNotificationId\":\"%s\"}",
    escaped, PS5SAVE_VERSION, created_at, id);
  free(escaped);
  return n >= 0 && (size_t)n < size;
}

/** Send an eligible message through libSceNotification, with our icon. */
static int
send_notification(const char *message) {
  static unsigned serial;
  char *payload;
  char created_at[32];
  char id[96];
  struct timespec ts;
  struct tm utc;

  if(!prepare_notification_icon()) {
    logq("Rich notification not sent: icon installation failed (%s)",
         strerror(errno));
    return 0;
  }
  if(clock_gettime(CLOCK_REALTIME, &ts) ||
     !gmtime_r(&ts.tv_sec, &utc) ||
     !strftime(created_at, sizeof created_at, "%Y-%m-%dT%H:%M:%S.000Z", &utc)) {
    logq("Rich notification not sent: cannot obtain timestamp");
    return 0;
  }
  snprintf(id, sizeof id, "ps5save-%lld-%ld-%d-%u",
           (long long)ts.tv_sec, ts.tv_nsec, (int)getpid(), ++serial);
  payload = malloc(NOTIFICATION_JSON_SIZE);
  if(!payload) {
    logq("Rich notification not sent: not enough memory");
    return 0;
  }
  char localized[2048];
  const char *display_text = ps5_localize_notification(message, localized, sizeof localized);
  if(!build_notification_payload(display_text, created_at, id,
                                 payload, NOTIFICATION_JSON_SIZE)) {
    free(payload);
    logq("Rich notification not sent: payload too large");
    return 0;
  }
  int rc = sceNotificationSend(NOTIFICATION_USER_ID, true, payload);
  free(payload);
  if(rc != 0) {
    logq("sceNotificationSend failed (0x%08x)", (unsigned)rc);
  }
  return rc == 0;
}


static int
notification_allowed(notice_level_t level) {
  return notification_settings.mode == NOTICES_ALL ||
         (notification_settings.mode == NOTICES_ERRORS && level != NOTICE_INFO);
}

static int
notification_emit_locked(notice_level_t level, const char *message) {
  if(!notification_allowed(level)) return 0;
  return (notification_settings.in_game || !notification_game_muted()) &&
         send_notification(message);
}

static void
notify_info(const char *fmt, ...) {
  char message[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(message, sizeof message, fmt, ap);
  va_end(ap);
  logq("%s", message);
  if(!notification_allowed(NOTICE_INFO)) return;
  pthread_mutex_lock(&notification_mutex);
  notification_emit_locked(NOTICE_INFO, message);
  pthread_mutex_unlock(&notification_mutex);
}

/* The problem identifier is a string literal. The subject is copied only
 * after a successful send; muted or failed sends never become pending toasts. */
static void
notify_problem(notice_level_t level, const char *problem, const char *subject,
               const char *fmt, ...) {
  char message[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(message, sizeof message, fmt, ap);
  va_end(ap);
  logq("%s: %s%s%s", level == NOTICE_ERROR ? "ERROR" : "WARNING",
       message, subject && subject[0] ? " " : "", subject ? subject : "");

  if(!subject) subject = "";
  pthread_mutex_lock(&notification_mutex);
  if(level == NOTICE_ERROR) notification_cycle_failed = 1;
  size_t free_slot = MAX_NOTIFICATION_ISSUES;
  for(size_t i = 0; i < MAX_NOTIFICATION_ISSUES; i++) {
    if(!notification_issues[i].problem) {
      if(free_slot == MAX_NOTIFICATION_ISSUES) free_slot = i;
    } else if(!strcmp(notification_issues[i].problem, problem) &&
              !strcmp(notification_issues[i].subject, subject)) {
      notification_issues[i].observed = 1;
      /* An existing warning must not conceal a later escalation to error. */
      if(level > notification_issues[i].level &&
         notification_emit_locked(level, message))
        notification_issues[i].level = level;
      goto done;
    }
  }

  if(!notification_emit_locked(level, message)) goto done;
  char *saved_subject = strdup(subject);
  if(!saved_subject) goto done;
  if(free_slot == MAX_NOTIFICATION_ISSUES) {
    free_slot = next_notification_issue;
    next_notification_issue = (next_notification_issue + 1) % MAX_NOTIFICATION_ISSUES;
    free(notification_issues[free_slot].subject);
  }
  notification_issues[free_slot].problem = problem;
  notification_issues[free_slot].subject = saved_subject;
  notification_issues[free_slot].observed = 1;
  notification_issues[free_slot].level = level;
done:
  pthread_mutex_unlock(&notification_mutex);
}

static void
notification_issues_begin(int pending_only) {
  pthread_mutex_lock(&notification_mutex);
  notification_cycle_failed = 0;
  notification_recovered = 0;
  if(!pending_only) {
    for(size_t i = 0; i < MAX_NOTIFICATION_ISSUES; i++)
      notification_issues[i].observed = 0;
  }
  pthread_mutex_unlock(&notification_mutex);
}

static void
notification_issues_end(void) {
  pthread_mutex_lock(&notification_mutex);
  int resolved_error = 0;
  int still_has_issues = 0;
  for(size_t i = 0; i < MAX_NOTIFICATION_ISSUES; i++) {
    if(!notification_issues[i].problem) continue;
    if(!notification_issues[i].observed) {
      if(notification_issues[i].level == NOTICE_ERROR)
        resolved_error = 1;
      free(notification_issues[i].subject);
      notification_issues[i].subject = NULL;
      notification_issues[i].problem = NULL;
    } else {
      still_has_issues = 1;
    }
  }
  /* Only claim full recovery when all previously reported problems cleared. */
  notification_recovered = resolved_error && !still_has_issues &&
                           !notification_cycle_failed;
  pthread_mutex_unlock(&notification_mutex);
}

static void
notify_progress(unsigned percent) {
  notify_info("Backup progress: %u%%", percent);
}

#include "backup_progress.h"


/* Announce the first copy operation, including a snapshot-only pass. */
static void
backup_activity_begin(const char *message) {
  if(!g_backup_busy) {
    g_backup_busy = 1;
    (void)message; /* Normal start is intentionally silent. */
    if(notification_allowed(NOTICE_INFO))
      progress_start(notify_progress);
  }
}


#ifndef BACKUP_HOST_TEST
/* Best-effort bounded USB log. Never report a logging error through logq. */
static pthread_mutex_t file_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static void persist_log_line(const char *message) {
  pthread_mutex_lock(&file_log_mutex);
  const char *dir = USB_ROOT "/ps5save-plus";
  const char *path = USB_ROOT "/ps5save-plus/events.log";
  const char *old = USB_ROOT "/ps5save-plus/events.log.1";
  if(!access(CONF_PATH, R_OK) && !mkdir_p(dir)) {
    struct stat st;
    if(!stat(path, &st) && st.st_size >= 256 * 1024) {
      unlink(old);
      rename(path, old);
    }
    FILE *f = fopen(path, "a");
    if(f) {
      time_t now = time(NULL);
      struct tm utc;
      char date[32] = "";
      if(gmtime_r(&now, &utc))
        strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%SZ", &utc);
      fprintf(f, "%s %s\n", date, message);
      fclose(f);
    }
  }
  pthread_mutex_unlock(&file_log_mutex);
}
#endif

/**
 * Quiet log: stdout, kernel log, and bounded persistent USB log.
 **/
static void
logq(const char *fmt, ...) {
  char msg[2048];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);

  printf("%s\n", msg);
  fflush(stdout);
  klog_printf("[backup] %s\n", msg);
#ifndef BACKUP_HOST_TEST
  persist_log_line(msg);
#endif
}


static void
format_size(uint64_t bytes, char *out, size_t len) {
  if(bytes >= 1000ULL * 1000ULL * 1000ULL) {
    snprintf(out, len, "%.1fGB", (double)bytes / (1000.0 * 1000.0 * 1000.0));
  } else if(bytes >= 10ULL * 1000ULL * 1000ULL) {
    snprintf(out, len, "%.0fMB", (double)bytes / (1000.0 * 1000.0));
  } else if(bytes >= 1000ULL * 1000ULL) {
    snprintf(out, len, "%.1fMB", (double)bytes / (1000.0 * 1000.0));
  } else if(bytes >= 1000ULL) {
    snprintf(out, len, "%.0fkB", (double)bytes / 1000.0);
  } else {
    snprintf(out, len, "%lluB", (unsigned long long)bytes);
  }
}


static double
now_seconds(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}


static time_t
wall_clock(void) {
  struct timespec ts;

#ifdef BACKUP_HOST_TEST
  /* the host test can set the clock through a file */
  const char *tf = getenv("TEST_NOW_FILE");
  if(tf) {
    FILE *fp = fopen(tf, "r");
    long long v;

    if(fp) {
      int ok = fscanf(fp, "%lld", &v) == 1;
      fclose(fp);
      if(ok) {
        return (time_t)v;
      }
    }
  }
#endif

  clock_gettime(CLOCK_REALTIME, &ts);
  return ts.tv_sec;
}


/**
 * mkdir -p
 **/
static int
mkdir_p(const char *path) {
  char tmp[PATH_MAX];
  size_t len;

  len = snprintf(tmp, sizeof tmp, "%s", path);
  if(len == 0 || len >= sizeof tmp) {
    return -1;
  }

  for(char *p = tmp + 1; *p; p++) {
    if(*p == '/') {
      *p = 0;
      if(mkdir(tmp, 0777) && errno != EEXIST) {
        return -1;
      }
      *p = '/';
    }
  }

  if(mkdir(tmp, 0777) && errno != EEXIST) {
    return -1;
  }

  return 0;
}


/**
 * Free space (bytes) on the file system holding path.
 **/
static uint64_t
fs_free(const char *path, const char *test_env) {
  struct statfs sf;

#ifdef BACKUP_HOST_TEST
  const char *o = getenv(test_env);
  if(o) {
    return strtoull(o, NULL, 10);
  }
#else
  (void)test_env;
#endif

  if(statfs(path, &sf) || (int64_t)sf.f_bavail <= 0) {
    return 0;
  }
  return (uint64_t)sf.f_bavail * (uint64_t)sf.f_bsize;
}


#define usb_free()   fs_free(USB_ROOT, "TEST_USB_FREE")
#define data_free()  fs_free(TMP_DIR, "TEST_DATA_FREE")

#include "dashboard.h"


/**
 * Total size of all regular files below path.
 **/
static void game_watch(void);

static uint64_t
dir_size(const char *path) {
  char sub[PATH_MAX];
  struct dirent *dp;
  struct stat st;
  uint64_t total = 0;
  DIR *dir;

  if(!(dir = opendir(path))) {
    return 0;
  }
  while((dp = readdir(dir))) {
    game_watch();
    if(g_watch_game && g_game_abort) break;
    if(!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")) {
      continue;
    }
    if(snprintf(sub, sizeof sub, "%s/%s", path, dp->d_name) >= (int)sizeof sub ||
       lstat(sub, &st)) {
      continue;
    }
    if(S_ISDIR(st.st_mode)) {
      total += dir_size(sub);
    } else if(S_ISREG(st.st_mode)) {
      total += st.st_size;
    }
  }
  closedir(dir);
  return total;
}


/* ------------------------------------------------------------------------
 * Game detection
 * ---------------------------------------------------------------------- */

#include "title_activity.h"


/**
 * Called regularly while an image is copied or mounted. If its title appears,
 * g_game_abort is set and save-image copy loops stop as soon as possible.
 * Ordinary configured files are independent of game detection.
 **/
static void
game_watch(void) {
  char why[160];
  double t;

  if(!g_watch_game || g_game_abort) {
    return;
  }

  t = now_seconds();
  if(t - g_last_watch < GAME_CHECK_SEC) {
    return;
  }
  g_last_watch = t;

  if(title_blocked(g_watch_title, why, sizeof why, 0)) {
    g_game_abort = 1;
    snprintf(g_abort_why, sizeof g_abort_why, "%s", why);
  }
}


#include "change_index.h"


/* ------------------------------------------------------------------------
 * Plain file backup
 * ---------------------------------------------------------------------- */

static const char *
backup_path_name(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static void
backup_scan_error(const char *problem, const char *subject, const char *src, int error) {
  if(g_progress_planning) return;
  logq("Cannot scan %s: %s", src, strerror(error));
  notify_problem(NOTICE_ERROR, problem, subject, "Backup cannot read: %s.",
                 backup_path_name(subject));
}

static struct dirent *
backup_readdir(DIR *dir, const char *subject, const char *src) {
  errno = 0;
  struct dirent *entry = readdir(dir);
  if(!entry && errno) {
    int error = errno;
    backup_scan_error("read-directory", subject, src, error);
    errno = error;
  }
  return entry;
}

/**
 * Copy a single file from src to dst. The data is written to a temporary file
 * first, so that an interrupted copy (full or removed USB drive) never
 * destroys the previous good backup.
 **/
static void
backup_file(const char *src, const char *dst, const struct stat *st,
            backup_stats_t *stats) {
  char tmp[PATH_MAX];
  char dir[PATH_MAX];
  const char *label = dst + strlen(BACKUP_ROOT);
  struct stat after;
  struct timeval tv[2];
  char *slash;
  ssize_t n;
  int in = -1;
  int out = -1;
  int ok = 0;
  int aborted = 0;
  int error;
  const char *operation = "temporary file path";
  unsigned reads = 0;

  int progress_started = 0;
  if(g_progress_planning) {
    if((!g_file_cap || (uint64_t)st->st_size <= g_file_cap) &&
       !file_current(dst, st))
      progress_plan(1);
    return;
  }

  /* per-file limit; not applied inside game saves, which are judged per game */
  if(g_file_cap && (uint64_t)st->st_size > g_file_cap) {
    logq("Abnormal size of %s; skipping backup", label);
    notify_problem(NOTICE_WARNING, "file-size", label, "File too large: %s.",
                   backup_path_name(label));
    stats->skipped++;
    return;
  }

  game_watch();
  if(g_watch_game && g_game_abort) {
    return;
  }

  tmp[0] = 0;
  if(snprintf(tmp, sizeof tmp, "%s.part", dst) >= (int)sizeof tmp) {
    tmp[0] = 0;
    errno = ENAMETOOLONG;
    goto done;
  }

  /* Exact source and destination metadata cached after a successful copy. */
  if(file_current(dst, st)) {
    stats->unchanged++;
    if(g_progress_image) progress_advance(st->st_size);
    return;
  }

  /* make sure the USB drive can hold the new file */
  if(usb_free() < (uint64_t)st->st_size + SPACE_MARGIN) {
    logq("Failed to backup %s (not enough space on USB)", label);
    notify_problem(NOTICE_ERROR, "usb-space", NULL, "Not enough USB space for backups.");
    stats->failed++;
    return;
  }

  snprintf(dir, sizeof dir, "%s", dst);
  if((slash = strrchr(dir, '/')) && slash != dir) {
    *slash = 0;
    operation = "create USB directory";
    if(mkdir_p(dir)) {
      goto done;
    }
  }

  operation = "open source file";
  if((in = open(src, O_RDONLY)) < 0) {
    goto done;
  }
  if(!g_progress_image) {
    progress_begin(st->st_size);
    progress_started = 1;
  }
  backup_activity_begin("Changes found. Backing up.");
  operation = "open USB file";
  if((out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) {
    goto done;
  }

  while((n = read(in, g_buf, COPY_BUF_SZ)) > 0) {
    char *p = g_buf;
    while(n > 0) {
      ssize_t w = write(out, p, n);
      if(w < 0) {
        if(errno == EINTR) {
          continue;
        }
        operation = "write USB file";
        goto done;
      }
      progress_advance(w);
      p += w;
      n -= w;
    }
    if(++reads) {
      game_watch();
      if(g_watch_game && g_game_abort) {
        aborted = 1;
        goto done;
      }
    }
  }
  if(n < 0) {
    operation = "read source file";
    goto done;
  }

  operation = "check source file";
  if(fstat(in, &after)) {
    goto done;
  }
  if(!source_same(st, &after)) {
    operation = "source changed during copy";
    errno = EAGAIN;
    goto done;
  }
  operation = "sync USB file";
  if(fsync(out)) {
    goto done;
  }
  operation = "close USB file";
  if(close(out)) {
    out = -1;
    goto done;
  }
  out = -1;

  /* keep the source mtime so the next cycle can detect "unchanged" */
  tv[0].tv_sec = st->st_mtime;
  tv[0].tv_usec = 0;
  tv[1] = tv[0];
  utimes(tmp, tv);

  operation = "commit USB file";
  if(rename(tmp, dst)) {
    goto done;
  }

  index_update(dst, st);
  ok = 1;

done:
  error = errno;
  if(progress_started) progress_finish();
  if(in >= 0) {
    close(in);
  }
  if(out >= 0) {
    close(out);
  }

  if(ok) {
    stats->copied++;
    stats->bytes += st->st_size;
  } else {
    if(tmp[0]) {
      unlink(tmp);
    }
    if(!aborted) {
      logq("Failed to back up %s (%s): %s", label, operation, strerror(error));
      if(error == ENOSPC) {
        notify_problem(NOTICE_ERROR, "usb-space", NULL, "Not enough USB space for backups.");
      } else if(error == EROFS) {
        notify_problem(NOTICE_ERROR, "usb-read-only", NULL, "Backup USB is read-only.");
      } else {
        notify_problem(NOTICE_ERROR, operation, label, "File backup failed: %s.",
                       backup_path_name(label));
      }
      stats->failed++;
    }
  }
}


/**
 * Non-zero if st describes the live backup destination. Descending into it
 * would copy the backup into itself, forever.
 **/
static int
is_backup_root(const struct stat *st) {
  struct stat root;

  return !stat(BACKUP_ROOT, &root) && root.st_dev == st->st_dev &&
         root.st_ino == st->st_ino;
}


/**
 * Back up everything below the directory src into dst, recursively.
 * When given the source stat, return true only for a verified empty tree.
 **/
static int
backup_tree(const char *src, const char *dst, backup_stats_t *stats,
            const struct stat *source) {
  char spath[PATH_MAX];
  char dpath[PATH_MAX];
  struct dirent *dp;
  struct stat st;
  DIR *dir;
  int empty = source != NULL;

  const char *label = dst + strlen(BACKUP_ROOT);
  if(!(dir = opendir(src))) {
    backup_scan_error("open-directory", label, src, errno);
    stats->failed++;
    return 0;
  }

  while((dp = backup_readdir(dir, label, src)) && !(g_watch_game && g_game_abort)) {
    if(!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")) {
      continue;
    }

    if(snprintf(spath, sizeof spath, "%s/%s", src, dp->d_name) >= (int)sizeof spath ||
       snprintf(dpath, sizeof dpath, "%s/%s", dst, dp->d_name) >= (int)sizeof dpath) {
      empty = 0;
      if(!g_progress_planning) {
        logq("Backup path too long: %s/%s", src, dp->d_name);
        notify_problem(NOTICE_ERROR, "backup-path-length", label,
                       "Backup path too long under %s.", backup_path_name(label));
        stats->failed++;
      }
      continue;
    }

    if(lstat(spath, &st)) {
      empty = 0;
      backup_scan_error("stat-file", dpath + strlen(BACKUP_ROOT), spath, errno);
      stats->failed++;
      continue;
    }

    if(S_ISDIR(st.st_mode)) {
      /* The destination lives inside the tree being copied whenever an entry
       * is one of its ancestors. Skip it by identity, not by name, so the
       * walk stops here instead of recursing into the growing backup. */
      if(is_backup_root(&st)) {
        empty = 0;
        logq("Skipping backup destination %s", spath);
        continue;
      }
      empty = backup_tree(spath, dpath, stats, empty ? &st : NULL);
    } else if(S_ISREG(st.st_mode)) {
      empty = 0;
      backup_file(spath, dpath, &st, stats);
    }
    /* symlinks and special files are ignored */
  }

  int read_error = !dp && errno;
  if(read_error && !g_progress_planning) stats->failed++;
  if(closedir(dir) || !empty || dp || read_error) return 0;

  /* A concurrent creation, removal or replacement can invalidate an empty scan. */
  return !stat(src, &st) && source_same(source, &st);
}


/* ------------------------------------------------------------------------
 * PS4 / PS5 save images
 * ---------------------------------------------------------------------- */

static void
priv_elevate(priv_state_t *old) {
  pid_t me = getpid();
  uint8_t caps[16];

  old->authid = kernel_get_ucred_authid(me);
  memset(old->caps, 0, sizeof old->caps);
  kernel_get_ucred_caps(me, old->caps);
  old->jaildir = kernel_get_proc_jaildir(me);

  kernel_set_ucred_authid(me, SAVE_AUTHID);
  memset(caps, 0xFF, sizeof caps);
  kernel_set_ucred_caps(me, caps);
  kernel_set_proc_rootdir(me, kernel_get_root_vnode());
  kernel_set_proc_jaildir(me, 0);
}


static void
priv_restore(const priv_state_t *old) {
  pid_t me = getpid();

  kernel_set_ucred_authid(me, old->authid);
  kernel_set_ucred_caps(me, old->caps);
  kernel_set_proc_jaildir(me, old->jaildir);
}


/* Configuration booleans accept exactly the documented lowercase tokens. */
static int
parse_bool(const char *value, int *out) {
  if(!strcmp(value, "true")) {
    *out = 1;
    return 1;
  }
  if(!strcmp(value, "false")) {
    *out = 0;
    return 1;
  }
  logq("Invalid boolean setting: %s (expected false or true)", value);
  return 0;
}

static int
is_hex_name(const char *s) {
  size_t len = strlen(s);

  if(len == 0 || len > 8) {
    return 0;
  }
  for(size_t i = 0; i < len; i++) {
    if(!isxdigit((unsigned char)s[i])) {
      return 0;
    }
  }
  return 1;
}


/**
 * Free space check for the temporary image copy in internal storage.
 **/
static int
enough_space(uint64_t need) {
  mkdir_p(TMP_DIR);
  return data_free() >= need;
}


/**
 * Where the decrypted files of one save image are backed up.
 **/
static void
save_dst_dir(char *out, size_t len, int ps5, const char *uid,
             const char *title, const char *save) {
  snprintf(out, len, "%s/saves/%s/%s/%s/%s", BACKUP_ROOT,
           ps5 ? "ps5" : "ps4", uid, title, save);
}


/**
 * Copy src to dst (plain, no temp file). Returns 0 on success, -1 on error
 * and -2 if the copy was stopped because a game appeared.
 **/
static int
copy_plain(const char *src, const char *dst) {
  unsigned reads = 0;
  int rc = -1;
  ssize_t n;
  int in;
  int out;

  if((in = open(src, O_RDONLY)) < 0) {
    return -1;
  }
  backup_activity_begin("Changes found. Backing up.");
  if((out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0777)) < 0) {
    close(in);
    return -1;
  }

  while((n = read(in, g_buf, COPY_BUF_SZ)) > 0) {
    char *p = g_buf;
    while(n > 0) {
      ssize_t w = write(out, p, n);
      if(w < 0) {
        if(errno == EINTR) {
          continue;
        }
        goto done;
      }
      progress_advance(w);
      p += w;
      n -= w;
    }
    if(++reads) {
      game_watch();
      if(g_game_abort) {
        rc = -2;
        goto done;
      }
    }
  }
  if(n == 0) {
    rc = 0;
  }

done:
  close(in);
  if(close(out)) {
    rc = rc ? rc : -1;
  }
  return rc;
}


/**
 * Decrypt the sealed save key (0x60 bytes) through the pfsmgr device.
 **/
static int
decrypt_key(const uint8_t *sealed, uint8_t key[0x20]) {
  uint8_t *data;
  int fd;
  int rc;

  if(!(data = calloc(1, 0x100))) {
    return -1;
  }
  memcpy(data, sealed, 0x60);

  if((fd = open(PFSMGR_DEV, O_RDWR)) < 0) {
    free(data);
    return -1;
  }
  rc = ioctl(fd, 0xc0845302, data);
  close(fd);

  if(rc >= 0) {
    memcpy(key, data + 0x60, 0x20);
  }
  free(data);
  return rc < 0 ? -1 : 0;
}


/**
 * Unmount the temporary save mount, retrying a few times.
 **/
static int
unmount_save(void) {
  UmountOpt uopt;
  int ret = -1;

  for(int i = 0; i < 5; i++) {
    memset(&uopt, 0, sizeof uopt);
    sceFsInitUmountSaveDataOpt(&uopt);
    if((ret = sceFsUmountSaveData(&uopt, MOUNT_POINT, 0, 0)) >= 0) {
      sync();
      return 0;
    }
    sleep(1);
  }

  logq("umount failed (0x%08x)", ret);
  return -1;
}


/**
 * Remove anything a previous (killed) instance may have left behind.
 **/
static void
cleanup_stale(void) {
  UmountOpt uopt;

  memset(&uopt, 0, sizeof uopt);
  sceFsInitUmountSaveDataOpt(&uopt);
  sceFsUmountSaveData(&uopt, MOUNT_POINT, 0, 0); /* fails if not mounted */

  if(rmdir(MOUNT_POINT) && errno != ENOENT) {
    g_mount_stuck = 1;
    return;
  }
  g_mount_stuck = 0;
  unlink(TMP_IMG);
}


static int
stamp_current(const char *dst_dir, const struct stat *img) {
  char path[PATH_MAX];
  file_signature_t stored, current = file_signature(img);
  snprintf(path, sizeof path, "%s/.image_stamp", dst_dir);
  FILE *f = fopen(path, "rb");
  if(!f) return 0;
  char magic[8];
  int ok = fread(magic, 1, 8, f) == 8 && !memcmp(magic, "PS5IMG2\n", 8) &&
           fread(&stored, 1, sizeof stored, f) == sizeof stored &&
           !memcmp(&stored, &current, sizeof stored);
  fclose(f);
  return ok;
}

static void
stamp_write(const char *dst_dir, const struct stat *img) {
  char path[PATH_MAX], part[PATH_MAX];
  file_signature_t sig = file_signature(img);
  mkdir_p(dst_dir);
  snprintf(path, sizeof path, "%s/.image_stamp", dst_dir);
  snprintf(part, sizeof part, "%s.part", path);
  FILE *f = fopen(part, "wb");
  if(!f) {
    logq("Cannot save image stamp %s: %s", path, strerror(errno));
    return;
  }
  int ok = fwrite("PS5IMG2\n", 1, 8, f) == 8 &&
           fwrite(&sig, 1, sizeof sig, f) == sizeof sig;
  if(fflush(f) || fsync(fileno(f))) ok = 0;
  if(fclose(f)) ok = 0;
  if(!ok || rename(part, path)) {
    logq("Cannot commit image stamp %s", path);
    unlink(part);
  }
}


/**
 * Back up one save image. The original is only read: a copy is mounted.
 **/
static sv_result_t
backup_save_image(const char *uid, const char *title, const char *tdir,
                  const char *name, int ps5, backup_stats_t *stats) {
  char image[PATH_MAX];
  char label[PATH_MAX];
  char dst_dir[PATH_MAX];
  char why[160];
  uint8_t sealed[0x60];
  uint8_t key[0x20];
  const char *save = name;
  const char *operation = "read save metadata";
  sv_result_t result = SV_FAILED;
  unsigned failed_before;
  struct stat ist, after;
  MountOpt mopt;
  int mounted = 0;
  int have_tmp = 0;
  int ret;
  int fd;

  g_game_abort = 0;
  snprintf(g_watch_title, sizeof g_watch_title, "%s", title);

  if(!strncmp(save, "sdimg_", 6)) {
    save += 6;
  }

  snprintf(image, sizeof image, "%s/%s", tdir, name);
  save_dst_dir(dst_dir, sizeof dst_dir, ps5, uid, title, save);
  snprintf(label, sizeof label, "%s", dst_dir + strlen(BACKUP_ROOT));

  if(stat(image, &ist)) {
    if(errno != ENOENT) backup_scan_error("stat-save-image", image, image, errno);
    return SV_SKIP;
  }
  if(!S_ISREG(ist.st_mode)) {
    return SV_SKIP;
  }

  /* nothing new since the last backup: no need to mount anything */
  if(stamp_current(dst_dir, &ist)) {
    stats->unchanged++;
    return SV_SKIP;
  }

  /* the size of the whole game was already checked per title id */
  if(ist.st_size < 0x860) {
    logq("Failed to backup %s (not a save image)", label);
    notify_problem(NOTICE_ERROR, "save-image-format", label, "Invalid save image (%s).", title);
    stats->failed++;
    return SV_FAILED;
  }

  /* Use the image size as a conservative bound; do not rescan the USB tree. */
  if(usb_free() < (uint64_t)ist.st_size + SPACE_MARGIN) {
    logq("Failed to backup %s (not enough space on USB)", label);
    notify_problem(NOTICE_ERROR, "usb-space", NULL, "Not enough USB space for backups.");
    stats->failed++;
    return SV_FAILED;
  }

  /* Never stage/mount this title while it is running or suspended. */
  if(title_blocked(title, why, sizeof why, 1)) {
    g_game_abort = 1;
    snprintf(g_abort_why, sizeof g_abort_why, "%s", why);
    return SV_GAME;
  }

  /* sealed key: PS4 keeps it in a companion .bin file, PS5 inside the image */
  if(ps5) {
    if((fd = open(image, O_RDONLY)) < 0) {
      goto fail;
    }
    ret = pread(fd, sealed, sizeof sealed, 0x800);
    close(fd);
  } else {
    char bin[PATH_MAX];

    snprintf(bin, sizeof bin, "%s/%s.bin", tdir, save);
    if((fd = open(bin, O_RDONLY)) < 0) {
      goto fail;
    }
    ret = read(fd, sealed, sizeof sealed);
    close(fd);
  }
  if(ret != (int)sizeof sealed) {
    goto fail;
  }

  if(!enough_space((uint64_t)ist.st_size + SPACE_MARGIN)) {
    logq("Failed to backup %s (not enough free space)", label);
    notify_problem(NOTICE_ERROR, "console-space", NULL, "Not enough console storage for save backups.");
    stats->failed++;
    return SV_FAILED;
  }

  g_watch_game = 1;
  g_last_watch = now_seconds();

  /* work on a private copy; the original image is never mounted */
  mkdir_p(TMP_DIR);
  unlink(TMP_IMG);
  have_tmp = 1;
  g_progress_image = 1;
  progress_begin(ist.st_size);
  progress_stage(ist.st_size, 0, 0.5);
  operation = "stage save image";
  ret = copy_plain(image, TMP_IMG);
  if(ret == -2) {
    result = SV_GAME;
    goto cleanup;
  }
  if(ret < 0) {
    goto fail_cleanup;
  }

  /* last check right before the mount */
  if(title_blocked(title, why, sizeof why, 1)) {
    g_game_abort = 1;
    snprintf(g_abort_why, sizeof g_abort_why, "%s", why);
    result = SV_GAME;
    goto cleanup;
  }

  if(stat(image, &after) || !source_same(&ist, &after)) {
    operation = "save image changed while staging";
    goto fail_cleanup;
  }

  operation = "prepare save key";
  if(decrypt_key(sealed, key)) {
    goto fail_cleanup;
  }

  operation = "prepare save mount directory";
  if(mkdir(PFS_ROOT, 0777) && errno != EEXIST) {
    goto fail_cleanup;
  }
  mkdir(MOUNT_POINT, 0777);

  memset(&mopt, 0, sizeof mopt);
  sceFsInitMountSaveDataOpt(&mopt);
  mopt.budgetid = "system";

  if(title_blocked(title, why, sizeof why, 1)) {
    g_game_abort = 1;
    snprintf(g_abort_why, sizeof g_abort_why, "%s", why);
    result = SV_GAME;
    goto cleanup;
  }
  operation = "mount save image";
  if(sceFsMountSaveData(&mopt, TMP_IMG, MOUNT_POINT, key) < 0) {
    rmdir(MOUNT_POINT);
    goto fail_cleanup;
  }
  mounted = 1;

  progress_stage(dir_size(MOUNT_POINT), 0.5, 0.5);
  failed_before = stats->failed;
  g_file_cap = 0;     /* the whole game was already judged against its limit */
  backup_tree(MOUNT_POINT, dst_dir, stats, NULL);
  g_file_cap = MAX_FILE_SZ;

  game_watch();
  if(title_blocked(title, why, sizeof why, 1)) g_game_abort = 1;
  if(g_game_abort) {
    result = SV_GAME;
  } else if(stat(image, &after) || !source_same(&ist, &after)) {
    operation = "save image changed during backup";
  } else if(stats->failed == failed_before) {
    result = SV_OK;
  } else {
    operation = NULL;   /* The file or directory failure was already reported. */
  }
  goto cleanup;

fail_cleanup:
  result = SV_FAILED;

cleanup:
  /* the image must ALWAYS be unmounted again */
  if(mounted) {
    if(unmount_save()) {
      g_mount_stuck = 1;
      logq("Failed to unmount save image; PS4/PS5 saves backup disabled");
      notify_problem(NOTICE_ERROR, "save-cleanup", NULL, "Save backup paused: cleanup failed.");
      g_watch_game = 0;
      progress_finish();
      g_progress_image = 0;
      stats->failed++;
      return SV_FAILED;
    }
    rmdir(MOUNT_POINT);
  }
  if(have_tmp) {
    unlink(TMP_IMG);
  }
  g_watch_game = 0;
  progress_finish();
  g_progress_image = 0;

  if(result == SV_OK) {
    stamp_write(dst_dir, &ist);
  } else if(result == SV_FAILED) {
    goto fail;
  }
  return result;

fail:
  logq("Failed to back up save %s (%s)", label, operation ? operation : "copy saved files");
  if(operation) {
    notify_problem(NOTICE_ERROR, operation, label, "Save backup failed (%s): %s.", title, save);
  }
  stats->failed++;
  return SV_FAILED;
}


typedef struct img_info {
  char     name[256];
  uint64_t size;
  int      needed;       /* changed since the last backup */
} img_info_t;


/**
 * Is this directory entry a save image we should back up?
 **/
static int
is_save_image(const char *n, int ps5) {
  size_t len = strlen(n);

  if(n[0] == '.') {
    return 0;
  }
  if(len > 4 && !strcmp(n + len - 4, ".bin")) {
    return 0;                       /* PS4 sealed key */
  }
  if(!strncmp(n, "sdimg_sce_bu_", 13) || !strncmp(n, "sce_bu_", 7)) {
    return 0;                       /* system backup copies */
  }
  if(!ps5 && strncmp(n, "sdimg_", 6)) {
    return 0;                       /* PS4 images are always sdimg_* */
  }
  return 1;
}


/**
 * Scan <HOME>/<uid>/savedata (PS4) or savedata_prospero (PS5).
 *
 * A game (title id folder) is backed up completely or not at all: the size of
 * all its images is checked against the per-game limit, and the USB drive and
 * internal storage are checked for room, before the first image is mounted.
 **/
static void
scan_saves(const char *uid, int ps5, backup_stats_t *stats) {
  char base[PATH_MAX];
  img_info_t *imgs;
  struct dirent *td;
  DIR *bd;

  snprintf(base, sizeof base, "%s/%s/%s", HOME_ROOT, uid,
           ps5 ? "savedata_prospero" : "savedata");
  if(!(bd = opendir(base))) {
    if(errno != ENOENT) backup_scan_error("open-save-directory", base, base, errno);
    return;
  }
  if(!(imgs = calloc(MAX_IMAGES, sizeof *imgs))) {
    closedir(bd);
    if(!g_progress_planning) {
      notify_problem(NOTICE_ERROR, "save-scan-memory", NULL,
                     "Not enough memory to scan saves.");
    }
    return;
  }

  while((td = backup_readdir(bd, base, base)) && !g_mount_stuck) {
    char tdir[PATH_MAX];
    char tlabel[PATH_MAX];
    char dst_dir[PATH_MAX];
    char a[32];
    char b[32];
    uint64_t total = 0;       /* all images of the game */
    uint64_t need_total = 0;  /* images that must be backed up now */
    uint64_t max_need = 0;
    uint64_t required;
    int n = 0;
    int needed = 0;
    int overflow = 0;
    struct dirent *ie;
    struct stat st;
    DIR *id;

    if(td->d_name[0] == '.') {
      continue;
    }
    if(snprintf(tdir, sizeof tdir, "%s/%s", base, td->d_name) >= (int)sizeof tdir) {
      continue;
    }
    if(stat(tdir, &st)) {
      if(errno != ENOENT) backup_scan_error("stat-save-title", tdir, tdir, errno);
      continue;
    }
    if(!S_ISDIR(st.st_mode)) continue;
    pending_title_t *queued = pending_find(uid, td->d_name, ps5);
    if(g_pending_only && (!queued || !queued->ready)) continue;
    if(!g_progress_planning && queued) {
      queued->ready = 0;
      queued->retry_at = now_seconds() + g_interval;
    }
    if(!(id = opendir(tdir))) {
      if(errno != ENOENT) backup_scan_error("open-save-title", tdir, tdir, errno);
      continue;
    }

    /* pass 1: size up the whole game without mounting anything */
    while((ie = backup_readdir(id, tdir, tdir))) {
      const char *save;
      char path[PATH_MAX];

      if(!is_save_image(ie->d_name, ps5)) {
        continue;
      }
      if(snprintf(path, sizeof path, "%s/%s", tdir, ie->d_name) >= (int)sizeof path) {
        continue;
      }
      if(stat(path, &st)) {
        if(errno != ENOENT) backup_scan_error("stat-save-image", path, path, errno);
        continue;
      }
      if(!S_ISREG(st.st_mode)) continue;
      if(n >= MAX_IMAGES) {
        overflow = 1;
        break;
      }

      save = ie->d_name;
      if(!strncmp(save, "sdimg_", 6)) {
        save += 6;
      }
      save_dst_dir(dst_dir, sizeof dst_dir, ps5, uid, td->d_name, save);

      snprintf(imgs[n].name, sizeof imgs[n].name, "%s", ie->d_name);
      imgs[n].size = st.st_size;
      imgs[n].needed = !stamp_current(dst_dir, &st);
      total += st.st_size;
      if(imgs[n].needed) {
        needed++;
        need_total += st.st_size;
        /* Image size bounds decrypted data without walking the USB tree. */
        if((uint64_t)st.st_size > max_need) {
          max_need = st.st_size;
        }
      }
      n++;
    }
    closedir(id);

    if(n == 0) {
      continue;
    }

    if(g_progress_planning) {
      if(!overflow && total <= g_max_game) progress_plan(needed);
      continue;
    }

    snprintf(tlabel, sizeof tlabel, "/saves/%s/%s/%s", ps5 ? "ps5" : "ps4",
             uid, td->d_name);

    /* 1. per-game size limit: the whole game or nothing */
    if(overflow || total > g_max_game) {
      if(overflow) {
        logq("Too many save images in %s (limit %d)", tlabel, MAX_IMAGES);
        notify_problem(NOTICE_WARNING, "save-image-count", tlabel,
                       "Too many save images (%s).", td->d_name);
      } else {
        format_size(total, a, sizeof a);
        format_size(g_max_game, b, sizeof b);
        logq("Abnormal size of %s; skipping backup (%s, limit %s)", tlabel, a, b);
        notify_problem(NOTICE_WARNING, "title-size", tlabel,
                       "Game saves exceed the size limit (%s).", td->d_name);
      }
      stats->skipped++;
      continue;
    }

    if(!needed) {
      pending_remove(uid, td->d_name, ps5);
      stats->unchanged += n;
      continue;
    }

    char why[160];
    if(title_blocked(td->d_name, why, sizeof why, 1)) {
      pending_defer(uid, td->d_name, ps5);
      logq("Deferred %s: %s", td->d_name, why);
      continue;
    }

    /* 2. the largest image is copied to internal storage first */
    if(!enough_space(max_need + SPACE_MARGIN)) {
      format_size(max_need + SPACE_MARGIN, a, sizeof a);
      logq("Not enough internal storage for %s (need %s)", tlabel, a);
      notify_problem(NOTICE_WARNING, "console-space", NULL, "Not enough console storage for save backups.");
      stats->skipped++;
      continue;
    }

    /* 3. the USB drive must be able to hold everything that is going to be
     * written (image sizes are an upper bound of the decrypted content) */
    required = need_total + SPACE_MARGIN;
    uint64_t available = usb_free();
    if(available < required) {
      format_size(required, a, sizeof a);
      format_size(available, b, sizeof b);
      logq("Not enough space on USB for %s (need ~%s, free %s)", tlabel, a, b);
      notify_problem(NOTICE_WARNING, "usb-space", NULL, "Not enough USB space for backups.");
      stats->skipped++;
      continue;
    }

    /* pass 2: back up only this safe title; other titles remain independent. */
    int complete = 1;
    for(int i = 0; i < n && !g_mount_stuck; i++) {
      if(!imgs[i].needed) {
        stats->unchanged++;
        continue;
      }
      sv_result_t result = backup_save_image(uid, td->d_name, tdir, imgs[i].name, ps5, stats);
      if(result == SV_GAME) {
        pending_defer(uid, td->d_name, ps5);
        complete = 0;
        break;
      }
      if(result == SV_FAILED) complete = 0;
    }
    g_game_abort = 0;
    if(complete) pending_remove(uid, td->d_name, ps5);
  }

  free(imgs);
  closedir(bd);
}


/* Copy an optional console directory, keeping its original path on USB. */
static void
backup_console_dir(const char *src, backup_stats_t *stats) {
  char dst[PATH_MAX];
  struct stat st;

  if(lstat(src, &st)) {
    if(errno != ENOENT) {
      backup_scan_error("stat-console-directory", src, src, errno);
      stats->failed++;
    }
    return;
  }
  if(!S_ISDIR(st.st_mode) || is_backup_root(&st)) return;
  if(snprintf(dst, sizeof dst, "%s%s", BACKUP_ROOT, src) >= (int)sizeof dst) {
    if(!g_progress_planning) {
      logq("Backup path too long: %s", src);
      notify_problem(NOTICE_ERROR, "backup-path-length", src,
                     "Backup path too long: %s.", backup_path_name(src));
    }
    stats->failed++;
    return;
  }
  backup_tree(src, dst, stats, NULL);
}


/* Back up the selected console save and trophy data for every local user. */
static void
backup_saves(backup_stats_t *stats) {
  priv_state_t old;
  struct dirent *ud;
  DIR *hd;
  int decrypted = g_ps4saves || g_ps5saves;
  int direct = !g_pending_only &&
               (g_encrypted_saves || g_ps4trophies || g_ps5trophies);

  if(!decrypted && !direct) return;
  priv_elevate(&old);

  if(decrypted && !g_progress_planning && (g_need_cleanup || g_mount_stuck)) {
    cleanup_stale();
    g_need_cleanup = 0;
  }
  if(decrypted && g_mount_stuck) {
    if(!g_progress_planning) {
      logq("A previous save mount could not be removed; decrypted saves backup disabled");
      notify_problem(NOTICE_ERROR, "save-cleanup", NULL, "Save backup paused: cleanup failed.");
    }
    stats->failed++;
  }

  if((hd = opendir(HOME_ROOT))) {
    while((ud = backup_readdir(hd, HOME_ROOT, HOME_ROOT))) {
      struct stat st;
      char path[PATH_MAX];

      if(!is_hex_name(ud->d_name)) {
        continue;
      }
      snprintf(path, sizeof path, "%s/%s", HOME_ROOT, ud->d_name);
      if(lstat(path, &st)) {
        if(errno != ENOENT) backup_scan_error("stat-console-user", path, path, errno);
        continue;
      }
      if(!S_ISDIR(st.st_mode)) continue;

      if(g_ps4saves && !g_mount_stuck) {
        scan_saves(ud->d_name, 0, stats);
      }
      if(g_ps5saves && !g_mount_stuck) {
        scan_saves(ud->d_name, 1, stats);
      }
      if(direct && g_encrypted_saves) {
        uint64_t file_cap = g_file_cap;
        g_file_cap = 0;  /* Encrypted images can be larger than ordinary files. */
        snprintf(path, sizeof path, "%s/%s/savedata", HOME_ROOT, ud->d_name);
        backup_console_dir(path, stats);
        snprintf(path, sizeof path, "%s/%s/savedata_prospero", HOME_ROOT, ud->d_name);
        backup_console_dir(path, stats);
        g_file_cap = file_cap;
      }
      if(direct && g_ps4trophies) {
        snprintf(path, sizeof path, "%s/%s/trophy", HOME_ROOT, ud->d_name);
        backup_console_dir(path, stats);
      }
      if(direct && g_ps5trophies) {
        snprintf(path, sizeof path, "%s/%s/trophy2", HOME_ROOT, ud->d_name);
        backup_console_dir(path, stats);
      }
    }
    closedir(hd);
  } else {
    backup_scan_error("open-console-users", HOME_ROOT, HOME_ROOT, errno);
    stats->failed++;
  }

  if(direct && g_ps4trophies) backup_console_dir("/user/trophy", stats);
  if(direct && g_ps5trophies) backup_console_dir("/user/trophy2", stats);
  g_watch_game = 0;
  priv_restore(&old);
}


/* ------------------------------------------------------------------------
 * backup.conf handling and the main loop
 * ---------------------------------------------------------------------- */

/* Match a setting name, allowing spaces before and after '='. */
static const char *
config_value(const char *line, const char *key, const char *legacy) {
  const char *names[2] = {key, legacy};
  for(int i = 0; i < 2; i++) {
    if(!names[i]) continue;
    size_t n = strlen(names[i]);
    if(strncmp(line, names[i], n)) {
      continue;
    }
    const char *p = line + n;
    while(*p == ' ' || *p == '\t') p++;
    if(*p++ != '=') continue;
    while(*p == ' ' || *p == '\t') p++;
    return p;
  }
  return NULL;
}

static int
config_number(const char *value, long min, long max, long *out) {
  char *end;
  errno = 0;
  long v = strtol(value, &end, 10);
  if(errno || end == value || *end) {
    logq("Invalid numeric setting: %s", value);
    return 0;
  }
  if(v < min || v > max) {
    logq("Numeric setting out of range: %s (expected %ld..%ld)",
         value, min, max);
    return 0;
  }
  *out = v;
  return 1;
}

static char *
config_trim(char *line) {
  while(*line == ' ' || *line == '\t') {
    line++;
  }
  size_t len = strlen(line);
  while(len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                    line[len-1] == ' '  || line[len-1] == '\t')) {
    line[--len] = 0;
  }
  return line;
}

static void
notification_config_entry(const char *line, notification_settings_t *settings) {
  const char *value;
  if((value = config_value(line, "backup_notifications", NULL))) {
    if(!strcmp(value, "all")) settings->mode = NOTICES_ALL;
    else if(!strcmp(value, "errors")) settings->mode = NOTICES_ERRORS;
    else if(!strcmp(value, "off")) settings->mode = NOTICES_OFF;
    else logq("Invalid backup_notifications: %s (expected all, errors or off)", value);
  } else if((value = config_value(line, "backup_notifications_in_game", NULL))) {
    parse_bool(value, &settings->in_game);
  }
}

static int
notification_config_read(FILE *f) {
  notification_settings_t settings = notification_defaults;
  char line[PATH_MAX + 16];
  while(fgets(line, sizeof line, f)) notification_config_entry(config_trim(line), &settings);
  if(ferror(f)) return 0;
  notification_settings = settings;
  return 1;
}

/* Read the policy even if the payload cannot start a backup pass. */
static void
notification_config_on_startup_error(void) {
  FILE *f = fopen(CONF_PATH, "r");
  if(!f) return;
  notification_config_read(f);
  fclose(f);
}

/**
 * Handle a trimmed line from backup.conf.
 **/
static void
backup_entry(char *line, backup_stats_t *stats) {
  char dst[PATH_MAX];
  struct stat st;
  size_t len = strlen(line);

  if(len == 0 || line[0] == '#') {
    return;
  }

  /* Keep legacy key aliases for existing backup.conf files. All MB values
   * now mean decimal megabytes, including the legacy max_game_mb key. */
  const char *value;
  if((value = config_value(line, "backup_interval_seconds", "interval"))) {
    long v;
    if(!config_number(value, MIN_INTERVAL, MAX_INTERVAL, &v)) {
      return;
    }
    g_interval = v;
    return;
  }
  if((value = config_value(line, "max_title_size_mb", "max_game_mb"))) {
    long v;
    if(!config_number(value, MIN_GAME_MB, MAX_GAME_MB, &v)) {
      return;
    }
    g_max_game = (uint64_t)v * BYTES_PER_MB;
    return;
  }
  if((value = config_value(line, "daily_snapshots_to_keep", "dailybackups"))) {
    long v;
    if(!config_number(value, 0, MAX_DAILY, &v)) {
      return;
    }
    g_daily = (int)v;
    return;
  }
  if((value = config_value(line, "backup_ps4_saves", "ps4saves"))) {
    parse_bool(value, &g_ps4saves);
    return;
  }
  if((value = config_value(line, "backup_ps5_saves", "ps5saves"))) {
    parse_bool(value, &g_ps5saves);
    return;
  }
  if((value = config_value(line, "backup_encrypted_saves", NULL))) {
    parse_bool(value, &g_encrypted_saves);
    return;
  }
  if((value = config_value(line, "backup_ps4_trophies", NULL))) {
    parse_bool(value, &g_ps4trophies);
    return;
  }
  if((value = config_value(line, "backup_ps5_trophies", NULL))) {
    parse_bool(value, &g_ps5trophies);
    return;
  }

  if(g_pending_only) return;

  /* only absolute paths are valid entries */
  if(line[0] != '/') {
    return;
  }

  /* strip the trailing star wildcard (and the slash before it) */
  if(line[len-1] == '*') {
    line[--len] = 0;
    while(len > 1 && line[len-1] == '/') {
      line[--len] = 0;
    }
  }

  /* never back up the backup itself */
  if(!strncmp(line, USB_ROOT, strlen(USB_ROOT))) {
    return;
  }

  if(snprintf(dst, sizeof dst, "%s%s", BACKUP_ROOT, line) >= (int)sizeof dst) {
    if(!g_progress_planning)
      notify_problem(NOTICE_ERROR, "config-path-length", line, "Backup path is too long.");
    return;
  }

  if(stat(line, &st)) {
    if(!g_progress_planning) {
      int error = errno;
      const char *name = backup_path_name(line);
      if(!*name || strlen(name) > 255) name = "configured path";
      if(error == ENOENT || error == ENOTDIR)
        notify_problem(NOTICE_WARNING, "config-path-missing", line, "Backup source missing: %s", name);
      else
        notify_problem(NOTICE_ERROR, "config-path-read", line, "Cannot read backup source: %s", name);
      logq("Cannot stat %s: %s", line, strerror(error));
    }
    stats->failed++;
    return;
  }

  /* A lone wildcard ("*") collapses to "/" above, and "/.." resolves to it.
   * Copying the whole filesystem to the USB drive is never intended: refuse it
   * by identity so that every spelling of the root is covered. */
  struct stat root;
  if(!stat("/", &root) && root.st_dev == st.st_dev && root.st_ino == st.st_ino) {
    if(!g_progress_planning)
      notify_problem(NOTICE_ERROR, "config-root", line, "Filesystem-root backup refused.");
    stats->failed++;
    return;
  }

  /* The source path is appended to BACKUP_ROOT to form the destination. */
  for(const char *part = strstr(line, "/.."); part; part = strstr(part + 3, "/..")) {
    if(part[3] == '/' || part[3] == 0) {
      if(!g_progress_planning)
        notify_problem(NOTICE_ERROR, "config-parent", line, "Backup path contains a parent directory (..).");
      stats->failed++;
      return;
    }
  }
  if(S_ISDIR(st.st_mode) && is_backup_root(&st)) {
    if(!g_progress_planning)
      notify_problem(NOTICE_ERROR, "config-destination", line, "Backup destination cannot be a source.");
    stats->failed++;
    return;
  }

  if(S_ISDIR(st.st_mode)) {
    if(backup_tree(line, dst, stats, g_progress_planning ? NULL : &st)) {
      stats->empty_sources++;
      const char *end = line + len;
      while(end > line && end[-1] == '/') end--;
      const char *name = end;
      while(name > line && name[-1] != '/') name--;
      logq("No files to back up in %.*s.", (int)(end - name), name);
    }
  } else if(S_ISREG(st.st_mode)) {
    backup_file(line, dst, &st, stats);
  }
}


/* ------------------------------------------------------------------------
 * Daily snapshots (daily_snapshots_to_keep=N)
 *
 * A snapshot is a copy of the live backup folder, stored as
 * /mnt/usb0/backup_daily/YYYY-MM-DD (console local date). The first run on a
 * given date creates it and it is never touched again that day. Only the N
 * newest date folders are kept.
 * ---------------------------------------------------------------------- */

typedef struct clock_info {
  char date[16];        /* YYYY-MM-DD in console local time */
  int  offset_min;      /* local time minus UTC, in minutes */
  long to_midnight;     /* seconds until the next local midnight */
} clock_info_t;


static void
local_clock(time_t now, clock_info_t *ci) {
  struct tm lt;
  struct tm gt;
  int dd;

  memset(&lt, 0, sizeof lt);
  memset(&gt, 0, sizeof gt);
  localtime_r(&now, &lt);
  gmtime_r(&now, &gt);

  snprintf(ci->date, sizeof ci->date, "%04d-%02d-%02d", lt.tm_year + 1900,
           lt.tm_mon + 1, lt.tm_mday);

  /* offset between local time and UTC, taking a date/year change into account */
  dd = lt.tm_yday - gt.tm_yday;
  if(dd > 1) {
    dd = -1;
  } else if(dd < -1) {
    dd = 1;
  }
  ci->offset_min = dd * 1440 + (lt.tm_hour * 60 + lt.tm_min) -
                   (gt.tm_hour * 60 + gt.tm_min);

  ci->to_midnight = 86400 - (lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec);
}


static void
format_offset(int offset_min, char *out, size_t len) {
  int a = offset_min < 0 ? -offset_min : offset_min;

  snprintf(out, len, "UTC%c%d:%02d", offset_min < 0 ? '-' : '+', a / 60, a % 60);
}


/**
 * Exactly YYYY-MM-DD. Only folders with such a name are ever deleted.
 **/
static int
is_date_name(const char *n) {
  if(strlen(n) != 10) {
    return 0;
  }
  for(int i = 0; i < 10; i++) {
    if(i == 4 || i == 7) {
      if(n[i] != '-') {
        return 0;
      }
    } else if(!isdigit((unsigned char)n[i])) {
      return 0;
    }
  }
  return 1;
}


static int
is_date_part_name(const char *n) {
  size_t len = strlen(n);
  char date[11];

  if(len != 15 || strcmp(n + 10, ".part")) {
    return 0;
  }
  memcpy(date, n, 10);
  date[10] = 0;
  return is_date_name(date);
}


/**
 * Delete a file or a whole directory tree.
 **/
static int
remove_tree(const char *path) {
  char sub[PATH_MAX];
  struct dirent *dp;
  struct stat st;
  int rc = 0;
  DIR *dir;

  if(lstat(path, &st)) {
    return errno == ENOENT ? 0 : -1;
  }
  if(!S_ISDIR(st.st_mode)) {
    return unlink(path);
  }
  if(!(dir = opendir(path))) {
    return -1;
  }
  while((dp = readdir(dir))) {
    if(!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")) {
      continue;
    }
    if(snprintf(sub, sizeof sub, "%s/%s", path, dp->d_name) >= (int)sizeof sub ||
       remove_tree(sub)) {
      rc = -1;
    }
  }
  closedir(dir);

  if(rmdir(path)) {
    rc = -1;
  }
  return rc;
}


/**
 * Copy one regular file, keeping its modification time.
 **/
static int
copy_file_keep_time(const char *src, const char *dst, const struct stat *st) {
  struct timeval tv[2];
  ssize_t n;
  int rc = -1;
  int in;
  int out;

  if((in = open(src, O_RDONLY)) < 0) {
    return -1;
  }
  if((out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) {
    close(in);
    return -1;
  }

  while((n = read(in, g_buf, COPY_BUF_SZ)) > 0) {
    char *p = g_buf;
    while(n > 0) {
      ssize_t w = write(out, p, n);
      if(w < 0) {
        if(errno == EINTR) {
          continue;
        }
        goto done;
      }
      progress_advance(w);
      p += w;
      n -= w;
    }
  }
  if(n == 0) {
    rc = 0;
  }

done:
  close(in);
  if(close(out)) {
    rc = -1;
  }
  if(!rc) {
    tv[0].tv_sec = st->st_mtime;
    tv[0].tv_usec = 0;
    tv[1] = tv[0];
    utimes(dst, tv);
  }
  return rc;
}


/**
 * Copy a directory tree from src to dst. Internal bookkeeping files of the
 * live backup (.part, .image_stamp) are not part of a snapshot.
 **/
static int
copy_tree(const char *src, const char *dst, uint64_t *bytes) {
  char spath[PATH_MAX];
  char dpath[PATH_MAX];
  struct dirent *dp;
  struct stat st;
  int rc = 0;
  DIR *dir;

  if(mkdir(dst, 0777) && errno != EEXIST) {
    return -1;
  }
  if(!(dir = opendir(src))) {
    return -1;
  }

  while(!rc) {
    errno = 0;
    dp = readdir(dir);
    if(!dp) {
      if(errno) rc = -1;
      break;
    }
    size_t len = strlen(dp->d_name);

    if(!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")) {
      continue;
    }
    if(!strcmp(dp->d_name, ".ps5save-index") ||
       !strcmp(dp->d_name, ".image_stamp") ||
       (len > 5 && !strcmp(dp->d_name + len - 5, ".part"))) {
      continue;
    }
    if(snprintf(spath, sizeof spath, "%s/%s", src, dp->d_name) >= (int)sizeof spath ||
       snprintf(dpath, sizeof dpath, "%s/%s", dst, dp->d_name) >= (int)sizeof dpath ||
       lstat(spath, &st)) {
      rc = -1;
      break;
    }

    if(S_ISDIR(st.st_mode)) {
      rc = copy_tree(spath, dpath, bytes);
    } else if(S_ISREG(st.st_mode)) {
      rc = copy_file_keep_time(spath, dpath, &st);
      if(!rc) {
        *bytes += st.st_size;
      }
    }
  }

  closedir(dir);
  return rc;
}


static int
cmp_desc(const void *a, const void *b) {
  return strcmp((const char*)b, (const char*)a);
}


/**
 * Keep only the newest g_daily date folders. Today's folder is never deleted,
 * and nothing but folders named exactly YYYY-MM-DD (and stale YYYY-MM-DD.part
 * folders of interrupted copies) is ever touched.
 **/
static void
daily_prune(const char *today) {
  char (*names)[11];
  char (*stale)[16];
  char path[PATH_MAX];
  struct dirent *dp;
  struct stat st;
  int n = 0;
  int ns = 0;
  DIR *dir;

  names = malloc((size_t)MAX_DAILY_DIRS * sizeof *names);
  stale = malloc((size_t)MAX_DAILY_DIRS * sizeof *stale);
  if(!names || !stale || !(dir = opendir(DAILY_ROOT))) {
    free(names);
    free(stale);
    notify_problem(NOTICE_WARNING, "daily-prune-scan", NULL, "Could not check daily snapshot retention.");
    return;
  }

  /* collect first, delete afterwards (never modify a directory while reading it) */
  while((dp = readdir(dir))) {
    if(snprintf(path, sizeof path, "%s/%s", DAILY_ROOT, dp->d_name) >= (int)sizeof path) {
      continue;
    }
    if(is_date_part_name(dp->d_name)) {
      /* nothing is being copied while we prune, so this is stale */
      if(ns < MAX_DAILY_DIRS) {
        memcpy(stale[ns++], dp->d_name, 16);
      }
    } else if(is_date_name(dp->d_name) && n < MAX_DAILY_DIRS &&
              !stat(path, &st) && S_ISDIR(st.st_mode)) {
      memcpy(names[n++], dp->d_name, 11);
    }
  }
  closedir(dir);

  for(int i = 0; i < ns; i++) {
    snprintf(path, sizeof path, "%s/%s", DAILY_ROOT, stale[i]);
    if(remove_tree(path))
      notify_problem(NOTICE_WARNING, "daily-prune-part", stale[i], "Could not remove an unfinished snapshot.");
  }

  qsort(names, n, sizeof *names, cmp_desc);

  for(int i = g_daily; i < n; i++) {
    if(!strcmp(names[i], today)) {
      continue;
    }
    snprintf(path, sizeof path, "%s/%s", DAILY_ROOT, names[i]);
    if(!remove_tree(path)) {
      logq("Daily backup %s deleted (keeping newest %d)", names[i], g_daily);
    } else {
      notify_problem(NOTICE_WARNING, "daily-prune", names[i], "Could not remove an old snapshot.");
    }
  }

  free(names);
  free(stale);
}


/**
 * Create today's snapshot if there is none yet. Called after every live
 * backup pass, which covers every payload start, every wake-up from rest mode
 * and the midnight wake-up. Returns 1 for a saved snapshot, 0 when no
 * snapshot is needed or possible yet, and -1 if a snapshot is skipped or fails.
 **/
static int
daily_check(void) {
  char final[PATH_MAX];
  char part[PATH_MAX];
  char tz[24];
  char a[32];
  char b[32];
  clock_info_t ci;
  struct stat st;
  uint64_t bytes = 0;
  uint64_t need;

  if(g_daily <= 0) {
    return 0;
  }

  local_clock(wall_clock(), &ci);
  format_offset(ci.offset_min, tz, sizeof tz);
  snprintf(final, sizeof final, "%s/%s", DAILY_ROOT, ci.date);
  snprintf(part, sizeof part, "%s.part", final);

  /* one snapshot per date: an existing one is never wiped or refreshed */
  if(!stat(final, &st) && S_ISDIR(st.st_mode)) {
    daily_prune(ci.date);
    return 0;
  }

  if(stat(BACKUP_ROOT, &st) || !S_ISDIR(st.st_mode)) {
    logq("Daily backup %s skipped: no live backup to copy yet", ci.date);
    return 0;
  }

  need = dir_size(BACKUP_ROOT) + SPACE_MARGIN;
  if(usb_free() < need) {
    format_size(need, a, sizeof a);
    format_size(usb_free(), b, sizeof b);
    logq("Not enough space on USB for daily backup %s (need ~%s, free %s)", ci.date, a, b);
    notify_problem(NOTICE_WARNING, "usb-space", NULL, "Not enough USB space for backups.");
    return -1;
  }

  progress_begin(need - SPACE_MARGIN);
  backup_activity_begin("Creating daily snapshot.");
  mkdir_p(DAILY_ROOT);
  remove_tree(part);                     /* leftover of an interrupted copy */

  /* copy into a .part folder and rename at the end, so an interrupted copy is
   * never mistaken for a finished snapshot */
  if(copy_tree(BACKUP_ROOT, part, &bytes) || (sync(), rename(part, final))) {
    progress_finish();
    remove_tree(part);
    logq("Failed to create daily backup %s", ci.date);
    notify_problem(NOTICE_ERROR, "daily-copy", NULL, "Daily snapshot could not be saved.");
    return -1;
  }

  progress_finish();
  format_size(bytes, a, sizeof a);
  logq("Daily backup %s created (%s, console time %s)", ci.date, a, tz);
  daily_prune(ci.date);
  return 1;
}


/**
 * Time of the next backup pass: the regular interval, or just after the next
 * local midnight if that comes first (so the new day's snapshot is made
 * promptly).
 **/
static double
next_due(double started) {
  double due = started + g_interval;
  if(g_daily > 0) {
    clock_info_t ci;
    local_clock(wall_clock(), &ci);
    double midnight = now_seconds() + ci.to_midnight + 1;
    if(midnight < due) due = midnight;
  }
  /* Long passes never overlap or generate a catch-up burst. */
  double now = now_seconds();
  return due > now ? due : now + g_interval;
}


/**
 * Run one backup pass. Returns -1 if backup.conf could not be opened,
 * or -2 if it could not be read or rewound for the next scan.
 **/
static int
run_cycle(int pending_only) {
  backup_stats_t stats;
  char line[PATH_MAX + 16];
  char size[32];
  pid_t pid = getpid();
  double start;
  FILE *f;

  /* escape the jail so that /data and /mnt/usb0 are reachable; done on every
   * cycle in case the state was reset while the console was suspended */
  kernel_set_proc_rootdir(pid, kernel_get_root_vnode());

  if(!(f = fopen(CONF_PATH, "r"))) {
    logq("Cannot open %s: %s", CONF_PATH, strerror(errno));
    return -1;
  }

#ifndef BACKUP_HOST_TEST
  dashboard_set_running(1);
#endif
  progress_reset();
  /* Apply the policy before sending the scan status, including when it is
   * declared after file paths in backup.conf. This pass reads only settings. */
  if(!notification_config_read(f) || fseek(f, 0, SEEK_SET)) {
    logq("Cannot read or rewind %s: %s", CONF_PATH, strerror(errno));
    fclose(f);
    return -2;
  }
  notification_issues_begin(pending_only);
  index_load();
  g_pending_only = pending_only;
  g_cycle_deferred = 0;
  g_backup_busy = 0;
  memset(&stats, 0, sizeof stats);
  start = now_seconds();

  /* Settings are re-read from defaults on every cycle. */
  g_interval = DEF_INTERVAL;
  g_max_game = DEF_GAME_MB * BYTES_PER_MB;
  g_daily = 0;
  g_ps4saves = 0;
  g_ps5saves = 0;
  g_encrypted_saves = g_ps4trophies = g_ps5trophies = 0;
  g_game_abort = 0;
  g_watch_game = 0;

  g_progress_planning = 1;
  while(fgets(line, sizeof line, f)) {
    backup_entry(config_trim(line), &stats);
  }
  int config_read_failed = ferror(f);
  backup_saves(&stats);
  if(!g_pending_only && g_daily > 0) {
    clock_info_t ci;
    char final[PATH_MAX];
    struct stat st;
    local_clock(wall_clock(), &ci);
    snprintf(final, sizeof final, "%s/%s", DAILY_ROOT, ci.date);
    if((stat(final, &st) || !S_ISDIR(st.st_mode)) &&
       (progress_total || (!stat(BACKUP_ROOT, &st) && S_ISDIR(st.st_mode))))
      progress_plan(1);
  }
  g_progress_planning = 0;
  if(fseek(f, 0, SEEK_SET)) {
    logq("Cannot rewind %s: %s", CONF_PATH, strerror(errno));
    fclose(f);
    return -2;
  }
  memset(&stats, 0, sizeof stats);
  g_interval = DEF_INTERVAL;
  g_max_game = DEF_GAME_MB * BYTES_PER_MB;
  g_daily = g_ps4saves = g_ps5saves = 0;
  g_encrypted_saves = g_ps4trophies = g_ps5trophies = 0;
  while(fgets(line, sizeof line, f)) {
    backup_entry(config_trim(line), &stats);
  }
  config_read_failed |= ferror(f);
  fclose(f);
  if(config_read_failed)
    notify_problem(NOTICE_ERROR, "config-read", NULL, "Could not read backup.conf completely.");

  pending_disable();
  if(g_pending_only) {
    for(pending_title_t *p = pending_titles; p; p = p->next)
      if(p->ready) p->retry_at = now_seconds() + g_interval;
  }
  backup_saves(&stats);

  /* Regular files have already been processed. A deferred save-image pass
   * must not prevent a snapshot of the available live backup. */
  int live_work_started = g_backup_busy;
  index_save();
  int snapshot_result = g_pending_only ? 0 : daily_check();
  progress_stop();
  if(!pending_only && !g_cycle_deferred && !g_mount_stuck && !config_read_failed)
    notification_issues_end();
#ifndef BACKUP_HOST_TEST
  dashboard_cycle_done(&stats, g_cycle_deferred, snapshot_result,
                       notification_cycle_failed || config_read_failed);
#endif

  if(stats.failed || notification_cycle_failed) {
    logq("Backup incomplete (%u failed, %u copied)", stats.failed, stats.copied);
  } else if(g_cycle_deferred) {
    logq("Backup pass finished; protected titles queued (%u files unchanged)", stats.unchanged);
    /* A deferred save is expected while its title is protected. */
  } else if(stats.skipped || snapshot_result < 0) {
    logq("Backup pass finished with skips (%u files skipped)", stats.skipped);
    /* Specific warnings are already emitted by the affected operation. */
  } else if(live_work_started) {
    format_size(stats.bytes, size, sizeof size);
    logq("Copied %s in %.1f seconds.", size, now_seconds() - start);
    notify_info("Backup complete (%s).", size);
  } else if(snapshot_result > 0) {
    notify_info("Daily snapshot complete.");
  } else {
    logq("Backup up to date (%u files unchanged)", stats.unchanged);
    /* No changes is the normal outcome; avoid repetitive popups. */
  }

  /* A previously reported failure has actually cleared. For normal mode,
   * the success toast is sufficient when data was copied. In errors mode,
   * report recovery explicitly instead of showing a second success toast.
   */
  if(notification_recovered && !stats.failed && !notification_cycle_failed &&
     !stats.skipped && !g_cycle_deferred && snapshot_result >= 0 &&
     ((!live_work_started && snapshot_result == 0) ||
      notification_settings.mode == NOTICES_ERRORS)) {
    pthread_mutex_lock(&notification_mutex);
    notification_emit_locked(NOTICE_WARNING, "Backup protection restored.");
    pthread_mutex_unlock(&notification_mutex);
  }

  return 0;
}

static void
report_config_error(int result) {
  if(result == -2)
    notify_problem(NOTICE_ERROR, "config-read", NULL, "Could not read backup.conf on USB.");
  else
    notify_problem(NOTICE_ERROR, "config-open", NULL, "Could not open backup.conf on USB.");
}


/**
 * Make sure only one instance runs. If another instance is listening on the
 * loopback lock port, ask it to quit (by connecting) and take over.
 * Returns the listening socket or -1 if the lock could not be set up.
 **/
static int
acquire_lock(void) {
  struct sockaddr_in addr;
  int one = 1;

  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(LOCK_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  for(int attempt = 0; attempt < 10; attempt++) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if(s < 0) {
      return -1;
    }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    if(!bind(s, (struct sockaddr*)&addr, sizeof addr) && !listen(s, 1)) {
      return s;
    }
    close(s);

    if(errno != EADDRINUSE) {
      return -1;
    }

    /* someone holds the port: tell it to quit, then retry */
    if(attempt == 0) {
      logq("Replacing running backup instance");
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if(s >= 0) {
      connect(s, (struct sockaddr*)&addr, sizeof addr);
      close(s);
    }
    sleep(1);
  }

  return -1;
}


/**
 * Wait on monotonic interval deadlines, with short checks for closed queued
 * titles. A close retry does not reset the next regular backup deadline.
 **/
/* 0 = regular deadline, 1 = replacement requested, 2 = queued title closed. */
static int
wait_until(double due, int lock) {
  for(;;) {
    double remaining = due - now_seconds();
    if(remaining <= 0) return 0;
    int slice = pending_titles ? CLOSE_POLL_SEC : SLICE_SEC;
    if(pending_ready()) return 2;
    int ms = remaining > slice ? slice * 1000 : (int)(remaining * 1000) + 1;
    struct pollfd pfd = {lock, POLLIN, 0};
    if(poll(&pfd, lock >= 0 ? 1 : 0, ms) > 0 && (pfd.revents & POLLIN)) {
      int c = accept(lock, 0, 0);
      if(c >= 0) close(c);
      return 1;
    }
  }
}


int
main(void) {
  int missing = 0;
  int lock;
  double due, started;
#ifdef BACKUP_HOST_TEST
  int cycles = 1;
  const char *max_cycles = getenv("TEST_CYCLES");
#endif

  /* the loader's connection may be closed at any time; never die on SIGPIPE */
  signal(SIGPIPE, SIG_IGN);

  kernel_set_proc_rootdir(getpid(), kernel_get_root_vnode());
  lock = acquire_lock();
  if(lock < 0) {
    notification_config_on_startup_error();
    notify_problem(NOTICE_ERROR, "instance-lock", NULL, "Backup could not start: instance lock unavailable.");
    return EXIT_FAILURE;
  }

  if(!(g_buf = malloc(COPY_BUF_SZ))) {
    notification_config_on_startup_error();
    notify_problem(NOTICE_ERROR, "startup-memory", NULL, "Backup could not start: not enough memory.");
    return EXIT_FAILURE;
  }

#ifndef BACKUP_HOST_TEST
  dashboard_start();
#endif
  /* Initial backup immediately; subsequent regular deadlines use monotonic time. */
  started = now_seconds();
  int result = run_cycle(0);
  if(result < 0) {
    report_config_error(result);
    return EXIT_FAILURE;
  }
  /* An initial successful pass is the only time we confirm startup. */
  if(!notification_cycle_failed && !g_backup_busy)
    notify_info("Backup protection active.");
  if(g_daily > 0) {
    /* Log the date and UTC offset the payload sees. */
    clock_info_t ci;
    char tz[24];

    local_clock(wall_clock(), &ci);
    format_offset(ci.offset_min, tz, sizeof tz);
    logq("Daily backups on: keeping newest %d (console date %s, %s)",
         g_daily, ci.date, tz);
  }

  due = next_due(started);

  for(;;) {
#ifdef BACKUP_HOST_TEST
    if(max_cycles && cycles >= atoi(max_cycles)) {
      return EXIT_SUCCESS;
    }
#endif
    int wake = wait_until(due, lock);
    if(wake == 1) {
      logq("Previous backup instance stopped.");
      break;
    }

    started = now_seconds();
    result = run_cycle(wake == 2);
    if(result < 0) {
      /* USB drive may still be mounting after rest mode: retry shortly and
       * only complain if it stays missing */
      if(missing < MISSING_TRIES) missing++;
      if(missing == MISSING_TRIES) {
        report_config_error(result);
      }
      pending_retry_after = now_seconds() + SLICE_SEC * 3;
      due = pending_retry_after;
      continue;
    }
#ifdef BACKUP_HOST_TEST
    cycles++;
#endif

    if(missing >= MISSING_TRIES) {
      logq("backup.conf is back");
    }
    missing = 0;
    pending_retry_after = 0;

    /* g_interval may have been changed by the conf file */
    if(wake != 2) due = next_due(started);
  }

  return EXIT_SUCCESS;
}

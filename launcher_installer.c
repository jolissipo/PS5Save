/* PS5Save+ dashboard tile installer, embedded in the backup daemon.
 *
 * Installs the tile once during startup, without replacing existing titles.
 * Uses the specific-title AppInstUtil entry point, based on the GPLv3
 * implementations in ps5-payload-dev/ftpsrv and itsPLK/ps5-payload-manager.
 * Unlike those installers, this intentionally does NOT fall back to the
 * global AppInstallAll scan, nor does it uninstall other applications.
 *
 * It creates only /user/app/PSSV00001 and registers that single title.
 * It refuses to overwrite any pre-existing, unmarked title directory.
 * The launcher opens http://127.0.0.1:8755/; it does not start the daemon.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "notification_icon.h"

#define LAUNCHER_ID "PSSV00001"
#define LAUNCHER_ROOT "/user/app/" LAUNCHER_ID
#define LAUNCHER_SYS LAUNCHER_ROOT "/sce_sys"
#define LAUNCHER_MARKER LAUNCHER_ROOT "/.ps5save-plus-launcher"
#define LAUNCHER_MARKER_VALUE "PS5Save+ standalone launcher v1\n"
#define LAUNCHER_READY LAUNCHER_ROOT "/.ps5save-plus-registered-v3"
#define LAUNCHER_READY_VALUE "PS5Save+ title registration succeeded v3\n"
#define LAUNCHER_LOG "/mnt/usb0/ps5save-plus/launcher-installer.log"
#define LAUNCHER_LOG_FALLBACK "/data/ps5save-plus-launcher.log"
#define LAUNCHER_ICON LAUNCHER_SYS "/icon0.png"
#define LAUNCHER_PARAM LAUNCHER_SYS "/param.json"
#define LAUNCHER_JSON "launcher/param.json"

_Static_assert(sizeof LAUNCHER_ID == 10, "PS5 title ID must have 9 characters");

__asm__(".section .rodata\n"
        ".global launcher_param\n"
        ".global launcher_param_end\n"
        ".global launcher_param_size\n"
        ".align 16\n"
        "launcher_param:\n"
        ".incbin \"" LAUNCHER_JSON "\"\n"
        "launcher_param_end:\n"
        "launcher_param_size:\n"
        ".quad launcher_param_end - launcher_param\n"
        ".previous");
extern const unsigned char launcher_param[];
extern const size_t launcher_param_size;

/* Matching the proven Debug notification ABI in PS5 Web File Manager.
 * Persist each step, so a silent notification does not hide the error.
 */
typedef struct {
  char padding[45];
  char message[3075];
} launcher_debug_request_t;
int sceKernelSendNotificationRequest(int, void *, size_t, int);

static void launcher_log(const char *message) {
  const char *paths[] = { LAUNCHER_LOG, LAUNCHER_LOG_FALLBACK };
  for(size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
    const char *path = paths[i];
    struct stat st;
    if(!lstat(path, &st) && S_ISREG(st.st_mode) && st.st_size >= 64 * 1024) {
      char old[256];
      int n = snprintf(old, sizeof old, "%s.1", path);
      if(n > 0 && (size_t)n < sizeof old) {
        unlink(old);
        rename(path, old);
      }
    }
    int fd = open(path, O_CREAT | O_WRONLY | O_APPEND | O_NOFOLLOW, 0600);
    if(fd < 0) continue;
    if(!fstat(fd, &st) && S_ISREG(st.st_mode)) {
      char line[512];
      int n = snprintf(line, sizeof line, "[%lld] %s\n",
                       (long long)time(NULL), message);
      if(n > 0) (void)write(fd, line, (size_t)n < sizeof line ?
                                         (size_t)n : sizeof line - 1);
    }
    close(fd);
    return;
  }
}

static void launcher_debug(const char *fmt, ...) {
  launcher_debug_request_t req = {0};
  va_list args;
  va_start(args, fmt);
  vsnprintf(req.message, sizeof req.message, fmt, args);
  va_end(args);
  printf("[PS5Save+ Installer] %s\n", req.message);
  launcher_log(req.message);
  int rc = sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
  if(rc) {
    char text[100];
    snprintf(text, sizeof text,
             "sceKernelSendNotificationRequest failed: 0x%08X", (unsigned)rc);
    launcher_log(text);
  }
}

int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppInstallTitleDir(const char *title_id,
                                    const char *app_root, void *reserved);

/* The launcher refuses to operate on a symlink or a foreign directory.
 * Any existing directory must carry our exact ownership marker.
 */
static int launcher_safe_directory(const char *path, int create, mode_t mode) {
  struct stat st;
  if(!lstat(path, &st)) return S_ISDIR(st.st_mode);
  if(errno != ENOENT || !create) return 0;
  if(mkdir(path, mode)) return 0;
  return !lstat(path, &st) && S_ISDIR(st.st_mode);
}

static int launcher_file_equals(const char *path,
                                const unsigned char *data, size_t len) {
  struct stat st;
  if(lstat(path, &st) || !S_ISREG(st.st_mode) ||
     (uint64_t)st.st_size != (uint64_t)len) return 0;
  int fd = open(path, O_RDONLY | O_NOFOLLOW);
  if(fd < 0) return 0;
  unsigned char buf[4096];
  size_t pos = 0;
  int same = 1;
  while(pos < len) {
    size_t want = len - pos;
    if(want > sizeof buf) want = sizeof buf;
    ssize_t n = read(fd, buf, want);
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0 || memcmp(buf, data + pos, (size_t)n)) {
      same = 0;
      break;
    }
    pos += (size_t)n;
  }
  if(close(fd)) same = 0;
  return same && pos == len;
}

static int launcher_write_exact(int fd, const unsigned char *data, size_t size) {
  size_t pos = 0;
  while(pos < size) {
    ssize_t n = write(fd, data + pos, size - pos);
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) return 0;
    pos += (size_t)n;
  }
  return 1;
}

static int launcher_write_atomic(const char *path,
                                 const unsigned char *data, size_t len) {
  if(launcher_file_equals(path, data, len)) return 1;
  char temp[384];
  int n = snprintf(temp, sizeof temp, "%s.part", path);
  if(n < 0 || (size_t)n >= sizeof temp) return 0;
  if(unlink(temp) && errno != ENOENT) return 0;
  int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
  if(fd < 0) return 0;
  int ok = launcher_write_exact(fd, data, len) && !fsync(fd);
  if(close(fd)) ok = 0;
  if(ok) ok = rename(temp, path) == 0;
  if(!ok) unlink(temp);
  return ok;
}

static int launcher_marker_owned(void) {
  const unsigned char marker[] = LAUNCHER_MARKER_VALUE;
  return launcher_file_equals(LAUNCHER_MARKER, marker,
                              sizeof marker - 1);
}

static int launcher_registered_and_current(void) {
  static const unsigned char ready[] = LAUNCHER_READY_VALUE;
  struct stat p, icon;
  return launcher_file_equals(LAUNCHER_READY, ready, sizeof ready - 1) &&
         launcher_marker_owned() &&
         !lstat(LAUNCHER_PARAM, &p) && S_ISREG(p.st_mode) &&
         !lstat(LAUNCHER_ICON, &icon) && S_ISREG(icon.st_mode);
}

static int ps5save_launcher_install(void) {
  /* Skip quietly on repeat launches; no user-facing Debug noise. */
  if(launcher_registered_and_current()) return 0;
  launcher_debug("PS5Save+ launcher: starting installation.");

  int rc = kernel_set_proc_rootdir(getpid(), kernel_get_root_vnode());
  if(rc) {
    launcher_debug("ERROR: root access failed: 0x%08X.", (unsigned)rc);
    return 1;
  }
  launcher_debug("PS5Save+ launcher: root access confirmed.");

  /* The exact issue fixed in Payload Manager PR #108: on some consoles
   * /user/app does not exist. Create it before touching the title folder. */
  if(!launcher_safe_directory("/user/app", 1, 0770)) {
    launcher_debug("ERROR: /user/app directory unavailable (errno %d).", errno);
    return 1;
  }

  struct stat st;
  int existing = !lstat(LAUNCHER_ROOT, &st);
  if(existing && (!S_ISDIR(st.st_mode) || !launcher_marker_owned())) {
    launcher_debug("ERROR: title ID %s belongs to another application; nothing changed.",
                   LAUNCHER_ID);
    return 1;
  }
  if(!existing && errno != ENOENT) {
    launcher_debug("ERROR: could not inspect /user/app/%s (errno %d).",
                   LAUNCHER_ID, errno);
    return 1;
  }
  if(existing && launcher_registered_and_current()) {
    launcher_debug("PS5Save+ launcher already installed; no changes needed.");
    return 0;
  }

  launcher_debug("PS5Save+ launcher: initializing application installer.");
  rc = sceAppInstUtilInitialize();
  if(rc) {
    launcher_debug("ERROR: sceAppInstUtilInitialize returned 0x%08X.", (unsigned)rc);
    return 1;
  }

  if(!existing && !launcher_safe_directory(LAUNCHER_ROOT, 1, 0755)) {
    launcher_debug("ERROR: could not create launcher directory (errno %d).",
                   errno);
    sceAppInstUtilTerminate();
    return 1;
  }
  if(!existing) {
    static const unsigned char marker[] = LAUNCHER_MARKER_VALUE;
    if(!launcher_write_atomic(LAUNCHER_MARKER, marker, sizeof marker - 1)) {
      launcher_debug("ERROR: could not save launcher marker (errno %d).", errno);
      sceAppInstUtilTerminate();
      return 1;
    }
  }
  if(!launcher_safe_directory(LAUNCHER_SYS, 1, 0755) ||
     !launcher_write_atomic(LAUNCHER_PARAM, launcher_param, launcher_param_size) ||
     !launcher_write_atomic(LAUNCHER_ICON, notification_icon_png,
                            sizeof notification_icon_png)) {
    launcher_debug("ERROR: could not prepare param.json or icon0.png (errno %d).",
                   errno);
    sceAppInstUtilTerminate();
    return 1;
  }

  launcher_debug("PS5Save+ launcher: files ready; registering home-screen tile...");
  /* Register only our title; do not globally scan installed applications. */
  rc = sceAppInstUtilAppInstallTitleDir(LAUNCHER_ID, "/user/app/", NULL);
  int terminate_rc = sceAppInstUtilTerminate();
  if(rc) {
    launcher_debug("ERROR: PSSV00001 registration failed: 0x%08X.", (unsigned)rc);
    return 1;
  }
  if(terminate_rc)
    launcher_debug("WARNING: sceAppInstUtilTerminate returned 0x%08X.",
                   (unsigned)terminate_rc);

  static const unsigned char ready[] = LAUNCHER_READY_VALUE;
  if(!launcher_write_atomic(LAUNCHER_READY, ready, sizeof ready - 1))
    launcher_debug("WARNING: launcher registered but local status could not be saved.");

  launcher_debug("PS5Save+ launcher: registration accepted. Check Media or Game Library.");
  return 0;
}

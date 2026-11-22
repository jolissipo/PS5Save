/* Optional PS5Save+ home-screen tile installer (PS5 ELF).
 *
 * This is NOT part of the backup daemon and is NEVER run automatically.
 * Uses the specific-title AppInstUtil entry point, based on the GPLv3
 * implementations in ps5-payload-dev/ftpsrv and itsPLK/ps5-payload-manager.
 * Unlike those installers, this intentionally does NOT fall back to the
 * global AppInstallAll scan, nor does it uninstall other applications.
 *
 * It creates only /user/app/P5SV00001 and registers that single title.
 * It refuses to overwrite any pre-existing, unmarked title directory.
 * The launcher opens http://127.0.0.1:8755/; it does not start the daemon.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "notification_icon.h"

#define LAUNCHER_ID "P5SV00001"
#define LAUNCHER_ROOT "/user/app/" LAUNCHER_ID
#define LAUNCHER_SYS LAUNCHER_ROOT "/sce_sys"
#define LAUNCHER_MARKER LAUNCHER_ROOT "/.ps5save-plus-launcher"
#define LAUNCHER_MARKER_VALUE "PS5Save+ standalone launcher v1\n"
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

int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);

typedef int (*install_title_fn)(const char *title_id,
                                const char *app_root,
                                void *reserved);

/* The launcher refuses to operate on a symlink or a foreign directory.
 * Any existing directory must carry our exact ownership marker.
 */
static int launcher_safe_directory(const char *path, int create) {
  struct stat st;
  if(!lstat(path, &st)) return S_ISDIR(st.st_mode);
  if(errno != ENOENT || !create) return 0;
  if(mkdir(path, 0755)) return 0;
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

int main(void) {
  struct stat st;
  kernel_set_proc_rootdir(getpid(), kernel_get_root_vnode());

  if(!lstat(LAUNCHER_ROOT, &st)) {
    if(!S_ISDIR(st.st_mode) || !launcher_marker_owned()) {
      printf("PS5Save+ launcher: refusing existing unowned title %s\n",
             LAUNCHER_ID);
      return 1;
    }
  } else if(errno != ENOENT) {
    perror("PS5Save+ launcher: cannot inspect title");
    return 1;
  } else {
    if(!launcher_safe_directory(LAUNCHER_ROOT, 1)) {
      perror("PS5Save+ launcher: cannot create title folder");
      return 1;
    }
    static const unsigned char marker[] = LAUNCHER_MARKER_VALUE;
    if(!launcher_write_atomic(LAUNCHER_MARKER, marker,
                              sizeof marker - 1)) {
      printf("PS5Save+ launcher: could not save ownership marker\n");
      return 1;
    }
  }

  if(!launcher_safe_directory(LAUNCHER_SYS, 1)) {
    printf("PS5Save+ launcher: cannot create sce_sys\n");
    return 1;
  }
  if(!launcher_write_atomic(LAUNCHER_PARAM, launcher_param,
                            launcher_param_size)) {
    printf("PS5Save+ launcher: could not save param.json\n");
    return 1;
  }
  if(!launcher_write_atomic(LAUNCHER_ICON, notification_icon_png,
                            sizeof notification_icon_png)) {
    printf("PS5Save+ launcher: could not save icon0.png\n");
    return 1;
  }

  int rc = sceAppInstUtilInitialize();
  if(rc) {
    printf("PS5Save+ launcher: AppInstUtil init failed: 0x%08x\n",
           (unsigned)rc);
    return 1;
  }

  /* NID of sceAppInstUtilAppInstallTitleDir. A targeted registration is
   * essential: never scan or modify unrelated titles in /user/app.
   */
  uint32_t handle = 0;
  install_title_fn install = NULL;
  if(!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle))
    install = (install_title_fn)kernel_dynlib_resolve(-1, handle,
                                                     "Wudg3Xe3heE");
  if(!install) {
    puts("PS5Save+ launcher: targeted install API is unavailable.");
    sceAppInstUtilTerminate();
    return 1;
  }

  rc = install(LAUNCHER_ID, "/user/app/", NULL);
  sceAppInstUtilTerminate();
  if(rc) {
    printf("PS5Save+ launcher: Sony registration failed: 0x%08x\n",
           (unsigned)rc);
    return 1;
  }
  puts("PS5Save+ launcher: registration requested successfully.");
  puts("The dashboard must already be running at http://127.0.0.1:8755/");
  return 0;
}

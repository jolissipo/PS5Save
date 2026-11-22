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
#include <stdarg.h>
#include <stdlib.h>
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
#define LAUNCHER_READY LAUNCHER_ROOT "/.ps5save-plus-registered-v2"
#define LAUNCHER_READY_VALUE "PS5Save+ title registration succeeded v2\n"
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

/* The console's Debug notifications are intentionally used ONLY by this
 * standalone installer. The backup daemon keeps its rich PS5 notifications. */
typedef struct {
  char padding[45];
  char message[3075];
} launcher_debug_request_t;
int sceKernelSendNotificationRequest(int, launcher_debug_request_t *, size_t, int);
static void launcher_debug(const char *fmt, ...) {
  launcher_debug_request_t req = {0};
  va_list args;
  va_start(args, fmt);
  vsnprintf(req.message, sizeof req.message, fmt, args);
  va_end(args);
  printf("[PS5Save+ Installer] %s\n", req.message);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

/* The browser-app installer initializes these services before AppInstUtil.
 * Initialization failures are diagnostic: the installer still reports the
 * actual AppInstUtil result rather than presuming a particular cause. */
int sceNetCtlInit(void);
int sceUserServiceInitialize(void *);
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

static int launcher_registered_and_current(void) {
  static const unsigned char ready[] = LAUNCHER_READY_VALUE;
  return launcher_file_equals(LAUNCHER_READY, ready, sizeof ready - 1) &&
         launcher_file_equals(LAUNCHER_PARAM, launcher_param, launcher_param_size) &&
         launcher_file_equals(LAUNCHER_ICON, notification_icon_png,
                              sizeof notification_icon_png);
}

int main(void) {
  struct stat st;
  kernel_set_proc_rootdir(getpid(), kernel_get_root_vnode());
  launcher_debug("PS5Save+: a verificar atalho do dashboard.");

  if(!lstat(LAUNCHER_ROOT, &st)) {
    if(!S_ISDIR(st.st_mode) || !launcher_marker_owned()) {
      launcher_debug("ERRO PS5Save+: %s ja existe e nao pertence ao PS5Save+. Sem alteracoes.", LAUNCHER_ID);
      return 1;
    }
  } else if(errno != ENOENT) {
    launcher_debug("ERRO PS5Save+: nao foi possivel verificar a pasta (errno %d).", errno);
    return 1;
  } else {
    if(!launcher_safe_directory(LAUNCHER_ROOT, 1)) {
      launcher_debug("ERRO PS5Save+: nao foi possivel criar a pasta (errno %d).", errno);
      return 1;
    }
    static const unsigned char marker[] = LAUNCHER_MARKER_VALUE;
    if(!launcher_write_atomic(LAUNCHER_MARKER, marker,
                              sizeof marker - 1)) {
      launcher_debug("ERRO PS5Save+: falha no marcador de propriedade (errno %d).", errno);
      return 1;
    }
  }

  /* Repeated runs are intentionally idempotent. Do not change or re-register
   * a title that our previous successful install already staged unchanged.
   * A previous failed attempt has no READY marker and can safely be retried. */
  if(launcher_registered_and_current()) {
    launcher_debug("PS5Save+: atalho ja registado. Verifica o separador Multimedia ou Biblioteca.");
    return 0;
  }
  launcher_debug("PS5Save+: a preparar o icone e o registo...");
  if(!launcher_safe_directory(LAUNCHER_SYS, 1)) {
    launcher_debug("ERRO PS5Save+: nao foi possivel criar sce_sys (errno %d).", errno);
    return 1;
  }
  if(!launcher_write_atomic(LAUNCHER_PARAM, launcher_param,
                            launcher_param_size)) {
    launcher_debug("ERRO PS5Save+: falha ao guardar param.json (errno %d).", errno);
    return 1;
  }
  if(!launcher_write_atomic(LAUNCHER_ICON, notification_icon_png,
                            sizeof notification_icon_png)) {
    launcher_debug("ERRO PS5Save+: falha ao guardar icon0.png (errno %d).", errno);
    return 1;
  }

  int user_priority = 256;
  int net_rc = sceNetCtlInit();
  int user_rc = sceUserServiceInitialize(&user_priority);
  if(net_rc || user_rc)
    launcher_debug("PS5Save+: servicos PS5: NetCtl=0x%08X UserService=0x%08X.",
                   (unsigned)net_rc, (unsigned)user_rc);

  int rc = sceAppInstUtilInitialize();
  if(rc) {
    launcher_debug("ERRO PS5Save+: AppInstUtilInitialize 0x%08X.", (unsigned)rc);
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
    launcher_debug("ERRO PS5Save+: API de registo individual indisponivel (NID Wudg3Xe3heE).");
    sceAppInstUtilTerminate();
    return 1;
  }

  rc = install(LAUNCHER_ID, "/user/app/", NULL);
  int term_rc = sceAppInstUtilTerminate();
  if(rc) {
    launcher_debug("ERRO PS5Save+: registo individual falhou: 0x%08X.", (unsigned)rc);
    return 1;
  }
  if(term_rc)
    launcher_debug("PS5Save+: aviso ao terminar AppInstUtil: 0x%08X.", (unsigned)term_rc);
  /* Mark a successful registration only AFTER the registration API returns 0.
   * Never treat a merely created directory as an installed launcher. */
  static const unsigned char ready[] = LAUNCHER_READY_VALUE;
  if(!launcher_write_atomic(LAUNCHER_READY, ready, sizeof ready - 1))
    launcher_debug("PS5Save+: registado, mas sem marcador de estado (errno %d).", errno);
  else
    launcher_debug("PS5Save+: registo aceite! Procura o atalho em Multimedia/Biblioteca.");
  launcher_debug("PS5Save+: o atalho abre o dashboard apenas com o backup.elf ativo.");
  return 0;
}

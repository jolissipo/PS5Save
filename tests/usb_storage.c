/* Host regression checks for PS5Save+ USB-device selection and migration.
 * All mount paths are inside TEST_ROOT. Never requires root privileges. */
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

#define PS5SAVE_DYNAMIC_STORAGE 1
#define PS5SAVE_USB_TEST 1
#define STORAGE_USB_PREFIX TEST_ROOT "/usb"
#define SPACE_MARGIN (1024)
static char storage_usb_path[PATH_MAX], storage_root_path[PATH_MAX],
            storage_conf_path[PATH_MAX], storage_backup_path[PATH_MAX],
            storage_daily_path[PATH_MAX], storage_legacy_conf[PATH_MAX],
            storage_legacy_bak[PATH_MAX], storage_legacy_backup[PATH_MAX],
            storage_legacy_daily[PATH_MAX], storage_conf_bak[PATH_MAX],
            storage_conf_part[PATH_MAX], storage_log_path[PATH_MAX],
            storage_log_rotated[PATH_MAX], storage_history_path[PATH_MAX],
            storage_history_rotated[PATH_MAX], storage_index_path[PATH_MAX],
            storage_index_part[PATH_MAX], storage_installer_log[PATH_MAX];
#define USB_ROOT storage_usb_path
#define STORAGE_ROOT storage_root_path
#define CONF_PATH storage_conf_path
#define BACKUP_ROOT storage_backup_path
#define DAILY_ROOT storage_daily_path
#include "../usb_storage.h"

static int rebase_calls, save_calls, notification_calls, file_copy_calls;
static int migration_started, migration_finished;
static void progress_advance(size_t value) {(void)value;}
static void logq(const char *format, ...) {(void)format;}
static void usb_transfer_notice(int complete, const char *message) {
  notification_calls++;
  if(complete) {
    assert(!strcmp(message, "USB transfer complete."));
    migration_finished++;
  } else {
    assert(strstr(message, "Backup USB changed. Copying to "));
    migration_started++;
  }
}
static void index_rebase_usb(const char *from, const char *to) {
  assert(strstr(from,"/ps5save-plus/backup"));
  assert(strstr(to,"/ps5save-plus/backup"));
  rebase_calls++;
}
static void index_save(void) {save_calls++;}

static void make_dirs(const char *path) {
  char buffer[PATH_MAX];
  snprintf(buffer,sizeof buffer,"%s",path);
  for(char *p=buffer+1;*p;p++)if(*p=='/') {
    *p=0;assert(!mkdir(buffer,0700)||errno==EEXIST);*p='/';
  }
  assert(!mkdir(buffer,0700)||errno==EEXIST);
}
static void file_write(const char *path,const char *data){
  int fd=open(path,O_CREAT|O_TRUNC|O_WRONLY,0600); assert(fd>=0);
  size_t size=strlen(data);assert(write(fd,data,size)==(ssize_t)size);
  assert(!close(fd));
}
static void file_expect(const char *path,const char *expected){
  FILE *f=fopen(path,"r");assert(f);
  char data[256];size_t n=fread(data,1,sizeof data-1,f);data[n]=0;
  assert(!fclose(f));assert(!strcmp(data,expected));
}
static int copy_file_keep_time(const char *source,const char *target,
                               const struct stat *st) {
  file_copy_calls++;
  int in=open(source,O_RDONLY),out=open(target,O_CREAT|O_TRUNC|O_WRONLY,0600);
  if(in<0||out<0)return -1;
  char buffer[256];ssize_t n;
  int ok=0;
  while((n=read(in,buffer,sizeof buffer))>0){
    if(write(out,buffer,(size_t)n)!=n){ok=-1;break;}
    progress_advance((size_t)n);
  }
  if(n<0)ok=-1;
  if(close(in)||close(out))ok=-1;
  if(!ok){
    struct timeval tv[2]={{st->st_mtime,0},{st->st_mtime,0}};
    if(utimes(target,tv))ok=-1;
  }
  return ok;
}
static int remove_tree(const char *path) {
  struct stat st;
  if(lstat(path,&st))return errno==ENOENT?0:-1;
  if(!S_ISDIR(st.st_mode))return unlink(path);
  DIR *d=opendir(path);if(!d)return -1;
  struct dirent *e;int ok=0;
  while((e=readdir(d))) {
    if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
    char child[PATH_MAX];assert(snprintf(child,sizeof child,"%s/%s",path,e->d_name)<(int)sizeof child);
    if(remove_tree(child))ok=-1;
  }
  closedir(d);
  return rmdir(path)||ok?-1:0;
}
static uint64_t dir_size(const char *path) {
  DIR *d=opendir(path); if(!d)return 0;
  uint64_t n=0; struct dirent *e;
  while((e=readdir(d))){
    if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
    char child[PATH_MAX]; struct stat st;
    assert(snprintf(child,sizeof child,"%s/%s",path,e->d_name)<(int)sizeof child);
    assert(!lstat(child,&st));
    if(S_ISDIR(st.st_mode))n+=dir_size(child);
    if(S_ISREG(st.st_mode))n+=(uint64_t)st.st_size;
  }
  closedir(d);return n;
}
#include "../usb_migrate.h"

int main(void) {
  char first[PATH_MAX],second[PATH_MAX],path[PATH_MAX];
  snprintf(first,sizeof first,"%s0",STORAGE_USB_PREFIX);
  snprintf(second,sizeof second,"%s1",STORAGE_USB_PREFIX);
  make_dirs(first);make_dirs(second);
  assert(storage_usb_number(first)==0 && storage_usb_number(second)==1);
  assert(storage_requested_usb[0] == 0);
  assert(atomic_load(&storage_migration_percent) == 0);
  assert(storage_usb_number("/data")==-1);
  assert(storage_usb_online(first,NULL,NULL));
  assert(storage_usb_online(second,NULL,NULL));

  /* Initially a backup on USB0 wins over the blank, larger USB1. */
  snprintf(path,sizeof path,"%s/ps5save-plus/backup",first);make_dirs(path);
  snprintf(path,sizeof path,"%s/ps5save-plus/backup.conf",first);
  file_write(path,"backup_usb_device=auto\n");
  snprintf(path,sizeof path,"%s/ps5save-plus/backup/file.bin",first);
  file_write(path,"USB0 user data");
  assert(!storage_choose_initial_usb() && !strcmp(USB_ROOT,first));
  assert(!storage_mark_active());

  /* A pre-existing destination is NEVER replaced. */
  snprintf(path,sizeof path,"%s/ps5save-plus",second);make_dirs(path);
  snprintf(path,sizeof path,"%s/ps5save-plus/keep.txt",second);
  file_write(path,"other user backup");
  assert(usb_migrate_to(second)!=0);
  file_expect(path,"other user backup");
  snprintf(path,sizeof path,"%s/ps5save-plus/backup/file.bin",first);
  file_expect(path,"USB0 user data");
  snprintf(path,sizeof path,"%s/ps5save-plus",second);
  assert(!remove_tree(path));

  /* Interrupted transfer: recognize an owned staging folder, reuse only a
   * verified complete file and recopy the partial/absent files. */
  snprintf(path,sizeof path,"%s/ps5save-plus/backup_daily/2026-11-22",first);
  make_dirs(path);
  snprintf(path,sizeof path,"%s/ps5save-plus/backup_daily/2026-11-22/save.bin",first);
  file_write(path,"snapshot data");
  snprintf(path,sizeof path,"%s/%s/backup",second,USB_STAGE_NAME);
  make_dirs(path);
  snprintf(path,sizeof path,"%s/%s/%s",second,USB_STAGE_NAME,USB_STAGE_MARK);
  file_write(path,USB_STAGE_MAGIC);
  snprintf(path,sizeof path,"%s/%s/backup/file.bin",second,USB_STAGE_NAME);
  file_write(path,"USB0 user data");
  snprintf(path,sizeof path,"%s/%s/backup_daily/2026-11-22",second,USB_STAGE_NAME);
  make_dirs(path);
  snprintf(path,sizeof path,"%s/%s/backup_daily/2026-11-22/save.bin",second,USB_STAGE_NAME);
  file_write(path,"snapshot dat"); /* Incomplete file must be recopied. */
  int copies_before_recovery=file_copy_calls;
  assert(!usb_migrate_to(second));
  assert(file_copy_calls==copies_before_recovery+2); /* config and partial snapshot */
  assert(!strcmp(USB_ROOT,second));
  snprintf(path,sizeof path,"%s/ps5save-plus/backup/file.bin",second);
  file_expect(path,"USB0 user data");
  snprintf(path,sizeof path,"%s/ps5save-plus/backup_daily/2026-11-22/save.bin",second);
  file_expect(path,"snapshot data");
  snprintf(path,sizeof path,"%s/ps5save-plus",first);
  assert(access(path,F_OK)!=0);
  assert(rebase_calls==1 && save_calls==1 && notification_calls==2);
  assert(migration_started==1 && migration_finished==1);

  /* Attempts to migrate to internal paths must not change anything. */
  assert(usb_migrate_to("/data") != 0); /* internal paths are rejected */

  /* A symlink in backup content must reject the transfer and keep source. */
  snprintf(path,sizeof path,"%s/ps5save-plus/backup/unsafe",second);
  assert(!symlink("/etc",path));
  assert(usb_migrate_to(first)!=0);
  snprintf(path,sizeof path,"%s/ps5save-plus/backup/file.bin",second);
  file_expect(path,"USB0 user data");
  assert(!strcmp(USB_ROOT,second));

  /* Restart: the only existing backup on USB1 wins, even if USB0 is free. */
  char warning[1024];
  assert(!storage_choose_initial_usb() && !strcmp(USB_ROOT, second));
  assert(!storage_startup_conflict_message(warning, sizeof warning));

  /* If another independent PS5Save+ folder exists, warn and preserve BOTH. */
  snprintf(path, sizeof path, "%s/ps5save-plus/backup", first);
  make_dirs(path);
  snprintf(path, sizeof path, "%s/ps5save-plus/backup.conf", first);
  file_write(path, "backup_usb_device=auto\n");
  snprintf(path, sizeof path, "%s/ps5save-plus/backup/spare.bin", first);
  file_write(path, "other USB saves");
  assert(!storage_choose_initial_usb());
  assert(!strcmp(USB_ROOT, second)); /* USB1's active marker has priority. */
  assert(storage_startup_conflict_message(warning, sizeof warning));
  assert(strstr(warning, first) && strstr(warning, second));
  assert(strstr(warning, "Selected ") && strstr(warning, "Other backups left unchanged."));
  snprintf(path, sizeof path, "%s/ps5save-plus/backup/spare.bin", first);
  file_expect(path, "other USB saves");
  snprintf(path, sizeof path, "%s/ps5save-plus/backup/file.bin", second);
  file_expect(path, "USB0 user data");

  /* An explicit preference in the first USB's own config outranks a marker. */
  snprintf(path, sizeof path, "%s/ps5save-plus/backup.conf", first);
  char preferred[PATH_MAX + 64];
  snprintf(preferred, sizeof preferred, "backup_usb_device=%s\n", first);
  file_write(path, preferred);
  assert(!storage_choose_initial_usb() && !strcmp(USB_ROOT, first));
  assert(storage_startup_conflict_message(warning, sizeof warning));
  assert(strstr(warning, "Other backups left unchanged."));
  snprintf(path, sizeof path, "%s/ps5save-plus/backup/file.bin", second);
  file_expect(path, "USB0 user data");

  puts("PASS USB detection, preferences, migration, duplicate-install diagnostics and data preservation");
  return 0;
}

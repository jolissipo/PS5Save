#ifndef TEST_PS5_KERNEL_H
#define TEST_PS5_KERNEL_H

#include <stdint.h>
#include <sys/types.h>

intptr_t kernel_get_root_vnode(void);
intptr_t kernel_get_proc_jaildir(pid_t pid);
int32_t kernel_set_proc_rootdir(pid_t pid, intptr_t vnode);
int32_t kernel_set_proc_jaildir(pid_t pid, intptr_t vnode);
uint64_t kernel_get_ucred_authid(pid_t pid);
int32_t kernel_set_ucred_authid(pid_t pid, uint64_t authid);
int32_t kernel_get_ucred_caps(pid_t pid, uint8_t caps[16]);
int32_t kernel_set_ucred_caps(pid_t pid, const uint8_t caps[16]);

#endif

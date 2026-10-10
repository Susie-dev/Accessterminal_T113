/*
 * devstubs/sys/mman.h — 主机端语法检查用的 mmap 桩。
 */
#ifndef ACCESSTERMINAL_DEVSTUB_SYS_MMAN_H
#define ACCESSTERMINAL_DEVSTUB_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PROT_READ
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#endif
#ifndef MAP_SHARED
#define MAP_SHARED 0x01
#define MAP_FAILED ((void*)-1)
#endif

void* mmap(void* addr, size_t length, int prot, int flags, int fd,
           long offset);
int munmap(void* addr, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* ACCESSTERMINAL_DEVSTUB_SYS_MMAN_H */

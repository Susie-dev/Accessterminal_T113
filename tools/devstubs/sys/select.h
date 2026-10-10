/*
 * devstubs/sys/select.h — 主机端语法检查用的 select 桩。
 *
 * MinGW 没有 <sys/select.h>（其 fd_set/select 在 winsock2.h 中，
 * 且需要额外初始化）。按 POSIX glibc 复刻工程用到的子集，
 * 供 Windows 上对 T113 工程做真实类型检查。
 * 不是实现；交叉编译必须使用目标平台真实头文件。
 */
#ifndef ACCESSTERMINAL_DEVSTUB_SYS_SELECT_H
#define ACCESSTERMINAL_DEVSTUB_SYS_SELECT_H

#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif

typedef struct {
    unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

#define FD_ZERO(set) \
    do { __builtin_memset((set), 0, sizeof(fd_set)); } while (0)
#define FD_SET(fd, set) \
    do { ((fd_set*)(set))->fds_bits[(fd) / (8 * sizeof(unsigned long))] |= \
             (1UL << ((fd) % (8 * sizeof(unsigned long)))); } while (0)
#define FD_CLR(fd, set) \
    do { ((fd_set*)(set))->fds_bits[(fd) / (8 * sizeof(unsigned long))] &= \
             ~(1UL << ((fd) % (8 * sizeof(unsigned long)))); } while (0)
#define FD_ISSET(fd, set) \
    ((((fd_set*)(set))->fds_bits[(fd) / (8 * sizeof(unsigned long))] & \
      (1UL << ((fd) % (8 * sizeof(unsigned long))))) != 0)

int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
           struct timeval* timeout);

#ifdef __cplusplus
}
#endif

#endif /* ACCESSTERMINAL_DEVSTUB_SYS_SELECT_H */

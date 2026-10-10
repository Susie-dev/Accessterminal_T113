/*
 * devstubs/host_compat.h — 主机端（Windows/MinGW）检查时的补丁头。
 *
 * 仅用于让开发机的 GCC 能编译 T113 工程**自身**的代码。通过命令行
 * `-include host_compat.h` 在一切之前注入，因此不会影响工程源码。
 *
 * 补的内容都是 POSIX 在 Linux 上必然存在、而 MinGW 缺失的定义：
 *   - <fcntl.h> 的 F_GETFL / F_SETFL / O_NONBLOCK
 * 目标平台编译时完全不使用本文件。
 */
#ifndef ACCESSTERMINAL_HOST_COMPAT_H
#define ACCESSTERMINAL_HOST_COMPAT_H

/* 先引入 MinGW 自己的头，避免影响其正常内容 */
#include <fcntl.h>

/* POSIX fcntl 命令与标志（Linux 取值） */
#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 04000
#endif

/* MinGW 完全没有 fcntl() 这个函数（它用 _setmode / ioctlsocket 代替），
 * 因此这里补一个声明，仅用于让主机端能对工程代码做类型检查。 */
#ifdef __cplusplus
extern "C" {
#endif
int fcntl(int fd, int cmd, ...);
#ifdef __cplusplus
}
#endif

/* MSG_DONTWAIT 由 devstubs/sys/socket.h 提供；此处兜底 */
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0x40
#endif

#endif /* ACCESSTERMINAL_HOST_COMPAT_H */

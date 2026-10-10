/*
 * devstubs/sys/ioctl.h — 主机端语法检查用的 ioctl 桩。
 */
#ifndef ACCESSTERMINAL_DEVSTUB_SYS_IOCTL_H
#define ACCESSTERMINAL_DEVSTUB_SYS_IOCTL_H

#ifdef __cplusplus
extern "C" {
#endif

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif /* ACCESSTERMINAL_DEVSTUB_SYS_IOCTL_H */

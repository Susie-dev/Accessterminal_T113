/*
 * devstubs/arpa/inet.h — 主机端语法检查用的地址转换桩（仅声明）。
 */
#ifndef ACCESSTERMINAL_DEVSTUB_ARPA_INET_H
#define ACCESSTERMINAL_DEVSTUB_ARPA_INET_H

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

int inet_pton(int af, const char* src, void* dst);
const char* inet_ntop(int af, const void* src, char* dst, socklen_t size);

#ifdef __cplusplus
}
#endif

#endif /* ACCESSTERMINAL_DEVSTUB_ARPA_INET_H */

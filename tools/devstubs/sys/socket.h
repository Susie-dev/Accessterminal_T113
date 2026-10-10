/*
 * devstubs/sys/socket.h — 主机端语法检查用的 POSIX 套接字桩。
 *
 * 仅复刻工程实际用到的子集与签名，供 Windows/MinGW 上做类型检查。
 * 不是实现；交叉编译必须使用目标平台的真实头文件。
 */
#ifndef ACCESSTERMINAL_DEVSTUB_SYS_SOCKET_H
#define ACCESSTERMINAL_DEVSTUB_SYS_SOCKET_H

#include <stddef.h>
#include <sys/types.h>
#include <_mingw.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned short sa_family_t;
typedef int socklen_t;

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

struct sockaddr_in {
    sa_family_t sin_family;
    unsigned short sin_port;
    struct in_addr {
        unsigned int s_addr;
    } sin_addr;
    char sin_zero[8];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    char __pad[126];
};

#ifndef AF_INET
#define AF_INET 2
#endif
#ifndef SOCK_DGRAM
#define SOCK_DGRAM 2
#endif
#ifndef SOCK_STREAM
#define SOCK_STREAM 1
#endif

/* 与 Linux 一致：host -> network 字节序 */
unsigned short htons(unsigned short hostshort);
unsigned short ntohs(unsigned short netshort);
unsigned int htonl(unsigned int hostlong);
unsigned int ntohl(unsigned int netlong);

#define INADDR_ANY 0U

int socket(int domain, int type, int protocol);
int bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
int connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
ssize_t sendto(int sockfd, const void* buf, size_t len, int flags,
               const struct sockaddr* dest_addr, int addrlen);
ssize_t recvfrom(int sockfd, void* buf, size_t len, int flags,
                 struct sockaddr* src_addr, int* addrlen);
int setsockopt(int sockfd, int level, int optname, const void* optval,
               int optlen);

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0x40
#endif

#ifdef __cplusplus
}
#endif

#endif /* ACCESSTERMINAL_DEVSTUB_SYS_SOCKET_H */

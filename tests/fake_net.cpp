/*
 * fake_net.cpp — 假套接字层实现。
 *
 * 只提供 AudioSenderThread 需要的符号。签名一律取自 devstubs 的桩头文件
 * （<sys/socket.h> / <arpa/inet.h>），因此与工程代码的调用方式编译期一致。
 *
 * 特别注意：MinGW 自带 winsock2 也有 socket/sendto，但工程在 Linux 上用的是
 * POSIX 版本，且 devstubs 已经定义好了类型（socklen_t 为 int 等）。这里必须
 * 与 devstubs 的头文件保持一致，否则会因为重定义而编不过。
 */
#include "fake_net.h"

#include <QMutex>
#include <QMutexLocker>

#include <sys/socket.h>
#include <arpa/inet.h>

#include <cstdio>
#include <cstring>

namespace {
QMutex g_mutex;
bool g_sendFail = false;
int g_sendtoCount = 0;
std::vector<fakenet::Datagram> g_sent;

/// 把 a.b.c.d 打包成网络序。注意 devstubs 里 in_addr 是 sockaddr_in 的
/// 嵌套类型，不能单独取名。
unsigned int parseIpv4(const char* dotted)
{
    unsigned int b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    if (dotted && std::sscanf(dotted, "%u.%u.%u.%u", &b0, &b1, &b2, &b3) == 4) {
        return htonl((b0 << 24) | (b1 << 16) | (b2 << 8) | b3);
    }
    return 0;
}
} // namespace

namespace fakenet {

void reset()
{
    QMutexLocker l(&g_mutex);
    g_sendFail = false;
    g_sendtoCount = 0;
    g_sent.clear();
}

void setSendFail(bool fail) { QMutexLocker l(&g_mutex); g_sendFail = fail; }

std::vector<Datagram> sent()
{
    QMutexLocker l(&g_mutex);
    return g_sent;
}

int sendtoCount() { QMutexLocker l(&g_mutex); return g_sendtoCount; }

} // namespace fakenet

extern "C" {

// 字节序转换由 devstubs/stub_impl.c 提供；本测试不链接它（两边都定义
// sendto/inet_pton），因此这里补上等价实现。语义与 POSIX 一致。
unsigned short htons(unsigned short hostshort)
{
    return static_cast<unsigned short>(((hostshort & 0x00FFU) << 8) |
                                       ((hostshort & 0xFF00U) >> 8));
}

unsigned short ntohs(unsigned short netshort) { return htons(netshort); }

unsigned int htonl(unsigned int hostlong)
{
    return ((hostlong & 0x000000FFU) << 24) |
           ((hostlong & 0x0000FF00U) << 8)  |
           ((hostlong & 0x00FF0000U) >> 8)  |
           ((hostlong & 0xFF000000U) >> 24);
}

unsigned int ntohl(unsigned int netlong) { return htonl(netlong); }

// inet_pton 必须成功，否则 AudioSenderThread 会在建连阶段直接 return，
// 根本走不到发送路径（devstubs 的桩永远返回 0，正是这个原因让该测试此前
// 不可能写得出来）。
int inet_pton(int af, const char* src, void* dst)
{
    if (af != AF_INET || !src || !dst) {
        return 0;
    }
    const unsigned int addr = parseIpv4(src);
    std::memcpy(dst, &addr, sizeof(addr));
    return addr == 0 ? 0 : 1;
}

const char* inet_ntop(int af, const void* src, char* dst, socklen_t size)
{
    (void)af; (void)src; (void)size;
    return dst;
}

ssize_t sendto(int sockfd, const void* buf, size_t len, int flags,
               const struct sockaddr* dest_addr, socklen_t addrlen)
{
    (void)flags; (void)addrlen;
    QMutexLocker l(&g_mutex);
    ++g_sendtoCount;
    if (g_sendFail) {
        return -1;
    }
    fakenet::Datagram d;
    d.fd = sockfd;
    d.destPort = 0;
    d.destAddr = 0;
    if (dest_addr) {
        const struct sockaddr_in* in =
            reinterpret_cast<const struct sockaddr_in*>(dest_addr);
        d.destAddr = in->sin_addr.s_addr;
        // sin_port 是网络序，还原成主机序便于断言。
        d.destPort = ntohs(in->sin_port);
    }
    const char* bytes = static_cast<const char*>(buf);
    d.payload.assign(bytes, bytes + len);
    g_sent.push_back(d);
    return static_cast<ssize_t>(len);
}

} // extern "C"

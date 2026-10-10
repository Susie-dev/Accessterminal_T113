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
#include <sys/select.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace {
QMutex g_mutex;
bool g_sendFail = false;
int g_sendtoCount = 0;
std::vector<fakenet::Datagram> g_sent;

// 接收方向
std::vector<std::vector<char> > g_incoming;   // 待收队列（FIFO）
std::vector<fakenet::Datagram> g_received;    // 已被取走
int g_recvfromCount = 0;
int g_selectCount = 0;
int g_recvFailFirst = 0;
int g_recvFailErrno = 0;

// 视频推送方向（CameraPushThread 自己 socket()）
int g_socketCount = 0;
bool g_socketFail = false;

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
    g_incoming.clear();
    g_received.clear();
    g_recvfromCount = 0;
    g_selectCount = 0;
    g_recvFailFirst = 0;
    g_recvFailErrno = 0;
    g_socketCount = 0;
    g_socketFail = false;
}

void pushIncoming(const std::vector<char>& payload)
{
    QMutexLocker l(&g_mutex);
    g_incoming.push_back(payload);
}

void pushIncomingFilled(int n, int len, int seed)
{
    QMutexLocker l(&g_mutex);
    for (int k = 0; k < n; ++k) {
        std::vector<char> v(static_cast<size_t>(len));
        for (int i = 0; i < len; ++i) {
            v[static_cast<size_t>(i)] = static_cast<char>((seed + k + (i % 7)) & 0xFF);
        }
        g_incoming.push_back(v);
    }
}

void setRecvFailFirst(int n, int errnoValue)
{
    QMutexLocker l(&g_mutex);
    g_recvFailFirst = n;
    g_recvFailErrno = errnoValue;
}

void clearIncoming() { QMutexLocker l(&g_mutex); g_incoming.clear(); }

std::vector<Datagram> received()
{
    QMutexLocker l(&g_mutex);
    return g_received;
}

int recvfromCount() { QMutexLocker l(&g_mutex); return g_recvfromCount; }
int selectCount() { QMutexLocker l(&g_mutex); return g_selectCount; }

void setSendFail(bool fail) { QMutexLocker l(&g_mutex); g_sendFail = fail; }

// ---- 视频推送方向 ----

void setSocketFail(bool fail) { QMutexLocker l(&g_mutex); g_socketFail = fail; }
int socketCount() { QMutexLocker l(&g_mutex); return g_socketCount; }

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

ssize_t recvfrom(int sockfd, void* buf, size_t len, int flags,
                 struct sockaddr* src_addr, socklen_t* addrlen)
{
    (void)flags; (void)src_addr; (void)addrlen;
    QMutexLocker l(&g_mutex);
    ++g_recvfromCount;

    if (g_recvFailFirst > 0) {
        --g_recvFailFirst;
        errno = g_recvFailErrno;
        return -1;
    }
    if (g_incoming.empty()) {
        // 真实非阻塞 socket 的语义：暂时无数据。
        errno = EAGAIN;
        return -1;
    }
    std::vector<char> payload = g_incoming.front();
    g_incoming.erase(g_incoming.begin());

    fakenet::Datagram d;
    d.fd = sockfd;
    d.destPort = 0;
    d.destAddr = 0;
    d.payload = payload;
    g_received.push_back(d);

    const size_t n = payload.size() < len ? payload.size() : len;
    if (buf && n > 0) {
        std::memcpy(buf, &payload[0], n);
    }
    return static_cast<ssize_t>(n);
}

/**
 * 假 select()：只在"队列里有待收数据"时报告可读，否则超时返回 0。
 *
 * 这正是让 AudioReceiverThread 的两段循环都能真正跑起来的关键 ——
 * devstubs 的 select() 恒返回 0，于是两个循环体一次都不执行。
 */
int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
           struct timeval* timeout)
{
    (void)nfds; (void)writefds; (void)exceptfds; (void)timeout;
    QMutexLocker l(&g_mutex);
    ++g_selectCount;
    if (g_incoming.empty()) {
        if (readfds) {
            FD_ZERO(readfds);
        }
        return 0; // 超时
    }
    if (readfds) {
        FD_ZERO(readfds);
        for (int fd = 0; fd < nfds; ++fd) {
            FD_SET(fd, readfds);
        }
    }
    return 1;
}

int fcntl(int fd, int cmd, ...)
{
    (void)fd; (void)cmd;
    // 接收线程用 F_GETFL 取标志、F_SETFL 置 O_NONBLOCK。
    // 假的 fd 无需真正改状态，返回 0 即可。
    return 0;
}

/**
 * fake socket()：返回递增的假 fd。
 *
 * CameraPushThread（UDP 23456）自己创建 socket，而 MinGW 没有 socket()，
 * 因此这里必须提供。用递增 fd 是为了让测试能断言"每个线程各用各的 fd"。
 * 负值注入由 setSocketFail 控制，用于验证 socket 创建失败的兜底分支。
 */
int socket(int domain, int type, int protocol)
{
    (void)domain; (void)type; (void)protocol;
    QMutexLocker l(&g_mutex);
    ++g_socketCount;
    if (g_socketFail) {
        errno = EMFILE;
        return -1;
    }
    return 100 + g_socketCount; // 避开 0/1/2，避免与 stdio 混淆
}

} // extern "C"

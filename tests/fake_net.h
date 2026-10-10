/*
 * fake_net.h — 供单元测试使用的假 POSIX 套接字层（只覆盖 AudioSenderThread 用到的子集）。
 *
 * 为什么需要它：AudioSenderThread 把 UDP 12345 的上行音频写进一个**外部传入**
 * 的 socket fd。这条通路的字节格式是协议硬约束（裸 PCM、S16_LE/16000/单声道、
 * 每包 320 帧 640 字节、无包头），但 tools/devstubs 里的 sendto() 只是
 * "返回 len" 的桩，**不记录也不校验任何载荷**，因此协议格式一直没被真正验证过。
 *
 * 这里提供一份可记录的实现：把每次 sendto 的目标地址与载荷原样存下来，
 * 使测试能逐字节断言"线上到底是什么"。
 *
 * 它只实现本测试需要的符号，不与 devstubs/stub_impl.c 同时链接。
 */
#ifndef ACCESSTERMINAL_TESTS_FAKE_NET_H
#define ACCESSTERMINAL_TESTS_FAKE_NET_H

#include <cstddef>
#include <vector>

namespace fakenet {

/// 一次 sendto 的完整记录。
struct Datagram {
    // 默认初始化：cppcheck 的 uninitMemberVarNoCtor 会告警，且未初始化的字段
    // 在"某次 sendto 没被记录/记录不完整"时会给出难以察觉的假数据。
    int fd = -1;
    unsigned short destPort = 0;   ///< 主机序（已从网络序还原）
    unsigned int destAddr = 0;     ///< 网络序原样保存
    std::vector<char> payload;
};

void reset();

/// 让 sendto 返回失败（模拟对端不可达 / fd 已关闭）。
void setSendFail(bool fail);

/// 记录到的数据报，按发送顺序。
std::vector<Datagram> sent();

int sendtoCount();

// ---- 接收方向（AudioReceiverThread 用）----

/// 投递一个待接收的数据报（内容即为 UDP 载荷）。
void pushIncoming(const std::vector<char>& payload);

/// 投递 n 个由 seed 递增填充的载荷，每个长度 len。
void pushIncomingFilled(int n, int len, int seed);

/// 令接下来 n 次 recvfrom 失败，并把 errno 设为 errnoValue。
/// errnoValue 用 11=EAGAIN / 4=EINTR / 5=EIO 等。
void setRecvFailFirst(int n, int errnoValue);

/// 清空待收队列。
void clearIncoming();

/// 已被 recvfrom 取走的数据报。
std::vector<Datagram> received();

int recvfromCount();
int selectCount();

// ---- 视频推送方向（CameraPushThread / UDP 23456）----

/// 让下一批 socket() 失败（模拟 fd 耗尽）。
void setSocketFail(bool fail);

/// socket() 被调用的次数。
int socketCount();

} // namespace fakenet

#endif // ACCESSTERMINAL_TESTS_FAKE_NET_H

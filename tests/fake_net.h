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
    int fd;
    unsigned short destPort;   ///< 主机序（已从网络序还原）
    unsigned int destAddr;     ///< 网络序原样保存
    std::vector<char> payload;
};

void reset();

/// 让 sendto 返回失败（模拟对端不可达 / fd 已关闭）。
void setSendFail(bool fail);

/// 记录到的数据报，按发送顺序。
std::vector<Datagram> sent();

int sendtoCount();

} // namespace fakenet

#endif // ACCESSTERMINAL_TESTS_FAKE_NET_H

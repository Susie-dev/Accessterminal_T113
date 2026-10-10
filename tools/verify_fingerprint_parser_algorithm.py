"""
独立验证 FingerprintFrameParser 的算法（不编译 C++）。

这是把 src/hal/fingerprint_frame_parser.h 的逻辑逐条照搬成 Python，
再用与 tests/test_fingerprint_parser.cpp 相同的用例（外加随机模糊测试）
跑一遍。目的是在无法编译的环境下，仍能确认算法本身正确、且确实解决了
原实现的丢包问题。

注意：此处也实现了「原实现」的对照版本，用于证明旧代码在多帧场景下
真的会丢数据——即证明修的是真缺陷，不是臆想。
"""
import random

HEADER = 9
CKSUM = 2
MAXBUF = 4096


def make_frame(tag, payload):
    pkt = bytearray([0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, tag])
    plen = len(payload) + 2
    pkt += bytes([(plen >> 8) & 0xFF, plen & 0xFF])
    pkt += payload
    s = 0
    for i in range(6, len(pkt)):
        s += pkt[i]
    pkt += bytes([(s >> 8) & 0xFF, s & 0xFF])
    return bytes(pkt)


def make_ack(confirm, p1=0, p2=0):
    return make_frame(0x07, bytes([0x00, confirm, p1, p2]))


# --------------------------------------------------------------------------
# 被验证的实现：与 fingerprint_frame_parser.h 的 takeOne/feed 一一对应
# --------------------------------------------------------------------------
class Parser:
    def __init__(self):
        self.buf = b""

    def reset(self):
        self.buf = b""

    def find_header(self):
        for i in range(len(self.buf) - 1):
            if self.buf[i] == 0xEF and self.buf[i + 1] == 0x01:
                return i
        return -1

    def take_one(self):
        while True:
            start = self.find_header()
            if start < 0:
                if len(self.buf) > 1:
                    self.buf = self.buf[-1:]
                return None
            if start > 0:
                self.buf = self.buf[start:]
            if len(self.buf) < HEADER:
                return None
            plen = (self.buf[7] << 8) | self.buf[8]
            total = HEADER + plen
            if plen < CKSUM or total > MAXBUF:
                self.buf = self.buf[2:]
                continue
            if len(self.buf) < total:
                return None
            out = self.buf[:total]
            self.buf = self.buf[total:]
            return out

    def feed(self, data):
        if data:
            self.buf += data
        frames = []
        while True:
            f = self.take_one()
            if f is None:
                break
            frames.append(f)
        if len(self.buf) > MAXBUF:
            self.buf = self.buf[-1:]
        return frames


# --------------------------------------------------------------------------
# 对照实现：原 readPacket() 的行为（局部 buffer + 只取第一帧）
# --------------------------------------------------------------------------
def legacy_read(stream):
    """返回原实现在‘一次 readAll 拿到 stream 全部数据’下读到的帧与丢弃量。"""
    buf = b""
    buf += stream
    if len(buf) >= 12:
        plen = (buf[7] << 8) | buf[8]
        total = 9 + plen
        if len(buf) >= total:
            return buf[:total], len(buf) - total
    return None, len(buf)


results = []


def check(name, cond, detail=""):
    results.append((name, cond, detail))
    print(("  PASS  " if cond else "  FAIL  ") + name + (("  " + detail) if detail else ""))


print("=" * 76)
print("1) 基础正确性")
p = Parser()
f = make_ack(0x00)
got = p.feed(f)
check("单帧解析", got == [f])
check("无残留", p.buf == b"")

print("\n2) 核心缺陷验证：一次收到多帧")
f1, f2, f3 = make_ack(0x00, 1), make_ack(0x00, 2), make_ack(0x00, 3)
p = Parser()
got = p.feed(f1 + f2 + f3)
check("新实现产出全部 3 帧", got == [f1, f2, f3], f"实得 {len(got)} 帧")

legacy_frame, legacy_dropped = legacy_read(f1 + f2 + f3)
check("【对照】原实现确实丢弃了后面的帧",
      legacy_dropped > 0,
      f"丢弃 {legacy_dropped} 字节（= {len(f2) + len(f3)} 字节的 2 帧）")
check("【对照】丢弃量恰为 2 帧", legacy_dropped == len(f2) + len(f3))

print("\n3) 核心缺陷验证：半包 + 超时")
f = make_ack(0x00, 0xAA)
p = Parser()
check("半包不产出帧", p.feed(f[:len(f) // 2]) == [])
check("半包被保留", len(p.buf) > 0)
got = p.feed(f[len(f) // 2:])
check("补齐后产出完整帧", got == [f])

legacy_frame2, _ = legacy_read(f[:len(f) // 2])
check("【对照】原实现超时返回空（半包丢失）", legacy_frame2 is None)

print("\n4) 逐字节到达")
p = Parser()
allf = []
for i in range(len(f)):
    allf += p.feed(f[i:i + 1])
check("逐字节也能拼出完整帧", allf == [f])

print("\n5) 半包 + 后续多帧")
p = Parser()
p.feed(f1[:3])
got = p.feed(f1[3:] + f2 + f3)
check("半包补齐并连续取出 3 帧", got == [f1, f2, f3], f"实得 {len(got)} 帧")

print("\n6) 包头同步 / 噪声")
p = Parser()
noise = bytes.fromhex("0011223344")
check("跳过前导噪声", p.feed(noise + f) == [f])

p = Parser()
got = p.feed(f1 + bytes.fromhex("DEADBEEF") + f2)
check("跳过帧间噪声", got == [f1, f2], f"实得 {len(got)} 帧")

p = Parser()
bogus = bytes([0xEF, 0x01, 0xFF, 0xFF])
check("伪造包头后可恢复", p.feed(bogus + f) == [f])

print("\n7) 缓冲区上限")
p = Parser()
p.feed(b"\x5a" * (64 * 1024))
check("噪声不导致无限增长", len(p.buf) <= MAXBUF, f"残留 {len(p.buf)} 字节")
check("之后仍可定帧", p.feed(f) == [f])

print("\n8) reset")
p = Parser()
p.feed(make_ack(0x00)[:4])
p.reset()
check("reset 清空半包", p.buf == b"")
check("reset 后不串包", p.feed(b"\x00") == [])

print("\n9) 校验和")
fp = make_ack(0x00, 0x12, 0x34)
body = fp[:-2]
s = 0
for i in range(6, len(body)):
    s += body[i]
actual = (fp[-2] << 8) | fp[-1]
check("校验和自下标 6 起算", s == actual)

print("\n10) 随机模糊测试：任意切分点，数据必须无损")
random.seed(20240607)
fuzz_ok = True
fuzz_detail = ""
for trial in range(3000):
    nframes = random.randint(1, 5)
    frames = [make_ack(random.randint(0, 3),
                       random.randint(0, 255),
                       random.randint(0, 255)) for _ in range(nframes)]
    stream = b"".join(frames)
    # 随机切分成若干块
    chunks = []
    i = 0
    while i < len(stream):
        k = random.randint(1, max(1, len(stream) - i))
        chunks.append(stream[i:i + k])
        i += k
    p = Parser()
    out = []
    for c in chunks:
        out += p.feed(c)
    if out != frames:
        fuzz_ok = False
        fuzz_detail = f"第 {trial} 次: 期望 {len(frames)} 帧, 实得 {len(out)} 帧"
        break
check("3000 组随机切分均无损还原", fuzz_ok, fuzz_detail)

print("\n11) 随机模糊测试：混入随机噪声仍能还原所有真实帧")
random.seed(999)
fuzz2_ok = True
fuzz2_detail = ""
for trial in range(2000):
    frames = [make_ack(random.randint(0, 3)) for _ in range(random.randint(1, 4))]
    parts = []
    for fr in frames:
        # 每帧前塞入 0~4 字节随机噪声（避开 0xEF 开头以免本身就是伪包头）
        noise = bytes(random.choice([0x00, 0x11, 0x22, 0x5A]) for _ in range(random.randint(0, 4)))
        parts.append(noise + fr)
    stream = b"".join(parts)
    p = Parser()
    out = p.feed(stream)
    if out != frames:
        fuzz2_ok = False
        fuzz2_detail = f"第 {trial} 次: 期望 {len(frames)}, 实得 {len(out)}"
        break
check("2000 组含噪声数据均还原真实帧", fuzz2_ok, fuzz2_detail)

print("\n" + "=" * 76)
failed = [r for r in results if not r[1]]
if failed:
    print(f"结论: {len(failed)} 项失败")
    for n, _, d in failed:
        print("  X " + n + ("  " + d if d else ""))
    raise SystemExit(1)
print(f"结论: 全部 {len(results)} 项检查通过")

"""
证明 autoIdentify() 的越界是真实且可达的。

思路：把 fingerprint_frame_parser.h 的定帧算法与 autoIdentify() 的取值逻辑
都照搬成 Python，构造一个**最短的合法帧**（11 字节），走一遍控制路径。
控制实现（control）复刻修复前的判断，只检查 isEmpty()。
"""
import struct

HEADER = 9
CHECKSUM = 2
MAXBUF = 4096


def make_frame(payload: bytes) -> bytes:
    """按协议构造帧：EF 01 | FF FF FF FF | tag | len(be,=len(payload)+2) | payload | cksum(be)"""
    body = bytes([0x07]) + struct.pack(">H", len(payload) + CHECKSUM) + payload
    cksum = sum(body[0:]) & 0xFFFF  # 自下标 6 起（即 body 的首字节）
    return bytes([0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF]) + body + struct.pack(">H", cksum)


def parse(buf: bytes):
    """复刻 FingerprintFrameParser：只保证长度字段自洽。"""
    frames = []
    b = bytearray(buf)
    while True:
        # findHeader
        start = -1
        for i in range(len(b) - 1):
            if b[i] == 0xEF and b[i + 1] == 0x01:
                start = i
                break
        if start < 0:
            break
        if start > 0:
            del b[:start]
        if len(b) < HEADER:
            break
        payload_and_ck = (b[7] << 8) | b[8]
        total = HEADER + payload_and_ck
        if payload_and_ck < CHECKSUM or total > MAXBUF:
            del b[:2]
            continue
        if len(b) < total:
            break
        frames.append(bytes(b[:total]))
        del b[:total]
    return frames


def legacy_auto_identify(resp: bytes):
    """修复前的逻辑：只检查 isEmpty()，然后读到 resp[14] -> 越界。"""
    if len(resp) == 0:
        return "无应答", None
    # resp[6] 是 tag
    tag = resp[6]
    if tag != 0x07:
        return "应答包标识错误", None
    # 下面这几行就是缺陷：索引 9..14
    try:
        confirm = resp[9]
        param = resp[10]
        _id = (resp[11] << 8) | resp[12]
        _score = (resp[13] << 8) | resp[14]
    except IndexError as e:
        return "IndexError: {}".format(e), None
    return "ok", (confirm, param, _id, _score)


def fixed_auto_identify(resp: bytes):
    """修复后：先判 size() < 15。"""
    if len(resp) == 0:
        return "无应答", None
    if len(resp) < 15:
        return "应答过短({}字节)".format(len(resp)), None
    tag = resp[6]
    if tag != 0x07:
        return "应答包标识错误", None
    confirm = resp[9]
    param = resp[10]
    _id = (resp[11] << 8) | resp[12]
    _score = (resp[13] << 8) | resp[14]
    return "ok", (confirm, param, _id, _score)


print("=" * 68)
print("构造各种合法帧，看修复前后行为差异")
print("=" * 68)

cases = [
    ("空 payload（最短合法帧 11 字节）", b""),
    ("1 字节 payload（12 字节）", b"\x00"),
    ("2 字节 payload（13 字节）", b"\x00\x00"),
    ("3 字节 payload（14 字节）", b"\x00\x00\x00"),
    ("5 字节 payload（16 字节，刚好够读 score）", b"\x00\x05\x00\x01\x00\x02"),
    ("正常识别应答（含 id/score）", b"\x00\x05\x00\x2A\x01\x2C"),
]

bad = 0
for name, payload in cases:
    frame = make_frame(payload)
    frames = parse(frame)
    assert len(frames) == 1, "定帧失败: {}".format(name)
    got = frames[0]
    st_old, _ = legacy_auto_identify(got)
    st_new, _ = fixed_auto_identify(got)
    flag = ""
    if st_old.startswith("IndexError"):
        flag = "  <-- 旧实现越界！"
        bad += 1
    print("  {:<42} len={:<3} 旧={:<22} 新={}{}".format(
        name, len(got), st_old[:22], st_new[:22], flag))

print()
print("=" * 68)
print("结论")
print("=" * 68)
print("  最短合法帧 = 9(包头/地址/标识/长度) + 2(校验和) = 11 字节，")
print("  它是解析器**会正常切出并返回**的合法帧。")
print("  修复前有 {} 种输入会让 autoIdentify() 越界读取 resp[11..14]。".format(bad))
print("  修复后全部被 '应答过短' 挡住，不再越界。")
assert bad >= 4, "预期至少 4 种越界输入"
print("\n  [控制实现证明缺陷真实可达]")

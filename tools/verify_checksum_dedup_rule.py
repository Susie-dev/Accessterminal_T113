# -*- coding: utf-8 -*-
"""两条"校验和唯一实现"规则的故障注入自检（跨平台，纯 Python）。

覆盖：
  - check_fingerprint_checksum_logic_is_not_duplicated
  - check_fake_fingerprint_builds_same_bytes（其中校验和辅助函数体那一半）

为什么需要它（第 31 轮）
------------------------
这两条规则都是本轮**新写/加强**的，而按本工程的取向（README 约定）：
**不失败的回归测试没有价值** —— 一条无法被故障注入打红的规则，
既不能证明它抓得住问题，也无法在将来被人改坏时发出声音。

为什么不用 verify_fingerprint_wire_test.py 兼做这件事
-----------------------------------------------------
那个夹具是**运行时**故障注入：它要 msys64 工具链、Windows 的 `%TEMP%`、
以及写死的 `C:\\Users\\user\\...` 路径。它证明的是"变异会让单元测试变红"。
本脚本是**静态**故障注入，直接在合成仓库上调用两条规则函数，
不依赖任何编译器或平台 —— 这样它在 Linux/CI 上也能跑，能覆盖"规则本身
逻辑写错"这一层（例如正则只认 `pkt[i]` 不认 `pkt.at(i)`）。

判据（与 verify_fp_ack_tag.py 同一套路）：
  - 合成一个"正确"的仓库 -> 两条规则都**不**报 fail（真阴性）；
  - 逐个注入退化 -> 必须报 fail（真阳性）。
"""
import importlib.util
import io
import os
import shutil
import sys
import tempfile

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

_HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.dirname(_HERE)
VERIFY = os.path.join(_HERE, "verify.py")

FAILED = 0
TOTAL = 0

# ---------------------------------------------------------------------------
# 合成的"生产"指纹实现。
#
# 刻意用与真实仓库**不同**的变量名与书写风格（pkt.at(i) / 名字不叫 pkt），
# 用来检验规则的正则不是只认真实文件里那一种写法 —— 否则规则就是"对着样本
# 描出来的"，换个写法立刻失效。这是第 51 条"规则匹配别用子串"的延伸。
# ---------------------------------------------------------------------------
GOOD_PROD = """#include "hal/fingerprint_hailin.h"

QByteArray HailinFingerprint::buildPacket(const QByteArray& payload)
{
    QByteArray out;
    out.append((char)0xEF);
    out.append((char)0x01);
    out.append((char)0xFF);
    out.append((char)0xFF);
    out.append((char)0xFF);
    out.append((char)0xFF);
    out.append(PKT_CMD);
    quint16 pktLen = payload.size() + 2;
    out.append((char)(pktLen >> 8));
    out.append((char)(pktLen & 0xFF));
    out.append(payload);
    quint16 sum = calcChecksum(out);
    out.append((char)(sum >> 8));
    out.append((char)(sum & 0xFF));
    return out;
}

quint16 HailinFingerprint::calcChecksum(const QByteArray& buf)
{
    quint16 total = 0;
    for (int i = 6; i < buf.size(); ++i) total += (quint8)buf.at(i);
    return total;
}
"""

# 与 GOOD_PROD 同构的替身：辅助函数只是换了名字。
GOOD_FAKE = """#include "hal/fingerprint_hailin.h"

namespace {

quint16 checksumOf(const QByteArray& buf)
{
    quint16 total = 0;
    for (int i = 6; i < buf.size(); ++i) {
        total = static_cast<quint16>(total + static_cast<quint8>(buf.at(i)));
    }
    return total;
}

QByteArray buildFrame(const QByteArray& payload)
{
    QByteArray out;
    out.append((char)0xEF);
    out.append((char)0x01);
    out.append((char)0xFF);
    out.append((char)0xFF);
    out.append((char)0xFF);
    out.append((char)0xFF);
    out.append((char)0x01);
    const quint16 pktLen = static_cast<quint16>(payload.size() + 2);
    out.append((char)(pktLen >> 8));
    out.append((char)(pktLen & 0xFF));
    out.append(payload);
    const quint16 sum = checksumOf(out);
    out.append((char)(sum >> 8));
    out.append((char)(sum & 0xFF));
    return out;
}

} // namespace
"""

GOOD_HEADER = """#include "hal/fingerprint_hailin.h"

class HailinFingerprint {
public:
    QByteArray buildPacket(const QByteArray& payload);
private:
    quint16 calcChecksum(const QByteArray& buf);
};
"""


def load():
    spec = importlib.util.spec_from_file_location("v", VERIFY)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def make_repo(prod=GOOD_PROD, fake=GOOD_FAKE):
    """建一个最小仓库：src/hal + tests，内容由参数决定。"""
    root = tempfile.mkdtemp(prefix="cksum_")
    os.makedirs(os.path.join(root, "src", "hal"))
    os.makedirs(os.path.join(root, "tests"))
    with io.open(os.path.join(root, "src", "hal", "fingerprint_hailin.cpp"),
                 "w", encoding="utf-8", newline="\n") as f:
        f.write(prod)
    with io.open(os.path.join(root, "src", "hal", "fingerprint_hailin.h"),
                 "w", encoding="utf-8", newline="\n") as f:
        f.write(GOOD_HEADER)
    with io.open(os.path.join(root, "tests", "fake_fingerprint_device.cpp"),
                 "w", encoding="utf-8", newline="\n") as f:
        f.write(fake)
    return root


def run_rules(root):
    """跑两条规则，返回 (dedup错误数, fakewire错误数)。"""
    m = load()
    m.FAIL.clear()
    m.WARN.clear()
    src = os.path.join(root, "src")
    m.check_fingerprint_checksum_logic_is_not_duplicated(root, src)
    dedup = list(m.FAIL)
    m.FAIL.clear()
    m.check_fake_fingerprint_builds_same_bytes(root, src)
    fakewire = list(m.FAIL)
    return dedup, fakewire


def case(name, prod, fake, want):
    """want: 'ok'（两条都不报）| 'dedup' | 'fakewire' | 'both'。"""
    global FAILED, TOTAL
    TOTAL += 1
    root = make_repo(prod, fake)
    try:
        dedup, fakewire = run_rules(root)
        got = []
        if dedup:
            got.append("dedup")
        if fakewire:
            got.append("fakewire")
        if want == "ok":
            ok = not got
        elif want == "both":
            ok = "dedup" in got and "fakewire" in got
        else:
            ok = got == [want]
        print("  [%s] %-46s (判据=%s, 期望=%s)"
              % ("PASS" if ok else "FAIL", name, ",".join(got) or "无", want))
        if not ok:
            FAILED += 1
            for x in (dedup + fakewire)[:1]:
                print("        -> " + x[:160])
    finally:
        shutil.rmtree(root, ignore_errors=True)


print("== 校验和唯一实现：两条规则的故障注入自检 ==")

# ---- 0) 真阴性：合成仓库本身必须是绿的，否则后面全是噪声 ----
case("基线：唯一实现 + 替身同构", GOOD_PROD, GOOD_FAKE, "ok")

# ---- 1) 重复实现回归：buildPacket 又自己内联了一份循环 ----
DUP_PROD = GOOD_PROD.replace(
    "    quint16 sum = calcChecksum(out);",
    "    quint16 sum = 0;\n"
    "    for (int i = 6; i < out.size(); ++i) sum += (quint8)out[i];")
# 两条规则都会响：dedup 抓到"出现 2 次"，fake-wire 抓到"生产多了个 LOOP
# 操作"。两者都是真阳性，故期望 both。
case("重复实现回归（buildPacket 又内联一份）", DUP_PROD, GOOD_FAKE, "both")

# ---- 2) 死代码回归：循环只剩一份，但没人调用 calcChecksum ----
DEAD_PROD = GOOD_PROD.replace(
    "    quint16 sum = calcChecksum(out);", "    quint16 sum = 0;")
# 同理：dedup 抓到"零调用者"，fake-wire 抓到"生产写成 LEN=0、替身是
# SUM=HELPER(pkt)"。都是真阳性。
case("死代码回归（calcChecksum 零调用者）", DEAD_PROD, GOOD_FAKE, "both")

# ---- 3) 替身里多了一份循环：fake-wire 应抓到（buildFrame 操作序列变了）----
DUP_FAKE = GOOD_FAKE.replace(
    "    const quint16 sum = checksumOf(out);",
    "    quint16 sum = 0;\n"
    "    for (int i = 6; i < out.size(); ++i) sum += (quint8)out.at(i);")
case("替身 buildFrame 改用内联循环", GOOD_PROD, DUP_FAKE, "fakewire")

# ---- 4) 累加范围**两边同时**漂移（6 -> 0）：静态规则**抓不到**，如实记录 ----
# 这不是缺陷：两份实现仍然"一样"，静态一致性规则在定义上就无法区分
# "双方一起改错"。钉住这一点的是**运行时**契约 —— test_fingerprint_wire.cpp
# 用独立手写的 expectedFrame() 逐字节比对，故障注入实测：把唯一实现的起点
# 6 改成 0，18 个用例里 10 个变红。故这里期望 'ok' 并注明边界，
# 而不是假装静态规则能覆盖它（那会变成第 46 条批评的"把猜测写成契约"）。
RANGE_PROD = GOOD_PROD.replace("for (int i = 6;", "for (int i = 0;")
RANGE_FAKE = GOOD_FAKE.replace("for (int i = 6;", "for (int i = 0;")
case("两边同时把累加起点 6 改成 0（静态抓不到，靠运行时）",
     RANGE_PROD, RANGE_FAKE, "ok")

# ---- 5) 只有一边漂移（生产 6、替身 0）-> 同样必须被抓到 ----
RANGE_FAKE_ONE = GOOD_FAKE.replace("for (int i = 6;", "for (int i = 0;")
case("只有替身把起点 6 改成 0", GOOD_PROD, RANGE_FAKE_ONE, "fakewire")

# ---- 6) 辅助函数改名是**无害**的：不该被误报成漂移 ----
# 早先的规则把辅助函数名写死成 calcChecksum|checksumOf，于是替身改名
# 会被误报成"报文契约漂移"。现在按内容定位、按形态归一，改名应当无声通过。
# 这条用例正是防"规则过于脆弱、把无害改动当成缺陷"。
RENAME_FAKE = GOOD_FAKE.replace("checksumOf", "computeSum")
case("替身辅助函数改名 computeSum（无害，不应报）", GOOD_PROD,
     RENAME_FAKE, "ok")

print()
print("用例 %d 个，致命问题 %d 个" % (TOTAL, FAILED))
if FAILED:
    print("问题清单: 上面标 FAIL 的用例")
sys.exit(1 if FAILED else 0)

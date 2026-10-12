# -*- coding: utf-8 -*-
"""第 23 轮两条规则的故障注入自检（两仓库通用）。

check_declared_slot_is_reachable：警告级
check_live_fingerprint_enroll_sequence_is_pinned：失败级
"""
import importlib.util
import os
import shutil
import stat
import sys
import tempfile
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
VERIFY = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else \
    os.path.join(_HERE, "verify.py")
REPO = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.dirname(_HERE)
FAILED = 0
TOTAL = 0
SKIPPED = 0


def load():
    spec = importlib.util.spec_from_file_location("v", VERIFY)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def _force_rmtree(path):
    """删除目录树，遇到只读文件先改权限再删。

    为什么需要：仓库里有 deploy/vendor 下的只读资源（如 thd.tar），
    shutil.rmtree 在 Windows 上会因权限失败而**静默留下整个沙箱目录** ——
    本轮开发中因脚本崩溃累积了 22 个残留沙箱（每个都含完整仓库副本，
    数百 MB）。清理失败必须被重试且不可无声。
    """
    def onerr(func, p, exc):
        try:
            os.chmod(p, stat.S_IWRITE | stat.S_IREAD)
            func(p)
        except Exception:
            pass
    for _ in range(3):
        if not os.path.exists(path):
            return
        shutil.rmtree(path, onerror=onerr)
        if not os.path.exists(path):
            return
        time.sleep(0.3)
    if os.path.exists(path):
        print("  警告: 沙箱目录未能删除: " + path)


def sandbox(mutate=None):
    tmp = tempfile.mkdtemp(prefix="r23_")
    root = os.path.join(tmp, "repo")
    shutil.copytree(REPO, root, ignore=shutil.ignore_patterns(
        ".git", "build", "build-*"))
    if mutate:
        mutate(root)
    return tmp, root


def run(kind, root):
    m = load()
    m.FAIL.clear()
    m.WARN.clear()
    src = os.path.join(root, "src")
    if kind == "slot":
        m.check_declared_slot_is_reachable(root, src)
        return list(m.WARN)
    m.check_live_fingerprint_enroll_sequence_is_pinned(root, src)
    return list(m.FAIL)


def case(name, kind, mutate, expect_min, expect_exact=None, requires=None):
    global FAILED, TOTAL, SKIPPED
    if requires is not None and not requires:
        SKIPPED += 1
        print("  [SKIP] %-52s 本仓库无对应代码" % name)
        return
    TOTAL += 1
    tmp, root = sandbox(mutate)
    try:
        hits = run(kind, root)
        n = len(hits)
        ok = (n == expect_exact) if expect_exact is not None else (n >= expect_min)
        print("  [%s] %-52s 命中 %d (期望 %s)" % (
            "PASS" if ok else "FAIL", name, n,
            expect_exact if expect_exact is not None else ">=%d" % expect_min))
        if not ok:
            FAILED += 1
            for x in hits[:2]:
                print("        -> " + x[:120])
    finally:
        _force_rmtree(tmp)


def del_file(rel):
    def f(root):
        p = os.path.join(root, rel)
        if os.path.exists(p):
            os.remove(p)
    return f


def patch(rel, old, new, expect=1):
    """按字面替换；expect 表示期望出现次数（用于防止锚点不唯一）。"""
    def f(root):
        p = os.path.join(root, rel)
        s = open(p, encoding='utf-8', errors='replace').read()
        assert s.count(old) == expect, (rel, s.count(old), expect)
        open(p, 'w', encoding='utf-8', newline='\n').write(
            s.replace(old, new, expect if expect > 0 else 1))
    return f


FF = os.path.join("src", "ui", "form_finger.cpp")
FM = os.path.join("src", "managers", "fingerprint_manager.cpp")

# 本仓库是否具备对应代码。桌面端（室内机）完全没有指纹通路，依赖这些文件的
# 用例无从下手 —— 跳过并说明，而不是判失败（那是"测试自身的缺陷"）。
# 两个仓库共用同一个自检脚本，必须能容忍这种差异。
HAS_FP = os.path.exists(os.path.join(REPO, "src", "managers",
                                     "fingerprint_manager.cpp"))
HAS_FF = os.path.exists(os.path.join(REPO, "src", "ui", "form_finger.cpp"))

if not (HAS_FP or HAS_FF):
    print("提示: 本仓库没有指纹代码，两条规则均为 no-op，仅执行 no-op 校验。")

print("== check_declared_slot_is_reachable ==")
# 真阳性：门口机仓库本就有不可达槽
case("真实仓库（有不可达槽）", "slot", None, 1 if HAS_FP else 0,
     requires=None)
# 真阴性：把 startEnroll 变成被调用，则该项消失。
#
# 第 41 轮修正：期望值从 **2 改为 0**。
# 第 23 轮写这条时全仓库有 **3** 个不可达槽，让 startEnroll 可达后
# 还剩 2 个，所以当年写的是 2。此后 18 轮里另外两个槽陆续被处理掉
# （一个接了线、一个删了），基线降到 **1 条**；于是"少 1 条"的正确结果
# 变成 **0**，而旧期望 2 永远不可能再满足 —— 夹具表现为恒定的假红。
#
# 这里同时把"基线确实是 1 条、且那 1 条就是 startEnroll"钉死：
# 第一条用例只断言 >=1，无法区分"基线 3 条"和"基线 1 条"，
# 正是这个空档让过期期望藏了 18 轮。下面加一条精确基线的断言。
case("真实仓库：不可达槽基线恰为 1 条（即 startEnroll）", "slot", None,
     1, 1, requires=HAS_FP)
case("给 startEnroll 加上调用者 -> 归零", "slot",
     patch(FM, 'emit enrollResult(false, id, "请按下手指进行第1次采集");',
           'startEnroll(0);\n    emit enrollResult(false, id, "请按下手指进行第1次采集");'),
     0, 0, requires=HAS_FP)

print()
print("== check_live_fingerprint_enroll_sequence_is_pinned ==")
case("真实仓库（序列与 baseline 一致）", "seq", None, 0, 0)
# 变异 1：GET_IMAGE 的轮询超时 500 -> 999
case("轮询超时 500->999", "seq",
     patch(FF, 'readPacket(500)', 'readPacket(999)'), 1, 1, requires=HAS_FF)
# 变异 2：GET_IMAGE 字节 0x01 -> 0x77
# `payload.append((char)0x01); // CMD_GET_IMAGE` 在文件里出现**两次**
# （轮询探手指一处、doFingerCollect 采集前一处）。两处都改掉同样能证明
# 规则确实在盯这条链路上的字节；expect=2 顺带断言"我知道有两处"。
case("GET_IMAGE 字节 0x01->0x77（两处）", "seq",
     patch(FF, 'payload.append((char)0x01); // CMD_GET_IMAGE',
           'payload.append((char)0x77); // CMD_GET_IMAGE(变异)', expect=2),
     1, 1, requires=HAS_FF)
# 变异 3：STORE_CHAR 的 BufferID 1 -> 2
case("STORE_CHAR 的 BufferID 1->2", "seq",
     patch(FF, 'payload.append((char)1); // BufferID',
           'payload.append((char)2); // BufferID'),
     1, 1, requires=HAS_FF)

print()
print("结果: {}/{} 通过{}".format(
    TOTAL - FAILED, TOTAL,
    "（跳过 {} 项：本仓库无对应代码）".format(SKIPPED) if SKIPPED else ""))
sys.exit(1 if FAILED else 0)

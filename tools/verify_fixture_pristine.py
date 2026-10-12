# -*- coding: utf-8 -*-
"""`fixture_pristine.PristineGuard` 的单元自检（第 41 轮）。

用法：python3 tools/verify_fixture_pristine.py

为什么必须有它
-------------
`fixture_pristine` 保护的是**其它所有夹具的还原保证**。它自己错了，
受害的不是某个规则，而是**仓库里的源文件** —— 第 41 轮就真的发生过：

  `%TEMP%\\fi37_pristine\\widget.h` 是 7528 字节的**第 37 轮之前**版本，
  磁盘上是 8421 字节。夹具被强杀（Windows `TerminateProcess`）留下了
  `.in-progress` 标记，下一次启动便拿那份**过期副本**"自愈"，
  把第 37 轮给影子头加的全部内容静默抹掉。

本脚本用一个**临时目录里的假仓库**复现这条时间线，断言：

  1. 正常结束 -> 副本被刷新，**不**覆盖当前文件；
  2. 被中断 + 当前文件==记录长度 -> 允许自愈（这是设计意图）；
  3. 被中断 + 当前文件长度已被改过 -> **拒绝自愈**（第 41 轮的修复）；
  4. 还原必须逐字节一致；
  5. 锁：同一批文件的第二个实例必须被拒绝；
  6. 陈旧锁（持有者已死）必须能被接管。

全部在临时目录里做，**不碰真实仓库**。
"""
import io
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import fixture_pristine as fp  # noqa: E402

FAILS = []


def check(name, cond, detail=""):
    if cond:
        print("  [PASS] %s" % name)
    else:
        print("  [FAIL] %s  %s" % (name, detail))
        FAILS.append(name)


class Sandbox(object):
    """临时"假仓库"：一个源文件 + 独立的备份/锁目录。"""

    def __init__(self, tag):
        self.root = tempfile.mkdtemp(prefix="fixturepristine-")
        self.file = os.path.join(self.root, "widget.h")
        fp.write(self.file, "ORIGINAL CONTENT\n")
        self.tag = tag

    def make_guard(self, lock_name=None):
        g = fp.PristineGuard(files=[self.file], tag=self.tag,
                             lock_name=lock_name or (self.tag + ".lock"))
        return g

    def cleanup(self):
        shutil.rmtree(self.root, ignore_errors=True)
        # 清掉本沙箱自己的备份目录与锁
        bdir = fp.temp_dir(self.tag + "_pristine")
        shutil.rmtree(bdir, ignore_errors=True)
        for ln in (self.tag + ".lock",):
            try:
                os.remove(fp.temp_dir(ln))
            except OSError:
                pass


# ---------------------------------------------------------------------------
print("=== 1. 正常结束：刷新副本，绝不覆盖当前文件 ===")
sb = Sandbox("fptest1")
g = sb.make_guard()
check("拿到锁", g.acquire_lock())
g.prepare()
# 模拟"变异进行中" -> 然后正常 restore
fp.write(sb.file, "MUTATED CONTENT\n")
g.restore()
check("还原逐字节一致", g.verify_restored())
check("内容回到原文", fp.read(sb.file) == "ORIGINAL CONTENT\n")

# 开发者随后**合法**修改了文件，再跑一次夹具：不得被回滚
fp.write(sb.file, "DEVELOPER NEW CONTENT (longer)\n")
g2 = sb.make_guard()
check("第二次拿到锁", g2.acquire_lock())
healed2 = g2.prepare()
check("无中断证据时不自愈", healed2 == [], "healed=%r" % (healed2,))
check("合法修改保留", fp.read(sb.file) == "DEVELOPER NEW CONTENT (longer)\n")
g2.restore()
sb.cleanup()

# ---------------------------------------------------------------------------
print()
print("=== 2. 被中断 + 长度未变 -> 允许自愈（设计意图）===")
sb = Sandbox("fptest2")
g = sb.make_guard()
g.acquire_lock()
g.prepare()
# 模拟"被强杀"：写变异，但**不**调 restore（标记留着）
fp.write(sb.file, "POLLUTED\n")
g.release_lock()
# 注意：这里副本是 ORIGINAL CONTENT；当前文件被改成 POLLUTED（长度也不同），
# 但标记里记录的长度是"prepare 时的长度"= ORIGINAL 的长度。
# 按新规则：长度不符 -> **拒绝**自愈。这正是第 3 条要测的。
g2 = sb.make_guard()
check("第三次拿到锁（陈旧锁已被清理）", g2.acquire_lock())
g2.prepare()
check("中断且长度不符 -> 拒绝自愈", "widget.h" in g2.unfresh,
      "unfresh=%r healed=%r" % (g2.unfresh, g2.healed))
check("污染没有被'自愈'成更旧的副本", fp.read(sb.file) == "POLLUTED\n")
g2.restore()
sb.cleanup()

# ---------------------------------------------------------------------------
print()
print("=== 3. 被中断 + 长度与记录一致 -> 自愈（真正的残留场景）===")
sb = Sandbox("fptest3")
g = sb.make_guard()
g.acquire_lock()
g.prepare()
# 真实残留场景：变异**改变了内容但没改变长度**（例如把 TRUE 改成 FALSE，
# 长度相同）。这时"长度一致"成立，且我们无法用长度区分 —— 但这正是
# 标记存在的意义：上次没跑完，当前文件就是变异残留。
fp.write(sb.file, "ORIGINAL CONTENU\n")     # 同长度，内容被改
g.release_lock()                             # 模拟标记残留
g2 = sb.make_guard()
g2.acquire_lock()
healed = g2.prepare()
check("同长度残留 -> 允许自愈", "widget.h" in healed, "healed=%r" % (healed,))
check("内容被恢复成副本", fp.read(sb.file) == "ORIGINAL CONTENT\n")
g2.restore()
sb.cleanup()

# ---------------------------------------------------------------------------
print()
print("=== 4. 副本恰好等于当前文件 -> 不需要自愈，且不报错 ===")
sb = Sandbox("fptest4")
g = sb.make_guard()
g.acquire_lock()
g.prepare()
g.release_lock()
g2 = sb.make_guard()
g2.acquire_lock()
h = g2.prepare()
check("无差异时不报自愈", h == [] and g2.unfresh == [])
check("内容未变", fp.read(sb.file) == "ORIGINAL CONTENT\n")
g2.restore()
sb.cleanup()

# ---------------------------------------------------------------------------
print()
print("=== 5. 互斥锁：活着的持有者必须挡住第二个实例 ===")
sb = Sandbox("fptest5")
g = sb.make_guard()
check("第一个实例拿到锁", g.acquire_lock())
g2 = sb.make_guard()
check("第二个实例被拒绝", not g2.acquire_lock())
g.release_lock()
check("释放后第三个实例可拿到", sb.make_guard().acquire_lock())
sb.cleanup()

# ---------------------------------------------------------------------------
print()
print("=== 6. 陈旧锁：持有者已死，必须能接管 ===")
sb = Sandbox("fptest6")
lock_path = fp.temp_dir(sb.tag + ".lock")
# 写一个**不存在**的 pid（几乎不可能正在运行）
fp.write(lock_path, "999999999\n")
g = sb.make_guard()
check("陈旧锁被接管", g.acquire_lock())
check("接管后锁文件属于本进程",
      fp.read(lock_path).strip() == str(os.getpid()))
g.release_lock()
sb.cleanup()

# ---------------------------------------------------------------------------
print()
print("=== 7. temp_dir 在无 TEMP 变量时也能用（Linux/CI）===")
saved = os.environ.pop("TEMP", None)
try:
    p = fp.temp_dir("x")
    check("无 TEMP 时不抛异常", bool(p))
    check("回退到临时目录", tempfile.gettempdir() in p)
finally:
    if saved is not None:
        os.environ["TEMP"] = saved

# ---------------------------------------------------------------------------
print()
print("=== 8. restore 可重复调用（atexit + finally 都会调）===")
sb = Sandbox("fptest8")
g = sb.make_guard()
g.acquire_lock()
g.prepare()
g.restore()
try:
    g.restore()
    ok = True
except Exception as e:  # noqa: BLE001
    ok = False
    print("   异常: %r" % (e,))
check("重复 restore 不抛异常", ok)
check("内容仍是原文", fp.read(sb.file) == "ORIGINAL CONTENT\n")
sb.cleanup()

print()
if FAILS:
    print("失败 %d 项：%s" % (len(FAILS), FAILS))
    sys.exit(1)
print("结论: 全部通过")
sys.exit(0)

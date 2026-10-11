# -*- coding: utf-8 -*-
"""verify.py 的 [actuator] 规则（开锁唯一出口 + 执行器不掺补光）故障注入自检。

用法：python3 tools/verify_actuator_separation_rule.py
返回码 0 表示：每个危险退化都被抓到、对照未误报、且还原后逐字节一致。

为什么需要这个夹具
------------------
第 40 轮给"开锁"补上了唯一出口 `DoorActuator`，但这条约束**极易被无意破坏**：
下一个人要开锁时，最省事的写法就是在手边那条已经打开的输出线上写
`setValue(1)`。规则的价值全在"能不能抓到这种回退"，所以每个变异都朝真实的
危险退化去改，并要求规则以 **X [actuator]** 报出来。

变异（必须被抓到，报 X [actuator]）
  M1 widget.cpp 的 requestUnlock 里把 `m_doorActuator->unlock(...)`
     换成 `m_fillLight->setValue(1)`（**正是要防的那种回退**：
     既绕开执行器，又把补光线当门锁）
  M2 door_actuator.cpp 里出现补光标识符（执行器掺补光）
  M3 删掉 widget.cpp 里唯一的 `m_doorActuator->unlock(` 调用（执行器变死代码）

对照（必须通过：无 X [actuator]）
  C1 widget.cpp 的**注释**里同时写 setValue 与 unlock
  C2 door_actuator.cpp 的**注释**里写 fillLight

跨平台：仓库根由 __file__ 推导、解释器用 sys.executable，
不依赖 msys64 与 Windows 路径，Linux/CI 上可直接跑。
"""
import io
import os
import re
import subprocess
import sys
import tempfile

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
T = os.path.dirname(HERE)                       # 仓库根 = tools/ 的上一级
VERIFY = os.path.join(T, "tools", "verify.py")
WIDGET = os.path.join(T, "src", "ui", "widget.cpp")
ACTUATOR = os.path.join(T, "src", "managers", "door_actuator.cpp")
PY = sys.executable

# 唯一放行出口的那一行（锚点）：它必须逐字节存在，否则夹具已过期。
UNLOCK_CALL = "    m_accessController->request(method, source);\n"
# 危险回退：绕开中枢，直接驱动补光那条线
BAD_UNLOCK = "    m_fillLight->setValue(1);\n"
# 另一种回退：把出口删掉（中枢成了死代码）
NO_UNLOCK = "    Q_UNUSED(method);\n    Q_UNUSED(source);\n"

INJECT_FILL_IDENT = (
    "\n// 夹具注入：执行器里出现补光标识符（应被 [actuator] 抓到）\n"
    "namespace accessterminal {\n"
    "void fixtureTouchesFillLight(GpioLine* fillLight)\n"
    "{\n"
    "    (void)fillLight;\n"
    "}\n"
    "} // namespace accessterminal\n"
)
# 危险回退：绕过 AccessController，由 widget 直接调执行器
INJECT_DIRECT_ACTUATOR = (
    "\n// 夹具注入：widget 直接调用执行器（绕过放行中枢）\n"
    "void Widget::fixtureDirectActuatorCall(const QString& s)\n"
    "{\n"
    "    m_doorActuator->unlock(s);\n"
    "}\n"
)
INJECT_COMMENT_SETVALUE = (
    "\n// 夹具对照：注释里写 m_fillLight->setValue(1) 与 unlock 不算违规\n"
)
INJECT_COMMENT_FILLLIGHT = (
    "\n// 夹具对照：注释里提到 fillLight 不算违规\n"
)


def read(p):
    return io.open(p, encoding="utf-8", errors="replace").read()


def write(p, s):
    io.open(p, "w", encoding="utf-8", newline="\n").write(s)


def run_verify():
    r = subprocess.run([PY, VERIFY, T], capture_output=True, cwd=T)
    out = (r.stdout + r.stderr).decode("utf-8", "replace")
    x = re.findall(r"^\s*X\s*\[(actuator)\]", out, re.M)
    return r.returncode, x


_ORIG = {p: read(p) for p in (WIDGET, ACTUATOR, VERIFY)}
assert UNLOCK_CALL in _ORIG[WIDGET], (
    "锚点未找到：widget.cpp 里没有 `%s`，本夹具需要同步更新" % UNLOCK_CALL.strip())


def sub(path, old, new):
    o = _ORIG[path]
    assert o.count(old) >= 1, "锚点未找到: %r in %s" % (old[:60], path)
    write(path, o.replace(old, new, 1))
    return lambda: write(path, o)


def app(path, text):
    o = _ORIG[path]
    write(path, o + text)
    return lambda: write(path, o)


def restore_all():
    for p, s in _ORIG.items():
        write(p, s)


MUT = [
    ("requestUnlock 改成直接写补光电平（真实回退）",
     lambda: sub(WIDGET, UNLOCK_CALL, BAD_UNLOCK), "actuator"),

    ("door_actuator.cpp 里出现补光标识符",
     lambda: app(ACTUATOR, INJECT_FILL_IDENT), "actuator"),

    ("删掉唯一放行出口调用（中枢变死代码）",
     lambda: sub(WIDGET, UNLOCK_CALL, NO_UNLOCK), "actuator"),

    ("widget 绕过中枢直接调执行器",
     lambda: app(WIDGET, INJECT_DIRECT_ACTUATOR), "actuator"),

    ("对照：注释里同时写 setValue 与 unlock（不得误报）",
     lambda: app(WIDGET, INJECT_COMMENT_SETVALUE), None),

    ("对照：door_actuator.cpp 注释里写 fillLight（不得误报）",
     lambda: app(ACTUATOR, INJECT_COMMENT_FILLLIGHT), None),
]


def _run():
    if len(sys.argv) > 1:
        print("提示: 本脚本不需要参数，已忽略: %s" % " ".join(sys.argv[1:]))

    print("仓库根: %s" % T)
    print()

    baseline_rc, baseline_x = run_verify()
    print("基线: rc=%d, X [actuator] 行数=%d" % (baseline_rc, len(baseline_x)))
    if baseline_rc != 0 or baseline_x:
        print("*** 基线不干净：请先让 `python3 tools/verify.py .` 通过再跑本夹具")
        return 1
    print()

    problems = []
    try:
        for name, apply_mut, expect in MUT:
            undo = apply_mut()
            try:
                changed = any(read(p) != _ORIG[p] for p in _ORIG)
                if not changed:
                    print("  [夹具错误] %-46s 文件未变" % name)
                    problems.append(name)
                    continue
                rc, x = run_verify()
                if expect is None:
                    if rc == 0 and not x:
                        print("  %-46s 通过 ✓（不误报）" % name)
                    else:
                        print("  %-46s *** 误报: X=%s rc=%d" % (name, x, rc))
                        problems.append(name)
                else:
                    if expect in x:
                        print("  %-46s 被抓到 ✓ [%s]" % (name, expect))
                    else:
                        print("  %-46s *** 未被抓到（假绿）X=%s" % (name, x))
                        problems.append(name)
            finally:
                undo()
                restore_all()
    finally:
        restore_all()

    same = all(read(p) == _ORIG[p] for p in _ORIG)
    print()
    rc, x = run_verify()
    print("还原逐字节一致:", same)
    print("还原后 rc=%d, X [actuator] 行数=%d" % (rc, len(x)))
    print()
    print("变异/对照 %d 个，致命问题 %d 个" % (len(MUT), len(problems)))
    if problems:
        print("问题清单:", problems)
    return 1 if (problems or not same or rc != 0 or x) else 0


# ---------------------------------------------------------------------------
# 仓库级"正在改写源码"文件锁
#
# 为什么要锁：夹具是"原地改写源文件 → 跑校验 → 还原"，任何**读源码**的操作与
# 它并发都会看到中间状态。第 40 轮实测：一边跑夹具、一边后台
# `cmake --build && ctest`，构建把**变异后的** door_actuator.cpp 编进目标文件，
# 于是 t113_door_actuator / t113_access_controller 双双 Failed，而串行重跑
# 20/20 全过 —— 与第 37 轮"两个夹具互相踩"是同一类错误，只是这次踩的是构建。
#
# 锁名沿用既有两个 form_finger 夹具的 fi37_rewrite.lock，因此四个夹具之间也
# 互斥；目录优先用 TEMP（Windows 惯例），没有就退到系统临时目录（Linux）——
# 既有两个夹具直接读 os.environ['TEMP']，在 Linux 上会 KeyError，本夹具不重复
# 那个跨平台问题。
# ---------------------------------------------------------------------------
LOCK = os.path.join(os.environ.get('TEMP') or tempfile.gettempdir(),
                    'fi37_rewrite.lock')


def _acquire_lock():
    for _ in range(2):
        try:
            fd = os.open(LOCK, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, ('%d\n' % os.getpid()).encode('ascii'))
            os.close(fd)
            return True
        except OSError:
            try:
                with io.open(LOCK, encoding='ascii') as fh:
                    other = int(fh.read().strip())
            except (OSError, ValueError):
                other = None
            alive = False
            if other is not None:
                try:
                    os.kill(other, 0)
                    alive = True
                except OSError:
                    alive = False
            if alive:
                return False
            try:
                os.remove(LOCK)
            except OSError:
                return False
    return False


def _release_lock():
    try:
        os.remove(LOCK)
    except OSError:
        pass


def main():
    if not _acquire_lock():
        print("另一个夹具正在改写源码（%s）—— 请等它跑完再试" % LOCK)
        print("（构建/ctest/verify.py 也不要与夹具并发：会读到变异后的源码）")
        return 2
    try:
        return _run()
    finally:
        _release_lock()


if __name__ == "__main__":
    sys.exit(main())

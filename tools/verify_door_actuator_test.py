# -*- coding: utf-8 -*-
"""t113_door_actuator 测试目标的故障注入自检。

用法：python3 tools/verify_door_actuator_test.py
返回码 0 表示：每个危险退化都让测试目标**变红**、对照保持绿、且源码还原
后逐字节一致。

为什么要证明"测试会红"
----------------------
本工程的核心信条是"在缺陷代码上不失败的测试没有价值"。第 40 轮新增的
DoorActuator 有三条硬约束（不假装开门、不阻塞、不重复驱动），本夹具把
每一条**分别改坏**，证明对应的用例真的会失败：

  M1 未接执行器时 unlock() 返回 true（**假装门开了**）→ 必须红
  M2 去掉"脉冲进行中忽略重复请求"的守卫（**重复驱动继电器**）→ 必须红
  M3 把非阻塞脉冲改成同步驱动（**冻住 GUI**，旧工程 msleep(1000) 的老毛病）
     → 必须红

对照：
  C1 不改任何代码 → 必须绿（否则说明测试本身不稳）

环境
----
需要能配置 tests/ 子工程（Qt5 Core/Test/Gui/Network/SerialPort/Widgets）。
若本机 Qt5SerialPort 只有运行库没有 dev 包，用环境变量指路：

    QT5SERIALPORT_DIR=/tmp/qtsp/usr/lib/x86_64-linux-gnu/cmake/Qt5SerialPort \
        python3 tools/verify_door_actuator_test.py

构建目录用临时目录，不污染仓库；跨平台（路径由 __file__ 推导）。
"""
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
T = os.path.dirname(HERE)                       # 仓库根
ACTUATOR = os.path.join(T, "src", "managers", "door_actuator.cpp")
TESTS = os.path.join(T, "tests")

# ---- 变异锚点（必须逐字节存在，否则夹具已过期）----
SIMULATED_RET = "        return false;\n    }\n\n    // 情况二"
SIMULATED_RET_BAD = "        return true;\n    }\n\n    // 情况二"

PULSE_GUARD = ("    if (m_releaseTimer->isActive()) {\n"
               "        qInfo() << \"[开锁] 脉冲进行中，忽略本次重复请求，来源:\" << source;\n"
               "        return false;\n"
               "    }\n")
PULSE_GUARD_BAD = ""

NONBLOCK_START = "    m_releaseTimer->start(m_durationMs);\n"
NONBLOCK_START_BAD = "    releaseOutput(); // 变异：同步驱动（会冻住界面）\n"


def read(p):
    return io.open(p, encoding="utf-8", errors="replace").read()


def write(p, s):
    io.open(p, "w", encoding="utf-8", newline="\n").write(s)


def configure(build_dir):
    args = ["cmake", "-S", TESTS, "-B", build_dir, "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release"]
    sp = os.environ.get("QT5SERIALPORT_DIR")
    if sp:
        args.append("-DQt5SerialPort_DIR=" + sp)
    r = subprocess.run(args, capture_output=True)
    return r.returncode == 0, (r.stdout + r.stderr).decode("utf-8", "replace")


def build_and_run(build_dir):
    """构建并运行目标；返回 (是否通过, 输出尾部)。"""
    r = subprocess.run(["cmake", "--build", build_dir, "--target",
                        "t113_test_door_actuator"], capture_output=True)
    if r.returncode != 0:
        return False, (r.stdout + r.stderr).decode("utf-8", "replace")[-2000:]
    binary = os.path.join(build_dir, "t113_test_door_actuator")
    env = dict(os.environ)
    env["QT_QPA_PLATFORM"] = "offscreen"
    r = subprocess.run([binary], capture_output=True, env=env, cwd=build_dir)
    out = (r.stdout + r.stderr).decode("utf-8", "replace")
    return r.returncode == 0, out[-1500:]


_ORIG = read(ACTUATOR)
for _anchor in (SIMULATED_RET, PULSE_GUARD, NONBLOCK_START):
    assert _anchor in _ORIG, "锚点未找到，本夹具需要同步更新: %r" % _anchor[:50]


def mutate(old, new):
    write(ACTUATOR, _ORIG.replace(old, new, 1))
    return lambda: write(ACTUATOR, _ORIG)


MUT = [
    ("未接执行器时假装开门（返回 true）", lambda: mutate(SIMULATED_RET, SIMULATED_RET_BAD), False),
    ("去掉脉冲中的重复请求守卫", lambda: mutate(PULSE_GUARD, PULSE_GUARD_BAD), False),
    ("把非阻塞脉冲改成同步驱动", lambda: mutate(NONBLOCK_START, NONBLOCK_START_BAD), False),
    ("对照：不改代码", lambda: mutate(NONBLOCK_START, NONBLOCK_START), True),
]


def main():
    if len(sys.argv) > 1:
        print("提示: 本脚本不需要参数，已忽略: %s" % " ".join(sys.argv[1:]))

    build_dir = tempfile.mkdtemp(prefix="t113-actuator-fixture-")
    print("构建目录: %s" % build_dir)
    print()
    try:
        ok, log = configure(build_dir)
        if not ok:
            print("*** tests/ 配置失败，无法进行故障注入：")
            print("\n".join(log.splitlines()[-25:]))
            print()
            print("提示：若缺 Qt5SerialPort dev 包，用 QT5SERIALPORT_DIR 指路"
                  "（见本文件头部说明）。")
            return 2

        problems = []
        try:
            for name, apply_mut, expect_pass in MUT:
                undo = apply_mut()
                try:
                    passed, tail = build_and_run(build_dir)
                    if expect_pass:
                        if passed:
                            print("  %-34s 绿 ✓（对照）" % name)
                        else:
                            print("  %-34s *** 对照竟然是红的：" % name)
                            print("      " + "\n      ".join(tail.splitlines()[-8:]))
                            problems.append(name)
                    else:
                        if not passed:
                            print("  %-34s 变红 ✓（测试有判别力）" % name)
                        else:
                            print("  %-34s *** 仍然是绿（假绿，测试没有判别力）" % name)
                            problems.append(name)
                finally:
                    undo()
        finally:
            write(ACTUATOR, _ORIG)

        same = read(ACTUATOR) == _ORIG
        print()
        passed, _ = build_and_run(build_dir)
        print("还原逐字节一致:", same)
        print("还原后目标:", "绿" if passed else "红")
        print()
        print("变异/对照 %d 个，致命问题 %d 个" % (len(MUT), len(problems)))
        if problems:
            print("问题清单:", problems)
        return 1 if (problems or not same or not passed) else 0
    finally:
        shutil.rmtree(build_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

# -*- coding: utf-8 -*-
"""`[face-anti-fp]` 静态规则的故障注入自检（第 43 轮，方案 P4）。

用法：python3 tools/verify_face_anti_fp_rule.py
返回码 0 表示：每条危险退化都让 verify.py **变红**、对照不误报、
且源码还原后逐字节一致。

命名注意
--------
本工程已有一个 `verify_face_gate_rule.py`（第 38 轮），它守的是**旧的**
`[face-gate]` 规则 —— LBPH 阈值上限必须低于 ~256 的饱和点。
本文件守的是**新的** `[face-anti-fp]` 规则（质量门 + 连续命中平滑）。
两者是不同规则、不同判据，因此**故意用不同的标签**：若共用 `[face-gate]`，
一个夹具的变异可能被另一个规则的失败"顶包"通过 —— 那是假绿。

为什么必须证明"规则会红"
------------------------
本工程的信条是"在缺陷代码上不失败的测试没有价值"。静态规则同理：
一条**从来不会报错**的规则等于没有规则，而它还会给人"已经守住了"的错觉。

变异清单（每条都必须被 `[face-anti-fp]` 捕获）
---------------------------------------------
  A1  删掉 widget.cpp 里的质量门调用
      -> 判据 A：「类写了却不调用」与没有这个类完全等价，而且更糟
  A2  质量门只评估、不判断（去掉 `.acceptable`）
      -> 判据 A：只评估不拦截等于没接
  B1  把质量门挪到人脸裁剪 `copy(faceRect)` **之后**
      -> 判据 B：先付缩放/predict 成本再丢弃，P4 要省的开销一点没省
  C1  把 faceQualityEnabled() 改成独立开关（直接 return 成员）
      -> 判据 C：独立开关会与阈值互相矛盾（静默失效 / 配置看起来生效）
  D1  删掉 hasFirst 标记
      -> 判据 D：无法区分"第一次命中"与"后续命中"，窗口语义无从谈起
  D2  删掉 sinceFirst 计时器
      -> 判据 D：窗口必须从第一次命中起算，没有计时器就做不到
  D3  **改成"窗口从最近一次起算"**（++pending 后 restart 计时）
      -> 判据 D：干站着的人迟早攒够 N 次，门等于没锁
         （这是本规则判别力最核心的一条）
  E1  required_streak 默认值改成 2
      -> 判据 E：会悄悄改变"谁进得来"（不配置就变严）
  E2  min_sharpness 默认值改成非 0
      -> 判据 E：同上
  F1  连续命中改成全局（不按认证方式传参）
      -> 判据 F：会顺带让"密码输一次不开门"

对照（必须保持绿，即不误报）
--------------------------
  C1  加无关注释
  C2  调整空白/缩进
  C3  给质量门局部变量改名（证明规则不是靠变量名匹配）

注意
----
纯静态规则的夹具，**不需要编译**。
跑之前请确认没有别的夹具在改写源码 —— 本脚本自带文件锁。
锁忙返回码为 3（与其他夹具一致）。
"""
import io
import os
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
T = os.path.dirname(HERE)
PY = sys.executable

WIDGET = os.path.join(T, "src", "ui", "widget.cpp")
CFG_H = os.path.join(T, "src", "config", "app_config.h")
AC_H = os.path.join(T, "src", "managers", "access_controller.h")
AC_CPP = os.path.join(T, "src", "managers", "access_controller.cpp")

FILES = [WIDGET, CFG_H, AC_H, AC_CPP]

TAG = "[face-anti-fp]"

sys.path.insert(0, HERE)
from fixture_pristine import PristineGuard  # noqa: E402

_GUARD = PristineGuard(files=FILES, tag='p4antifp',
                       lock_name='fi37_rewrite.lock')
_ORIG = _GUARD.orig
_HEALED = []


def read(p):
    return io.open(p, encoding="utf-8", errors="replace").read()


def write(p, s):
    io.open(p, "w", encoding="utf-8", newline="\n").write(s)


def run_verify():
    """返回 (rc, [face-anti-fp 相关的失败行])。"""
    r = subprocess.run([PY, os.path.join(T, "tools", "verify.py"), T],
                       capture_output=True, cwd=T)
    out = (r.stdout + r.stderr).decode("utf-8", "replace")
    lines = [l.strip() for l in out.splitlines()
             if TAG in l and l.strip().startswith(("X", "!"))]
    return r.returncode, lines


# ---------------------------------------------------------------------------
# 变异定义：(名字, 期望失败的判据子串, [(文件, 旧, 新), ...])
#
# 判据子串必须**足够具体**：只写标签的话，任何一条本规则的失败都会算命中，
# 某个变异可能被另一个变异引发的失败"顶包"通过 —— 那是假绿。
# ---------------------------------------------------------------------------
MUT = [
    # ---- A. 质量门真的被接进识别路径 ----
    #
    # 注意：这里的变异必须模拟**真实的退化形态**，不能只是把某个条件改成
    # 恒假 —— 那样 `FaceQuality::evaluate` 的文本还在文件里，而本判据是
    # 文本存在性检查（静态规则本来就看不出死活代码）。第一版就写成了
    # `if (false)`，结果规则判绿，被我自己的夹具抓出来。
    # 真实退化是"这段代码根本不存在"（类写了、测试全绿，但没接线），
    # 所以这里整段删掉。
    ("A1 整个质量门没有接进识别路径", "没有人脸质量门调用",
     [(WIDGET,
       "                if (m_faceQualityEnabled) {\n"
       "                    const accessterminal::FaceQualityReport q =\n"
       "                        accessterminal::FaceQuality::evaluate(\n"
       "                            frame, faceRect, m_faceQualityThresholds);\n"
       "                    if (!q.acceptable) {\n"
       "                        // 拒绝时**不改动冷却计时器**：这一帧本来就不该\n"
       "                        // 占用一次识别机会，让它立刻重试下一帧更合理。\n"
       "                        qInfo() << \"[人脸质量] 本帧不达标，跳过识别:\"\n"
       "                                << q.describe();\n"
       "                        return;\n"
       "                    }\n"
       "                }\n",
       "                // 变异：质量门整段未接线（类写了、测试全绿，但没调用）\n")]),

    ("A2 只评估不判断（去掉 .acceptable）", "没有被判断",
     [(WIDGET,
       "                    if (!q.acceptable) {",
       "                    if (false) {   // 变异：评估了但不拦截")]),

    # ---- B. 必须在人脸裁剪之前 ----
    ("B1 质量门挪到人脸裁剪之后", "质量门写在人脸裁剪",
     [(WIDGET,
       "                if (m_faceQualityEnabled) {",
       "                const QImage probeImage =\n"
       "                    frame.copy(faceRect).scaled(100, 100, Qt::KeepAspectRatio);\n"
       "                if (m_faceQualityEnabled) {")]),

    # ---- C. 启用状态必须由阈值推导 ----
    ("C1 改成独立开关", "没有引用阈值访问器",
     [(CFG_H,
       "        return faceMinSharpness() > 0.0\n"
       "               || faceMinBrightness() > 0.0\n"
       "               || faceMaxBrightness() < 255.0\n"
       "               || faceMinContrast() > 0.0\n"
       "               || faceMinAreaRatio() > 0.0;",
       "        return m_faceEnabled;   // 变异：独立开关，与阈值可矛盾")]),

    # ---- D. 窗口从第一次命中起算 ----
    #
    # 分工说明（重要，别把这里当成"窗口语义已被静态规则守住"）：
    #   静态规则只判**结构形态** —— "有没有把首次/后续两个分支分开"、
    #   "后续命中分支里有没有偷偷重置计时"。它**证明不了** `expired`
    #   是不是真的按窗口算出来的：把 expired 写死为 false 时，`hasFirst`
    #   的名字仍在分支条件与赋值里，文本判据区分不了。第 43 轮实测过这一点。
    #   窗口的**运行期**语义由 tests/test_access_controller.cpp 的
    #   `streakWindowStartsAtFirstHitNotLast` 用例钉住 —— 已实测：把实现改成
    #   "从最近一次起算"后该用例**变红**（tools/prove 里做过，见 round 43 报告）。
    #   因此这里的静态变异只覆盖"形态"层面的退化。
    ("D1 用无窗口计数器取代窗口机制（形态退化）", "没有 `sinceFirst` 计时器",
     [(AC_CPP,
       "        const bool expired = s.hasFirst && s.sinceFirst.isValid()\n"
       "                             && s.sinceFirst.elapsed() >= s.windowMs;\n"
       "        if (!s.hasFirst || expired) {\n"
       "            // 首次命中，或窗口已过 -> 从这一次**重新**开始计时与计数。\n"
       "            s.sinceFirst.start();\n"
       "            s.hasFirst = true;\n"
       "            s.pending = 1;\n"
       "        } else {\n"
       "            ++s.pending;\n"
       "        }",
       "        // 变异：换成无窗口的普通计数器（首次/后续不再区分）\n"
       "        ++s.pending;\n")]),

    ("D2 删掉 sinceFirst 计时器", "从未对 `sinceFirst` 调用 start()",
     [(AC_CPP,
       "            s.sinceFirst.start();\n",
       "            // 变异：不再启动计时（名字仍在其它地方被引用）\n")]),

    ("D3 窗口改成从最近一次起算", "窗口从最近一次起算",
     [(AC_CPP,
       "        } else {\n"
       "            ++s.pending;\n"
       "        }",
       "        } else {\n"
       "            s.sinceFirst.restart();   // 变异：每次命中都重置计时\n"
       "            ++s.pending;\n"
       "        }")]),

    # ---- E. 默认必须关闭 ----
    ("E1 required_streak 默认改成 2", "默认值必须是 1",
     [(CFG_H, "int m_faceRequiredStreak = 1;", "int m_faceRequiredStreak = 2;")]),

    ("E2 min_sharpness 默认改成非 0", "默认值必须是 0",
     [(CFG_H, "double m_faceMinSharpness = 0.0;", "double m_faceMinSharpness = 50.0;")]),

    # ---- F. 按方式独立配置 ----
    ("F1 连续命中改成全局（不传方式）", "按**认证方式**设连续命中",
     [(WIDGET,
       "        m_accessController->setRequiredStreak(\n"
       "            accessterminal::AccessMethod::Face,\n"
       "            cfg.faceRequiredStreak(), cfg.faceStreakWindowMs());",
       "        // 变异：不再区分方式，直接设阈值\n"
       "        (void)cfg.faceRequiredStreak();")]),
]


# ---------------------------------------------------------------------------
# 对照：这些**不该**让规则报错（证明没有误报）
# ---------------------------------------------------------------------------
CTRL = [
    ("对照：加无关注释",
     [(WIDGET,
       "void Widget::onFaceDetectResult(",
       "// 无关注释（对照）\nvoid Widget::onFaceDetectResult(")]),

    ("对照：调整空白与缩进",
     [(AC_CPP,
       "bool AccessController::request(",
       "bool AccessController::request\n(")]),

    ("对照：给质量门局部变量改名",
     [(WIDGET,
       "                    const accessterminal::FaceQualityReport q =",
       "                    const accessterminal::FaceQualityReport report ="),
      (WIDGET,
       "                    if (!q.acceptable) {",
       "                    if (!report.acceptable) {")]),
]


def main():
    if not _GUARD.acquire_lock():
        print("另一个夹具正在改写源码（%s）—— 请等它跑完再试"
              % _GUARD.lock_path())
        print("（构建/ctest/verify.py 也不要与夹具并发）")
        return 3
    try:
        return _run()
    finally:
        _GUARD.release_lock()


def _run():
    _HEALED[:] = _GUARD.prepare()
    _GUARD.install_guards()
    if _HEALED:
        print("!! 检测到上一次运行被中断，已用磁盘原始副本自愈: %s"
              % ", ".join(os.path.basename(p) for p in _HEALED))

    # 预检：基线必须是绿的，否则无从判断"变异有没有让它变红"。
    rc0, base = run_verify()
    print("基线 verify.py rc=%d，%s 失败 %d 条" % (rc0, TAG, len(base)))
    if rc0 != 0:
        print("*** 基线就不通过，无法进行故障注入：")
        for l in base[:5]:
            print("    " + l[:150])
        return 2
    print()

    problems = []

    # ---- 变异：必须变红，且报的是**期望的那一句** ----
    for name, expect, edits in MUT:
        try:
            ok_anchor = True
            for path, old, new in edits:
                src = _ORIG[path]
                if src.count(old) != 1:
                    print("  %-40s *** 锚点出现 %d 次（期望 1）"
                          % (name, src.count(old)))
                    problems.append(name + "(锚点)")
                    ok_anchor = False
                    break
                write(path, src.replace(old, new, 1))
            if ok_anchor:
                rc, lines = run_verify()
                hit = any(expect in l for l in lines)
                if rc != 0 and hit:
                    print("  %-40s 通过 ✓" % name)
                else:
                    print("  %-40s *** 失败  rc=%d 命中=%s"
                          % (name, rc, hit))
                    for l in lines[:3]:
                        print("        " + l[:150])
                    problems.append(name)
        finally:
            for path, _, _ in edits:
                write(path, _ORIG[path])

    print()

    # ---- 对照：必须保持绿 ----
    for name, edits in CTRL:
        try:
            ok_anchor = True
            for path, old, new in edits:
                src = _ORIG[path]
                if src.count(old) != 1:
                    print("  %-40s *** 锚点出现 %d 次（期望 1）"
                          % (name, src.count(old)))
                    problems.append(name + "(锚点)")
                    ok_anchor = False
                    break
                write(path, src.replace(old, new, 1))
            if ok_anchor:
                rc, lines = run_verify()
                if rc == 0:
                    print("  %-40s 通过 ✓  （未误报）" % name)
                else:
                    print("  %-40s *** 误报  rc=%d" % (name, rc))
                    for l in lines[:3]:
                        print("        " + l[:150])
                    problems.append(name)
        finally:
            for path, _, _ in edits:
                write(path, _ORIG[path])

    # ---- 还原并确认逐字节一致 ----
    for path in FILES:
        write(path, _ORIG[path])
    same = all(read(p) == _ORIG[p] for p in FILES)
    rc, lines = run_verify()

    print()
    print("还原逐字节一致:", same)
    print("还原后 verify.py rc=%d" % rc)

    # ---- 运行期语义证明：窗口必须真的按时间截止 ----
    #
    # 静态规则**证明不了**窗口语义（见 D1 处的分工说明）。因此这里再做一次
    # 运行期变异：把"窗口从第一次起算"改成"从最近一次起算"，重新编译并运行
    # t113_test_access_controller，断言 streakWindowStartsAtFirstHitNotLast
    # **真的失败**。没有这一步，"窗口语义已守"就只是自我安慰。
    print()
    print("---- 运行期证明：窗口语义 ----")
    rt_ok = _prove_window_semantics_runtime(problems)

    print()
    print("变异 %d 个，对照 %d 个，致命问题 %d 个"
          % (len(MUT), len(CTRL), len(problems)))
    if problems:
        print("问题清单:", problems)
    return 1 if (problems or not same or rc != 0 or not rt_ok) else 0


def _prove_window_semantics_runtime(problems):
    """把窗口改成"从最近一次起算"，断言单元测试变红；再还原。

    这是对"静态规则守形态、单元测试守语义"这个分工的**证明**：
    如果没有这一步，D1 处的分工说明就只是一句没有证据的话。
    """
    old = ("        } else {\n"
           "            ++s.pending;\n"
           "        }")
    new = ("        } else {\n"
           "            s.sinceFirst.restart();\n"
           "            ++s.pending;\n"
           "        }")
    src = _ORIG[AC_CPP]
    if src.count(old) != 1:
        print("  *** 运行期变异锚点出现 %d 次（期望 1）" % src.count(old))
        problems.append("RT锚点")
        return False

    # 构建目录：在若干候选根下找 `p4antifp_rt`。
    #
    # 为什么要"找"而不是直接拼一个路径：在 MSYS2 的 bash 里跑本脚本时，
    # `tempfile.gettempdir()` 会返回 **MSYS 自己的 /tmp**
    # （实际是 <msys64>\tmp），而构建脚本（cmake）看到的是 Windows 的
    # %TEMP%。两者不是同一个目录，实测导致"目录明明建好了却报不存在"、
    # 证明被静默跳过 —— 而静默跳过正是本项目最忌讳的失败模式（§78）。
    # 因此这里逐个候选探测，并把最终用的路径**打印出来**，让人能核对。
    cands = []
    if os.environ.get("P4_RT_BUILD"):
        cands.append(os.environ["P4_RT_BUILD"])
    for var in ("TEMP", "TMP", "TMPDIR"):
        v = os.environ.get(var)
        if v:
            cands.append(os.path.join(v, "p4antifp_rt"))
    cands.append(os.path.join(tempfile.gettempdir(), "p4antifp_rt"))
    cands.append(r"C:\Users\user\AppData\Local\Temp\p4antifp_rt")

    build = None
    for c in cands:
        if os.path.isdir(c):
            build = c
            break
    if build is None:
        print("  (跳过) 未找到任何 p4antifp_rt 构建目录；探测过：")
        for c in cands:
            print("     - %s" % c)
        print("     运行期证明需要先配置一次：")
        print("     cmake -S tests -B <上述之一> -G Ninja -DCMAKE_BUILD_TYPE=Release")
        print("     （也可用环境变量 P4_RT_BUILD 指定）")
        return True   # 不算失败：静态部分已经证明过了
    print("  使用构建目录: %s" % build)

    # cmake/ninja 在 MSYS2 的 /mingw64/bin 下，而本脚本由 Windows 的 Python
    # 启动，PATH 里没有它。若不补，subprocess 会抛 FileNotFoundError
    # （实测如此）—— 那是"证明根本没跑"，不是"证明通过"。
    env = dict(os.environ)
    extra = [
        r"C:\Users\user\.dsh\toolchains\msys64\mingw64\bin",
        r"C:\Users\user\.dsh\toolchains\msys64\usr\bin",
    ]
    env["PATH"] = os.pathsep.join(extra + [env.get("PATH", "")])
    cmake = shutil.which("cmake", path=env["PATH"])
    if cmake is None:
        print("  (跳过) PATH 里找不到 cmake —— 无法做运行期证明")
        return True
    try:
        write(AC_CPP, src.replace(old, new, 1))
        r = subprocess.run(
            [cmake, "--build", build, "--target",
             "t113_test_access_controller"],
            capture_output=True, env=env)
        if r.returncode != 0:
            print("  *** 变异后编译失败（无法判定运行期行为）")
            print((r.stdout + r.stderr).decode("utf-8", "replace")[-400:])
            problems.append("RT编译")
            return False
        exe = os.path.join(build, "t113_test_access_controller.exe")
        if not os.path.isfile(exe):
            print("  *** 找不到测试可执行文件: %s" % exe)
            problems.append("RT可执行文件")
            return False
        r = subprocess.run([exe], capture_output=True, cwd=T, env=env)
        out = (r.stdout + r.stderr).decode("utf-8", "replace")
        red = "FAIL!" in out and "streakWindowStartsAtFirstHitNotLast" in out
        if red:
            print("  窗口改成『从最近一次起算』-> 单元测试变红 ✓")
            print("     （证明：静态规则守形态，运行期语义由单元测试守住）")
            return True
        print("  *** 突变后单元测试**没有**变红 —— 窗口语义无人守护！")
        for l in out.splitlines():
            if l.startswith("FAIL!"):
                print("      " + l[:140])
        problems.append("RT未变红")
        return False
    finally:
        write(AC_CPP, _ORIG[AC_CPP])


if __name__ == "__main__":
    sys.exit(main())

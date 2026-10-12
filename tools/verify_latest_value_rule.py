# -*- coding: utf-8 -*-
"""`[latest-value]` 静态规则的故障注入自检（第 42 轮，方案 P3）。

用法：python3 tools/verify_latest_value_rule.py
返回码 0 表示：每条危险退化都让 verify.py **变红**、对照不误报、
且源码还原后逐字节一致。

为什么必须证明"规则会红"
------------------------
本工程的信条是"在缺陷代码上不失败的测试没有价值"。静态规则同理：
一条**从来不会报错**的规则等于没有规则，而它还会给人"已经守住了"的错觉。
因此每个变异都对应规则的一条判据，逐个改坏，逐个确认 verify.py 真的报了。

变异清单（每条都必须被 `[latest-value]` 捕获）
--------------------------------------------
  M1  删掉 frameGeneration() 的声明
      -> 判据 A：消费者无法判别帧的新旧，会反复处理同一帧或处理旧帧
  M2  代际号自增从与 lastFrame 同临界区里**挪出去**（放到 if 块之外）
      -> 判据 A：消费者可能读到"新代际号 + 旧帧"，防旧帧机制静默失效
  M3  frameGeneration() 不返回代际号（改成常量）
      -> 判据 A：代际号恒为 0，消费者永远认为"没新帧"
  M4  frameSize() 退化成 `return getFrame().size();`
      -> 判据 B：为读两个整数深拷贝整幅图（1.2 MB），P3 要消除的浪费
  M5  frameSize() 里出现 QImage
      -> 判据 B：本函数只应返回 QSize，不应触碰图像数据
  M6  拿掉人脸检测的丢弃式背压闸门（去掉 compare_exchange）
      -> 判据 C：任务会在无界队列里堆积，结果来自越来越旧的帧
  M7  拿掉「取消式」闸门（audio_manager 的 stop() + append 登记）
      -> 判据 C：另一种正确的闸门形态，同样不许缺

对照（必须保持绿，即不误报）
---------------------------
  C1  加无关注释
  C2  调整空白/缩进
  C3  在**无关函数**里加一个 QtConcurrent::run，但它自带丢弃式闸门
      —— 证明规则是按"提交点所在函数"判定，不是按文件判定

注意
----
需要能读仓库源码即可，**不需要编译**（这是纯静态规则的夹具）。
跑之前请确认没有别的夹具在改写源码 —— 本脚本自带文件锁。
锁忙返回码为 3（与其他夹具一致）。
"""
import io
import os
import subprocess
import sys

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
T = os.path.dirname(HERE)
PY = sys.executable

CAM_H = os.path.join(T, "src", "core", "vision", "v4l2_camera.h")
CAM_CPP = os.path.join(T, "src", "core", "vision", "v4l2_camera.cpp")
WIDGET_CPP = os.path.join(T, "src", "ui", "widget.cpp")
AUDIO_CPP = os.path.join(T, "src", "managers", "audio_manager.cpp")

FILES = [CAM_H, CAM_CPP, WIDGET_CPP, AUDIO_CPP]

sys.path.insert(0, HERE)
from fixture_pristine import PristineGuard  # noqa: E402

_GUARD = PristineGuard(files=FILES, tag='p3latest',
                       lock_name='fi37_rewrite.lock')
_ORIG = _GUARD.orig
_HEALED = []


def read(p):
    return io.open(p, encoding="utf-8", errors="replace").read()


def write(p, s):
    io.open(p, "w", encoding="utf-8", newline="\n").write(s)


def run_verify():
    """返回 (rc, [latest-value 相关的失败行])。"""
    r = subprocess.run([PY, os.path.join(T, "tools", "verify.py"), T],
                       capture_output=True, cwd=T)
    out = (r.stdout + r.stderr).decode("utf-8", "replace")
    lines = [l.strip() for l in out.splitlines()
             if "[latest-value]" in l and l.strip().startswith(("X", "!"))]
    return r.returncode, lines


# ---------------------------------------------------------------------------
# 变异定义：(名字, 期望失败的判据子串, [(文件, 旧, 新), ...])
#
# 判据子串必须**足够具体**，不能只写 "[latest-value]"：那样任何一条
# latest-value 失败都会算命中，M4（frameSize 深拷贝）可能被 M2 引发的
# 失败"顶包"通过 —— 那是假绿。这里逐条写清是哪个函数/哪条判据。
# ---------------------------------------------------------------------------
MUT = [
    ("删掉 frameGeneration() 的声明", "未提供 frameGeneration()",
     [(CAM_H,
       "    quint64 frameGeneration() const;",
       "    // quint64 frameGeneration() const;  // 变异：删掉声明")]),

    ("代际号自增挪出 lastFrame 的临界区", "没有和 lastFrame 的赋值放在同一个",
     # 注意：这里的锚点在第 45 轮清理掉 `// 逐行注释：…` 噪声后**同步更新过**。
     # 那些机器生成的注释曾被当成锚点文本的一部分，清理时如果不改这里，
     # 夹具会报"锚点出现 0 次"——幸好它**报错**而不是静默跳过（见 README §78）。
     [(CAM_CPP,
       "        lastFrame = img;\n"
       "        // 代际号与帧**在同一临界区内**自增，消费者才能把\"代际号\"当作\n"
       "        // \"这一帧\"的可靠标识（见头文件里 frameGeneration 的说明）。\n"
       "        ++lastFrameGeneration;\n"
       "    }",
       "        lastFrame = img;\n"
       "    }\n"
       "    // 变异：自增搬到了临界区之外\n"
       "    if (loaded) { ++lastFrameGeneration; }")]),

    ("frameGeneration() 不返回代际号", "没有返回代际号成员",
     [(CAM_CPP,
       "QSize V4L2Camera::frameSize() const {",
       "quint64 V4L2Camera::frameGeneration() const { return 0; }\n\n"
       "QSize V4L2Camera::frameSize() const {")]),

    ("frameSize() 退化成 getFrame().size()", "里调用了 getFrame()",
     [(CAM_CPP,
       "    // QImage::size() 对空图返回 QSize(0,0)，因此没有帧时天然给出 (0,0)，\n"
       "    // 无需额外分支 —— 与\"先 getFrame() 判 isNull() 再取 size()\"等价。\n"
       "    return lastFrame.size();",
       "    return getFrame().size();")]),

    ("frameSize() 里出现 QImage", "里出现了 QImage",
     [(CAM_CPP,
       "    return lastFrame.size();",
       "    const QImage snapshot = lastFrame;\n"
       "    return snapshot.size();")]),

    ("拿掉人脸检测的丢弃式背压闸门", "没有『丢弃式』背压闸门",
     [(WIDGET_CPP,
       "        std::shared_ptr<std::atomic<bool>> running = m_faceDetectRunning;\n"
       "        bool expected = false;\n"
       "        if (!running->compare_exchange_strong(expected, true)) {\n"
       "            return; // 上一帧还在检测，直接放弃本次（画面/识别不受影响）\n"
       "        }",
       "        std::shared_ptr<std::atomic<bool>> running = m_faceDetectRunning;\n"
       "        // 变异：删掉背压闸门，回到即发即忘\n"
       "        running->store(true);")]),

    ("拿掉 audio_manager 的取消式闸门", "也没有『取消式』闸门",
     [(AUDIO_CPP, "    stop(); // 打断上一次",
       "    // 变异：不再取消上一次播放\n"
       "    // 注意 isRunning() 与 append() 仍在，缺的正是\"取消上一次\"这一半")]),

    # 这一条是判据 C 的**判别力**证明，别删：
    # widget.cpp 里已经有一个**带闸门**的 QtConcurrent 提交点。如果规则写成
    # "这个文件里有没有闸门"（文件级），那么再加一个**不带闸门**的提交点也
    # 会被判绿 —— 假绿，而且是最危险的那种：真实项目就是这样一点点长出
    # 无界任务堆积的。只有按"提交点所在函数"判定才会红。
    ("在已有闸门的文件里再加一个无闸门提交点", "没有『丢弃式』背压闸门",
     [(WIDGET_CPP,
       "void Widget::onFaceDetectResult(const QVector<cv::Rect>& detected, int imgW, int imgH)\n{",
       "void Widget::spawnUngatedProbe()\n{\n"
       "    // 变异：即发即忘，无任何背压\n"
       "    QtConcurrent::run([]() { /* probe */ });\n"
       "}\n\n"
       "void Widget::onFaceDetectResult(const QVector<cv::Rect>& detected, int imgW, int imgH)\n{")]),
]


# ---------------------------------------------------------------------------
# 对照：这些**不该**让规则报错（证明没有误报）
# ---------------------------------------------------------------------------
CTRL = [
    ("对照：加无关注释",
     [(CAM_H,
       "class V4L2Camera",
       "// 无关注释（对照）\nclass V4L2Camera")]),

    ("对照：调整空白与缩进",
     [(CAM_CPP,
       "QImage V4L2Camera::getFrame() {",
       "QImage V4L2Camera::getFrame()\n{")]),
]


def _prepare_backup():
    _HEALED[:] = _GUARD.prepare()
    return _HEALED


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
    _prepare_backup()
    _GUARD.install_guards()
    if _HEALED:
        print("!! 检测到上一次运行被中断，已用磁盘原始副本自愈: %s"
              % ", ".join(_HEALED))

    # 预检：所有锚点必须逐字节存在，否则夹具已过期
    for name, _expect, edits in MUT:
        for path, old, _new in edits:
            if _ORIG[path].count(old) != 1:
                print("[夹具错误] %s 的锚点在 %s 出现 %d 次（期望 1）"
                      % (name, os.path.basename(path), _ORIG[path].count(old)))
                return 2
    for name, edits in CTRL:
        for path, old, _new in edits:
            if _ORIG[path].count(old) != 1:
                print("[夹具错误·对照] %s 的锚点在 %s 出现 %d 次（期望 1）"
                      % (name, os.path.basename(path), _ORIG[path].count(old)))
                return 2

    rc, _ = run_verify()
    print("基线 rc = %d" % rc)
    if rc != 0:
        print("*** 基线就不通过，无法进行故障注入")
        return 2
    print()

    problems = []
    try:
        for name, expect, edits in MUT:
            staged = dict(_ORIG)
            anchor_ok = True
            for path, old, new in edits:
                if staged[path].count(old) != 1:
                    anchor_ok = False
                    break
                staged[path] = staged[path].replace(old, new, 1)
            if not anchor_ok:
                print("  %-40s *** 锚点在本轮失效" % name)
                problems.append(name)
                continue
            try:
                for path in set(p for p, _o, _n in edits):
                    write(path, staged[path])
                changed = all(read(p) != _ORIG[p]
                              for p in set(p for p, _o, _n in edits))
                if not changed:
                    print("  %-40s *** 变异未落到磁盘" % name)
                    problems.append(name)
                    continue

                rc, lines = run_verify()
                hit = any(expect in l for l in lines)
                if rc != 0 and hit:
                    print("  %-40s 被抓到 ✓" % name)
                else:
                    print("  %-40s *** 未被抓到（假绿）rc=%d" % (name, rc))
                    for l in lines[:3]:
                        print("      " + l[:120])
                    problems.append(name)
            finally:
                for path in set(p for p, _o, _n in edits):
                    write(path, _ORIG[path])

        # ---- 对照：必须保持绿（不误报）----
        for name, edits in CTRL:
            staged = dict(_ORIG)
            ok = True
            for path, old, new in edits:
                if staged[path].count(old) != 1:
                    ok = False
                    break
                staged[path] = staged[path].replace(old, new, 1)
            if not ok:
                print("  %-40s *** 对照锚点失效" % name)
                problems.append(name)
                continue
            try:
                for path in set(p for p, _o, _n in edits):
                    write(path, staged[path])
                rc, lines = run_verify()
                if rc == 0:
                    print("  %-40s 通过 ✓（不误报）" % name)
                else:
                    print("  %-40s *** 误报：rc=%d" % (name, rc))
                    for l in lines[:3]:
                        print("      " + l[:120])
                    problems.append(name)
            finally:
                for path in set(p for p, _o, _n in edits):
                    write(path, _ORIG[path])
    finally:
        _GUARD.restore()

    same = all(read(p) == _ORIG[p] for p in FILES)
    rc, _ = run_verify()
    print()
    print("还原逐字节一致:", same)
    print("还原后: %s" % ("结论: 全部检查通过" if rc == 0
                       else "*** 仍不通过 rc=%d" % rc))
    print()
    print("变异 %d 个，对照 %d 个，致命问题 %d 个"
          % (len(MUT), len(CTRL), len(problems)))
    if problems:
        print("问题清单:", problems)
    return 1 if (problems or not same or rc != 0) else 0


if __name__ == "__main__":
    sys.exit(main())

# -*- coding: utf-8 -*-
"""verify.py 的 [qt5] 规则（invokeMethod 的 functor 重载）故障注入自检。

用法：python3 tools/verify_qt5_invoke_method_rule.py
返回码 0 表示：每个危险退化都被抓到、对照未误报、且还原后逐字节一致。

为什么需要这个夹具
------------------
这条规则很容易写成**恒真**或**恒假**：

  * 只看函数名 `QMetaObject::invokeMethod` → 会把合法的字符串重载
    （`invokeMethod(obj, "slot", ...)`，自 Qt 5.0 起就有）一起报掉，是误报；
  * 只在 `widget.cpp` 里查那一处 → 别处再写一个 functor 重载就漏报。

所以夹具要同时证明三件事：**能抓真违规**、**不误报合法写法**、
**判别力不依赖具体文件**。

变异（必须被抓到，报 X [qt5]）
  M1 widget.cpp 退回 functor 重载（**当初的真实缺陷**，Qt 5.10+ API）
  M2 在另一个源文件里新写一处 functor 重载（证明规则不绑定 widget.cpp）
  M3 注入一处 5.15 才有的信号，证明既有 [qt5] 判别力仍在

对照（必须通过：无 X [qt5]）
  C1 只把 functor 重载写进**注释**
  C2 只把 functor 重载写进**字符串字面量**

跨平台说明
----------
与多数夹具不同，本夹具不写死 Windows 路径、不调用 msys64：
仓库根由 `__file__` 推导，Python 解释器用 `sys.executable`，
因此 Linux/CI 上可直接运行（见交接文档 P8：夹具跨平台是一类独立工作）。
"""
import io
import os
import re
import subprocess
import sys

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
T = os.path.dirname(HERE)                       # 仓库根 = tools/ 的上一级
VERIFY = os.path.join(T, "tools", "verify.py")
WIDGET = os.path.join(T, "src", "ui", "widget.cpp")
OTHER = os.path.join(T, "src", "managers", "password_manager.cpp")
PY = sys.executable

# widget.cpp 里**修好后**的写法（锚点）。它必须逐字节存在，否则夹具已过期。
FIXED = (
    "            QTimer::singleShot(0, ctx, [self, qrects, w, h]() {\n"
    "                if (!self.isNull()) {\n"
    "                    self->onFaceDetectResult(qrects, w, h);\n"
    "                }\n"
    "            });\n"
)
# 退回到 Qt 5.10+ 的 functor 重载（当初的真实缺陷）
BROKEN = (
    "            QMetaObject::invokeMethod(ctx, [self, qrects, w, h]() {\n"
    "                if (!self.isNull()) {\n"
    "                    self->onFaceDetectResult(qrects, w, h);\n"
    "                }\n"
    "            }, Qt::QueuedConnection);\n"
)

INJECT_FUNCTOR = (
    "\n// 夹具注入：另一个文件里的 functor 重载（应被 [qt5] 规则抓到）\n"
    "static void fixtureInvokeOnOtherFile(QObject* o)\n"
    "{\n"
    "    QMetaObject::invokeMethod(o, []() {});\n"
    "}\n"
)
INJECT_515 = (
    "\n// 夹具注入：Qt 5.15 才有的信号（对照既有 [qt5] 判别力）\n"
    "static void fixtureErrorOccurredHook(QAbstractSocket* s)\n"
    "{\n"
    "    QObject::connect(s, &QAbstractSocket::errorOccurred, s, []() {});\n"
    "}\n"
)
INJECT_COMMENT = (
    "\n// 夹具对照：注释里的 QMetaObject::invokeMethod(ctx, [](){}) 不算违规\n"
)
INJECT_STRING = (
    "\nstatic const char* fixtureRuleNote =\n"
    "    \"QMetaObject::invokeMethod(ctx, [](){});\";\n"
)


def read(p):
    return io.open(p, encoding="utf-8", errors="replace").read()


def write(p, s):
    io.open(p, "w", encoding="utf-8", newline="\n").write(s)


def run_verify():
    r = subprocess.run([PY, VERIFY, T], capture_output=True, cwd=T)
    out = (r.stdout + r.stderr).decode("utf-8", "replace")
    x = re.findall(r"^\s*X\s*\[(qt5)\]", out, re.M)
    return r.returncode, x


_ORIG = {p: read(p) for p in (WIDGET, OTHER, VERIFY)}
assert FIXED in _ORIG[WIDGET], (
    "锚点未找到：widget.cpp 里的 QTimer::singleShot(0, ctx, ...) 写法变了，"
    "本夹具需要同步更新")
assert os.path.exists(VERIFY)


def sub(path, old, new, count=1):
    o = _ORIG[path]
    assert o.count(old) >= 1, "锚点未找到: %r in %s" % (old[:60], path)
    write(path, o.replace(old, new, count))
    return lambda: write(path, o)


def app(path, text):
    o = _ORIG[path]
    write(path, o + text)
    return lambda: write(path, o)


def restore_all():
    for p, s in _ORIG.items():
        write(p, s)


# ---- 变异/对照定义：(名字, 施加函数, 期望前缀 or None) ----
MUT = [
    ("widget.cpp 退回 functor 重载（当初的真实缺陷）",
     lambda: sub(WIDGET, FIXED, BROKEN), "qt5"),

    ("另一个源文件里新写 functor 重载（不绑定 widget.cpp）",
     lambda: app(OTHER, INJECT_FUNCTOR), "qt5"),

    ("注入 Qt 5.15 信号（既有 [qt5] 判别力仍在）",
     lambda: app(OTHER, INJECT_515), "qt5"),

    ("对照：functor 重载只写在注释里（不得误报）",
     lambda: app(OTHER, INJECT_COMMENT), None),

    ("对照：functor 重载只写在字符串字面量里（不得误报）",
     lambda: app(OTHER, INJECT_STRING), None),
]


def main():
    if len(sys.argv) > 1:
        print("提示: 本脚本不需要参数，已忽略: %s" % " ".join(sys.argv[1:]))

    print("仓库根: %s" % T)
    print()

    baseline_rc, baseline_x = run_verify()
    print("基线: rc=%d, X [qt5] 行数=%d" % (baseline_rc, len(baseline_x)))
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
                    print("  [夹具错误] %-44s 文件未变" % name)
                    problems.append(name)
                    continue
                rc, x = run_verify()
                if expect is None:
                    if rc == 0 and not x:
                        print("  %-44s 通过 ✓（不误报）" % name)
                    else:
                        print("  %-44s *** 误报: X=%s rc=%d" % (name, x, rc))
                        problems.append(name)
                else:
                    if expect in x:
                        print("  %-44s 被抓到 ✓ [%s]" % (name, expect))
                    else:
                        print("  %-44s *** 未被抓到（假绿）X=%s" % (name, x))
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
    print("还原后 rc=%d, X [qt5] 行数=%d" % (rc, len(x)))
    print()
    print("变异/对照 %d 个，致命问题 %d 个" % (len(MUT), len(problems)))
    if problems:
        print("问题清单:", problems)
    return 1 if (problems or not same or rc != 0 or x) else 0


if __name__ == "__main__":
    sys.exit(main())

# -*- coding: utf-8 -*-
"""第 30 轮两条新 verify.py 规则的故障注入自检（纯 Python，Windows 路径）。

用法：python tools/verify_v4l2_rule.py
返回码 0 表示：每个危险退化都被抓到，且对照未误报。

覆盖两条规则
------------
  [v4l2]          check_v4l2_camera_failure_paths_are_testable
  [harness-crash] check_fault_harnesses_classify_crashes_correctly

要证明的核心命题
----------------
这些规则的价值全在"能不能抓到**修复回退**与**假绿判据**"。
每个变异都朝真实的危险退化去改，并要求规则以 **X 行**报出来。

变异
  1. 删掉 QBUF 失败分支的 munmap（**当初的真实缺陷**）
  2. 删掉 STREAMON 失败分支的 munmap（**当初的真实缺陷**）
  3. 测试文件从 CMakeLists 移除（写了但不跑）
  4. 假 syscall 层从 CMakeLists 移除
  5. --wrap=mmap 从 CMakeLists 移除（假层不会被调用）
  6. 关键断言 qbufFailureMustStillUnmap 改名（词元边界）
  7. 关键断言 undecodableFrameStillRequeuesBuffer 改名
  8. 删掉故障注入夹具文件
  9. 删掉测试源文件
 10. 夹具退回猜测式崩溃判据 returncode not in (0,1)
 11. 夹具用 crashed = 但缺少 "and not failed"

对照（必须通过）
  C1. 只加无关注释
  C2. 在 verify.py 里改一条无关的告警文案
"""
import io
import os
import re
import shutil
import subprocess
import sys

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

if len(sys.argv) > 1:
    print("提示: 本脚本不需要参数，已忽略: %s" % " ".join(sys.argv[1:]))

T = r'C:\Users\user\Desktop\linux_ai\Accessterminal_T113'
D = r'C:\Users\user\Desktop\linux_ai\Accessterminal_desktop'
PY = r'C:\Users\user\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'
VERIFY = os.path.join(T, 'tools', 'verify.py')
SRC = os.path.join(T, 'src', 'core', 'vision', 'v4l2_camera.cpp')
TEST = os.path.join(T, 'tests', 'test_v4l2_camera.cpp')
FAKE = os.path.join(T, 'tests', 'fake_v4l2_io.cpp')
CM = os.path.join(T, 'tests', 'CMakeLists.txt')
HARNESS = os.path.join(T, 'tools', 'verify_v4l2_test.py')

MUNMAP = '        munmap(v4l2_buffer, v4l2_buffer_length);\n'


def read(p):
    # 变异会删文件，"证明变异生效"的检查必须容忍文件不存在。
    if not os.path.exists(p):
        return None
    return io.open(p, encoding='utf-8', errors='replace').read()


def write(p, s):
    io.open(p, 'w', encoding='utf-8', newline='\n').write(s)


def run_verify():
    r = subprocess.run([PY, VERIFY, T, D], capture_output=True, cwd=T)
    out = (r.stdout + r.stderr).decode('utf-8', 'replace')
    x = re.findall(r'^\s*X\s*\[(v4l2|harness-crash)\]', out, re.M)
    return r.returncode, x


_ORIG = {p: read(p) for p in (SRC, TEST, FAKE, CM, HARNESS, VERIFY)}


def sub(path, old, new, count=1):
    """在 path 里把 old 换成 new；返回还原用的闭包。"""
    o = _ORIG[path]
    assert o.count(old) >= 1, '锚点未找到: %r in %s' % (old[:60], path)
    write(path, o.replace(old, new, count))
    return lambda: write(path, o)


def del_file(path):
    os.remove(path)
    return lambda: write(path, _ORIG[path])


def m_remove_fake():
    """把假 syscall 层从 add_executable 的**源文件列表**里移除。

    这一条最初是假绿：规则当时在**整文件**里查 "fake_v4l2_io.cpp" 子串，
    而该文件名在本文件的**注释**里也出现（解释为什么用可编程假层），
    所以删掉真正的那一行后子串仍在匹配。规则已改为只在
    add_executable(...) 的源文件列表里查（并先去掉注释）。
    """
    o = _ORIG[CM]
    write(CM, o.replace('    fake_v4l2_io.cpp\n', '', 1))
    return lambda: write(CM, o)


def m_remove_wrap_ioctl():
    """只删 --wrap=ioctl：目标块里 --wrap 不再完整。"""
    o = _ORIG[CM]
    write(CM, o.replace('    -Wl,--wrap=ioctl\n', '', 1))
    return lambda: write(CM, o)


# ---- 变异定义：(名字, 施加函数, 期望触发的规则前缀) ----
def m_qbuf_unmap():
    i = _ORIG[SRC].index('    if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0) {')
    j = _ORIG[SRC].index(MUNMAP, i)
    return sub(SRC, _ORIG[SRC][j:j + len(MUNMAP)], '')


def m_stream_unmap():
    i = _ORIG[SRC].index('    if (ioctl(v4l2_fd, VIDIOC_STREAMON, &type) < 0) {')
    j = _ORIG[SRC].index(MUNMAP, i)
    return sub(SRC, _ORIG[SRC][j:j + len(MUNMAP)], '')


MUT = [
    ("删掉 QBUF 失败分支的 munmap（当初的真实缺陷）",
     m_qbuf_unmap, 'v4l2'),

    ("删掉 STREAMON 失败分支的 munmap（当初的真实缺陷）",
     m_stream_unmap, 'v4l2'),

    ("测试文件从 CMakeLists 移除（写了但不跑）",
     lambda: sub(CM, 'test_v4l2_camera.cpp\n', ''), 'v4l2'),

    ("假 syscall 层从 CMakeLists 源文件列表移除（注释仍在）",
     m_remove_fake, 'v4l2'),

    ("--wrap 不完整（删掉 --wrap=ioctl）",
     m_remove_wrap_ioctl, 'v4l2'),

    ("--wrap=mmap 从 CMakeLists 移除（假层不会被调用）",
     lambda: sub(CM, '    -Wl,--wrap=mmap\n', ''), 'v4l2'),

    ("关键断言 qbufFailureMustStillUnmap 改名（词元边界）",
     lambda: sub(TEST, 'qbufFailureMustStillUnmap(', 'qbufFailureMustStillUnmap_removed('),
     'v4l2'),

    ("关键断言 undecodableFrameStillRequeuesBuffer 改名",
     lambda: sub(TEST, 'undecodableFrameStillRequeuesBuffer(',
                 'undecodableFrameStillRequeuesBuffer_removed('), 'v4l2'),

    ("删掉故障注入夹具文件",
     lambda: del_file(HARNESS), 'v4l2'),

    ("删掉测试源文件",
     lambda: del_file(TEST), 'v4l2'),

    ("夹具退回猜测式崩溃判据 returncode not in (0,1)",
     lambda: sub(HARNESS, 'crashed = rr.returncode != 0 and not failed',
                 'crashed = rr.returncode not in (0, 1)'), 'harness-crash'),

    ("夹具用 crashed = 但缺少 and not failed",
     lambda: sub(HARNESS, 'crashed = rr.returncode != 0 and not failed',
                 'crashed = rr.returncode != 0'), 'harness-crash'),

    ("对照：在 v4l2_camera.cpp 加无关注释",
     lambda: sub(SRC, 'bool V4L2Camera::open() {',
                 'bool V4L2Camera::open() {\n    // 无关注释'), None),

    ("对照：改 verify.py 里一条无关注释文案",
     lambda: sub(VERIFY, '本规则钉住：tools/verify_*.py 里不得再出现猜测式的崩溃判据',
                 '本规则钉住：tools/verify_*.py 里不得再使用猜测式的崩溃判据'),
     None),
]


def restore_all():
    for p, o in _ORIG.items():
        if not os.path.exists(p) or read(p) != o:
            write(p, o)


def main():
    rc, x = run_verify()
    print('基线: rc=%d, X 行=%s' % (rc, x))
    if rc != 0 or x:
        print('基线不通过，先修基线再跑本夹具')
        return 2
    print()

    problems = []
    try:
        for name, apply_mut, expect in MUT:
            undo = apply_mut()
            try:
                # 证明变异真的落到了文件上
                changed = any(read(p) != _ORIG[p] for p in _ORIG)
                if not changed:
                    print('  [夹具错误] %-48s 文件未变' % name)
                    problems.append(name)
                    continue
                rc, x = run_verify()
                if expect is None:
                    if rc == 0 and not x:
                        print('  %-48s 通过 ✓（不误报）' % name)
                    else:
                        print('  %-48s *** 误报: %s' % (name, x))
                        problems.append(name)
                else:
                    if expect in x:
                        print('  %-48s 被抓到 ✓ [%s]' % (name, expect))
                    else:
                        print('  %-48s *** 未被抓到（假绿）X=%s' % (name, x))
                        problems.append(name)
            finally:
                undo()
                restore_all()
    finally:
        restore_all()

    same = all(read(p) == _ORIG[p] for p in _ORIG)
    print()
    rc, x = run_verify()
    print('还原逐字节一致:', same)
    print('还原后 rc=%d, X 行=%s' % (rc, x))
    print()
    print('变异 %d 个，致命问题 %d 个' % (len(MUT), len(problems)))
    if problems:
        print('问题清单:', problems)
    return 1 if (problems or not same or rc != 0 or x) else 0


if __name__ == '__main__':
    sys.exit(main())

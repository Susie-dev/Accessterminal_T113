# -*- coding: utf-8 -*-
"""check_gpio_failure_paths_are_testable 的故障注入自检。

用法：python tools/verify_gpio_rule.py

用例：
  0. 当前仓库通过
  1. 删除假实现头文件 -> 应失败
  2. 删除测试文件 -> 应失败
  3. 测试未接进 CMakeLists -> 应失败
  4. 假实现未列进 CMakeLists -> 应失败
  5. 去掉 busyLineCount -> 应失败
  6. 去掉 callLog -> 应失败
  7. 线号上限改回 64 -> 应失败
  8. 删除故障注入夹具 -> 应失败
  9. 对照：加无关注释 -> 应通过
 10. 对照：把 ownerOf 改名（只触发告警，不应失败）-> 只告警

注意 verify.py 的输出格式是**大写** X 与 !（第 27 轮踩过：按小写 'x' 判断
会把所有失败都漏掉，误报成"规则未触发"）。
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

if len(sys.argv) > 1:
    print("提示: 本脚本不需要参数，已忽略: %s" % " ".join(sys.argv[1:]))

T = r'C:\Users\user\Desktop\linux_ai\Accessterminal_T113'
VERIFY = os.path.join(T, 'tools', 'verify.py')
PY = sys.executable

HDR = os.path.join('tests', 'fake_gpiod.h')
IMPL = os.path.join('tests', 'fake_gpiod.cpp')
TF = os.path.join('tests', 'test_gpio.cpp')
CM = os.path.join('tests', 'CMakeLists.txt')
FX = os.path.join('tools', 'verify_gpio_test.py')


def run_in(root):
    r = subprocess.run([PY, VERIFY, root], capture_output=True, cwd=root)
    out = (r.stdout + r.stderr).decode('utf-8', 'replace')
    fails = re.findall(r'^\s*X\s*\[gpio-test\]', out, re.M | re.I)
    warns = re.findall(r'^\s*!\s*\[gpio-test\]', out, re.M)
    return fails, warns


def copy_repo():
    tmp = tempfile.mkdtemp(prefix='gpiolog_')
    for rel in ('tools', 'tests', 'src'):
        src = os.path.join(T, rel)
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(tmp, rel))
    return tmp


def del_file(rel):
    return lambda root: os.remove(os.path.join(root, rel))


def patch(rel, old, new):
    def f(root):
        p = os.path.join(root, rel)
        s = io.open(p, encoding='utf-8').read()
        assert s.count(old) >= 1, '锚点缺失: %r' % old[:60]
        io.open(p, 'w', encoding='utf-8', newline='\n').write(s.replace(old, new))
    return f


CASES = [
    ('删除假实现头文件', del_file(HDR), 'fail'),
    ('删除测试文件', del_file(TF), 'fail'),
    ('测试未接进 CMakeLists',
     patch(CM, 'test_gpio.cpp', 'test_renamed_gpio.cpp'), 'fail'),
    ('假实现未列进 CMakeLists',
     patch(CM, 'fake_gpiod.cpp', 'fake_renamed_gpiod.cpp'), 'fail'),
    ('去掉 busyLineCount', patch(IMPL, 'busyLineCount', 'lineCnt'), 'fail'),
    ('去掉 callLog', patch(IMPL, 'callLog', 'theLog'), 'fail'),
    ('线号上限改回 64',
     patch(IMPL, 'const int kMaxLines = 1024;', 'const int kMaxLines = 64;'),
     'fail'),
    ('删除故障注入夹具', del_file(FX), 'fail'),
    ('对照：加无关注释',
     patch(TF, 'QTEST_MAIN(TestGpio)', '// 无关注释\nQTEST_MAIN(TestGpio)'),
     'pass'),
    ('对照：ownerOf 改名（应只告警）',
     patch(IMPL, 'ownerOf', 'findOwner'), 'warn'),
]

problems = []
print('=== 0) 当前仓库 ===')
f0, w0 = run_in(T)
print('  失败=%d 告警=%d' % (len(f0), len(w0)))
if f0:
    problems.append('基线不应失败')
    print('  ***', f0[0][:200])

for name, mutate, want in CASES:
    tmp = copy_repo()
    try:
        mutate(tmp)
        f, w = run_in(tmp)
        if want == 'fail':
            ok = len(f) > 0
        elif want == 'warn':
            ok = len(w) > 0 and len(f) == 0
        else:
            ok = len(f) == 0 and len(w) == 0
        if ok:
            print('  %-34s %s ✓' % (name, '失败' if want == 'fail'
                                    else ('告警' if want == 'warn' else '通过')))
        else:
            print('  %-34s *** 期望 %s，实得 失败=%d 告警=%d'
                  % (name, want, len(f), len(w)))
            problems.append(name)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

print()
print('用例 %d 个，致命问题 %d 个' % (len(CASES), len(problems)))
if problems:
    print('问题清单:', problems)
sys.exit(1 if problems else 0)

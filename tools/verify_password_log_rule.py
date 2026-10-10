# -*- coding: utf-8 -*-
"""check_password_log_regression_is_enforced 的故障注入自检。

用法：python tools/verify_password_log_rule.py
返回码 0 表示：每条缺失都被规则识别，且两个对照未误报。

用例：
  0. 当前仓库通过
  1. 测试文件不存在 -> 应失败
  2. 测试没接进 CMakeLists -> 应失败
  3. 去掉阳性对照断言 -> 应失败
  4. 去掉夹具自证（往返断言）-> 应失败
  5. 去掉故障注入脚本 -> 应失败
  6. 对照：只加无关注释 -> 应通过
  7. 对照：把逐一前缀断言改为固定 left(4) 单查 -> 只应告警、不应失败
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

import tempfile

T = r'C:\Users\user\Desktop\linux_ai\Accessterminal_T113'
VERIFY = os.path.join(T, 'tools', 'verify.py')
TF_REL = os.path.join('tests', 'test_password_manager.cpp')
CM_REL = os.path.join('tests', 'CMakeLists.txt')
FX_REL = os.path.join('tools', 'verify_password_log_test.py')
PY = sys.executable


def run_in(root):
    # verify.py 的实际格式（务必按真实输出写，别凭印象）：
    #     失败: "  X [规则名] 说明"
    #     告警: "  ! [规则名] 说明"
    # 上一版按小写 'x' 判断，于是所有 fail 都没被识别 —— 5 个用例被误报成
    # "规则未触发"。夹具的解析逻辑同样要自证。
    r = subprocess.run([PY, VERIFY, root], capture_output=True, cwd=root)
    out = (r.stdout + r.stderr).decode('utf-8', 'replace')
    fails = re.findall(r'^\s*X\s*\[password-log\]', out, re.M | re.I)
    warns = re.findall(r'^\s*!\s*\[password-log\]', out, re.M)
    return fails, warns


def copy_repo():
    tmp = tempfile.mkdtemp(prefix='pwlog_')
    for rel in ('tools', 'tests', 'src'):
        src = os.path.join(T, rel)
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(tmp, rel))
    return tmp


problems = []
cases = []


def expect_fail(name, mutate):
    cases.append((name, mutate, 'fail'))


def expect_warn(name, mutate):
    cases.append((name, mutate, 'warn'))


def expect_pass(name, mutate):
    cases.append((name, mutate, 'pass'))


def del_file(rel):
    return lambda root: os.remove(os.path.join(root, rel))


def patch(rel, old, new):
    def f(root):
        p = os.path.join(root, rel)
        s = io.open(p, encoding='utf-8').read()
        assert s.count(old) >= 1, '锚点缺失: %r' % old[:60]
        io.open(p, 'w', encoding='utf-8', newline='\n').write(
            s.replace(old, new))
    return f


expect_fail('删除测试文件', del_file(TF_REL))
expect_fail('测试未接进 CMakeLists',
            patch(CM_REL, 'test_password_manager.cpp', 'test_renamed_x.cpp'))
expect_fail('去掉阳性对照断言',
            patch(TF_REL, 'MsgCapture::count() > 0', 'true'))
expect_fail('去掉夹具自证往返断言',
            patch(TF_REL, 'QCOMPARE(AppConfig::instance().doorPassword(), pwd);',
                  ';'))
expect_fail('删除故障注入脚本', del_file(FX_REL))
expect_pass('加无关注释（对照）',
            patch(TF_REL, 'QTEST_MAIN(TestPasswordManager)',
                  '// 无关注释\nQTEST_MAIN(TestPasswordManager)'))
expect_warn('逐一前缀断言改为单查 left(4)（应只告警）',
            patch(TF_REL, 'for (int n = 1; n <= secret.size(); ++n) {',
                  'for (int n = 4; n <= 4; ++n) {'))

print('=== 0) 当前仓库 ===')
f0, w0 = run_in(T)
print('  失败=%d 告警=%d' % (len(f0), len(w0)))
if f0:
    problems.append('基线不应失败')
    print('  ***', f0[0][:200])

for name, mutate, want in cases:
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
            print('  %-40s %s ✓' % (name, '失败' if want == 'fail'
                                    else ('告警' if want == 'warn' else '通过')))
        else:
            print('  %-40s *** 期望 %s，实得 失败=%d 告警=%d'
                  % (name, want, len(f), len(w)))
            if f:
                print('        ->', f[0][:180])
            problems.append(name)
    finally:
        shutil.rmtree(tmp, ignore_errors=True,
                      onerror=lambda *a: None)

print()
print('用例 %d 个，致命问题 %d 个' % (len(cases), len(problems)))
if problems:
    print('问题清单:', problems)
sys.exit(1 if problems else 0)

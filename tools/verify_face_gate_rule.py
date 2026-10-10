# -*- coding: utf-8 -*-
"""check_face_gate_hazard_is_documented 的故障注入自检。

用法：python tools/verify_face_gate_rule.py

注意 verify.py 的输出格式：失败是大写 `  X [face-gate]`，告警是 `  ! [face-gate]`。
（第 27 轮踩过：按小写判断会把所有失败漏掉，误报成"规则未触发"。）
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

TF = os.path.join('tests', 'test_face_manager.cpp')
CM = os.path.join('tests', 'CMakeLists.txt')
FX = os.path.join('tools', 'verify_face_manager_test.py')
CFG = os.path.join('src', 'config', 'app_config.cpp')


def run_in(root):
    r = subprocess.run([PY, VERIFY, root], capture_output=True, cwd=root)
    out = (r.stdout + r.stderr).decode('utf-8', 'replace')
    fails = re.findall(r'^\s*X\s*\[face-gate\]', out, re.M | re.I)
    warns = re.findall(r'^\s*!\s*\[face-gate\]', out, re.M)
    return fails, warns


def copy_repo():
    tmp = tempfile.mkdtemp(prefix='facegate_')
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
    ('删除门禁测试文件', del_file(TF), 'fail'),
    ('测试未接进 CMakeLists',
     patch(CM, 'test_face_manager.cpp', 'test_renamed_face.cpp'), 'fail'),
    ('生产实现未接进 CMakeLists',
     patch(CM, 'managers/face_manager.cpp', 'managers/renamed_face.cpp'), 'fail'),
    ('去掉「阈值是唯一判据」断言',
     patch(TF, 'void thresholdIsTheOnlyGateWithSingleLabel()',
           'void thresholdIsTheOnlyGateWithSingleLabel_removed()'), 'fail'),
    ('去掉「隐患记录」用例',
     patch(TF, 'void samplesWithoutTrainingMakeRecognizeThrow_documentedHazard()',
           'void hazardRemoved()'), 'fail'),
    ('删除故障注入夹具', del_file(FX), 'fail'),
    ('对照：加无关注释',
     patch(TF, 'QTEST_MAIN(TestFaceManager)', '// 无关注释\nQTEST_MAIN(TestFaceManager)'),
     'pass'),
    ('对照：给阈值加范围约束（告警应消失）',
     patch(CFG, '    m_faceConfidenceThreshold = 90.0;',
           '    m_faceConfidenceThreshold = 90.0;\n'
           '    // 约束\n    (void)0;\n'
           '    m_faceConfidenceThreshold = qBound(1.0, m_faceConfidenceThreshold, 1000.0);'),
     'nowarn'),
]

problems = []
print('=== 0) 当前仓库 ===')
f0, w0 = run_in(T)
print('  失败=%d 告警=%d' % (len(f0), len(w0)))
if f0:
    problems.append('基线不应失败')
    print('  ***', f0[0][:200])
if not w0:
    problems.append('基线应有阈值告警（安全相关）')
    print('  *** 基线缺少预期的告警')

for name, mutate, want in CASES:
    tmp = copy_repo()
    try:
        mutate(tmp)
        f, w = run_in(tmp)
        if want == 'fail':
            ok = len(f) > 0
        elif want == 'nowarn':
            ok = len(w) == 0 and len(f) == 0
        else:
            ok = len(f) == 0 and len(w) == len(w0)
        if ok:
            print('  %-38s %s ✓' % (name, {'fail': '失败', 'pass': '通过',
                                           'nowarn': '告警消失'}[want]))
        else:
            print('  %-38s *** 期望 %s，实得 失败=%d 告警=%d'
                  % (name, want, len(f), len(w)))
            problems.append(name)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

print()
print('用例 %d 个，致命问题 %d 个' % (len(CASES), len(problems)))
if problems:
    print('问题清单:', problems)
sys.exit(1 if problems else 0)

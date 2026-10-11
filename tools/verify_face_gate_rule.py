# -*- coding: utf-8 -*-
"""check_face_gate_hazard_is_documented 的故障注入自检。

用法：python tools/verify_face_gate_rule.py

第 38 轮起，本规则对**阈值上限**的判据由"仅告警"升级为"断言"：实测表明
LBPH（create() 默认参数）的距离在 ~256 处饱和，阈值 >= 256 时任何图像
（包括完全陌生的脸）都判"通过"。因此上限必须**低于饱和点**，而不是
"小于 1e6" —— 旧判据会把同样致命的 `1000` 判为合格。

夹具的期望也随之改变：基线现在**不应有任何 face-gate 告警**，
而"把上限放回 1e9 / 1000"从"告警消失"变成"必须失败"。

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

# 仓库根由本脚本位置推出（原实现写死了 Windows 路径，换到 Linux 即无法运行）。
T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERIFY = os.path.join(T, 'tools', 'verify.py')
PY = sys.executable

TF = os.path.join('tests', 'test_face_manager.cpp')
CM = os.path.join('tests', 'CMakeLists.txt')
FX = os.path.join('tools', 'verify_face_manager_test.py')
# 阈值的**真实**夹取在头文件的访问器里（第 31 轮修正：原夹具只改 .cpp，
# 而 .cpp 里根本没有夹取，规则第一版也因此误报"没有任何范围约束"）。
CFG_H = os.path.join('src', 'config', 'app_config.h')


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


def set_upper(new_upper):
    """把访问器里的上限 150.0 换成给定值（只动代码那一行）。"""
    return patch(CFG_H, '0.0, 150.0)', '0.0, %s)' % new_upper)


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
    ('去掉「上限仍拒绝冒充者」用例',
     patch(TF, 'void maxAllowedThresholdStillRejectsStrangers()',
           'void maxAllowedRemoved()'), 'fail'),
    ('删除故障注入夹具', del_file(FX), 'fail'),
    ('对照：加无关注释',
     patch(TF, 'QTEST_MAIN(TestFaceManager)',
           '// 无关注释\nQTEST_MAIN(TestFaceManager)'),
     'pass'),
    # --- 上限判据：>= 饱和点(256) 一律失败 ---
    ('上限放回 1e9（应失败）', set_upper('1e9'), 'fail'),
    ('上限 1000.0（应失败，旧判据的漏网值）', set_upper('1000.0'), 'fail'),
    ('上限正好 256.0（应失败，饱和点边界）', set_upper('256.0'), 'fail'),
    # --- 边界另一侧：< 饱和点应通过本规则 ---
    ('上限 255.0（应通过，饱和点下方）', set_upper('255.0'), 'pass'),
    ('上限 150.0（应通过，本工程取值）', set_upper('150.0'), 'pass'),
    # --- 连夹取都去掉（原隐患的措辞）---
    ('去掉夹取（应失败）',
     patch(CFG_H,
           'double faceConfidenceThreshold() const '
           '{ return clampDouble(m_faceConfidenceThreshold, 0.0, 150.0); }',
           'double faceConfidenceThreshold() const '
           '{ return m_faceConfidenceThreshold; }'),
     'fail'),
]

problems = []
print('=== 0) 当前仓库 ===')
f0, w0 = run_in(T)
print('  失败=%d 告警=%d' % (len(f0), len(w0)))
if f0:
    problems.append('基线不应失败')
    print('  ***', f0[0][:200])
if w0:
    problems.append('基线不应有 face-gate 告警（第 38 轮已由告警升级为断言）')
    print('  *** 出现预期外的告警:', w0[0][:200])

for name, mutate, want in CASES:
    tmp = copy_repo()
    try:
        mutate(tmp)
        f, w = run_in(tmp)
        if want == 'fail':
            ok = len(f) > 0
        else:
            # 'pass'：不该失败，也不该冒出告警（基线告警数已为 0）。
            ok = len(f) == 0 and len(w) == len(w0)
        if ok:
            print('  %-40s %s ✓'
                  % (name, {'fail': '失败', 'pass': '通过'}[want]))
        else:
            print('  %-40s *** 期望 %s，实得 失败=%d 告警=%d'
                  % (name, want, len(f), len(w)))
            problems.append(name)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

print()
print('用例 %d 个，致命问题 %d 个' % (len(CASES), len(problems)))
if problems:
    print('问题清单:', problems)
sys.exit(1 if problems else 0)

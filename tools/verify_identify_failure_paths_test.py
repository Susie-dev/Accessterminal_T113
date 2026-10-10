# -*- coding: utf-8 -*-
"""现场识别流程失败分支的故障注入自检（纯 Python 驱动）。

用法：python tools/verify_identify_failure_paths_test.py
返回码 0 表示每个变异都能让对应用例失败。

变异都针对**现场识别流程的真实判据**：
  1. 去掉 GEN_CHAR 失败判据（永远当作成功）
  2. 去掉 GEN_CHAR 失败后的 return（emit 了但继续走 SEARCH）
  3. SEARCH 确认码判据反相（0x00 当失败、非 0x00 当成功）
  4. 去掉 SEARCH 失败的 emit
  5. 对照：SEARCH 成功时 id/score 读错位置（id 用 resp3[10..11]）
"""
import io
import os
import shutil
import subprocess
import sys

# 把标准输出固定为 UTF-8 且不可编码字符降级为替代符。
# 本机控制台是 GBK；脚本里的 ✓ 会抛 UnicodeEncodeError，
# 且**崩在打印结果那一行** —— 前面的输出看着正常，极易被误读成"通过"。
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

# 本脚本不需要参数，但 tools/ 下其它自检脚本的惯例是接收 <verify.py> <repo>。
if len(sys.argv) > 1:
    print("提示: 本脚本不需要参数，已忽略: %s" % " ".join(sys.argv[1:]))


T = r'C:\Users\user\Desktop\linux_ai\Accessterminal_T113'
FM = os.path.join(T, 'src', 'managers', 'fingerprint_manager.cpp')
MSYS = r'C:\Users\user\.dsh\toolchains\msys64'
CMAKE = os.path.join(MSYS, 'mingw64', 'bin', 'cmake.exe')
BUILD = os.path.join(os.environ['TEMP'], 'fi26win')
EXE = os.path.join(BUILD, 't113_test_fingerprint_manager.exe')
OBJ = os.path.join(
    BUILD, 'CMakeFiles', 't113_test_fingerprint_manager.dir', 'C_', 'Users',
    'user', 'Desktop', 'linux_ai', 'Accessterminal_T113', 'src', 'managers',
    'fingerprint_manager.cpp.obj')

ENV = dict(os.environ)
ENV['PATH'] = (os.path.join(MSYS, 'mingw64', 'bin') + os.pathsep
               + os.path.join(MSYS, 'usr', 'bin') + os.pathsep + ENV['PATH'])

# 注意：这个判据在文件里出现**两次** —— startIdentify(L143) 与
# startEnroll(L269)。必须带上下文把锚点收窄到 startIdentify 的那一处
# （startEnroll 体内发的是 enrollResult，不是 identifyResult）。
GENCHAR_GUARD = (
    '            if (resp2.size() < 12 || (quint8)resp2[9] != 0x00) {\n'
    '                // 发出 Qt 信号 identifyResult 通知其他对象。\n'
    '                emit identifyResult(false, -1, 0, "生成特征失败");')
SEARCH_GUARD = ('            if (resp3.size() >= 16 && '
                '(quint8)resp3[9] == 0x00) {')

MUTATIONS = [
    ("去掉 GEN_CHAR 失败判据", [
        (GENCHAR_GUARD,
         '            if (false) {\n'
         '                emit identifyResult(false, -1, 0, "生成特征失败");')]),

    ("去掉 GEN_CHAR 失败后的 return", [
        ('''                emit identifyResult(false, -1, 0, "生成特征失败");
                // 执行本行语句完成对应变量、对象或业务状态处理。
                m_waitingFinger = false;
                // 返回当前函数的执行结果或结束函数。
                return;''',
         '''                emit identifyResult(false, -1, 0, "生成特征失败");
                m_waitingFinger = false;''')]),

    ("SEARCH 确认码判据反相", [
        (SEARCH_GUARD,
         '            if (resp3.size() >= 16 && (quint8)resp3[9] != 0x00) {')]),

    ("去掉 SEARCH 失败的 emit", [
        ('                emit identifyResult(false, -1, 0, "指纹识别失败");',
         '                ;')]),

    ("SEARCH 成功时 id/score 读错位置（id 用 resp3[10..11]）", [
        ('                int id = ((quint8)resp3[11] << 8) | (quint8)resp3[12];',
         '                int id = ((quint8)resp3[10] << 8) | (quint8)resp3[11];')]),
]


def main():
    orig = io.open(FM, encoding='utf-8', errors='replace').read()
    for _, pairs in MUTATIONS:
        for o, _ in pairs:
            if orig.count(o) != 1:
                print('[夹具错误] 锚点出现 %d 次: %r' % (orig.count(o), o[:70]))
                return 2

    shutil.rmtree(BUILD, ignore_errors=True)
    r = subprocess.run([CMAKE, '-S', os.path.join(T, 'tests'), '-B', BUILD,
                        '-G', 'Ninja'], capture_output=True, env=ENV, cwd=T)
    print('configure rc=%d' % r.returncode)
    if r.returncode != 0:
        print(r.stderr.decode('utf-8', 'replace')[-1200:])
        return 2
    print()

    problems = []
    try:
        for name, pairs in MUTATIONS:
            text = orig
            for o, n in pairs:
                text = text.replace(o, n, 1)
            io.open(FM, 'w', encoding='utf-8', newline='\n').write(text)
            if io.open(FM, encoding='utf-8',
                       errors='replace').read() == orig:
                print('  [夹具错误] %-42s 文件未变' % name)
                problems.append(name)
                io.open(FM, 'w', encoding='utf-8',
                        newline='\n').write(orig)
                continue

            t0 = os.path.getmtime(OBJ) if os.path.exists(OBJ) else 0
            rb = subprocess.run([CMAKE, '--build', BUILD, '--target',
                                 't113_test_fingerprint_manager'],
                                capture_output=True, env=ENV, cwd=T)
            if rb.returncode != 0:
                print('  [构建失败] %-42s %s' % (
                    name, rb.stdout.decode('utf-8', 'replace')[-200:]))
                problems.append(name)
                io.open(FM, 'w', encoding='utf-8',
                        newline='\n').write(orig)
                continue
            t1 = os.path.getmtime(OBJ) if os.path.exists(OBJ) else 0

            rr = subprocess.run([EXE], capture_output=True, env=ENV, cwd=T)
            out = (rr.stdout + rr.stderr).decode('utf-8', 'replace')
            nfail = out.count('\nFAIL') + (1 if out.startswith('FAIL') else 0)
            failed = nfail > 0

            if t1 <= t0:
                print('  [夹具错误] %-42s .obj 未重编' % name)
                problems.append(name)
            elif failed:
                names = []
                for l in out.split('\n'):
                    if l.startswith('FAIL'):
                        nm = l.split(':')[-1].strip().split('(')[0].strip()
                        names.append(nm)
                print('  %-42s 已按预期失败 ✓ %s' % (name, names[:3]))
            else:
                print('  %-42s *** 仍然通过（假绿）***' % name)
                problems.append(name)

            io.open(FM, 'w', encoding='utf-8', newline='\n').write(orig)
    finally:
        io.open(FM, 'w', encoding='utf-8', newline='\n').write(orig)

    same = io.open(FM, encoding='utf-8', errors='replace').read() == orig
    print()
    print('还原逐字节一致:', same)
    subprocess.run([CMAKE, '--build', BUILD, '--target',
                    't113_test_fingerprint_manager'],
                   capture_output=True, env=ENV, cwd=T)
    rr = subprocess.run([EXE], capture_output=True, env=ENV, cwd=T)
    for l in (rr.stdout + rr.stderr).decode('utf-8', 'replace').split('\n'):
        if 'Totals' in l:
            print('还原后全量:', l.strip())
    print()
    print('变异 %d 个，致命问题 %d 个' % (len(MUTATIONS), len(problems)))
    if problems:
        print('问题清单:', problems)
    return 1 if (problems or not same) else 0


if __name__ == '__main__':
    sys.exit(main())

# -*- coding: utf-8 -*-
"""对第 35 轮新增的 check_form_passwd_contract_is_pinned 做故障注入自检。

规则本身的价值在于"当测试退化时会红"。第 35 轮实测踩到的最大坑就是
逐位日志断言**没有判别力**（2/2 漏报），所以本夹具必须把"断言退化成
无判别力写法"这件事也当成一个变异来验。
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

T = r'C:\Users\user\Desktop\linux_ai\Accessterminal_T113'
D = r'C:\Users\user\Desktop\linux_ai\Accessterminal_desktop'
PY = (r'C:\Users\user\.dsh\dsh-runtimes\dsh-primary-runtime'
      r'\dependencies\python\python.exe')
VERIFY = os.path.join(T, 'tools', 'verify.py')

TF = os.path.join(T, 'tests', 'test_form_passwd.cpp')
CM = os.path.join(T, 'tests', 'CMakeLists.txt')
SW = os.path.join(T, 'tests', 'shadows', 'ui', 'widget.h')
SA = os.path.join(T, 'tests', 'shadows', 'managers', 'audio_manager.h')
FX = os.path.join(T, 'tools', 'verify_form_passwd_test.py')


def read(p):
    return io.open(p, encoding='utf-8', errors='replace').read()


FILES = (TF, CM, SW, SA, FX)
_ORIG = {p: read(p) for p in FILES}


def sub(p, old, new, count=1):
    s = _ORIG[p]
    n = s.count(old)
    if n != count:
        raise AssertionError('%s 锚点出现 %d 次（期望 %d）: %r'
                             % (os.path.basename(p), n, count, old[:60]))
    return s.replace(old, new, count)


MUT = [
    # ---- 断言退化成无判别力写法（第 35 轮真实踩到的坑）----
    #
    # 注意必须替换**整条断言**（含 `QVERIFY2(logA == logB,` 那一行）。
    # 第一版只替换了消息文案，条件本身还在 —— 那不是"断言退化"，是"改文案"，
    # 规则不报是对的。夹具自己写错会让好规则背上假绿的锅，这类错误在第 35 轮
    # 已经出现过三次（见下面两条的注释）。
    ("日志断言退化成『不含连续 4 位』（无判别力）", TF,
     '    QVERIFY2(logA == logB,\n'
     '             qPrintable(QStringLiteral(\n'
     '                 "日志内容随输入的数字变化，说明记录了按键内容：\\n"\n'
     '                 "输入 %1 时的日志：\\n%2\\n\\n输入 %3 时的日志：\\n%4")\n'
     '                            .arg(pwdA, logA, pwdB, logB)));',
     '    QVERIFY2(!logA.contains(pwdA.left(4)), QStringLiteral("x"));'),

    # ---- 阳性对照被删 ----
    # 必须删**定义**，只改声明不算（声明留着而定义没了，规则查定义才会红）。
    ("删掉日志捕获的阳性对照用例定义", TF,
     'void TestFormPasswd::logCapturePipelineWorks()\n{\n'
     '    // 上面三条"日志里没有 X"的断言都依赖捕获管道真的在工作。\n',
     'void TestFormPasswd::removedPositiveControl()\n{\n'
     '    // 上面三条"日志里没有 X"的断言都依赖捕获管道真的在工作。\n'),

    # ---- 安全阀被去掉 ----
    ("去掉影子 Widget 的 #error 安全阀", SW,
     '#  error "影子 ui/widget.h 只能用于测试；请在测试目标上定义 '
     'ACC_FORM_PASSWD_TEST"', '// 安全阀被移除'),

    ("去掉影子 AudioManager 的 #error 安全阀", SA,
     '#  error "影子 managers/audio_manager.h 只能用于测试；请定义 '
     'ACC_FORM_PASSWD_TEST"', '// 安全阀被移除'),

    # ---- 影子头 Q_OBJECT 被去掉 ----
    ("去掉影子 Widget 的 Q_OBJECT（编译期静态断言）", SW,
     'class Widget : public QWidget\n{\n    Q_OBJECT\n',
     'class Widget : public QWidget\n{\n'),

    # ---- 影子头没被列进源列表 ----
    ("影子头没列进 add_executable 源列表", CM,
     '    ${CMAKE_CURRENT_LIST_DIR}/shadows/ui/widget.h\n', ''),

    # ---- 测试没接进 CMake ----
    #
    # 第一版写的是把目标名 `t113_test_form_passwd` 改成别的名字 —— 那不是缺陷
    # （目标叫什么名字不影响它跑），规则不报是对的。真正的"没接进构建"是
    # **源文件不在 add_executable 的列表里**，所以这里删源文件那一行。
    ("测试文件没接进 CMake（源文件不在 add_executable 里）", CM,
     'add_executable(t113_test_form_passwd\n    test_form_passwd.cpp\n',
     'add_executable(t113_test_form_passwd\n'),

    # ---- 安全阀宏没定义 ----
    ("CMake 不定义 ACC_FORM_PASSWD_TEST", CM,
     'target_compile_definitions(t113_test_form_passwd PRIVATE\n'
     '    ACC_FORM_PASSWD_TEST\n)',
     '# target_compile_definitions removed'),
]

# 需要"改文件系统"而不是"改文件内容"的变异：(名字, 路径)。
# 第一版把"夹具被删"写成改 docstring 首行 —— 文件还在，os.path.exists()
# 当然仍为真，规则不报是对的。真正的"夹具没了"必须是**文件消失**。
FS_MUT = [
    ("故障注入夹具被改名（规则找不到它）", FX),
]

CTRL = [
    ("加无关注释（测试文件末尾）", TF,
     'QTEST_MAIN(TestFormPasswd)\n#include "test_form_passwd.moc"',
     '// 无关注释\nQTEST_MAIN(TestFormPasswd)\n'
     '#include "test_form_passwd.moc"'),
    ("改一条无关断言的报错文案", TF,
     '"两个密码必须不共用任何数字，否则差分没有判别力"',
     '"pwdA/pwdB must not share digits"'),
]


def run_verify():
    return subprocess.run([PY, VERIFY, T, D], capture_output=True, cwd=T)


def main():
    problems = []
    try:
        r = run_verify()
        print('基线 rc = %d' % r.returncode)
        if r.returncode != 0:
            print('*** 基线就不过')
            print((r.stdout + r.stderr).decode('utf-8', 'replace')[-2500:])
            return 2
        print()

        for name, p, old, new in MUT:
            try:
                text = sub(p, old, new)
            except AssertionError as e:
                print('  [夹具错误] %-46s %s' % (name, e))
                problems.append(name)
                continue
            io.open(p, 'w', encoding='utf-8', newline='\n').write(text)
            if read(p) == _ORIG[p]:
                print('  [夹具错误] %-46s 文件未变' % name)
                problems.append(name)
                io.open(p, 'w', encoding='utf-8',
                        newline='\n').write(_ORIG[p])
                continue

            r = run_verify()
            out = (r.stdout + r.stderr).decode('utf-8', 'replace')
            hits = re.findall(r'^\s*X (\[form-passwd\][^\n]*)', out, re.M)
            if r.returncode != 0 and hits:
                print('  %-46s 被抓到 ✓ %s' % (name, hits[0][:44]))
            else:
                print('  %-46s *** 未被抓到（假绿）rc=%d'
                      % (name, r.returncode))
                problems.append(name)

            io.open(p, 'w', encoding='utf-8', newline='\n').write(_ORIG[p])

        print()
        # 文件系统级变异：把夹具改成别的名字，模拟"夹具被删/改名"。
        # 必须真的让规则看不到那个文件名 —— 改内容改不出这个效果。
        for name, p in FS_MUT:
            parked = p + '.parked'
            try:
                os.replace(p, parked)
            except OSError as e:
                print('  [夹具错误] %-46s %s' % (name, e))
                problems.append(name)
                continue
            r = run_verify()
            out = (r.stdout + r.stderr).decode('utf-8', 'replace')
            hits = re.findall(r'^\s*X (\[form-passwd\][^\n]*)', out, re.M)
            if r.returncode != 0 and hits:
                print('  %-46s 被抓到 ✓ %s' % (name, hits[0][:44]))
            else:
                print('  %-46s *** 未被抓到（假绿）rc=%d'
                      % (name, r.returncode))
                problems.append(name)
            os.replace(parked, p)

        print()
        for name, p, old, new in CTRL:
            try:
                text = sub(p, old, new)
            except AssertionError as e:
                print('  [夹具错误] %-46s %s' % (name, e))
                problems.append(name)
                continue
            io.open(p, 'w', encoding='utf-8', newline='\n').write(text)
            r = run_verify()
            out = (r.stdout + r.stderr).decode('utf-8', 'replace')
            if r.returncode == 0:
                print('  %-46s 通过 ✓（不误报）' % name)
            else:
                print('  %-46s *** 误报 rc=%d' % (name, r.returncode))
                print(out[-800:])
                problems.append(name)
            io.open(p, 'w', encoding='utf-8', newline='\n').write(_ORIG[p])
    finally:
        for p in FILES:
            io.open(p, 'w', encoding='utf-8', newline='\n').write(_ORIG[p])
        for _n, p in FS_MUT:
            if os.path.exists(p + '.parked'):
                os.replace(p + '.parked', p)

    same = all(read(p) == _ORIG[p] for p in FILES)
    same = same and all(os.path.exists(p) for _n, p in FS_MUT)
    print()
    print('还原逐字节一致:', same)
    r = run_verify()
    for l in (r.stdout + r.stderr).decode('utf-8', 'replace').split('\n'):
        if '结论' in l:
            print('还原后:', l.strip())
    print()
    print('变异 %d 个，文件系统变异 %d 个，对照 %d 个，致命问题 %d 个'
          % (len(MUT), len(FS_MUT), len(CTRL), len(problems)))
    if problems:
        print('问题清单:', problems)
    return 1 if (problems or not same) else 0


if __name__ == '__main__':
    sys.exit(main())

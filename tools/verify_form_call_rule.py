# -*- coding: utf-8 -*-
"""对第 36 轮新增的 check_form_call_stopsend_is_wired 做故障注入自检。

规则的价值在于"测试退化时会红"。本夹具必须把**本轮真实踩到的假绿**也当成
变异来验：`startCallTimer 不归零` 第一版 rc=0（标签那句无条件 setText 掩盖了
计数未归零），所以"断言退化成无判别力写法"这里是一条**必须被抓到**的变异。
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

TF = os.path.join(T, 'tests', 'test_form_call.cpp')
CM = os.path.join(T, 'tests', 'CMakeLists.txt')
SW = os.path.join(T, 'tests', 'shadows', 'ui', 'widget.h')
SA = os.path.join(T, 'tests', 'shadows', 'managers', 'audio_manager.h')
FX = os.path.join(T, 'tools', 'verify_form_call_test.py')
WC = os.path.join(T, 'src', 'ui', 'widget.cpp')
FC = os.path.join(T, 'src', 'ui', 'form_call.cpp')


def read(p):
    return io.open(p, encoding='utf-8', errors='replace').read()


FILES = (TF, CM, SW, SA, FX, WC, FC)
_ORIG = {p: read(p) for p in FILES}


def sub(p, old, new, count=1):
    s = _ORIG[p]
    n = s.count(old)
    if n != count:
        raise AssertionError('%s 锚点出现 %d 次（期望 %d）: %r'
                             % (os.path.basename(p), n, count, old[:60]))
    return s.replace(old, new, count)


MUT = [
    # ---- 核心：真实现的发送点被改坏 ----
    #
    # 这是本规则存在的一半理由：测试跑的是**替身**，真 widget.cpp 链接不进来。
    # 若规则不单独核对真实现，替身发对了、原件改错了也不会被发现。
    ("真 Widget::notifyPeerStopSend 发成 Signal::Stop（前缀歧义）", WC,
     '        m_signaling->send(accessterminal::protocol::Signal::StopSend);',
     '        m_signaling->send(accessterminal::protocol::Signal::Stop);'),

    ("真 Widget::notifyPeerStopSend 完全不发信令", WC,
     '    if (m_signaling) {\n'
     '        m_signaling->send(accessterminal::protocol::Signal::StopSend);\n'
     '    }\n',
     '    (void)0;\n'),

    ("真 Widget::notifyPeerStopSend 改用裸字符串（绕过协议表）", WC,
     '        m_signaling->send(accessterminal::protocol::Signal::StopSend);',
     '        m_signaling->sendRaw(QByteArray("stopsend\\n"));'),

    ("真 Widget::notifyPeerStopSend 整个函数被删", WC,
     'void Widget::notifyPeerStopSend()\n{\n',
     'void Widget::notifyPeerStopSendRenamed()\n{\n'),

    # ---- 断言退化成无判别力写法 ----
    #
    # 说明：`startCallTimer` 归零断言退化的那条变异**故意不放在这里** ——
    # 本规则守的是 stopsend 契约，不该去管测试内部某个计时器用例怎么断言。
    # 那条由**行为**夹具 tools/verify_form_call_test.py 抓（实测已抓到）。
    # 把不属于本规则的变异塞进来，只会逼着规则去过拟合测试实现细节。

    # ---- 线上字节断言被抽掉 ----
    ("stopsend 的断言不再落在线上字节上", TF,
     '    const QByteArray got = f.awaitBytes(9);\n'
     '    QCOMPARE(got, QByteArray("stopsend\\n"));',
     '    QVERIFY(true);\n'),

    # ---- 测试没接进 CMake ----
    ("测试文件没接进 CMake（源文件不在 add_executable 里）", CM,
     'add_executable(t113_test_form_call\n    test_form_call.cpp\n',
     'add_executable(t113_test_form_call\n'),

    # ---- 真实 SignalingClient 没被链接（断言会退化成替身自记）----
    ("真实 signaling_client.cpp 没被链接", CM,
     '    # 真实信令客户端：stopsend 必须真的走到 TCP 上\n'
     '    ${ACCESSTERMINAL_SRC_ROOT}/core/net/signaling_client.cpp\n', ''),

    # ---- 影子头没被列进源列表 ----
    # 注意锚点必须带上目标名：`shadows/ui/widget.h` 那一行在两个目标里都出现
    # （form_passwd 与 form_call），裸行会命中 2 次 —— 第 36 轮夹具实测。
    ("影子头没列进 add_executable 源列表", CM,
     'add_executable(t113_test_form_call\n'
     '    test_form_call.cpp\n'
     '    # 影子头必须列进来，AUTOMOC 才会为它们生成元对象\n'
     '    ${CMAKE_CURRENT_LIST_DIR}/shadows/ui/widget.h\n', ''),

    # ---- BEFORE 被去掉 ----
    ("影子头 include 路径没有 BEFORE", CM,
     'target_include_directories(t113_test_form_call BEFORE PRIVATE\n'
     '    ${CMAKE_CURRENT_LIST_DIR}/shadows\n)',
     'target_include_directories(t113_test_form_call PRIVATE\n'
     '    ${CMAKE_CURRENT_LIST_DIR}/shadows\n)'),

    # ---- 安全阀宏没定义 ----
    ("CMake 不定义 ACC_FORM_CALL_TEST", CM,
     'target_compile_definitions(t113_test_form_call PRIVATE\n'
     '    ACC_FORM_CALL_TEST\n)',
     '# target_compile_definitions removed'),

    # ---- 影子头安全阀被去掉 ----
    ("去掉影子 Widget 的 #error 安全阀", SW,
     '#if !defined(ACC_FORM_PASSWD_TEST) && !defined(ACC_FORM_CALL_TEST)\n'
     '#  error "影子 ui/widget.h 只能用于测试；请在测试目标上定义 '
     'ACC_FORM_PASSWD_TEST 或 ACC_FORM_CALL_TEST"\n#endif',
     '// 安全阀被移除'),

    # ---- 影子头缺 notifyPeerStopSend ----
    ("影子 Widget 缺 notifyPeerStopSend()", SW,
     '    void notifyPeerStopSend()\n', '    void notifyPeerStopSendGone()\n'),
]

# 需要"改文件系统"而不是"改文件内容"的变异：(名字, 路径)。
FS_MUT = [
    ("故障注入夹具被改名（规则找不到它）", FX),
]

CTRL = [
    ("加无关注释（测试文件末尾）", TF,
     'QTEST_MAIN(TestFormCall)\n#include "test_form_call.moc"',
     '// 无关注释\nQTEST_MAIN(TestFormCall)\n'
     '#include "test_form_call.moc"'),
    ("改一条无关断言的报错文案", TF,
     '"刚构造的窗体不应处于已隐藏状态"',
     '"fresh form must not be hidden"'),
    ("真 widget.cpp 里加无关注释", WC,
     'void Widget::notifyPeerStopSend()\n{\n',
     '// 无关注释（对照）\nvoid Widget::notifyPeerStopSend()\n{\n'),
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
            hits = re.findall(r'^\s*X (\[form-call\][^\n]*)', out, re.M)
            if r.returncode != 0 and hits:
                print('  %-46s 被抓到 ✓ %s' % (name, hits[0][:40]))
            else:
                print('  %-46s *** 未被抓到（假绿）rc=%d'
                      % (name, r.returncode))
                problems.append(name)

            io.open(p, 'w', encoding='utf-8', newline='\n').write(_ORIG[p])

        print()
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
            hits = re.findall(r'^\s*X (\[form-call\][^\n]*)', out, re.M)
            if r.returncode != 0 and hits:
                print('  %-46s 被抓到 ✓ %s' % (name, hits[0][:40]))
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

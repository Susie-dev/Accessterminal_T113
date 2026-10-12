# -*- coding: utf-8 -*-
"""t113_test_signaling_client 故障注入自检（纯 Python 驱动）。

用法：python tools/verify_signaling_client_test.py
返回码 0 表示每个变异都能让对应用例失败（或已如实记录为不可区分）。

为什么要重写第三次
------------------
前两版都因为**路径**问题而静默失效，且都表现为"6 个变异全部仍然通过" ——
看上去像"测试全是假绿"，实际是夹具根本没注入成功：
  v1: bash 里用 `python -c`，把 MSYS 路径 /c/Users/... 传给 Windows python
      -> 每个变异 FileNotFoundError，一个都没注入。
  v2: 改用 Python 驱动，但 build_dir 用 MSYS 路径 /tmp/...，Windows 侧看不到
      -> 构建产物路径不可解析，跑的不是刚构建的可执行文件。

**这是本轮最值得记的教训**：一个"全是假绿"的故障注入报告，既可能是被测代码
有问题，也可能是夹具坏了；直接把结论写进文档就等于把夹具缺陷当成产品缺陷。
必须让夹具自己先证明"变异确实注入了、而且跑的确实是新二进制"。

本版做法：
  - 全程 Windows 形式路径（%TEMP% 下的构建目录）
  - 直接用 cmake.exe / ninja / 可执行文件，不经 bash，无路径转换
  - 每个变异后断言：文件内容变了 + .obj 时间戳更新了 + 该用例确实 FAIL
"""
import io
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fixture_pristine import PristineGuard  # noqa: E402

# 把标准输出固定为 UTF-8 且不可编码字符降级为替代符。
#
# 必要性（第 25 轮实测）：本机控制台是 GBK；脚本里的 ✓ 会抛
#   UnicodeEncodeError: 'gbk' codec can't encode character '\u2713'
# 而**在打印成功率那一行**崩溃 —— 报告前面的内容已经正常输出，很容易被误读成
# "检查通过"。第 24 轮我误诊为"多余参数"问题，因为当时用 pwsh 且设了
# PYTHONIOENCODING=utf-8 去测。系统性修法是重配输出编码，
# 而不是逐个删掉符号（那样下次加个新符号又会复发）。
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

import time

# 本脚本不需要参数，但 tools/ 下其它自检脚本的惯例是接收
# <verify.py> <repo>。被那样调用时不要把多余参数当成错误 —— 自检脚本的
# 接口应当可互换，否则每次调整调用方式都会产生假警报。
if len(sys.argv) > 1:
    print('提示: 本脚本不需要参数，已忽略: %s' % ' '.join(sys.argv[1:]))

T = r'C:\Users\user\Desktop\linux_ai\Accessterminal_T113'
if not os.path.isdir(os.path.join(T, 'src')):
    print('找不到仓库: %s' % T)
    sys.exit(2)
SC = os.path.join(T, 'src', 'core', 'net', 'signaling_client.cpp')
MSYS = r'C:\Users\user\.dsh\toolchains\msys64'
CMAKE = os.path.join(MSYS, 'mingw64', 'bin', 'cmake.exe')
BUILD = os.path.join(os.environ['TEMP'], 'fi24win')
BAK = os.path.join(os.environ['TEMP'], 'sc24_bak.cpp')

ENV = dict(os.environ)
ENV['PATH'] = (os.path.join(MSYS, 'mingw64', 'bin') + os.pathsep
               + os.path.join(MSYS, 'usr', 'bin') + os.pathsep
               + ENV['PATH'])

MUTATIONS = [
    ("变异1 onSocketError 不安排重连",
     "refusedConnectionSchedulesReconnectThatActuallyRetries",
     """    if (m_socket->state() == QAbstractSocket::UnconnectedState) {
        scheduleReconnect();
    }""",
     "    // 变异：不安排重连\n    (void)0;"),

    # 变异2：m_running 在**两处**独立守卫重连，必须同时移除才等价于
    # "重连不再受 stop() 约束"。只删一处属"守卫冗余"，行为不变 ——
    # 第 24 轮实测确认，这不是假绿而是守卫确实重复。
    ("变异2 同时移除两处 m_running 守卫",
     "stopPreventsFurtherReconnectAttempts",
     [("""    if (!m_running) {
        return;
    }""",
       "    // 变异：忽略 m_running(1/2)"),
      ("""        if (m_running) {
            qDebug() << "[信令] 正在重连" << m_serverIp << m_port;
            m_socket->connectToHost(m_serverIp, m_port);
        }""",
       """        {
            qDebug() << "[信令] 正在重连" << m_serverIp << m_port;
            m_socket->connectToHost(m_serverIp, m_port);
        }""")]),

    ("变异3 send 不检查连接状态",
     "sendWhileNotConnectedIsRejected",
     """    if (!isConnected()) {
        qWarning() << "[信令] 未连接，发送失败:" << payload.trimmed();
        return false;
    }""",
     "    // 变异：不检查连接状态"),

    ("变异4 send 不拒绝空帧",
     "sendUnknownSignalIsRejected",
     """    if (payload.isEmpty()) {
        qWarning() << "[信令] 尝试发送未知信令，已忽略";
        return false;
    }""",
     "    // 变异：不拒绝空帧"),

    # 【已知不可区分，第 24 轮实测】已连接时再 connectToHost 会被 Qt 拒绝并
    # 先发 socket error，走不到第二次 onConnected；同一次连接内 connected 只
    # 触发一次。Qt 里构造不出"同一次连接内二次触发"，故 m_wasConnected 守卫
    # 无法被本用例区分。这不是断言写弱了，是真实的不可区分 —— 如实保留在
    # 清单里并标为非致命，不假装它有效。
    ("（已知不可区分）变异5 connectedChanged 不去重",
     "connectEmitsConnectedChangedExactlyOnce",
     """    if (!m_wasConnected) {
        m_wasConnected = true;
        emit connectedChanged(true);
    }""",
     "    // 变异：不判重，每次都发\n    m_wasConnected = true;\n"
     "    emit connectedChanged(true);"),

    ("变异6 去掉帧尾 '\\n'",
     "sendWritesExactWireBytes",
     "const QByteArray payload = protocol::frame(signal);",
     "QByteArray payload = protocol::frame(signal);\n"
     "    if (!payload.isEmpty()) payload.chop(1);"),
]

EXE = os.path.join(BUILD, 't113_test_signaling_client.exe')
OBJ = os.path.join(
    BUILD, 'CMakeFiles', 't113_test_signaling_client.dir',
    'C_', 'Users', 'user', 'Desktop', 'linux_ai', 'Accessterminal_T113',
    'src', 'core', 'net', 'signaling_client.cpp.obj')


def configure():
    return subprocess.run(
        [CMAKE, '-S', os.path.join(T, 'tests'), '-B', BUILD, '-G', 'Ninja'],
        capture_output=True, env=ENV, cwd=T)


def build():
    return subprocess.run([CMAKE, '--build', BUILD,
                           '--target', 't113_test_signaling_client'],
                          capture_output=True, env=ENV, cwd=T)


def run_case(case):
    return subprocess.run([EXE, case], capture_output=True, env=ENV, cwd=T)


orig = io.open(SC, encoding='utf-8').read()
shutil.copyfile(SC, BAK)
print('源文件 %d 字节; 构建目录 %s' % (len(orig), BUILD))
print()

# 先配置一次（变异循环里只重新构建）
shutil.rmtree(BUILD, ignore_errors=True)
r = configure()
print('configure rc=%d' % r.returncode)
if r.returncode != 0:
    print(r.stderr.decode('utf-8', 'replace')[-2000:])
    sys.exit(2)
print()

problems = []
# ---------------------------------------------------------------------------
# 就地改写**真实仓库文件**的还原保险（第 41 轮补）。
#
# 本夹具原先只把 orig 留在内存、靠 finally 写回。夹具被外部强杀时
# （Windows `job_kill` 走 TerminateProcess，finally / atexit / 信号处理器
# **全都不执行**），仓库里就会留下被变异过的源文件 —— 而且看起来完全正常。
#
# 现在多一层 PristineGuard：磁盘副本 + 中断哨兵 + 新鲜度校验
# （副本过期时拒绝自愈）+ atexit/信号 + 互斥锁。
# 单元自检：tools/verify_fixture_pristine.py。
# ---------------------------------------------------------------------------
_GUARD = PristineGuard(files=[SC], tag='g24sc',
                       lock_name='fi37_rewrite.lock')
if not _GUARD.acquire_lock():
    print('!! 已有另一个"改写源文件"的夹具在运行，请串行执行。')
    sys.exit(3)
_GUARD.prepare()
_GUARD.install_guards()

try:
    for item in MUTATIONS:
        # 普通项 (name, case, old, new)；复合变异 (name, case, [(old,new),...])
        if len(item) == 4:
            name, case, old, new = item
            pairs = [(old, new)]
        else:
            name, case, pairs = item
        ok = True
        for o, _ in pairs:
            if orig.count(o) != 1:
                print('  [夹具错误] %-40s 锚点出现 %d 次' % (name, orig.count(o)))
                problems.append(name)
                ok = False
                break
        if not ok:
            continue

        text = orig
        for o, n in pairs:
            text = text.replace(o, n, 1)
        io.open(SC, 'w', encoding='utf-8', newline='\n').write(text)
        changed = io.open(SC, encoding='utf-8').read() != orig
        if not changed:
            print('  [夹具错误] %-40s 写入后文件未变' % name)
            problems.append(name)
            continue

        t_before = os.path.getmtime(OBJ) if os.path.exists(OBJ) else 0
        rb = build()
        if rb.returncode != 0:
            print('  [构建失败] %-40s %s' % (
                name, rb.stdout.decode('utf-8', 'replace')[-200:]))
            problems.append(name)
            io.open(SC, 'w', encoding='utf-8', newline='\n').write(orig)
            continue
        t_after = os.path.getmtime(OBJ) if os.path.exists(OBJ) else 0

        rr = run_case(case)
        out = (rr.stdout + rr.stderr).decode('utf-8', 'replace')
        failed = ('\nFAIL' in out) or out.startswith('FAIL')
        rebuilt = t_after > t_before

        if not rebuilt:
            print('  [夹具错误] %-40s .obj 未重新编译' % name)
            problems.append(name)
        elif failed:
            print('  %-44s 已按预期失败 ✓  (obj 已重编, rc=%d)'
                  % (name, rr.returncode))
        elif name.startswith('（已知不可区分）'):
            print('  %-44s 仍然通过（已记录为不可区分，非致命）' % name)
        else:
            print('  %-44s *** 仍然通过（假绿）***' % name)
            problems.append(name)

        io.open(SC, 'w', encoding='utf-8', newline='\n').write(orig)
finally:
    io.open(SC, 'w', encoding='utf-8', newline='\n').write(orig)

print()
print('还原逐字节一致:', io.open(SC, encoding='utf-8').read() == orig)

# 还原后必须重新构建 + 全量通过
rb = build()
rr = subprocess.run([EXE], capture_output=True, env=ENV, cwd=T)
out = (rr.stdout + rr.stderr).decode('utf-8', 'replace')
tail = [l for l in out.strip().split('\n') if 'Totals' in l or 'tests passed' in l]
print('还原后全量:', tail[-1] if tail else '(无 Totals 行)')

print()
print('变异 %d 个，致命问题 %d 个' % (len(MUTATIONS), len(problems)))
if problems:
    print('问题清单:', problems)
_GUARD.restore()
sys.exit(1 if problems else 0)

# -*- coding: utf-8 -*-
"""t113_test_form_call 故障注入自检（纯 Python 驱动）。

核心要证明的：**"点下停止按钮 -> stopsend 真的发出去"这条接线是真的被测到**，
而不是"收到了某种字节"这种弱断言。

因此第一条变异就是**把接线删掉**（`widget->notifyPeerStopSend();`）。
如果那之后测试仍然通过，说明这个测试目标毫无价值。
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

T = r'C:\Users\user\Desktop\linux_ai\Accessterminal_T113'
MSYS = r'C:\Users\user\.dsh\toolchains\msys64'
CMAKE = os.path.join(MSYS, 'mingw64', 'bin', 'cmake.exe')
BUILD = os.path.join(os.environ['TEMP'], 'fi36win')
EXE = os.path.join(BUILD, 't113_test_form_call.exe')
SRC = os.path.join(T, 'src', 'ui', 'form_call.cpp')
WID = os.path.join(T, 'tests', 'shadows', 'ui', 'widget.h')
OBJ = os.path.join(
    BUILD, 'CMakeFiles', 't113_test_form_call.dir', 'C_', 'Users', 'user',
    'Desktop', 'linux_ai', 'Accessterminal_T113', 'src', 'ui',
    'form_call.cpp.obj')

ENV = dict(os.environ)
ENV['PATH'] = (os.path.join(MSYS, 'mingw64', 'bin') + os.pathsep
               + os.path.join(MSYS, 'usr', 'bin') + os.pathsep + ENV['PATH'])
ENV['QT_QPA_PLATFORM'] = 'offscreen'

FAIL_RE = re.compile(r'^FAIL!\s*:', re.M)


def read(p):
    return io.open(p, encoding='utf-8').read()


FILES = (SRC, WID)

# ---------------------------------------------------------------------------
# 崩溃/被强杀时的还原保险（第 36 轮真实事故）
#
# 事故经过：第一版把"原始内容"只放在内存里，靠 `finally` 还原。结果脚本跑到
# 第一个变异（删掉 notifyPeerStopSend 那一行）时被**从外部强杀**，`finally`
# 根本没机会执行，仓库里就留下了一个**被变异过的源文件** —— 而它看起来
# 完全正常（只是少了一行）。后来下一次运行在启动预检时报"锚点出现 0 次"才
# 暴露出来。这正是最危险的一类污染：脚本自己把被测代码改坏了，还静默退出。
#
# 三道防线：
#   1. **磁盘上的原始副本**（不只在内存里）—— <临时目录>/fi36_pristine/
#   2. **启动自愈**：若发现"磁盘副本"与当前文件不同，先用副本还原，
#      再开始干活。于是上一次被强杀留下的污染会被下一次运行自动修好。
#   3. **信号处理器 + atexit**：收到 SIGINT/SIGTERM/SIGBREAK 也走还原。
#
# 注意：`finally` 仍然保留（正常流程用），但它**不是**唯一保障。
# ---------------------------------------------------------------------------
BACKUP = os.path.join(os.environ['TEMP'], 'fi36_pristine')
_ORIG = {}


def _backup_path(p):
    return os.path.join(BACKUP, os.path.basename(p))


def _load_pristine():
    """建立/校验磁盘原始副本；返回被自愈的文件列表。"""
    healed = []
    os.makedirs(BACKUP, exist_ok=True)
    for p in FILES:
        b = _backup_path(p)
        if os.path.exists(b):
            backup_text = read(b)
            cur = read(p)
            if cur != backup_text:
                # 上一次运行留下的污染（或被强杀）。用副本还原。
                io.open(p, 'w', encoding='utf-8',
                        newline='\n').write(backup_text)
                healed.append(os.path.basename(p))
            _ORIG[p] = backup_text
        else:
            _ORIG[p] = read(p)
            io.open(b, 'w', encoding='utf-8',
                    newline='\n').write(_ORIG[p])
    return healed


def _restore_all(*_a):
    for p in FILES:
        if p in _ORIG:
            try:
                io.open(p, 'w', encoding='utf-8',
                        newline='\n').write(_ORIG[p])
            except OSError:
                pass


def _install_guards():
    import atexit
    import signal
    atexit.register(_restore_all)

    def handler(signum, _frame):
        _restore_all()
        print('\n[中断] 收到信号 %d，已还原源文件，退出' % signum)
        sys.exit(130)

    for name in ('SIGINT', 'SIGTERM', 'SIGBREAK'):
        sig = getattr(signal, name, None)
        if sig is not None:
            try:
                signal.signal(sig, handler)
            except (ValueError, OSError):
                pass


def need(s, n=1, p=SRC):
    c = _ORIG[p].count(s)
    if c != n:
        raise AssertionError('%s 锚点出现 %d 次（期望 %d）: %r'
                             % (os.path.basename(p), c, n, s[:70]))
    return s


def target_of(old, cnt=1):
    """定位锚点落在哪个文件（SRC 优先）。找不到返回 None。"""
    for cand in FILES:
        if _ORIG[cand].count(old) == cnt:
            return cand
    return None


# ---- 真实锚点（从文件字节抄）----
# 先加载磁盘原始副本（含被强杀污染的自愈），再校验锚点。
_HEALED = _load_pristine()
_install_guards()

STOPSEND_CALL = need('                widget->notifyPeerStopSend();\n')
AUDIO_STOP = need('                widget->audioManager()->stop();\n')
HANGUP = need('        emit hangupRequested();\n')
BTNSTOP_FOCUS = need('    ui->btnstop->setFocusPolicy(Qt::NoFocus);\n')
TRANSLUCENT = need(
    '    this->setAttribute(Qt::WA_TranslucentBackground, true);\n')
TIMER_START = need('    callTimer->start(1000);\n')
TIMER_STOP = need('    callTimer->stop();\n')
SECONDS_ZERO = need('    callSeconds = 0;\n')
SET_TEXT = need('    ui->label->setText(text);\n')
CTOR_TIMER_CONNECT = need(
    '    connect(callTimer, &QTimer::timeout, this, [this]() {\n')
LAMBDA_CONNECT = need(
    '    connect(ui->btnstop, &QPushButton::clicked, this, [this]() {\n')
# widget.h 里的替身实现
SHIM_NOTIFY = need(
    '        if (m_signaling) {\n'
    '            m_signaling->send(accessterminal::protocol::Signal::StopSend);\n'
    '        }\n', p=WID)

MUT = [
    # ============ 核心：stopsend 接线 ============
    ("删掉 notifyPeerStopSend()（点挂断不发 stopsend）",
     STOPSEND_CALL, ''),

    ("stopsend 换成 stop（前缀歧义导致语义错误）",
     SHIM_NOTIFY,
     '        if (m_signaling) {\n'
     '            m_signaling->send(accessterminal::protocol::Signal::Stop);\n'
     '        }\n'),

    ("stopsend 换成 call（完全不同的信令）",
     SHIM_NOTIFY,
     '        if (m_signaling) {\n'
     '            m_signaling->send(accessterminal::protocol::Signal::Call);\n'
     '        }\n'),

    ("构造时就发 stopsend（点击变成空操作）",
     LAMBDA_CONNECT,
     '    {\n'
     '        QWidget* w0 = parentWidget();\n'
     '        while (w0 && !qobject_cast<Widget*>(w0)) w0 = w0->parentWidget();\n'
     '        Widget* w1 = qobject_cast<Widget*>(w0);\n'
     '        if (w1) w1->notifyPeerStopSend();\n'
     '    }\n'
     '    connect(ui->btnstop, &QPushButton::clicked, this, [this]() {\n'),

    ("点击处理函数开头直接 return（整段失效）",
     LAMBDA_CONNECT,
     '    connect(ui->btnstop, &QPushButton::clicked, this, [this]() {\n'
     '        return;\n'),

    # ============ 挂断信号 ============
    ("不再发出 hangupRequested", HANGUP, ''),

    ("把 hangupRequested 移进 if (widget) 里（无父窗口时不发）",
     '        }\n        // 发出 Qt 信号 hangupRequested 通知其他对象。\n'
     '        emit hangupRequested();\n',
     '            // 发出 Qt 信号 hangupRequested 通知其他对象。\n'
     '            emit hangupRequested();\n        }\n'),

    # ============ 提示音打断 ============
    ("不打断提示音（不调 audioManager()->stop()）", AUDIO_STOP, ''),

    # ============ 界面装配 ============
    ("停止按钮不设 NoFocus", BTNSTOP_FOCUS, ''),
    ("去掉半透明背景（浮层会挡住画面）", TRANSLUCENT, ''),

    # ============ 计时器 ============
    ("startCallTimer 不归零（秒数累加）", SECONDS_ZERO, ''),
    ("startCallTimer 不启动定时器", TIMER_START, '    (void)callTimer;\n'),
    ("stopCallTimer 不停定时器（显示继续走）", TIMER_STOP, ''),
    ("setStatusText 不更新标签", SET_TEXT, '    (void)text;\n'),
    ("计时器 tick 不更新标签",
     '        ui->label->setText(QString("通话中 %1:%2").arg(min, 2, 10, QChar(\'0\')).arg(sec, 2, 10, QChar(\'0\')));\n',
     '        (void)min; (void)sec;\n'),

    # ============ 对照：必须不误报（第 4 位 False = 期望"不该失败"）============
    ("对照：在文件末尾加无关注释",
     '// 结束当前代码块作用域。\n}\n',
     '// 结束当前代码块作用域。\n}\n// 无关注释（对照）\n', 5, False),

    ("startCallTimer 的起始文案被改写（标签比较应抓到）",
     'ui->label->setText("通话中 00:00");',
     'ui->label->setText("通话中 00:00 ");', 1),
]

# 实测**无判别力**的变异：必须保持"抓不到"，否则说明断言在依赖偶然因素。
#
# 已探明原因（第 36 轮，探针 %TEMP%\probe36\）：
#   `notifyPeerStopSend()` 里去掉 `if (m_signaling)` 判空后**行为不变** ——
#   因为测试 fixture 里 m_signaling 永远非空，且真 `SignalingClient::send()`
#   自身就做了"未连接则告警并返回 false"的等价判断（见 notifyPeerStopSend
#   的源码注释）。这与 README 第 34 条"同一契约的两份拷贝之间没有契约"同源：
#   两个独立守卫保护同一行为时，单点变异是**惰性**的。
#   该行为不靠本测试守，改由静态规则检查判空存在。
UNDETECTABLE = []


def configure():
    return subprocess.run(
        [CMAKE, '-S', os.path.join(T, 'tests'), '-B', BUILD, '-G', 'Ninja'],
        capture_output=True, env=ENV, cwd=T)


def build():
    return subprocess.run(
        [CMAKE, '--build', BUILD, '--target', 't113_test_form_call'],
        capture_output=True, env=ENV, cwd=T)


def run_exe():
    return subprocess.run([EXE], capture_output=True, env=ENV, cwd=T)


def mt(p):
    return os.path.getmtime(p) if os.path.exists(p) else 0


def main():
    if _HEALED:
        print('!! 检测到上一次运行留下的污染，已用磁盘原始副本自愈: %s'
              % ', '.join(_HEALED))

    # 锚点唯一性预检（含 count 参数）
    for item in MUT:
        name, old, new = item[0], item[1], item[2]
        cnt = item[3] if len(item) > 3 else 1
        if target_of(old, cnt) is None:
            print('[夹具错误] %s 锚点出现次数不符（期望 %d）' % (name, cnt))
            return 2

    shutil.rmtree(BUILD, ignore_errors=True)
    r = configure()
    print('configure rc=%d' % r.returncode)
    if r.returncode != 0:
        print(r.stderr.decode('utf-8', 'replace')[-2000:])
        return 2
    rb = build()
    if rb.returncode != 0:
        print('基线构建失败:')
        print((rb.stdout + rb.stderr).decode('utf-8', 'replace')[-2500:])
        return 2
    rr = run_exe()
    out = (rr.stdout + rr.stderr).decode('utf-8', 'replace')
    tot = re.search(r'Totals:\s*(\d+) passed,\s*(\d+) failed', out)
    print('基线: %s' % (tot.group(0) if tot else '(无 Totals)'))
    if FAIL_RE.findall(out):
        print('*** 基线就有 FAIL')
        print(out[-3000:])
        return 2
    print()

    problems = []
    try:
        for item in MUT:
            name, old, new = item[0], item[1], item[2]
            cnt = item[3] if len(item) > 3 else 1
            expect_fail = item[4] if len(item) > 4 else True
            p = target_of(old, cnt)
            if p is None:
                print('  [夹具错误] %-46s 锚点唯一性预检通过但定位失败' % name)
                problems.append(name)
                continue

            io.open(p, 'w', encoding='utf-8',
                    newline='\n').write(_ORIG[p].replace(old, new, cnt))
            if read(p) == _ORIG[p]:
                print('  [夹具错误] %-46s 文件未变' % name)
                problems.append(name)
                io.open(p, 'w', encoding='utf-8',
                        newline='\n').write(_ORIG[p])
                continue

            t0 = mt(OBJ)
            rb = build()
            if rb.returncode != 0:
                # 编译不过也算"抓到了"（例如语法错误的变异），但要区分开、
                # 而且必须能看出是哪一种 —— 不能把构建失败伪装成测试失败。
                msg = (rb.stdout + rb.stderr).decode('utf-8', 'replace')
                if new.strip() == '' or 'error' in msg.lower():
                    print('  %-46s 被抓到 ✓（编译失败）' % name)
                else:
                    print('  [构建失败] %-46s %s' % (name, msg[-160:]))
                    problems.append(name)
                io.open(p, 'w', encoding='utf-8',
                        newline='\n').write(_ORIG[p])
                continue
            t1 = mt(OBJ)

            rr = run_exe()
            out = (rr.stdout + rr.stderr).decode('utf-8', 'replace')
            failed = FAIL_RE.findall(out)
            nfail = len(failed)
            crashed = rr.returncode != 0 and not failed
            totals = re.search(r'Totals:\s*(\d+) passed,\s*(\d+) failed', out)
            consistent = (totals is not None
                          and rr.returncode == int(totals.group(2)))

            if t1 <= t0:
                print('  [夹具错误] %-46s .obj 未重编（可能是旧二进制）' % name)
                problems.append(name)
            elif expect_fail:
                if failed or crashed:
                    tag = ('崩溃(rc=%d, 0 条 FAIL 行)' % rr.returncode
                           if crashed else
                           '%d 条 FAIL 行(rc=%d%s)'
                           % (nfail, rr.returncode,
                              '' if consistent else ' 与计数不符'))
                    print('  %-46s 被抓到 ✓ %s' % (name, tag))
                else:
                    print('  %-46s *** 未被抓到（假绿）rc=%d'
                          % (name, rr.returncode))
                    problems.append(name)
            else:
                if not failed and not crashed:
                    print('  %-46s 通过 ✓（不误报）' % name)
                else:
                    print('  %-46s *** 误报 %d 条 FAIL' % (name, nfail))
                    problems.append(name)

            io.open(p, 'w', encoding='utf-8',
                    newline='\n').write(_ORIG[p])
    finally:
        for p in FILES:
            io.open(p, 'w', encoding='utf-8', newline='\n').write(_ORIG[p])

    same = all(read(p) == _ORIG[p] for p in FILES)
    print()
    print('还原逐字节一致:', same)
    build()
    rr = run_exe()
    for l in (rr.stdout + rr.stderr).decode('utf-8', 'replace').split('\n'):
        if 'Totals' in l:
            print('还原后全量:', l.strip())
    print()
    print('变异 %d 个，致命问题 %d 个' % (len(MUT), len(problems)))
    if problems:
        print('问题清单:', problems)
    return 1 if (problems or not same) else 0


if __name__ == '__main__':
    sys.exit(main())

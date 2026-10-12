# -*- coding: utf-8 -*-
"""t113_test_form_passwd 故障注入自检（纯 Python 驱动）。

重点验证"密码不得进日志"这条安全性质**真的有判别力** ——
也就是说：把密码漏出去的几种写法各自都能被抓到。
如果只是把原缺陷还原能抓到、换种写法就抓不到，那这条测试就是假绿。
"""
import io
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fixture_pristine import PristineGuard  # noqa: E402

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

T = r'C:\Users\user\Desktop\linux_ai\Accessterminal_T113'
MSYS = r'C:\Users\user\.dsh\toolchains\msys64'
CMAKE = os.path.join(MSYS, 'mingw64', 'bin', 'cmake.exe')
BUILD = os.path.join(os.environ['TEMP'], 'fi35win')
EXE = os.path.join(BUILD, 't113_test_form_passwd.exe')
SRC = os.path.join(T, 'src', 'ui', 'form_passwd.cpp')
OBJ = os.path.join(
    BUILD, 'CMakeFiles', 't113_test_form_passwd.dir', 'C_', 'Users', 'user',
    'Desktop', 'linux_ai', 'Accessterminal_T113', 'src', 'ui',
    'form_passwd.cpp.obj')

ENV = dict(os.environ)
ENV['PATH'] = (os.path.join(MSYS, 'mingw64', 'bin') + os.pathsep
               + os.path.join(MSYS, 'usr', 'bin') + os.pathsep + ENV['PATH'])
ENV['QT_QPA_PLATFORM'] = 'offscreen'

FAIL_RE = re.compile(r'^FAIL!\s*:', re.M)


def read():
    return io.open(SRC, encoding='utf-8').read()


_ORIG = read()


def need(s, n=1):
    c = _ORIG.count(s)
    if c != n:
        raise AssertionError('锚点出现 %d 次（期望 %d）: %r' % (c, n, s[:70]))
    return s


# ---- 真实锚点（从文件字节抄）----
DIGIT_LOG = need('    qDebug() << "[passwd] 数字键按下";\n')
OK_LOG = need(
    '    qDebug() << "[passwd] 确认键按下，已输入位数=" << inputBuffer.size();\n')
OK_EMIT = need(
    '    if (!inputBuffer.isEmpty())\n'
    '        // 发出 Qt 信号 passwordEntered 通知其他对象。\n'
    '        emit passwordEntered(inputBuffer);\n')
EMIT_RAW = need('        emit passwordEntered(inputBuffer);\n')
CLEAR = need('    inputBuffer.clear();\n    // 执行本行语句完成对应变量、对象或业务状态处理。\n    updateDisplay();\n    // 执行本行语句完成对应变量、对象或业务状态处理。\n    this->close();\n')
MAX8 = need('    if (inputBuffer.size() >= 8) return; // 最多8位\n')
APPEND = need('    inputBuffer += btn->text();\n')
CHOP = need('        inputBuffer.chop(1);\n')
OK_SLOT = need('void Form_passwd::on_btnok_clicked()\n')
BCK_SLOT = need('void Form_passwd::on_btnbck_clicked()\n')
KEYSOUND_CALL = need(
    '    inputBuffer += btn->text();\n'
    '    // 执行本行语句完成对应变量、对象或业务状态处理。\n'
    '    playKeySound(accessterminal::AppConfig::instance().pcmFile(QStringLiteral("beep.pcm")));\n')
NOFOCUS = need('        btn->setFocusPolicy(Qt::NoFocus);\n')
OK_ANCHOR = need(
    '    if (!inputBuffer.isEmpty())\n'
    '        // 发出 Qt 信号 passwordEntered 通知其他对象。\n'
    '        emit passwordEntered(inputBuffer);\n'
    '    // 执行本行语句完成对应变量、对象或业务状态处理。\n'
    '    inputBuffer.clear();\n')


MUT = [
    # ============ 安全性质：把密码弄进日志的**多种写法** ============
    # 这一组是本轮的核心：静态规则只认"标识符名字像密码"，下面这些写法
    # 静态规则**看不出来**，只有行为测试能拦。
    ("还原原始缺陷：打印 inputBuffer 明文",
     OK_LOG,
     '    qDebug() << "[passwd] OK clicked, inputBuffer=" << inputBuffer;\n'),

    ("经临时变量间接打印（静态规则看不出的写法）",
     OK_LOG,
     '    const QString leakTmp = inputBuffer;\n'
     '    qDebug() << "[passwd] len-ok tmp=" << leakTmp;\n'),

    ("打印拼接结果（静态规则看不出的写法）",
     OK_LOG,
     '    qDebug() << "[passwd] v=" << (inputBuffer + QStringLiteral("!"));\n'),

    ("打印 hex（原始缺陷的另一半）",
     OK_LOG,
     '    qDebug() << "[passwd] hex=" << inputBuffer.toUtf8().toHex();\n'),

    ("打印大写 hex",
     OK_LOG,
     '    qDebug() << "[passwd] hex=" << inputBuffer.toUtf8().toHex().toUpper();\n'),

    ("逐位日志（与整串叠加可还原全过程）",
     DIGIT_LOG,
     '    qDebug() << "[passwd] key" << btn->text();\n'),

    ("逐位日志写成 mid() 取一位",
     DIGIT_LOG,
     '    qDebug() << "[passwd] k" << inputBuffer.mid(0, 1);\n'),

    # ============ 功能召回 ============
    ("不发 passwordEntered（门永远打不开）", EMIT_RAW,
     '        (void)0;\n'),

    ("先 clear 再 emit（载荷变空串）", OK_ANCHOR,
     '    inputBuffer.clear();\n'
     '    if (!inputBuffer.isEmpty())\n'
     '        emit passwordEntered(inputBuffer);\n'),

    ("OK 后不清缓冲（下次输入被拼接）",
     '    inputBuffer.clear();\n    // 执行本行语句完成对应变量、对象或业务状态处理。\n    updateDisplay();\n    // 执行本行语句完成对应变量、对象或业务状态处理。\n    this->close();\n',
     '    // 执行本行语句完成对应变量、对象或业务状态处理。\n    updateDisplay();\n    // 执行本行语句完成对应变量、对象或业务状态处理。\n    this->close();\n'),

    ("去掉 8 位上限", MAX8, ''),

    ("数字键不追加到缓冲", APPEND, '    (void)btn;\n'),

    ("数字键追加两次", APPEND,
     '    inputBuffer += btn->text();\n    inputBuffer += btn->text();\n'),

    ("退格不删", CHOP, '        (void)0;\n'),

    ("数字键不响提示音", KEYSOUND_CALL,
     '    inputBuffer += btn->text();\n'
     '    // 执行本行语句完成对应变量、对象或业务状态处理。\n'),

    ("按钮文本不设 NoFocus", NOFOCUS, ''),

    # ============ 对照：必须不误报 ============
    # 锚点选末行的唯一片段（文件里 "// 结束当前代码块作用域。\n}\n" 出现 9 次）。
    ("对照：在文件末尾加无关注释",
     '   // qDebug() << "[passwd] key";\n// 结束当前代码块作用域。\n}\n',
     '   // qDebug() << "[passwd] key";\n// 结束当前代码块作用域。\n}\n'
     '// 无关注释（对照）\n', False),

    ("对照：改日志前缀文案",
     'qDebug() << "[passwd] 数字键按下";',
     'qDebug() << "[pwd-ui] digit pressed";', False),

    ("对照：只打印长度（本来就安全）",
     '    qDebug() << "[passwd] 确认键按下，已输入位数=" << inputBuffer.size();\n',
     '    qDebug() << "[passwd] len=" << inputBuffer.length()\n'
     '             << "empty=" << inputBuffer.isEmpty();\n', False),
]


def configure():
    return subprocess.run(
        [CMAKE, '-S', os.path.join(T, 'tests'), '-B', BUILD, '-G', 'Ninja'],
        capture_output=True, env=ENV, cwd=T)


def build():
    return subprocess.run(
        [CMAKE, '--build', BUILD, '--target', 't113_test_form_passwd'],
        capture_output=True, env=ENV, cwd=T)


def run_exe():
    return subprocess.run([EXE], capture_output=True, env=ENV, cwd=T)


def mt(p):
    return os.path.getmtime(p) if os.path.exists(p) else 0


# ---------------------------------------------------------------------------
# 就地改写**真实仓库文件**的还原保险（第 41 轮补）。
#
# 本夹具原先只靠 `finally` 把内容写回内存里的 orig。第 36/41 轮实测：
# 夹具被外部强杀时（Windows `job_kill` 走 TerminateProcess，finally /
# atexit / 信号处理器**全都不执行**），仓库里就会留下一个被变异过的源文件，
# 而且它看起来完全正常 —— 最危险的一类污染。
#
# 现在多一层 PristineGuard：磁盘副本 + 中断哨兵 + **新鲜度校验**
# （副本过期时拒绝自愈，宁可留污染也不删合法改动）+ atexit/信号 + 互斥锁。
# 它自己的单元自检：tools/verify_fixture_pristine.py。
#
# 注意：夹具内部原有的 `finally: 写回 orig` **保留**，它是第一道防线；
# 本层是唯一能在被强杀后仍然生效的那道。
# ---------------------------------------------------------------------------
_GUARD = PristineGuard(files=[SRC], tag='g35fp', lock_name='fi37_rewrite.lock')


def main():
    if not _GUARD.acquire_lock():
        print('!! 已有另一个"改写源文件"的夹具在运行，请串行执行。')
        return 3
    try:
        _GUARD.prepare()
        _GUARD.install_guards()
        return _main_locked()
    finally:
        _GUARD.restore()


def _main_locked():
    for item in MUT:
        n = _ORIG.count(item[1])
        if n != 1:
            print('[夹具错误] %s 锚点出现 %d 次（必须恰好 1 次）'
                  % (item[0], n))
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
            expect_fail = item[3] if len(item) > 3 else True

            io.open(SRC, 'w', encoding='utf-8',
                    newline='\n').write(_ORIG.replace(old, new, 1))
            if read() == _ORIG:
                print('  [夹具错误] %-44s 文件未变' % name)
                problems.append(name)
                io.open(SRC, 'w', encoding='utf-8',
                        newline='\n').write(_ORIG)
                continue

            t0 = mt(OBJ)
            rb = build()
            if rb.returncode != 0:
                print('  [构建失败] %-44s %s' % (
                    name, (rb.stdout + rb.stderr).decode(
                        'utf-8', 'replace')[-200:]))
                problems.append(name)
                io.open(SRC, 'w', encoding='utf-8',
                        newline='\n').write(_ORIG)
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
                print('  [夹具错误] %-44s .obj 未重编（可能是旧二进制）' % name)
                problems.append(name)
            elif expect_fail:
                if failed or crashed:
                    tag = ('崩溃(rc=%d, 0 条 FAIL 行)' % rr.returncode
                           if crashed else
                           '%d 条 FAIL 行(rc=%d%s)'
                           % (nfail, rr.returncode,
                              '' if consistent else ' 与计数不符'))
                    print('  %-44s 被抓到 ✓ %s' % (name, tag))
                else:
                    print('  %-44s *** 未被抓到（假绿）rc=%d'
                          % (name, rr.returncode))
                    problems.append(name)
            else:
                if not failed and not crashed:
                    print('  %-44s 通过 ✓（不误报）' % name)
                else:
                    print('  %-44s *** 误报' % name)
                    problems.append(name)

            io.open(SRC, 'w', encoding='utf-8',
                    newline='\n').write(_ORIG)
    finally:
        io.open(SRC, 'w', encoding='utf-8', newline='\n').write(_ORIG)

    same = read() == _ORIG
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

# -*- coding: utf-8 -*-
"""门禁密码日志回归测试的故障注入自检（纯 Python，Windows 路径）。

用法：python tools/verify_password_log_test.py
返回码 0 表示：每个泄漏形态都被对应用例抓到，且两个对照未误报。

要证明的核心命题
----------------
"密码不出现在日志里"这类断言有致命陷阱：若捕获管道没工作，**任何**"不包含"
断言都会通过 —— 看似很严、实则零验证。所以这里的每一个变异都朝**真实的
泄漏形态**去改 password_manager.cpp，并要求对应用例失败。

变异（全部朝真实泄漏形态）
  1. 还原 baseline 原句：input/password/两者 hex 全打  -> 应失败
  2. 只打明文密码（不打卡 hex）                        -> 应失败
  3. 只打密码的 hex                                    -> 应失败
  4. 前缀脱敏 password.left(4) + "***"                 -> 应失败
  5. 拼进字符串里再打（绕过"变量名 + <<"的静态正则形态）-> 应失败
  6. 经辅助函数转一手再打                              -> 应失败

对照（必须通过）
  7. 只打"本次是否匹配"（当前实现）                    -> 应通过
  8. 打密码**长度**而非内容                            -> 应通过
"""
import io
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fixture_pristine import PristineGuard  # noqa: E402

# 标准输出固定 UTF-8 且不可编码字符降级为替代符：本机控制台是 GBK，
# 脚本里的 ✓ 会抛 UnicodeEncodeError，且崩在**打印结果那一行**。
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

# 本脚本不需要参数，但容忍 tools/ 下其它自检脚本的调用惯例。
if len(sys.argv) > 1:
    print("提示: 本脚本不需要参数，已忽略: %s" % " ".join(sys.argv[1:]))


T = r'C:\Users\user\Desktop\linux_ai\Accessterminal_T113'
PM = os.path.join(T, 'src', 'managers', 'password_manager.cpp')
MSYS = r'C:\Users\user\.dsh\toolchains\msys64'
CMAKE = os.path.join(MSYS, 'mingw64', 'bin', 'cmake.exe')
BUILD = os.path.join(os.environ['TEMP'], 'fi27win')
EXE = os.path.join(BUILD, 't113_test_password_manager.exe')
OBJ = os.path.join(
    BUILD, 'CMakeFiles', 't113_test_password_manager.dir', 'C_', 'Users',
    'user', 'Desktop', 'linux_ai', 'Accessterminal_T113', 'src', 'managers',
    'password_manager.cpp.obj')

ENV = dict(os.environ)
ENV['PATH'] = (os.path.join(MSYS, 'mingw64', 'bin') + os.pathsep
               + os.path.join(MSYS, 'usr', 'bin') + os.pathsep + ENV['PATH'])

# 当前实现里那条"安全"日志语句，作为替换锚点。
SAFE_LOG = ('    qDebug() << "[PasswordManager] 密码校验完成，结果:" '
            '<< (matched ? "通过" : "拒绝");')

# ---------------------------------------------------------------------------
# 第 41 轮更新：P2 把门级口令改成**加盐哈希**存储后，`PasswordManager` 里
# **不再有** `password` 这个成员，因此这些变异原来引用的 `password` 变量
# 已经不存在 —— 逐一替换成 `input`（用户本次输入）。
#
# 这不是"为了编译通过而凑合"：**泄漏口本来就只剩 `input` 了**。
# 重构前有两个泄漏源（用户输入 + 已存储的正确密码）；现在产线代码里
# 根本拿不到明文口令（只有盐和摘要），所以"打日志"能泄露的只有用户输入。
# 而值守用例正是拿**正确口令**当输入去调的（`checkPassword(secret)`），
# 所以打 `input` 依然是真实泄漏，那两条用例依然抓得住。
#
# 反过来说：**原来的"已存储密码"类变异现在不可能成立了**，这正是 P2 的
# 预期效果。夹具如实反映这一点，而不是留一组编译不过的死变异。
# ---------------------------------------------------------------------------
MUT = [
    ("还原 baseline 原句（input 与双方 hex 全打）",
     '    qDebug() << "[PasswordManager] checkPassword: input=" << input\n'
     '             << ", inputHex=" << input.toUtf8().toHex();', True),

    ("只打明文输入（用户键入的正确口令会落盘）",
     '    qDebug() << "[PasswordManager] 密码校验完成，结果:"\n'
     '             << (matched ? "通过" : "拒绝") << input;', True),

    ("只打输入的 hex",
     '    qDebug() << "[PasswordManager] 密码校验完成，结果:"\n'
     '             << (matched ? "通过" : "拒绝") << input.toUtf8().toHex();', True),

    ("前缀脱敏 input.left(4)",
     '    qDebug() << "[PasswordManager] 密码校验完成，结果:"\n'
     '             << (matched ? "通过" : "拒绝") << input.left(4) + "***";', True),

    ("拼进字符串再打（绕过变量名+<<的静态正则形态）",
     '    qDebug() << QStringLiteral("[PasswordManager] 密码校验完成，结果:%1 raw=%2")\n'
     '                    .arg(matched ? "通过" : "拒绝", input);', True),

    ("经辅助函数转一手再打",
     '    { const QString dbg = input; qDebug() << "[PasswordManager] 密码校验完成，结果:"\n'
     '             << (matched ? "通过" : "拒绝") << dbg; }', True),

    ("对照：只打长度",
     '    qDebug() << "[PasswordManager] 密码校验完成，结果:"\n'
     '             << (matched ? "通过" : "拒绝") << input.size();', False),

    ("对照：只打空判定",
     '    qDebug() << "[PasswordManager] 密码校验完成，结果:"\n'
     '             << (matched ? "通过" : "拒绝") << input.isEmpty();', False),
]

# 说明：P2 之后**不再有**"已存储明文口令"这一类变异，因为产线代码里
# 已经不存在明文口令变量。夹具若保留那种变异，只会编译失败并报假红。


FAIL_CASES = ('correctPasswordNeverAppearsInLogs',
              'partialMaskingWouldStillCountAsLeak')


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
_GUARD = PristineGuard(files=[PM], tag='g27pl', lock_name='fi37_rewrite.lock')


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
    orig = io.open(PM, encoding='utf-8', errors='replace').read()
    if orig.count(SAFE_LOG) != 1:
        print('[夹具错误] 安全日志锚点出现 %d 次' % orig.count(SAFE_LOG))
        return 2

    shutil.rmtree(BUILD, ignore_errors=True)
    r = subprocess.run([CMAKE, '-S', os.path.join(T, 'tests'), '-B', BUILD,
                        '-G', 'Ninja'], capture_output=True, env=ENV, cwd=T)
    print('configure rc=%d' % r.returncode)
    if r.returncode != 0:
        print(r.stderr.decode('utf-8', 'replace')[-1500:])
        return 2
    print()

    problems = []
    try:
        for name, replacement, expect_fail in MUT:
            text = orig.replace(SAFE_LOG, replacement, 1)
            io.open(PM, 'w', encoding='utf-8', newline='\n').write(text)
            if io.open(PM, encoding='utf-8',
                       errors='replace').read() == orig:
                print('  [夹具错误] %-44s 文件未变' % name)
                problems.append(name)
                io.open(PM, 'w', encoding='utf-8',
                        newline='\n').write(orig)
                continue

            t0 = os.path.getmtime(OBJ) if os.path.exists(OBJ) else 0
            rb = subprocess.run([CMAKE, '--build', BUILD, '--target',
                                 't113_test_password_manager'],
                                capture_output=True, env=ENV, cwd=T)
            if rb.returncode != 0:
                print('  [构建失败] %-44s %s' % (
                    name, rb.stdout.decode('utf-8', 'replace')[-250:]))
                problems.append(name)
                io.open(PM, 'w', encoding='utf-8',
                        newline='\n').write(orig)
                continue
            t1 = os.path.getmtime(OBJ) if os.path.exists(OBJ) else 0

            rr = subprocess.run([EXE], capture_output=True, env=ENV, cwd=T)
            out = (rr.stdout + rr.stderr).decode('utf-8', 'replace')
            # QtTest 失败行：FAIL!  : Class::caseName() <消息>
            # 必须用正则从行首格式里取用例名 —— 按 ':' 切会切在消息内部，
            # 取到泄漏文本，导致"已抓到"被误报成"假绿"。
            failed = re.findall(r'^FAIL!\s*:\s*\w+::(\w+)\(\)', out,
                                re.M)
            leak_caught = any(f in FAIL_CASES for f in failed)

            if t1 <= t0:
                print('  [夹具错误] %-44s .obj 未重编' % name)
                problems.append(name)
            elif expect_fail:
                if leak_caught:
                    print('  %-44s 泄漏被抓到 ✓ %s' % (name, failed[:2]))
                else:
                    print('  %-44s *** 泄漏未被抓到（假绿）失败项=%s'
                          % (name, failed[:3]))
                    problems.append(name)
            else:
                if not failed:
                    print('  %-44s 通过 ✓（不误报）' % name)
                else:
                    print('  %-44s *** 误报：%s' % (name, failed[:3]))
                    problems.append(name)

            io.open(PM, 'w', encoding='utf-8', newline='\n').write(orig)
    finally:
        io.open(PM, 'w', encoding='utf-8', newline='\n').write(orig)

    same = io.open(PM, encoding='utf-8', errors='replace').read() == orig
    print()
    print('还原逐字节一致:', same)
    subprocess.run([CMAKE, '--build', BUILD, '--target',
                    't113_test_password_manager'],
                   capture_output=True, env=ENV, cwd=T)
    rr = subprocess.run([EXE], capture_output=True, env=ENV, cwd=T)
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

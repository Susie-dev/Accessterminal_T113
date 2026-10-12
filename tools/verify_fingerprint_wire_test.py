# -*- coding: utf-8 -*-
"""test_fingerprint_wire.cpp 的故障注入自检（纯 Python 驱动，Windows 路径）。

第 24 轮的教训已内建：夹具必须自证"变异确实改动文件 + .obj 确实重编 +
锚点唯一"，否则"全部仍然通过"既可能是产品问题也可能是夹具坏了。

这里注入的都是**真实可能被写错**的报文契约点：
  1. 校验和起始下标 6 -> 0（少减包头与地址）
  2. buildPacket() 不再调用唯一的 calcChecksum()（重复实现回归 / 校验和变 0）
  3. 长度字段 = payload+2 -> payload（漏掉校验和 2 字节）
  4. 长度字段大端 -> 小端
  5. 校验和大端 -> 小端
  6. 包头 0xEF01 -> 0xEF02
  7. 包标识 PKT_CMD 0x01 -> 0x02

第 31 轮起：`buildPacket()` 不再内联累加循环，而是调用 `calcChecksum()`。
于是第 25 轮那条"（已知不可区分·死代码）"变异**变成了可区分** —— 因为
被改的那份现在是唯一生效的实现。本夹具据此把它转成正常的致命变异。
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


T = r'C:\Users\user\Desktop\linux_ai\Accessterminal_T113'
SC = os.path.join(T, 'src', 'hal', 'fingerprint_hailin.cpp')
MSYS = r'C:\Users\user\.dsh\toolchains\msys64'
CMAKE = os.path.join(MSYS, 'mingw64', 'bin', 'cmake.exe')
BUILD = os.path.join(os.environ['TEMP'], 'fi25win')

ENV = dict(os.environ)
ENV['PATH'] = (os.path.join(MSYS, 'mingw64', 'bin') + os.pathsep
               + os.path.join(MSYS, 'usr', 'bin') + os.pathsep + ENV['PATH'])

MUTATIONS = [
    # 第 31 轮起：buildPacket() 不再内联累加循环，而是 `sum = calcChecksum(pkt)`。
    # 故"起始下标"这一处变异必须打到**唯一实现** calcChecksum() 身上。
    ("校验和起始下标 6 -> 0（calcChecksum，唯一实现）", [
        ("""    quint16 sum = 0;
    // 开始循环处理满足条件的数据或状态。
    for (int i = 6; i < pkt.size(); ++i) sum += (quint8)pkt[i];
    // 返回当前函数的执行结果或结束函数。
    return sum;""",
         """    quint16 sum = 0;
    // 变异：从下标 0 起算
    for (int i = 0; i < pkt.size(); ++i) sum += (quint8)pkt[i];
    return sum;""")]),

    # 同一处契约的另一半：生产 buildPacket() 必须**调用**这个辅助函数。
    # 若有人把它改回内联（重复实现回归），[fake-wire] 与 [dup-checksum]
    # 都会失败；下面这条直接从调用点入手，钉住"确实在用它"。
    ("buildPacket 不再调用 calcChecksum（改回内联 / 直接用 0）", [
        ("quint16 sum = calcChecksum(pkt);",
         "quint16 sum = 0; // 变异：不再复用唯一实现")]),

    ("长度字段漏掉校验和的 2 字节", [
        ("quint16 pktLen = payload.size() + 2; // 包长度=payload+校验和",
         "quint16 pktLen = payload.size(); // 变异")]),

    ("长度字段大端 -> 小端", [
        ("""    pkt.append((char)(pktLen >> 8));
    // 执行本行语句完成对应变量、对象或业务状态处理。
    pkt.append((char)(pktLen & 0xFF));""",
         """    pkt.append((char)(pktLen & 0xFF));
    pkt.append((char)(pktLen >> 8));""")]),

    ("校验和大端 -> 小端", [
        ("""    pkt.append((char)(sum >> 8));
    // 执行本行语句完成对应变量、对象或业务状态处理。
    pkt.append((char)(sum & 0xFF));""",
         """    pkt.append((char)(sum & 0xFF));
    pkt.append((char)(sum >> 8));""")]),

    ("包头 0xEF01 -> 0xEF02", [
        ("""    pkt.append((char)0xEF);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    pkt.append((char)0x01);""",
         """    pkt.append((char)0xEF);
    pkt.append((char)0x02);""")]),

    ("包标识 PKT_CMD 0x01 -> 0x02", [
        ("pkt.append(PKT_CMD); // 包标识", "pkt.append((char)0x02); // 变异")]),
]

EXE = os.path.join(BUILD, 't113_test_fingerprint_wire.exe')
OBJ = os.path.join(
    BUILD, 'CMakeFiles', 't113_test_fingerprint_wire.dir',
    'C_', 'Users', 'user', 'Desktop', 'linux_ai', 'Accessterminal_T113',
    'src', 'hal', 'fingerprint_hailin.cpp.obj')


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
_GUARD = PristineGuard(files=[SC], tag='g25fw', lock_name='fi37_rewrite.lock')


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
    orig = io.open(SC, encoding='utf-8', errors='replace').read()
    shutil.rmtree(BUILD, ignore_errors=True)
    r = subprocess.run(
        [CMAKE, '-S', os.path.join(T, 'tests'), '-B', BUILD, '-G', 'Ninja'],
        capture_output=True, env=ENV, cwd=T)
    print('configure rc=%d' % r.returncode)
    if r.returncode != 0:
        print(r.stderr.decode('utf-8', 'replace')[-1500:])
        return 2
    print()

    problems = []
    try:
        for name, pairs in MUTATIONS:
            bad = False
            for o, _ in pairs:
                if orig.count(o) != 1:
                    print('  [夹具错误] %-40s 锚点出现 %d 次'
                          % (name, orig.count(o)))
                    problems.append(name)
                    bad = True
                    break
            if bad:
                continue

            text = orig
            for o, n in pairs:
                text = text.replace(o, n, 1)
            io.open(SC, 'w', encoding='utf-8', newline='\n').write(text)
            if io.open(SC, encoding='utf-8', errors='replace').read() == orig:
                print('  [夹具错误] %-40s 写入后文件未变' % name)
                problems.append(name)
                io.open(SC, 'w', encoding='utf-8', newline='\n').write(orig)
                continue

            t0 = os.path.getmtime(OBJ) if os.path.exists(OBJ) else 0
            rb = subprocess.run([CMAKE, '--build', BUILD,
                                 '--target', 't113_test_fingerprint_wire'],
                                capture_output=True, env=ENV, cwd=T)
            if rb.returncode != 0:
                print('  [构建失败] %-40s %s' % (
                    name, rb.stdout.decode('utf-8', 'replace')[-200:]))
                problems.append(name)
                io.open(SC, 'w', encoding='utf-8', newline='\n').write(orig)
                continue
            t1 = os.path.getmtime(OBJ) if os.path.exists(OBJ) else 0

            rr = subprocess.run([EXE], capture_output=True, env=ENV, cwd=T)
            out = (rr.stdout + rr.stderr).decode('utf-8', 'replace')
            failed = ('\nFAIL' in out) or out.startswith('FAIL')

            if t1 <= t0:
                print('  [夹具错误] %-40s .obj 未重编' % name)
                problems.append(name)
            elif failed:
                n_fail = out.count('\nFAIL') + (1 if out.startswith('FAIL') else 0)
                print('  %-42s 已按预期失败 ✓ (%d 个用例失败)' % (name, n_fail))
            elif name.startswith('（已知不可区分'):
                print('  %-42s 仍然通过（已记录为不可区分，非致命）' % name)
            else:
                print('  %-42s *** 仍然通过（假绿）***' % name)
                problems.append(name)

            io.open(SC, 'w', encoding='utf-8', newline='\n').write(orig)
    finally:
        io.open(SC, 'w', encoding='utf-8', newline='\n').write(orig)

    print()
    print('还原逐字节一致:',
          io.open(SC, encoding='utf-8', errors='replace').read() == orig)

    subprocess.run([CMAKE, '--build', BUILD,
                    '--target', 't113_test_fingerprint_wire'],
                   capture_output=True, env=ENV, cwd=T)
    rr = subprocess.run([EXE], capture_output=True, env=ENV, cwd=T)
    out = (rr.stdout + rr.stderr).decode('utf-8', 'replace')
    for l in out.strip().split('\n'):
        if 'Totals' in l:
            print('还原后全量:', l.strip())

    print()
    print('变异 %d 个，致命问题 %d 个' % (len(MUTATIONS), len(problems)))
    if problems:
        print('问题清单:', problems)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())

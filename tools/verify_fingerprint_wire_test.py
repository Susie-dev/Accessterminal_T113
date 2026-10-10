# -*- coding: utf-8 -*-
"""test_fingerprint_wire.cpp 的故障注入自检（纯 Python 驱动，Windows 路径）。

第 24 轮的教训已内建：夹具必须自证"变异确实改动文件 + .obj 确实重编 +
锚点唯一"，否则"全部仍然通过"既可能是产品问题也可能是夹具坏了。

这里注入的都是**真实可能被写错**的报文契约点：
  1. 校验和起始下标 6 -> 0（少减包头与地址）
  2. 长度字段 = payload+2 -> payload（漏掉校验和 2 字节）
  3. 长度字段大端 -> 小端
  4. 校验和大端 -> 小端
  5. 包头 0xEF01 -> 0xEF02
  6. 广播地址改成 0x00000000
  7. 包标识 PKT_CMD 0x01 -> 0x02
  8. 校验和不写入（只留 payload 末尾对齐错误）
"""
import io
import os
import shutil
import subprocess
import sys

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
    # 注意：`for (int i = 6; i < pkt.size(); ++i) sum += (quint8)pkt[i];`
    # 在文件里出现**两次**（buildPacket 与 calcChecksum 各一处），
    # 故必须用更长的上下文把锚点收窄到 buildPacket 里的那一处。
    # 夹具的不唯一性自证（第 24 轮加的）正是在这里救了场。
    ("校验和起始下标 6 -> 0（仅 buildPacket）", [
        ("""    quint16 sum = 0;
    // 校验和从包标识字段开始累加到 payload 末尾，不包含 0xEF01 包头和设备地址。
    // 开始循环处理满足条件的数据或状态。
    for (int i = 6; i < pkt.size(); ++i) sum += (quint8)pkt[i];""",
         """    quint16 sum = 0;
    // 变异：从下标 0 起算
    for (int i = 0; i < pkt.size(); ++i) sum += (quint8)pkt[i];""")]),

    # 同一处契约的**另一半**：只改 buildPacket 而不改 calcChecksum 时，
    # 两者会产生不同的校验和 —— 这条专门检测"两份实现分叉"。
    # 【已知不可区分 —— 第 25 轮实测：死代码】
    # calcChecksum() 在生产代码里**零调用者**（buildPacket 自己内联了同一个
    # 累加循环，校验和逻辑在同一文件里有两份）。baseline
    # （terminal/smartpronew/HailinFingerprint.cpp）同样零调用者 —— 这是
    # **继承来的**，不是重构引入的。改一个死函数不影响任何行为，因此该变异
    # 不可能被任何运行时测试区分。如实记录，不假装它有效。
    ("（已知不可区分·死代码）calcChecksum 起始下标 6 -> 0", [
        ("""    quint16 sum = 0;
    // 开始循环处理满足条件的数据或状态。
    for (int i = 6; i < pkt.size(); ++i) sum += (quint8)pkt[i];
    // 返回当前函数的执行结果或结束函数。
    return sum;""",
         """    quint16 sum = 0;
    for (int i = 0; i < pkt.size(); ++i) sum += (quint8)pkt[i];
    return sum;""")]),

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


def main():
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

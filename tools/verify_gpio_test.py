# -*- coding: utf-8 -*-
"""GpioLine 测试的故障注入自检（纯 Python，Windows 路径）。

用法：python tools/verify_gpio_test.py
返回码 0 表示：每个危险退化都被抓到，且两个对照未误报。

重要：多数变异是被**进程崩溃**（非零退出码）抓到的，不是干净的
FAIL 行 —— 那些变异真的造成了 double-free / use-after-free。
夹具因此必须同时检查 FAIL 行**与**退出码；只看 FAIL 行会漏报
一半变异（本文件就是这么写的：crashed = returncode not in (0,1)）。

要证明的核心命题
----------------
本测试的价值全在"能不能抓到资源泄漏与错误处理退化"。所以每个变异都朝
**gpio.cpp 真实的危险退化**去改，并要求对应用例失败。

变异（全部是真实危险的退化）
  1. 请求输出失败时不释放线（删掉 gpiod_line_release）
  2. 请求输出失败时不关闭 chip（删掉 gpiod_chip_close）
  3. 拿不到线时不关闭 chip
  4. openOutput 开头不再先 close()（重复打开会泄漏上一条）
  5. 成功路径改为先 close chip 再 release 线（**释放顺序颠倒**）
  6. 移动构造不把源置空（双重释放 / 源仍持有）
  7. 移动赋值不释放自己的旧资源
  8. 自移动赋值不做自检（把自己释放掉）
  9. 析构不释放（RAII 完全失效）
 10. setValue 失败仍返回 true（吞掉错误）
 11. value() 不检查 m_line（无效时也去读 libgpiod）

对照（必须通过）
  C1. 只加无关注释
  C2. 把 qWarning 文案换掉（不影响语义）
"""
import io
import os
import re
import shutil
import subprocess
import sys

# 标准输出固定 UTF-8：本机控制台是 GBK，脚本里的 ✓ 会抛 UnicodeEncodeError，
# 且崩在**打印结果那一行**，极易被误读成"通过"。
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

if len(sys.argv) > 1:
    print("提示: 本脚本不需要参数，已忽略: %s" % " ".join(sys.argv[1:]))


for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

if len(sys.argv) > 1:
    print("提示: 本脚本不需要参数，已忽略: %s" % " ".join(sys.argv[1:]))


T = r'C:\Users\user\Desktop\linux_ai\Accessterminal_T113'
GP = os.path.join(T, 'src', 'hal', 'gpio.cpp')
MSYS = r'C:\Users\user\.dsh\toolchains\msys64'
CMAKE = os.path.join(MSYS, 'mingw64', 'bin', 'cmake.exe')
BUILD = os.path.join(os.environ['TEMP'], 'fi28win')
EXE = os.path.join(BUILD, 't113_test_gpio.exe')
OBJ = os.path.join(
    BUILD, 'CMakeFiles', 't113_test_gpio.dir', 'C_', 'Users', 'user',
    'Desktop', 'linux_ai', 'Accessterminal_T113', 'src', 'hal', 'gpio.cpp.obj')

ENV = dict(os.environ)
ENV['PATH'] = (os.path.join(MSYS, 'mingw64', 'bin') + os.pathsep
               + os.path.join(MSYS, 'usr', 'bin') + os.pathsep + ENV['PATH'])

REQ_OUT_FAIL = '''    if (gpiod_line_request_output(m_line, consumer.toUtf8().constData(),
                                 initialValue) < 0) {
        qWarning() << "[GPIO] 请求输出失败:" << lineOffset << "@" << chipName;
        gpiod_line_release(m_line);
        m_line = nullptr;
        gpiod_chip_close(m_chip);
        m_chip = nullptr;
        return false;
    }'''

REQ_IN_FAIL = '''    if (gpiod_line_request_input(m_line, consumer.toUtf8().constData()) < 0) {
        qWarning() << "[GPIO] 请求输入失败:" << lineOffset << "@" << chipName;
        gpiod_line_release(m_line);
        m_line = nullptr;
        gpiod_chip_close(m_chip);
        m_chip = nullptr;
        return false;
    }'''

GETLINE_OUT = '''    m_line = gpiod_chip_get_line(m_chip, lineOffset);
    if (!m_line) {
        qWarning() << "[GPIO] 无法获取线" << lineOffset << "@" << chipName;
        gpiod_chip_close(m_chip);
        m_chip = nullptr;
        return false;
    }

    if (gpiod_line_request_output'''

MUT = [
    ("请求输出失败时不释放线",
     REQ_OUT_FAIL,
     REQ_OUT_FAIL.replace('        gpiod_line_release(m_line);\n', '', 1)),

    ("请求输出失败时不关闭 chip",
     REQ_OUT_FAIL,
     REQ_OUT_FAIL.replace('        gpiod_chip_close(m_chip);\n', '', 1)),

    ("拿不到线时不关闭 chip",
     GETLINE_OUT,
     GETLINE_OUT.replace('        gpiod_chip_close(m_chip);\n', '', 1)),

    ("openOutput 开头不再先 close()（重复打开泄漏旧线）",
     '''    close();

    m_chip = gpiod_chip_open_by_name(chipName.toUtf8().constData());
    if (!m_chip) {
        qWarning() << "[GPIO] 无法打开" << chipName;
        return false;
    }

    m_line = gpiod_chip_get_line(m_chip, lineOffset);
    if (!m_line) {
        qWarning() << "[GPIO] 无法获取线" << lineOffset << "@" << chipName;
        gpiod_chip_close(m_chip);
        m_chip = nullptr;
        return false;
    }

    if (gpiod_line_request_output''',
     '''    // 不再先 close()
    m_chip = gpiod_chip_open_by_name(chipName.toUtf8().constData());
    if (!m_chip) {
        qWarning() << "[GPIO] 无法打开" << chipName;
        return false;
    }

    m_line = gpiod_chip_get_line(m_chip, lineOffset);
    if (!m_line) {
        qWarning() << "[GPIO] 无法获取线" << lineOffset << "@" << chipName;
        gpiod_chip_close(m_chip);
        m_chip = nullptr;
        return false;
    }

    if (gpiod_line_request_output'''),

    ("close() 顺序颠倒：先关 chip 再释放线",
     '''    if (m_line) {
        gpiod_line_release(m_line);
        m_line = nullptr;
    }
    if (m_chip) {
        gpiod_chip_close(m_chip);
        m_chip = nullptr;
    }''',
     '''    if (m_chip) {
        gpiod_chip_close(m_chip);
        m_chip = nullptr;
    }
    if (m_line) {
        gpiod_line_release(m_line);
        m_line = nullptr;
    }'''),

    ("移动构造不把源置空",
     '''GpioLine::GpioLine(GpioLine&& other) noexcept
    : m_chip(other.m_chip), m_line(other.m_line)
{
    other.m_chip = nullptr;
    other.m_line = nullptr;
}''',
     '''GpioLine::GpioLine(GpioLine&& other) noexcept
    : m_chip(other.m_chip), m_line(other.m_line)
{
}'''),

    ("移动赋值不释放自己的旧资源",
     '''    if (this != &other) {
        close();
        m_chip = other.m_chip;''',
     '''    if (this != &other) {
        m_chip = other.m_chip;'''),

    ("自移动赋值不做自检（会把自己释放掉）",
     '''    if (this != &other) {
        close();
        m_chip = other.m_chip;
        m_line = other.m_line;
        other.m_chip = nullptr;
        other.m_line = nullptr;
    }''',
     '''    close();
    m_chip = other.m_chip;
    m_line = other.m_line;
    other.m_chip = nullptr;
    other.m_line = nullptr;'''),

    ("析构不释放（RAII 完全失效）",
     '''GpioLine::~GpioLine()
{
    close();
}''',
     '''GpioLine::~GpioLine()
{
}'''),

    ("setValue 失败仍返回 true（吞掉错误）",
     '''    return gpiod_line_set_value(m_line, v) >= 0;''',
     '''    gpiod_line_set_value(m_line, v);
    return true;'''),

    ("value() 不检查 m_line（无效时也读）",
     '''    if (!m_line) {
        return -1;
    }
    return gpiod_line_get_value(m_line);''',
     '''    return gpiod_line_get_value(m_line);'''),

    ("对照：加无关注释",
     'namespace accessterminal {',
     'namespace accessterminal {\n// 无关注释', False),

    # 注意："[GPIO] 无法打开" 在 gpio.cpp 里出现 2 次（openOutput 与
    # openInput 各一次）。夹具的"锚点唯一"自证会拒绝它 —— 不收窄就会
    # 只改到其中一个函数。用完整上下文钉到 openOutput 那一处。
    # 注意：openOutput 与 openInput 的"open 失败块"**逐字节相同**
    # （同一工程的平行实现），单用该块做锚点会命中 2 处。夹具的
    # "锚点唯一"自证会拒绝它 —— 这正是第 39 条：锚点要唯一到目标函数，
    # 必须带上只有目标处才有的上下文（这里把函数签名包进来）。
    ("对照：改 qWarning 文案（不影响语义）",
     '''bool GpioLine::openOutput(const QString& chipName, unsigned int lineOffset,
                          const QString& consumer, int initialValue)
{
    close();

    m_chip = gpiod_chip_open_by_name(chipName.toUtf8().constData());
    if (!m_chip) {
        qWarning() << "[GPIO] 无法打开" << chipName;
        return false;
    }''',
     '''bool GpioLine::openOutput(const QString& chipName, unsigned int lineOffset,
                          const QString& consumer, int initialValue)
{
    close();

    m_chip = gpiod_chip_open_by_name(chipName.toUtf8().constData());
    if (!m_chip) {
        qWarning() << "[GPIO] open failed" << chipName;
        return false;
    }''', False),
]


def main():
    orig = io.open(GP, encoding='utf-8', errors='replace').read()
    for item0 in MUT:
        name, old = item0[0], item0[1]
        if orig.count(old) != 1:
            print('[夹具错误] %s 锚点出现 %d 次' % (name, orig.count(old)))
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
        for item in MUT:
            name, old, new = item[0], item[1], item[2]
            expect_fail = item[3] if len(item) > 3 else True

            text = orig.replace(old, new, 1)
            io.open(GP, 'w', encoding='utf-8', newline='\n').write(text)
            if io.open(GP, encoding='utf-8',
                       errors='replace').read() == orig:
                print('  [夹具错误] %-42s 文件未变' % name)
                problems.append(name)
                io.open(GP, 'w', encoding='utf-8',
                        newline='\n').write(orig)
                continue

            t0 = os.path.getmtime(OBJ) if os.path.exists(OBJ) else 0
            rb = subprocess.run([CMAKE, '--build', BUILD, '--target',
                                 't113_test_gpio'],
                                capture_output=True, env=ENV, cwd=T)
            if rb.returncode != 0:
                print('  [构建失败] %-42s %s' % (
                    name, rb.stdout.decode('utf-8', 'replace')[-220:]))
                problems.append(name)
                io.open(GP, 'w', encoding='utf-8',
                        newline='\n').write(orig)
                continue
            t1 = os.path.getmtime(OBJ) if os.path.exists(OBJ) else 0

            rr = subprocess.run([EXE], capture_output=True, env=ENV, cwd=T)
            out = (rr.stdout + rr.stderr).decode('utf-8', 'replace')
            failed = re.findall(r'^FAIL!\s*:\s*\w+::(\w+)\(\)', out, re.M)
            crashed = rr.returncode not in (0, 1)

            if t1 <= t0:
                print('  [夹具错误] %-42s .obj 未重编' % name)
                problems.append(name)
            elif expect_fail:
                if failed or crashed:
                    tag = '崩溃(%d)' % rr.returncode if crashed else str(failed[:2])
                    print('  %-42s 被抓到 ✓ %s' % (name, tag))
                else:
                    print('  %-42s *** 未被抓到（假绿）rc=%d' % (name, rr.returncode))
                    problems.append(name)
            else:
                if not failed and not crashed:
                    print('  %-42s 通过 ✓（不误报）' % name)
                else:
                    print('  %-42s *** 误报：%s' % (name, failed[:3]))
                    problems.append(name)

            io.open(GP, 'w', encoding='utf-8', newline='\n').write(orig)
    finally:
        io.open(GP, 'w', encoding='utf-8', newline='\n').write(orig)

    same = io.open(GP, encoding='utf-8', errors='replace').read() == orig
    print()
    print('还原逐字节一致:', same)
    subprocess.run([CMAKE, '--build', BUILD, '--target', 't113_test_gpio'],
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

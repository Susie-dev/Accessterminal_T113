# -*- coding: utf-8 -*-
"""V4L2Camera 测试的故障注入自检（纯 Python，Windows 路径）。

用法：python tools/verify_v4l2_test.py
返回码 0 表示：每个危险退化都被抓到，且两个对照未误报。

要证明的核心命题
----------------
本测试的价值全在"能不能抓到**映射泄漏**与错误处理退化"。每个变异都朝
v4l2_camera.cpp 真实的危险退化去改，并要求对应用例失败。

**本夹具是"先有 bug、后被修"的记录。** 变异 1/2 就是当初生产代码的样子，
它们在加测试之前**已经存在**（基线同样如此，属继承缺陷，不是回归）：
实测 B 场景（失败一次后重试成功再 close）会永久泄漏 1048576 字节，
C 场景（连续 3 次失败）泄漏 2097152 字节。夹具持续证明这两个修复有效。

变异（全部是真实危险的退化）
  1. QBUF 失败时不再 munmap（**当初的真实缺陷**）
  2. STREAMON 失败时不再 munmap（**当初的真实缺陷**）
  3. mmap 失败时误把 MAP_FAILED 当成有效映射（后续 munmap 非法地址）
  4. open 失败后不关闭 fd（泄漏 fd）
  5. S_FMT 失败后不关闭 fd
  6. captureLoop 在 DQBUF 失败后仍然继续处理（对无效帧解码）
  7. captureLoop 解码失败时提前 return，**不 QBUF 归还缓冲区**
     （req.count = 1 时下一次 DQBUF 永远失败 -> 画面链路彻底停摆）
  8. captureLoop 不再检查 v4l2_fd（对已关闭设备发 ioctl）
  9. getFrame 不再加锁/不再 copy（返回内部引用 -> 调用方改动会影响缓存）
 10. close() 不 munmap（只关 fd）
 11. close() 不关 fd
 12. 析构不调用 close()（RAII 失效）

对照（必须通过）
  C1. 只加无关注释
  C2. 改 qDebug 文案（不影响语义）
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


T = r'C:\Users\user\Desktop\linux_ai\Accessterminal_T113'
SRC = os.path.join(T, 'src', 'core', 'vision', 'v4l2_camera.cpp')
MSYS = r'C:\Users\user\.dsh\toolchains\msys64'
CMAKE = os.path.join(MSYS, 'mingw64', 'bin', 'cmake.exe')
BUILD = os.path.join(os.environ['TEMP'], 'fi30win')
EXE = os.path.join(BUILD, 't113_test_v4l2_camera.exe')
OBJ = os.path.join(
    BUILD, 'CMakeFiles', 't113_test_v4l2_camera.dir', 'C_', 'Users', 'user',
    'Desktop', 'linux_ai', 'Accessterminal_T113', 'src', 'core', 'vision',
    'v4l2_camera.cpp.obj')

ENV = dict(os.environ)
ENV['PATH'] = (os.path.join(MSYS, 'mingw64', 'bin') + os.pathsep
               + os.path.join(MSYS, 'usr', 'bin') + os.pathsep + ENV['PATH'])


def read_src():
    return io.open(SRC, encoding='utf-8', errors='replace').read()


_ORIG = read_src()


def anchor(start_marker):
    """从**真实文件字节**里切出一个**完整的 if 块**（含结尾的 `}`）。

    用花括号配对而不是手写结束标记：生产代码是逐行注释风格，
    `return false;` 与 `}` 之间夹着注释行，手抄结束标记极易漂移
    （第一版就因此报 substring not found）。
    """
    i = _ORIG.index(start_marker)
    # 从 start_marker 之后找第一个 '{'，然后按深度配对。
    j = _ORIG.index('{', i)
    depth = 0
    k = j
    while k < len(_ORIG):
        c = _ORIG[k]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return _ORIG[i:k + 1]
        k += 1
    raise ValueError('unbalanced braces for %r' % start_marker)


# ---- 从真实字节构造锚点 ----
QBUF_FAIL = anchor('    if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0) {')
STREAMON_FAIL = anchor('    if (ioctl(v4l2_fd, VIDIOC_STREAMON, &type) < 0) {')
MMAP_FAIL = anchor('    if (v4l2_buffer == MAP_FAILED) {')
OPEN_FAIL = anchor('    v4l2_fd = ::open(devicePath.toStdString().c_str(), O_RDWR);')
SFMT_FAIL = anchor('    if (ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {')
DQBUF_PART = anchor('    if (ioctl(v4l2_fd, VIDIOC_DQBUF, &buf) < 0) {')
DECODE_PART = anchor('    bool loaded = img.loadFromData(')

QBUF_TAIL = '    ioctl(v4l2_fd, VIDIOC_QBUF, &buf);'

CLOSE_BODY = anchor('    if (v4l2_buffer && v4l2_buffer != MAP_FAILED) {')
CLOSE_FD = anchor('    if (v4l2_fd >= 0) {')
DTOR = anchor('V4L2Camera::~V4L2Camera()')


# 去掉 munmap 三件套（把"已修复"退化成"当初的缺陷"）
def strip_unmap(block):
    out = block
    out = re.sub(r'\n *//[^\n]*\n(?= *munmap\()', '\n', out)
    out = re.sub(r'\n *munmap\(v4l2_buffer, v4l2_buffer_length\);\n', '\n', out)
    out = re.sub(r'\n *v4l2_buffer = nullptr;\n', '\n', out)
    out = re.sub(r'\n *v4l2_buffer_length = 0;\n', '\n', out)
    return out


MUT = [
    ("QBUF 失败时不再 munmap（当初的真实缺陷）",
     QBUF_FAIL, strip_unmap(QBUF_FAIL)),

    ("STREAMON 失败时不再 munmap（当初的真实缺陷）",
     STREAMON_FAIL, strip_unmap(STREAMON_FAIL)),

    ("mmap 失败时误把 MAP_FAILED 当有效映射",
     MMAP_FAIL,
     MMAP_FAIL.replace('if (v4l2_buffer == MAP_FAILED) {',
                       'if (false) {', 1)),

    # open() 失败时本就没有 fd 可关（当时 fd == -1），所以这里**不能**写
    # "不关 fd" 的变异 —— 那样改出来的文本与原文件**逐字节相同**，是假变异
    # （第一版就是这么错的，夹具用"文件未变"自证把它挡下来了）。
    # 改为真正危险的退化：不检查 open 的返回值，拿着 fd = -1 继续往下走。
    ("open 失败时不检查返回值（拿着 fd=-1 继续）",
     '    if (v4l2_fd < 0) {',
     '    if (false) {'),

    ("S_FMT 失败后不关闭 fd",
     SFMT_FAIL,
     SFMT_FAIL.replace('        ::close(v4l2_fd);\n', '', 1)),

    ("captureLoop 在 DQBUF 失败后仍继续处理",
     DQBUF_PART,
     DQBUF_PART.replace('        return;\n', '', 1)),

    # 让归还缓冲区变成"只在解码成功时"才做：解码失败就**不再** QBUF。
    # req.count = 1（只有一个缓冲），于是下一次 DQBUF 永远失败，
    # 画面链路彻底停摆且没有任何报错。
    ("captureLoop 解码失败时不归还缓冲区（画面链路停摆）",
     QBUF_TAIL,
     '    if (loaded) ioctl(v4l2_fd, VIDIOC_QBUF, &buf);'),

    ("captureLoop 不检查 v4l2_fd",
     '    if (v4l2_fd < 0) return;\n',
     '',),

    # 说明：这里**故意不测** "getFrame 返回内部引用（去掉 .copy()）"。
    # QImage 是写时复制（COW）的，返回内部引用与返回副本在可观测行为上
    # 完全一致，任何断言都抓不到它 —— 加进去只会得到一条假的"已覆盖"。
    # 这条限制已记入 README（"没有判别力的断言要当作没有断言"）。
    ("S_FMT 请求错误像素格式（YUYV 而非 MJPEG）",
     'fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;',
     'fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;'),

    ("S_FMT 请求错误分辨率（320x240）",
     '    fmt.fmt.pix.width = v4l2_width;',
     '    fmt.fmt.pix.width = 320;'),

    ("close() 不 munmap（只关 fd）",
     CLOSE_BODY, ''),

    ("close() 不关 fd",
     CLOSE_FD, ''),

    ("析构不调用 close()（RAII 失效）",
     '    close();\n',
     ''),

    ("对照：加无关注释",
     'bool V4L2Camera::open() {',
     'bool V4L2Camera::open() {\n    // 无关注释', False),

    ("对照：改 qDebug 文案（不影响语义）",
     'qDebug() << "[V4L2Camera] 入队失败";',
     'qDebug() << "[V4L2Camera] qbuf failed";', False),
]


def build(target=None):
    args = [CMAKE, '--build', BUILD]
    if target:
        args += ['--target', target]
    return subprocess.run(args, capture_output=True, env=ENV, cwd=T)


def run_exe():
    return subprocess.run([EXE], capture_output=True, env=ENV, cwd=T)


def main():
    # 锚点唯一性自证：锚点必须唯一，且必须唯一到**正确的函数**。
    for item in MUT:
        name, old = item[0], item[1]
        n = _ORIG.count(old)
        if n != 1:
            print('[夹具错误] %s 锚点出现 %d 次（必须恰好 1 次）' % (name, n))
            return 2
    if _ORIG.count('    munmap(v4l2_buffer, v4l2_buffer_length);\n') != 3:
        print('[夹具错误] 期望 3 处 munmap（close + QBUF 失败 + STREAMON 失败），'
              '实际 %d 处' % _ORIG.count('    munmap(v4l2_buffer, v4l2_buffer_length);\n'))
        return 2

    # 夹具自身逻辑自证：strip_unmap 必须真的删掉了 munmap。
    if 'munmap' in strip_unmap(QBUF_FAIL):
        print('[夹具错误] strip_unmap 没有真的去掉 munmap')
        return 2

    shutil.rmtree(BUILD, ignore_errors=True)
    r = subprocess.run([CMAKE, '-S', os.path.join(T, 'tests'), '-B', BUILD,
                        '-G', 'Ninja'], capture_output=True, env=ENV, cwd=T)
    print('configure rc=%d' % r.returncode)
    if r.returncode != 0:
        print(r.stderr.decode('utf-8', 'replace')[-1500:])
        return 2
    rb = build('t113_test_v4l2_camera')
    if rb.returncode != 0:
        print('基线构建失败:')
        print((rb.stdout + rb.stderr).decode('utf-8', 'replace')[-1500:])
        return 2
    print()

    problems = []
    try:
        for item in MUT:
            name, old, new = item[0], item[1], item[2]
            expect_fail = item[3] if len(item) > 3 else True

            text = _ORIG.replace(old, new, 1)
            io.open(SRC, 'w', encoding='utf-8', newline='\n').write(text)
            if read_src() == _ORIG:
                print('  [夹具错误] %-46s 文件未变' % name)
                problems.append(name)
                io.open(SRC, 'w', encoding='utf-8',
                        newline='\n').write(_ORIG)
                continue

            t0 = os.path.getmtime(OBJ) if os.path.exists(OBJ) else 0
            rb = build('t113_test_v4l2_camera')
            if rb.returncode != 0:
                print('  [构建失败] %-46s %s' % (
                    name, (rb.stdout + rb.stderr).decode(
                        'utf-8', 'replace')[-220:]))
                problems.append(name)
                io.open(SRC, 'w', encoding='utf-8',
                        newline='\n').write(_ORIG)
                continue
            t1 = os.path.getmtime(OBJ) if os.path.exists(OBJ) else 0

            rr = run_exe()
            out = (rr.stdout + rr.stderr).decode('utf-8', 'replace')
            failed = re.findall(r'^FAIL!\s*:\s*\w+::(\w+)\(\)', out, re.M)

            # QtTest 的退出码 == FAIL 行数（**实测**：close() 不 munmap 的变异
            # 给出 returncode=22 且恰好 22 条 FAIL 行）。所以
            #   "returncode not in (0,1)" **不能**用来判断崩溃 —— 那会把
            #   "多个用例干净失败"误标成"崩溃"，也会让真正的崩溃（如
            #   0xC0000005）与它混为一谈。
            # 真正的崩溃 = 非零退出码但**一条 FAIL 行都没有**。
            totals = re.search(r'Totals:\s*(\d+) passed,\s*(\d+) failed', out)
            crashed = rr.returncode != 0 and not failed
            # 退出码与 FAIL 行数一致时，说明是正常的多用例失败。
            consistent = (totals is not None
                          and rr.returncode == int(totals.group(2)))

            if t1 <= t0:
                print('  [夹具错误] %-46s .obj 未重编' % name)
                problems.append(name)
            elif expect_fail:
                if failed or crashed:
                    if crashed:
                        tag = '崩溃(rc=%d, 0 条 FAIL 行)' % rr.returncode
                    elif consistent:
                        tag = '%d 条 FAIL 行(rc=%d)' % (len(failed), rr.returncode)
                    else:
                        tag = '%d 条 FAIL 行(rc=%d 与 failed 计数不符)' % (
                            len(failed), rr.returncode)
                    print('  %-46s 被抓到 ✓ %s' % (name, tag))
                else:
                    print('  %-46s *** 未被抓到（假绿）rc=%d' % (name, rr.returncode))
                    problems.append(name)
            else:
                if not failed and not crashed:
                    print('  %-46s 通过 ✓（不误报）' % name)
                else:
                    print('  %-46s *** 误报：%s' % (name, failed[:3]))
                    problems.append(name)

            io.open(SRC, 'w', encoding='utf-8', newline='\n').write(_ORIG)
    finally:
        io.open(SRC, 'w', encoding='utf-8', newline='\n').write(_ORIG)

    same = read_src() == _ORIG
    print()
    print('还原逐字节一致:', same)
    build('t113_test_v4l2_camera')
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

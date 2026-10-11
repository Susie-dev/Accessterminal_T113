# -*- coding: utf-8 -*-
"""t113_test_form_finger 故障注入自检（纯 Python 驱动）。

本文件要证明的核心命题
----------------------
`src/ui/form_finger.cpp` 的指纹注册状态机**真的被测到了**，而不是"界面
点得动、测试数量凑够了"。因此第一条变异就是把**信号互斥**删掉
（`setSuspended(true)`）：如果删掉之后测试仍然通过，这个测试目标毫无价值。

本文件的第二个命题更微妙：**"崩溃"不能作为回归守卫**。
第 37 轮先写的 use-after-free 回归测试是"不点退出让窗体直接析构，看进程崩不崩"。
故障注入实测：把 `QPointer` 改回裸指针（= 缺陷复发）后重编译重新运行，
**变异体 5/5 退出码全为 0，一次都没崩** —— use-after-free 是未定义行为，
刚释放的内存常常仍可读。详见 UNDETECTABLE 里的记录。所以那条路径改由
静态规则 `check_form_finger_suspended_manager_is_guarded` 守，本文件反而要
**断言"改成裸指针后本测试仍然通过"**，把这个反直觉的事实固定下来，防止
以后有人再写一条依赖"它会崩"的假守卫。
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
BUILD = os.path.join(os.environ['TEMP'], 'fi37win')
EXE = os.path.join(BUILD, 't113_test_form_finger.exe')

FF = os.path.join(T, 'src', 'ui', 'form_finger.cpp')
FH = os.path.join(T, 'src', 'ui', 'form_finger.h')
TF = os.path.join(T, 'tests', 'test_form_finger.cpp')
FPM = os.path.join(T, 'src', 'managers', 'fingerprint_manager.cpp')
SW = os.path.join(T, 'tests', 'shadows', 'ui', 'widget.h')
CM = os.path.join(T, 'tests', 'CMakeLists.txt')

OBJ = os.path.join(
    BUILD, 'CMakeFiles', 't113_test_form_finger.dir', 'C_', 'Users', 'user',
    'Desktop', 'linux_ai', 'Accessterminal_T113', 'src', 'ui',
    'form_finger.cpp.obj')

ENV = dict(os.environ)
ENV['PATH'] = (os.path.join(MSYS, 'mingw64', 'bin') + os.pathsep
               + os.path.join(MSYS, 'usr', 'bin') + os.pathsep + ENV['PATH'])
ENV['QT_QPA_PLATFORM'] = 'offscreen'

FAIL_RE = re.compile(r'^FAIL!\s*:', re.M)


def read(p):
    return io.open(p, encoding='utf-8').read()


FILES = (FF, FH, TF, SW, CM)

# ---------------------------------------------------------------------------
# 还原保险（README §61）：`finally` 不是还原保证。
# 磁盘原始副本 + 启动自愈 + atexit/SIGINT/SIGTERM/SIGBREAK。
#
# 第 36 轮真实事故：第一版只把原文放内存里靠 finally 还原，脚本在第一个变异
# 处被外部强杀，仓库里留下一个"看起来完全正常、只是少了一行"的源文件，
# 直到下次运行预检报"锚点出现 0 次"才暴露。脚本自己把被测代码改坏了还静默
# 退出 —— 最危险的一类污染。
# ---------------------------------------------------------------------------
BACKUP = os.path.join(os.environ['TEMP'], 'fi37_pristine')
MARKER = os.path.join(BACKUP, '.in-progress')
_ORIG = {}


def _backup_path(p):
    return os.path.join(BACKUP, os.path.basename(p))


def _load_pristine():
    """建立/校验磁盘原始副本；返回被自愈的文件列表。

    **只在"上一次运行被中断"时自愈。**
    ----------------------------------------------------------------------
    第 37 轮踩到的设计缺陷：第一版只要发现"磁盘副本 != 当前文件"就用副本
    覆盖当前文件。这在"上一次被强杀留下污染"时是对的，但在**我自己刚改了
    被测文件之后**（完全正常的工作流）就变成了**静默回滚我的修改** ——
    实测就这么丢掉了一整轮测试补强，而且看起来一切正常（基线还是绿的）。

    正确判据是"上一次运行是否**被中断**"，而不是"文件是否与副本不同"：
      - 启动时若存在 `.in-progress` 标记 → 上次没跑完（被强杀/崩溃）→ 自愈；
      - 否则 → 上次是正常结束的，副本只是**过期**了 → 用当前文件刷新副本，
        绝不覆盖用户/开发者的修改。
    标记在启动时创建、在正常结束时删除；`finally`/信号处理器也会把它删掉。
    """
    healed = []
    os.makedirs(BACKUP, exist_ok=True)
    interrupted = os.path.exists(MARKER)
    for p in FILES:
        b = _backup_path(p)
        if os.path.exists(b) and interrupted:
            backup_text = read(b)
            if read(p) != backup_text:
                io.open(p, 'w', encoding='utf-8',
                        newline='\n').write(backup_text)
                healed.append(os.path.basename(p))
            _ORIG[p] = backup_text
        else:
            # 没有中断证据：以**当前**文件为准刷新副本（副本可能已过期）。
            _ORIG[p] = read(p)
            io.open(b, 'w', encoding='utf-8',
                    newline='\n').write(_ORIG[p])
    # 标记"本次运行开始"，供下一次启动判断是否被中断。
    io.open(MARKER, 'w', encoding='utf-8', newline='\n').write('running\n')
    return healed


def _restore_all(*_a):
    for p in FILES:
        if p in _ORIG:
            try:
                io.open(p, 'w', encoding='utf-8',
                        newline='\n').write(_ORIG[p])
            except OSError:
                pass
    try:
        os.remove(MARKER)
    except OSError:
        pass
    try:
        _release_lock()
    except NameError:
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


# ---------------------------------------------------------------------------
# 并发互斥：**两个改写同一批源文件的夹具绝不能同时跑**。
#
# 第 37 轮真实踩到：行为夹具还在后台跑时，我又触发了规则夹具（以及一次手工
# 复现），三者都在改 form_finger.cpp / .h。结果是**交叉污染** ——
# 规则夹具报"析构时不恢复"没被抓到、行为夹具报两个莫名其妙的误报/假绿，
# 而单独串行重跑时**全部正常**。这属于第 36 轮"不要在扫描期间跑 verify.py"
# 的同类错误，只是这次是两个扫描器互相踩。
#
# 用文件锁（独占创建）挡住：拿到锁才开工，否则立刻退出并说明原因。
# 锁带 pid，便于人工确认；进程正常结束/被中断都会释放。
# ---------------------------------------------------------------------------
LOCK = os.path.join(os.environ['TEMP'], 'fi37_rewrite.lock')


def _acquire_lock():
    for _ in range(2):
        try:
            fd = os.open(LOCK, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, ('%d\n' % os.getpid()).encode('ascii'))
            os.close(fd)
            return True
        except OSError:
            # 已存在：判断持有者是否还活着（陈旧锁要能自动接管）。
            try:
                with io.open(LOCK, encoding='ascii') as fh:
                    other = int(fh.read().strip())
            except (OSError, ValueError):
                other = None
            alive = False
            if other is not None:
                try:
                    os.kill(other, 0)
                    alive = True
                except OSError:
                    alive = False
            if alive:
                return False
            try:
                os.remove(LOCK)
            except OSError:
                return False
    return False


def _release_lock():
    try:
        os.remove(LOCK)
    except OSError:
        pass


def need(s, n=1, p=FF):
    c = _ORIG[p].count(s)
    if c != n:
        raise AssertionError('%s 锚点出现 %d 次（期望 %d）: %r'
                             % (os.path.basename(p), c, n, s[:70]))
    return s


def target_of(old, cnt=1):
    for cand in FILES:
        if _ORIG[cand].count(old) == cnt:
            return cand
    return None


_HEALED = _load_pristine()
_install_guards()

# ---- 真实锚点（逐字节从文件抄；缩进必须精确）----

# form_finger.cpp：挂起/恢复常驻识别
SUSPEND_TRUE = need('        m_suspendedFingerprintManager->setSuspended(true);\n')
# 两处恢复调用文本完全相同，必须连带后续上下文才能定位到**具体函数**。
# 退出路径后面是 this->close()；析构路径后面是 "释放动态创建的对象资源"。
SUSPEND_FALSE_QUIT = need(
    '        m_suspendedFingerprintManager->setSuspended(false);\n'
    '        m_suspendedFingerprintManager = nullptr;\n'
    '    }\n\n    // 执行本行语句完成对应变量、对象或业务状态处理。\n'
    '    this->close();\n')
SUSPEND_FALSE_DTOR = need(
    '        m_suspendedFingerprintManager->setSuspended(false);\n'
    '        m_suspendedFingerprintManager = nullptr;\n'
    '    }\n    // 释放动态创建的对象资源。\n')
# 清空句柄（保证"恢复成对"不会退化成"只能恢复一次"），同样分两处。
CLEAR_HANDLE_QUIT = need(
    '        m_suspendedFingerprintManager = nullptr;\n'
    '    }\n\n    // 执行本行语句完成对应变量、对象或业务状态处理。\n'
    '    this->close();\n')
CLEAR_HANDLE_DTOR = need(
    '        m_suspendedFingerprintManager = nullptr;\n'
    '    }\n    // 释放动态创建的对象资源。\n')
# 停表。两处调用文本相同，用各自的后续注释精确定位到具体函数。
STOP_POLL_QUIT = need('    stopFingerPoll();\n    // 同时复位等待状态。')
STOP_POLL_DTOR = need('    stopFingerPoll();\n    // 兜底恢复')
# waitingFinger 复位（分散在多条成功/失败路径上）
WAITING_OFF = need('    waitingFinger = false;\n', 6)
# 采集图像失败那条路径的复位（紧跟失败日志）。用来验证"失败后能重试"。
WAITING_OFF_COLLECT_FAIL = need(
    '        qDebug() << QString("第%1次采集图像失败").arg(step);\n'
    '        // 执行本行语句完成对应变量、对象或业务状态处理。\n'
    '        waitingFinger = false;\n')
WAITING_GUARD = need('    if (waitingFinger) return; // 防止重复点击\n')
# 轮询定时器创建与 500ms 周期
POLL_TIMER_NEW = need('    m_pollTimer = new QTimer(this);\n')
POLL_TIMER_START = need('    timer->start(500);\n')
# 探手指命令 0x01 / 超时 500（采集路径另有一条 readPacket(2000)）。
# 两处缩进不同：定时器回调里是 8 空格，doFingerCollect 里是 4 空格。
# 4 空格那行是 8 空格那行的后缀，必须带上前置上下文才能唯一定位。
GET_IMAGE_POLL = need('        payload.append((char)0x01); // CMD_GET_IMAGE\n')
GET_IMAGE_COLLECT = need(
    '    payload.append((char)0x01); // CMD_GET_IMAGE\n'
    '    // 创建或使用字节数组保存协议或音视频数据。\n'
    '    QByteArray pkt = mainWidget->fingerprint()->buildPacket(payload);\n'
    '    // 执行本行语句完成对应变量、对象或业务状态处理。\n'
    '    mainWidget->fingerprint()->sendPacket(pkt);\n')
GET_IMAGE = GET_IMAGE_POLL
READ_500 = need('        QByteArray resp = mainWidget->fingerprint()->readPacket(500);\n')
# 采集路径的 2000ms 读（用来验证超时语义没被整体改掉）
READ_2000 = need(
    '    QByteArray resp = mainWidget->fingerprint()->readPacket(2000);\n')
# 采集：GEN_CHAR(0x02)
GEN_CHAR = need('    payload.append((char)0x02); // CMD_GEN_CHAR\n')
# 存储：STORE_CHAR(0x06) + 大端 id
STORE_CHAR = need('        payload.append((char)0x06); // CMD_STORE_CHAR\n')
ID_HI = need('        payload.append((char)(id >> 8));\n')
ID_LO = need('        payload.append((char)(id & 0xFF));\n')
# 成功后递增 ID
INCREMENT = need('        mainWidget->incrementNextFingerId();\n')
# 第 1 步采集成功的提示音
TONE_ONE = need(
    '    if (step == 1) mainWidget->audioManager()->playAsync('
    'accessterminal::AppConfig::instance().pcmFile('
    'QStringLiteral("fingerone.pcm")));\n')

# 影子 Widget：管理器/设备注入点必须存在（否则测试连不上真实现）
SHIM_MGR = need('    FingerprintManager* m_fingerprintManager = nullptr;\n', 1, SW)
SHIM_DEV = need('    HailinFingerprint* m_fingerprint = nullptr;\n', 1, SW)

MUT = [
    # ================= 核心：串口互斥（本文件存在的首要理由）=================
    #
    # 注册期间必须挂起常驻识别：两者操作同一个串口，识别轮询每 500ms 下发
    # CMD_GET_IMAGE 并抢读应答，会把注册流程等待的应答取走。
    ("注册时不挂起常驻识别（抢同一个串口）", SUSPEND_TRUE, '    (void)0;\n'),

    ("退出时不恢复常驻识别（门禁指纹永久失效）", SUSPEND_FALSE_QUIT,
     '        m_suspendedFingerprintManager = nullptr;\n'
     '    }\n\n    // 执行本行语句完成对应变量、对象或业务状态处理。\n'
     '    this->close();\n', 1),

    ("析构时不恢复常驻识别（兜底路径失效）", SUSPEND_FALSE_DTOR,
     '        m_suspendedFingerprintManager = nullptr;\n'
     '    }\n    // 释放动态创建的对象资源。\n', 1),

    # 句柄不清空 -> "成对恢复"退化成"只有第一次能恢复"。
    # 本条在 enterExitTwiceKeepsSuspensionPaired 上应当被抓到。
    ("退出后不清空句柄（第二轮注册无法再挂起）", CLEAR_HANDLE_QUIT,
     '    }\n\n    // 执行本行语句完成对应变量、对象或业务状态处理。\n'
     '    this->close();\n', 1),

    # 注："析构后不清空句柄"**故意不在这里** —— 析构只执行一次，而
    # setSuspended() 开头有 `if (m_suspended == suspended) return;`，
    # 清不清句柄在行为上完全不可观测（故障注入实测 rc=0）。它属于
    # UNDETECTABLE，由静态规则守"两处恢复都成对清空"。

    # ================= 轮询定时器生命周期 =================
    ("退出时不停轮询定时器（GUI 线程被 500ms 阻塞读拖死）",
     STOP_POLL_QUIT, '    // 同时复位等待状态。'),
    # 注："析构时不停轮询定时器"**故意不在这里** —— 轮询定时器是以 form 为
    # parent 的（`m_pollTimer = new QTimer(this)`），窗体析构时它会随子对象
    # 一起被销毁，且析构过程不跑事件循环，所以去掉析构里的 stopFingerPoll()
    # 在行为上不可观测（故障注入实测 rc=0）。真正的风险在**退出**路径：
    # close() 只是隐藏窗口、对象仍活着，那里不停表才会让定时器继续跑。
    # 该防御性调用由静态规则守（析构路径必须有恢复/停表调用）。
    ("轮询定时器不保存句柄（没人能停它）", POLL_TIMER_NEW, '    new QTimer(this);\n'),

    # ================= 重入守卫 =================
    ("去掉 waitingFinger 重入守卫（重复点击叠加注册）", WAITING_GUARD,
     '    if (false) return; // 防止重复点击\n'),
    ("采集失败后不复位 waitingFinger（现场表现为点了没反应）",
     WAITING_OFF_COLLECT_FAIL,
     '        qDebug() << QString("第%1次采集图像失败").arg(step);\n'
     '        return;\n', 1),

    # ================= 命令序列（与静态规则互为印证）=================
    ("探测手指命令改成 0x02（GEN_CHAR）", GET_IMAGE_POLL,
     '        payload.append((char)0x02); // CMD_GET_IMAGE\n', 1),
    ("采集前的 GET_IMAGE 改成 0x03（不存在的命令）", GET_IMAGE_COLLECT,
     '    payload.append((char)0x03); // CMD_GET_IMAGE\n'
     '    // 创建或使用字节数组保存协议或音视频数据。\n'
     '    QByteArray pkt = mainWidget->fingerprint()->buildPacket(payload);\n'
     '    // 执行本行语句完成对应变量、对象或业务状态处理。\n'
     '    mainWidget->fingerprint()->sendPacket(pkt);\n', 1),
    ("轮询超时从 500ms 改成 2000ms", READ_500,
     '        QByteArray resp = mainWidget->fingerprint()->readPacket(2000);\n'),
    ("采集超时从 2000ms 改成 500ms", READ_2000,
     '    QByteArray resp = mainWidget->fingerprint()->readPacket(500);\n'),
    ("GEN_CHAR 不再下发", GEN_CHAR, '    // payload.append((char)0x02); // CMD_GEN_CHAR\n'),
    ("STORE_CHAR 不再下发（模板永远存不进去）", STORE_CHAR,
     '        // payload.append((char)0x06); // CMD_STORE_CHAR\n'),

    # ================= 指纹库 ID =================
    ("注册成功后不递增指纹库 ID（下个用户覆盖刚注册的模板）", INCREMENT, ''),
    ("STORE_CHAR 的 ID 高低字节写反", ID_HI,
     '        payload.append((char)(id & 0xFF));\n'),
    ("STORE_CHAR 的 ID 低字节写错", ID_LO,
     '        payload.append((char)(id >> 8));\n'),

    # ================= 提示音 =================
    ("第 1 步采集成功不播放提示音", TONE_ONE,
     '    if (step == 1) (void)0;\n'),

    # ================= 夹具/接线被拆掉 =================
    ("影子 Widget 去掉管理器注入点（测试连不上真管理器）", SHIM_MGR, ''),
    ("影子 Widget 去掉设备注入点（测试连不上假串口）", SHIM_DEV, ''),

    # ================= 对照：必须不误报 =================
    # 注意：对照必须落在**真实存在**的文本上，且是"语义无关"的改动。
    ("对照：在 form_finger.cpp 里加无关注释",
     '        mainWidget->incrementNextFingerId();\n',
     '        // 无关注释（对照）\n        mainWidget->incrementNextFingerId();\n',
     1, False),
    ("对照：改一条断言的报错文案（措辞不该影响结果）",
     '"第二轮注册必须重新挂起识别"',
     '"second round must re-suspend the identifier"',
     1, False),
    # ---- 核心回归：use-after-free 复发（QPointer -> 裸指针）----
    #
    # 注意它的"被抓到"形态会随测试规模**变化**，这正是第 37 轮最重要的教训：
    #   - 用第 37 轮最初那份**较薄**的测试文件：该变异 5/5 运行全部正常退出
    #     （rc=0）—— **一次都没崩**，测试抓不到它；
    #   - 补强（加入 fullEnrollSequenceMatchesThePinnedContract 等）之后：
    #     同一变异 8/8 全部崩溃（rc=139 / 0xC0000005）。
    #
    # 原因是 use-after-free 属未定义行为，崩不崩取决于后续代码是否恰好复用了
    # 那块已释放内存 —— 是偶然的、随测试规模浮动的。所以**"崩溃"绝不能作为
    # 唯一的回归守卫**：真正的守卫是静态规则
    # check_form_finger_suspended_manager_is_guarded（见 verify_form_finger_rule.py）。
    # 本条留在 MUT 里，是因为当前套件确实能抓到它；但绝不能把它当成守卫的
    # 充分条件 —— 一旦有人删掉几个用例，它可能又变回静默通过。
    ("把 QPointer 改回裸指针（use-after-free 复发）",
     '    QPointer<FingerprintManager> m_suspendedFingerprintManager;\n',
     '    FingerprintManager* m_suspendedFingerprintManager = nullptr;\n',
     1, True),
]

# ---------------------------------------------------------------------------
# 实测**必须保持抓不到**的变异（行为层面确实不可观测，只能靠静态规则守）。
UNDETECTABLE = [
    # 析构里清不清句柄在行为上不可观测：析构只执行一次，而 setSuspended()
    # 开头就有 `if (m_suspended == suspended) return;`，重复调用无任何可见差异。
    # （同理，"恢复调用作用于别的成员"这种在行为上也接近不可观测，但它会让
    # 常驻识别**永久挂起**，能被 isSuspended() 断言抓到，故留在 MUT 里。）
    # 该性质由静态规则守：两处恢复都必须成对清空句柄。
    ("析构后不清空句柄（setSuspended 幂等，行为不可观测）", FF,
     '        m_suspendedFingerprintManager = nullptr;\n'
     '    }\n    // 释放动态创建的对象资源。\n',
     '    }\n    // 释放动态创建的对象资源。\n', 1),
]


def configure():
    return subprocess.run(
        [CMAKE, '-S', os.path.join(T, 'tests'), '-B', BUILD, '-G', 'Ninja'],
        capture_output=True, env=ENV, cwd=T)


def build():
    return subprocess.run(
        [CMAKE, '--build', BUILD, '--target', 't113_test_form_finger'],
        capture_output=True, env=ENV, cwd=T)


def run_exe(case=None):
    args = [EXE] + ([case] if case else [])
    return subprocess.run(args, capture_output=True, env=ENV, cwd=T)


def mt(p):
    return os.path.getmtime(p) if os.path.exists(p) else 0


def main():
    if not _acquire_lock():
        print('!! 已有另一个"改写源文件"的夹具在运行（锁: %s）。' % LOCK)
        print('   两个夹具同时改 form_finger.cpp 会互相污染 —— 第 37 轮实测：'
              '规则夹具报"析构时不恢复"没被抓到、行为夹具报莫名其妙的误报，'
              '而单独串行重跑全部正常。请等它结束再跑本夹具。')
        return 3
    try:
        return _main_locked()
    finally:
        _release_lock()


def _main_locked():
    if _HEALED:
        print('!! 检测到上一次运行留下的污染，已用磁盘原始副本自愈: %s'
              % ', '.join(_HEALED))

    # MUT 元组形如 (名字, 旧文本, 新文本[, 出现次数[, 期望失败]])；
    # UNDETECTABLE 形如 (名字, 文件, 旧文本, 新文本[, 出现次数])，故分开预检。
    for item in MUT:
        name, old = item[0], item[1]
        cnt = item[3] if len(item) > 3 else 1
        if not isinstance(cnt, int):
            cnt = 1   # 第 5 位是 expect_fail 标志时的兜底
        if target_of(old, cnt) is None:
            print('[夹具错误] %s 锚点出现次数不符（期望 %d）' % (name, cnt))
            return 2
    for item in UNDETECTABLE:
        name, p, old = item[0], item[1], item[2]
        cnt = item[4] if len(item) > 4 else 1
        if _ORIG[p].count(old) != cnt:
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
                print('  [夹具错误] %-46s 定位失败' % name)
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

        # ---- UNDETECTABLE：必须**抓不到**，否则说明认识过时了 ----
        print()
        print('--- 预期抓不到的变异（必须保持抓不到）---')
        for item in UNDETECTABLE:
            name, p, old, new = item[0], item[1], item[2], item[3]
            cnt = item[4] if len(item) > 4 else 1
            io.open(p, 'w', encoding='utf-8',
                    newline='\n').write(_ORIG[p].replace(old, new, cnt))
            t0 = mt(OBJ)
            rb = build()
            if rb.returncode != 0:
                print('  %-46s *** 编译失败（预期能编译）' % name)
                problems.append(name)
            else:
                t1 = mt(OBJ)
                rr = run_exe()
                out = (rr.stdout + rr.stderr).decode('utf-8', 'replace')
                failed = FAIL_RE.findall(out)
                crashed = rr.returncode != 0 and not failed
                if t1 <= t0:
                    print('  [夹具错误] %-46s .obj 未重编' % name)
                    problems.append(name)
                elif failed or crashed:
                    print('  %-46s 竟被抓到（说明测试变强了）: '
                          '%d FAIL rc=%d' % (name, len(failed), rr.returncode))
                else:
                    print('  %-46s 如预期未被抓到 ✓（UAF 不必然崩溃）' % name)
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
    print('变异 %d 个，预期抓不到 %d 个，致命问题 %d 个'
          % (len(MUT), len(UNDETECTABLE), len(problems)))
    if problems:
        print('问题清单:', problems)
    return 1 if (problems or not same) else 0


if __name__ == '__main__':
    sys.exit(main())

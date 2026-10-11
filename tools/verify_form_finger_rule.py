# -*- coding: utf-8 -*-
"""对第 37 轮新增的 check_form_finger_suspended_manager_is_guarded 做故障注入自检。

这条规则的价值在于：**测试抓不到 use-after-free**。第 37 轮实测把
`QPointer` 改回裸指针后，行为测试 5/5 全绿（UB 不必然崩溃）。所以"成员必须
是 QPointer"这个性质**只有静态规则在守** —— 它一旦退化，就再也没有防线。
本夹具因此必须证明：把 QPointer 换成裸指针、去掉 <QPointer> 包含、删掉挂起
或恢复中的任何一处，规则都会红。
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

FH = os.path.join(T, 'src', 'ui', 'form_finger.h')
FF = os.path.join(T, 'src', 'ui', 'form_finger.cpp')


def read(p):
    return io.open(p, encoding='utf-8', errors='replace').read()


FILES = (FH, FF)

# ---------------------------------------------------------------------------
# 还原保险（README §61）：`finally` 不是还原保证。
# 磁盘原始副本 + **中断哨兵** + atexit/SIGINT/SIGTERM/SIGBREAK。
#
# 哨兵（`.in-progress`）是第 37 轮补上的关键一环：只靠"磁盘副本 != 当前文件"
# 来判断该不该自愈，会在**开发者刚改了被测文件**（完全正常）时静默回滚他的
# 修改 —— 实测就这样丢过一整轮测试补强，而且基线还是绿的、看不出异常。
# 正确判据是"上一次运行是否被**中断**"：有哨兵才自愈，否则用当前文件刷新副本。
# ---------------------------------------------------------------------------
BACKUP = os.path.join(os.environ['TEMP'], 'fi37rule_pristine')
MARKER = os.path.join(BACKUP, '.in-progress')
_ORIG = {}


def _backup_path(p):
    return os.path.join(BACKUP, os.path.basename(p))


def _load_pristine():
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
            _ORIG[p] = read(p)
            io.open(b, 'w', encoding='utf-8',
                    newline='\n').write(_ORIG[p])
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
# 并发互斥：与 verify_form_finger_test.py **共用同一把锁**。
#
# 两个夹具改的是同一批文件（form_finger.h/.cpp）。第 37 轮实测：并行跑会
# 交叉污染 —— 规则夹具报"析构时不恢复"没被抓到，行为夹具报莫名其妙的误报，
# 而单独串行重跑全部正常。共用锁即可彻底避免。
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


_HEALED = _load_pristine()
_install_guards()


def sub(p, old, new, count=1):
    s = _ORIG[p]
    n = s.count(old)
    if n != count:
        raise AssertionError('%s 锚点出现 %d 次（期望 %d）: %r'
                             % (os.path.basename(p), n, count, old[:70]))
    return s.replace(old, new, count)


# 真实锚点
QP_MEMBER = '    QPointer<FingerprintManager> m_suspendedFingerprintManager;\n'
QP_INCLUDE = '#include <QPointer>\n'
SUSPEND_TRUE = '        m_suspendedFingerprintManager->setSuspended(true);\n'
SUSPEND_FALSE = '        m_suspendedFingerprintManager->setSuspended(false);\n'

MUT = [
    # ============ 核心：QPointer 被换回裸指针（UAF 复发）============
    # 元组统一为 (名字, 文件, 旧文本, 新文本, 出现次数)
    ("成员改回裸指针（use-after-free 复发）", FH,
     QP_MEMBER,
     '    FingerprintManager* m_suspendedFingerprintManager = nullptr;\n', 1),

    # 变体：写成裸指针但不加默认初始化（更隐蔽）
    ("成员改回裸指针（无默认初始化）", FH,
     QP_MEMBER,
     '    FingerprintManager* m_suspendedFingerprintManager;\n', 1),

    # 变体：QPointer 包的是别的类型（看似有守卫，实际守错对象）
    ("QPointer 包了错误的类型", FH,
     QP_MEMBER,
     '    QPointer<QWidget> m_suspendedFingerprintManager;\n', 1),

    # 变体：成员被改名（规则依赖的名字没了）
    ("成员被改名", FH,
     QP_MEMBER,
     '    QPointer<FingerprintManager> m_suspendedMgr;\n', 1),

    # ============ <QPointer> 包含被去掉 ============
    ("去掉 #include <QPointer>（可能靠间接包含侥幸编过）", FH,
     QP_INCLUDE, '// #include <QPointer> removed\n', 1),

    # ============ 挂起/恢复成对性 ============
    ("注册时不挂起（互斥失效）", FF, SUSPEND_TRUE, '        (void)0;\n', 1),

    # 退出路径：用后续的 `this->close();` 精确定位（两处恢复文本完全相同，
    # 裸锚点会命中 2 次）。保留 if 块结构，只抽掉恢复调用本身。
    ("退出时不恢复（门禁指纹永久失效）", FF,
     SUSPEND_FALSE + '        m_suspendedFingerprintManager = nullptr;\n'
     '    }\n\n    // 执行本行语句完成对应变量、对象或业务状态处理。\n'
     '    this->close();\n',
     '        m_suspendedFingerprintManager = nullptr;\n'
     '    }\n\n    // 执行本行语句完成对应变量、对象或业务状态处理。\n'
     '    this->close();\n', 1),

    # 析构路径：用后续的"释放动态创建的对象资源"注释精确定位。
    ("析构时不恢复（兜底路径失效）", FF,
     SUSPEND_FALSE + '        m_suspendedFingerprintManager = nullptr;\n'
     '    }\n    // 释放动态创建的对象资源。\n',
     '        m_suspendedFingerprintManager = nullptr;\n'
     '    }\n    // 释放动态创建的对象资源。\n', 1),

    # 析构路径停表被删。**行为上不可观测**（定时器是 form 的子对象，随窗体
    # 一起销毁；析构不跑事件循环），所以只能靠静态规则守 —— 这条变异就是
    # 用来证明"静态规则确实守住了它"。
    ("析构时不调 stopFingerPoll（只能靠静态规则抓）", FF,
     '    stopFingerPoll();\n    // 兜底恢复',
     '    // 兜底恢复', 1),

    # 退出路径停表被删（这条行为测试也能抓到，静态规则同样要能抓到）。
    ("退出时不调 stopFingerPoll", FF,
     '    stopFingerPoll();\n    // 同时复位等待状态。',
     '    // 同时复位等待状态。', 1),

    # 把两处恢复都删掉 -> 数量不足
    ("两处恢复调用都被删掉", FF, SUSPEND_FALSE, '        (void)0;\n', 2),

    # 恢复调用作用于别的对象（名字对不上）—— 两处都换掉
    ("恢复调用作用于别的成员", FF,
     SUSPEND_FALSE,
     '        m_otherManager->setSuspended(false);\n', 2),

    # ============ 规则应能发现函数被改名/删除 ============
    ("析构函数被改名（规则找不到恢复点）", FF,
     'Form_finger::~Form_finger()\n',
     'Form_finger::~Form_fingerRenamed()\n', 1),
]

# 那些"退出/析构各删一处"的变异都锚在同一串文本上，`sub` 每次只改 count 处；
# 单处变异（不管删的是哪一处）都会被 (4) 的按函数定位抓到，因为规则分别检查
# onAddQuitClicked 与 ~Form_finger 各自体内是否有恢复调用。

# 需要"改文件系统"的变异：(名字, 路径)
FS_MUT = []

CTRL = [
    ("加无关注释（头文件）", FH,
     '#endif // FORM_FINGER_H',
     '// 无关注释（对照）\n#endif // FORM_FINGER_H', 1),
    ("加无关注释（源文件）", FF,
     'void Form_finger::stopFingerPoll()\n',
     '// 无关注释（对照）\nvoid Form_finger::stopFingerPoll()\n', 1),
    ("重排版空白（成员声明前多几个空格）", FH,
     QP_MEMBER,
     '        QPointer<FingerprintManager> m_suspendedFingerprintManager;\n', 1),
]


def run_verify():
    return subprocess.run([PY, VERIFY, T, D], capture_output=True, cwd=T)


def main():
    if not _acquire_lock():
        print('!! 已有另一个"改写源文件"的夹具在运行（锁: %s）。' % LOCK)
        print('   两个夹具同时改 form_finger.h/.cpp 会互相污染（第 37 轮实测），'
              '请等它结束再跑本夹具。')
        return 3
    try:
        return _main_locked()
    finally:
        _release_lock()


def _main_locked():
    if _HEALED:
        print('!! 检测到上一次运行被中断，已用磁盘原始副本自愈: %s'
              % ', '.join(_HEALED))
    problems = []
    try:
        r = run_verify()
        print('基线 rc = %d' % r.returncode)
        if r.returncode != 0:
            print('*** 基线就不过')
            print((r.stdout + r.stderr).decode('utf-8', 'replace')[-2500:])
            return 2
        print()

        for name, p, old, new, cnt in MUT:
            try:
                text = sub(p, old, new, cnt)
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
            hits = re.findall(r'^\s*X (\[fp-manager-uaf\][^\n]*)', out, re.M)
            if r.returncode != 0 and hits:
                print('  %-46s 被抓到 ✓ %s' % (name, hits[0][:38]))
            else:
                print('  %-46s *** 未被抓到（假绿）rc=%d'
                      % (name, r.returncode))
                problems.append(name)

            io.open(p, 'w', encoding='utf-8', newline='\n').write(_ORIG[p])

        print()
        for name, p, old, new, cnt in CTRL:
            try:
                text = sub(p, old, new, cnt)
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
                print(out[-900:])
                problems.append(name)
            io.open(p, 'w', encoding='utf-8', newline='\n').write(_ORIG[p])
    finally:
        for p in FILES:
            io.open(p, 'w', encoding='utf-8', newline='\n').write(_ORIG[p])

    same = all(read(p) == _ORIG[p] for p in FILES)
    print()
    print('还原逐字节一致:', same)
    r = run_verify()
    for l in (r.stdout + r.stderr).decode('utf-8', 'replace').split('\n'):
        if '结论' in l:
            print('还原后:', l.strip())
    print()
    print('变异 %d 个，对照 %d 个，致命问题 %d 个'
          % (len(MUT), len(CTRL), len(problems)))
    if problems:
        print('问题清单:', problems)
    return 1 if (problems or not same) else 0


if __name__ == '__main__':
    sys.exit(main())

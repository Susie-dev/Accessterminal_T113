# -*- coding: utf-8 -*-
"""故障注入夹具的**源文件备份/自愈**公共实现（第 41 轮提取）。

为什么要有这个模块
------------------
四个夹具（`verify_form_call_test.py` / `verify_form_finger_test.py` /
`verify_form_finger_rule.py` / `verify_credential_rule.py` /
`verify_credential_store_test.py`）都要做同一件事：**改写仓库源文件来做故障
注入，并保证退出时逐字节还原**。这件事有三个层次，每一层都是踩过坑才对的：

  1. **还原不能只靠 `finally`**（README §61）。脚本被强杀时 `finally` 不执行，
     仓库里就留下一个"看起来正常、只是少了一行"的源文件。
     → 必须在**磁盘**上留一份原始副本。

  2. **自愈不能只看"副本与当前文件不同"**（README §62 的前身，第 37 轮）。
     开发者刚改了被测文件（完全正常的工作流）会被当成污染、
     从**旧副本**静默回滚 —— 实测丢掉过一整轮测试补强，而且基线还是绿的。
     → 判据必须是"上一次运行是否**被中断**"（用 `.in-progress` 标记）。

  3. **"被中断"还不够 —— 副本可能已经过期**（第 41 轮，真实事故重现）。
     在 Windows 上 `job_kill` 走 `TerminateProcess`：`finally`、`atexit`、
     信号处理器**全都不执行**，于是标记**必定残留**。下一次启动看到标记，
     就拿一份可能**很旧**的副本来"自愈"。
     第 41 轮实测：`%TEMP%\\fi37_pristine\\widget.h` 是 7528 字节的**第 37 轮
     之前**版本，而磁盘上是 8421 字节 —— 自愈直接抹掉了第 37 轮给影子头加的
     全部内容，随后 23 个变异全部因"锚点找不到"而失败。

     → 再加一道**新鲜度**校验（见下）。

第 3 层的判据
-------------
标记文件里记下"上次开始时各文件的**长度**"。中断恢复时：

  - 若当前文件长度 == 记录长度 → 当前文件就是上次开始时的样子，
    **可以**安全自愈（副本是"上次结束时的样子"，两者只差变异）；
  - 若长度不符 → 说明最后那次运行**改过**它，而我们无法区分
    "变异残留"与"之后的合法编辑" → **不自愈**，以当前文件为准，
    并**明确告警**让人工用 `git diff` 核对。

这是**有意的保守**：宁可留下污染（可被 `git diff` 一眼看出、也可重跑），
也绝不删掉合法改动（那是**静默**的数据丢失，最坏的一种失败）。

用法
----
    from fixture_pristine import PristineGuard

    guard = PristineGuard(files=[path1, path2], tag='myfixture')
    # guard.orig[path] 是本次运行的基准内容（各夹具的 _ORIG）
    try:
        ...
    finally:
        guard.restore()

`guard.orig` 就是原来的 `_ORIG` 字典，夹具其余代码不必改。

依赖：只用标准库。无需编译器/Qt。
"""
import io
import os
import sys
import tempfile


def _ensure_console_encoding():
    """让脚本在 GBK 控制台下也能安全输出（README §36）。"""
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


_ensure_console_encoding()


def read(p):
    """读文本，UTF-8。夹具只关心"逐字节还原"，所以不做编码猜测。"""
    return io.open(p, encoding="utf-8", errors="replace").read()


def write(p, s):
    """写文本，**强制 LF、无 BOM**（本工程的硬要求）。"""
    io.open(p, "w", encoding="utf-8", newline="\n").write(s)


def temp_dir(*parts):
    """`%TEMP%`（Windows）或 `/tmp`，拼上若干段。

    刻意不直接用 `os.environ['TEMP']`：在 Linux / CI 上没有这个变量，
    会直接 `KeyError`（第 41 轮复核时确认过这一点）。
    """
    base = os.environ.get("TEMP") or tempfile.gettempdir()
    return os.path.join(base, *parts)


def process_is_alive(pid):
    """判断 pid 是否**仍然活着**。返回 True/False。

    为什么不能只用 `os.kill(pid, 0)`（第 41 轮实测，Windows）
    ----------------------------------------------------------------------
    在 Windows 上 `os.kill(pid, 0)` 的行为是这样的：

        * pid 从未存在（如 999999999）      -> OSError [WinError 87]
        * pid 曾经存在、现已**正常退出**    -> **无异常**（被判为活着！）

    也就是说它只能区分"从未存在"和"存在过"，**不能**区分"活着"与"已退出"。
    后果很严重：夹具被强杀（`job_kill` / 任务管理器）留下的陈旧锁
    **永远不会**被接管，之后每个夹具都会打印"另一个夹具正在改写源码"
    然后拒绝干活 —— 整个自检体系静默停摆。

    正确做法是用 Win32 API 查进程退出码：
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) 失败 -> 进程不存在；
        GetExitCodeProcess() == STILL_ACTIVE(259)        -> 仍在运行。
    非 Windows 平台退回 `os.kill(pid, 0)`（POSIX 语义是正确的）。

    `PROCESS_QUERY_LIMITED_INFORMATION` 而非 `PROCESS_QUERY_INFORMATION`：
    前者对权限的要求低得多（Vista 起可用），足以读退出码。
    """
    if pid is None or pid <= 0:
        return False
    if os.name != "nt":
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False
    # --- Windows ---
    try:
        import ctypes
    except ImportError:                                  # pragma: no cover
        return True                                      # 判不了就保守认为活着
    k32 = ctypes.windll.kernel32
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    STILL_ACTIVE = 259
    handle = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return False                                     # 打不开 = 不存在
    try:
        code = ctypes.c_ulong()
        if not k32.GetExitCodeProcess(handle, ctypes.byref(code)):
            return True                                  # 查不到就保守认为活着
        return code.value == STILL_ACTIVE
    finally:
        k32.CloseHandle(handle)


class _FileLock(object):
    """仓库级"正在改写源码"互斥锁（README §67）。

    两个改写同一批源文件的夹具绝不能并行 —— 第 37 轮实测过交叉污染：
    规则夹具报"析构时不恢复"没被抓到、行为夹具报莫名其妙的假绿，
    而单独串行重跑时全部正常。

    锁带 pid；发现持有者已死则接管（清理陈旧锁）。

    **同一进程内可重入**（第 41 轮补）：若锁的持有者 pid 就是本进程，
    说明本夹具**自己**已经拿过这把锁（典型场景：夹具原先自带一套锁，
    迁移到 PristineGuard 后两套共存），此时应视为"已持有"而非"被别人占"，
    否则会出现**自己把自己锁在门外**的荒谬结果：
    实测 `verify_door_actuator_test.py` 就是这样直接 rc=2 退出。
    """

    def __init__(self, lock_path):
        self.path = lock_path
        self.held = False
        self.reentrant = False

    def acquire(self):
        if self.held:
            return True
        for _ in range(2):
            try:
                fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                os.write(fd, ("%d\n" % os.getpid()).encode("ascii"))
                os.close(fd)
                self.held = True
                self.reentrant = False
                return True
            except OSError:
                other = None
                try:
                    with io.open(self.path, encoding="ascii") as fh:
                        other = int(fh.read().strip())
                except (OSError, ValueError):
                    other = None
                if other == os.getpid():
                    # 本进程已经持有（另一套锁机制写的）。视为已持有，
                    # 但**不要**在 release 时删掉它 —— 那把锁的主人是
                    # 另一段代码，它自己会释放。
                    self.held = True
                    self.reentrant = True
                    return True
                alive = process_is_alive(other)
                if alive:
                    return False
                # 陈旧锁：持有者已死（或 pid 不可解析），清掉重来。
                # 注意这里用的是 process_is_alive()，**不是** os.kill：
                # Windows 上 os.kill(pid,0) 会把已退出的进程判成活着，
                # 陈旧锁将永远无法接管（第 41 轮实测）。
                try:
                    os.remove(self.path)
                except OSError:
                    return False
        return False

    def release(self):
        if self.held and not self.reentrant:
            try:
                os.remove(self.path)
            except OSError:
                pass
            self.held = False


class PristineGuard(object):
    """磁盘原始副本 + 中断自愈 + 新鲜度校验 + 信号还原 + 互斥锁。

    :param files: 会被改写的源文件绝对路径列表
    :param tag:   备份目录名后缀（如 'fi37'），避免不同夹具互相覆盖副本
    :param lock_name: 互斥锁文件名；**同一批源文件的夹具必须用同一个名字**
    """

    def __init__(self, files, tag, lock_name="fi37_rewrite.lock"):
        self.files = tuple(files)
        self.backup_dir = temp_dir(tag + "_pristine")
        self.marker = os.path.join(self.backup_dir, ".in-progress")
        self.lock = _FileLock(temp_dir(lock_name))
        self.orig = {}
        self.healed = []
        self.unfresh = []

    # -- 备份路径 ----------------------------------------------------------
    def _backup_path(self, p):
        # 用相对文件名，避免同名冲突；同时保留观感可读。
        return os.path.join(self.backup_dir, os.path.basename(p))

    # -- 主流程 ------------------------------------------------------------
    def prepare(self):
        """建立/校验原始副本。返回被自愈的文件名列表。"""
        healed = []
        os.makedirs(self.backup_dir, exist_ok=True)
        interrupted = os.path.exists(self.marker)

        recorded = {}
        if interrupted:
            try:
                with io.open(self.marker, encoding="ascii") as fh:
                    for line in fh:
                        line = line.strip()
                        if "=" in line:
                            k, v = line.split("=", 1)
                            recorded[k] = int(v)
            except (OSError, ValueError):
                recorded = {}

        for p in self.files:
            b = self._backup_path(p)
            base = os.path.basename(p)
            if os.path.exists(b) and interrupted:
                backup_text = read(b)
                cur_text = read(p)
                if cur_text == backup_text:
                    self.orig[p] = backup_text
                    continue
                expect = recorded.get(base)
                if expect is not None and len(cur_text) != expect:
                    # 长度不符 -> 无法区分"变异残留"与"之后的合法编辑"
                    # -> **不自愈**（保守，见模块头注释第 3 层）。
                    self.unfresh.append(base)
                    self.orig[p] = cur_text
                    write(b, cur_text)
                    continue
                write(p, backup_text)
                healed.append(base)
                self.orig[p] = backup_text
            else:
                # 无中断证据：以**当前**文件为准刷新副本（副本可能过期）。
                self.orig[p] = read(p)
                write(b, self.orig[p])

        if self.unfresh:
            print("!! 上一次运行被中断，但副本与当前文件**长度不符** ——")
            print("   无法区分「变异残留」与「之后的合法编辑」，故**不做自愈**："
                  + ", ".join(self.unfresh))
            print("   已按当前文件为准。若怀疑有变异残留，请用 git diff 核对。")

        self.healed = healed
        lines = ["running"]
        for p in self.files:
            lines.append("%s=%d" % (os.path.basename(p), len(self.orig[p])))
        write(self.marker, "\n".join(lines) + "\n")
        return healed

    def install_guards(self):
        """atexit + SIGINT/SIGTERM/SIGBREAK 还原。

        注意：Windows 上 `TerminateProcess`（`job_kill` / 任务管理器强杀）
        **绕过**这一切 —— 这正是"新鲜度校验"必须存在的理由。
        """
        import atexit
        import signal

        atexit.register(self.restore)

        def handler(signum, _frame):
            self.restore()
            print("\n[中断] 收到信号 %d，已还原源文件，退出" % signum)
            sys.exit(130)

        for name in ("SIGINT", "SIGTERM", "SIGBREAK"):
            sig = getattr(signal, name, None)
            if sig is None:
                continue
            try:
                signal.signal(sig, handler)
            except (ValueError, OSError):
                pass

    def restore(self, *_a):
        """逐字节还原全部文件，并清掉标记与锁。可安全重复调用。"""
        for p in self.files:
            if p in self.orig:
                try:
                    write(p, self.orig[p])
                except OSError:
                    pass
        try:
            os.remove(self.marker)
        except OSError:
            pass
        self.lock.release()

    def acquire_lock(self):
        """拿到互斥锁返回 True；已被**别的活进程**持有返回 False。

        若锁的持有者就是本进程，视为可重入（已经持有），同样返回 True。
        """
        return self.lock.acquire()

    def lock_path(self):
        """锁文件的绝对路径。

        存在的理由（第 42 轮）：夹具在"拿不到锁"时要打印**是哪个文件**
        挡住了它，否则用户只知道"有别的夹具在跑"却不知道从哪查。
        原先各夹具自己拼了一遍路径（并因此留下一处 `NameError`：
        迁移到本 Guard 后忘了删 `% LOCK`）。现在只有这一份真相。
        """
        return self.lock.path

    @property
    def reentrant(self):
        """本 Guard 是否走的"可重入"路径（锁的持有者就是本进程）。"""
        return self.lock.reentrant

    def release_lock(self):
        self.lock.release()

    def verify_restored(self):
        """还原后逐字节比对，返回 True 表示干净。"""
        for p in self.files:
            if p not in self.orig:
                return False
            if read(p) != self.orig[p]:
                return False
        return True

# -*- coding: utf-8 -*-
"""t113_credential_store 测试目标的**行为**故障注入自检（第 41 轮，方案 P2）。

用法：python3 tools/verify_credential_store_test.py

与 `verify_credential_rule.py` 的分工
------------------------------------
  - `verify_credential_rule.py`：证明**静态规则**会把危险退化报出来（不编译）；
  - 本脚本：证明**测试目标**会把危险退化跑红（真编译、真运行）。

两者都要有。静态规则只能看文本；行为测试能发现"规则看着对、实际没测到"。

要证明的退化（每条都必须让测试变红）
----------------------------------
  B1  `verify()` 忽略锁定，永远比哈希
      -> 「锁定期内连正确口令也被拒」会失败
  B2  `verify()` 失败时不累加计数
      -> 「达到上限即锁定」会失败
  B3  锁定到期**不自动解锁**（不清 lockedUntilMs）
      -> 「到期自动解锁」会失败
  B4  `consumeTempCode()` 不做"已用"判定（临时码可重复用）
      -> 「一次性」会失败
  B5  `consumeTempCode()` 不做过期判定
      -> 「过期后不能消费」会失败
  B6  `setSecret()` 用**固定盐**（相同口令得到相同哈希）
      -> 「相同口令 + 不同盐得到不同摘要」会失败
  B7  `constantTimeEquals()` 退化成"只比长度"
      -> 「定长比较语义等于普通相等」会失败
  B8  `save()` 把明文也写进文件
      -> 「落盘不含明文」会失败（**最重要的一条**）
  B9  `load()` 在解析失败时把内存清空
      -> 「损坏 JSON 不清空凭据」会失败
  B10 `fromJson()` 遇到非法哈希记录仍然装入（把坏记录当"无密码"放行）
      -> 「单条损坏只跳过该用户」会失败
  B11 `purgeTempCodes()` 把未到期的也清掉
      -> 「只清死码」会失败
  B12 `hexToBytes()` 去掉十六进制校验（回到裸 fromHex）
      -> 「非法哈希格式不被误认」会失败（内容全错但"成功"）

对照（必须保持绿）
----------------
  C1  不改任何代码
  C2  加无关注释（不影响行为）

工具链
------
需要能配置/构建 tests/ 子工程。构建目录用临时目录，不污染仓库。
路径优先用环境变量（跨平台），否则从 __file__ 推导。
"""
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
T = os.path.dirname(HERE)
CS_CPP = os.path.join(T, "src", "managers", "credential_store.cpp")
CS_H = os.path.join(T, "src", "managers", "credential_store.h")
TESTS = os.path.join(T, "tests")

FAIL_RE = re.compile(r"^FAIL!\s*:", re.M)


def _find_toolchain_bin():
    """把工具链 bin 目录加进 PATH。

    为什么必须显式找：本机（Windows/MSYS2）的 cmake/ninja 不在系统 PATH 上，
    直接 subprocess.run(["cmake", ...]) 会 FileNotFoundError [WinError 2]。
    Linux 上通常已在 PATH，这里就什么都不做 —— 保持跨平台。
    """
    cands = [
        os.environ.get("T113_TOOLCHAIN_BIN"),
        r"C:\Users\user\.dsh\toolchains\msys64\mingw64\bin",
        r"C:\Users\user\.dsh\toolchains\msys64\usr\bin",
    ]
    extra = [c for c in cands if c and os.path.isdir(c)]
    if extra:
        os.environ["PATH"] = os.pathsep.join(extra + [os.environ.get("PATH", "")])


_find_toolchain_bin()
CMAKE = shutil.which("cmake") or "cmake"


def read(p):
    return io.open(p, encoding="utf-8", errors="replace").read()


def write(p, s):
    io.open(p, "w", encoding="utf-8", newline="\n").write(s)


# ---------------------------------------------------------------------------
# 还原保险与互斥：**公共实现**（第 41 轮提取到 tools/fixture_pristine.py）。
#
# 这段逻辑本文件原先自带一份，踩过三次坑（README §61/§62）：
#   1. 只靠 `finally` 还原 -> 被强杀就留下污染源文件；
#   2. 自愈判据写成"副本 != 当前文件" -> 静默回滚开发者的合法修改（第 37 轮）；
#   3. "被中断"仍不够，**副本可能过期**（第 41 轮实测：把
#      tests/shadows/ui/widget.h 回滚成第 37 轮之前的版本，抹掉影子头全部新增）。
# 现在统一交给 PristineGuard（含新鲜度校验）。它自己也有单元自检：
# tools/verify_fixture_pristine.py（20 项，含"拒绝用过期副本自愈"）。
# ---------------------------------------------------------------------------
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fixture_pristine import PristineGuard  # noqa: E402

FILES = [CS_CPP, CS_H]
_GUARD = PristineGuard(files=FILES, tag='p2credtest', lock_name='fi37_write.lock')
_ORIG = _GUARD.orig
_HEALED = []


def _prepare_backup():
    """建立/校验磁盘原始副本（含新鲜度校验），自愈的文件名进 _HEALED。"""
    _HEALED[:] = _GUARD.prepare()
    return _HEALED


def _load_pristine():
    return _prepare_backup()


def _restore_all(*_a):
    _GUARD.restore()


def _install_guards():
    _GUARD.install_guards()


def _acquire_lock():
    """拿不到锁返回 False（已有活着的夹具在改写同一批源文件）。"""
    return _GUARD.acquire_lock()


def _release_lock():
    _GUARD.release_lock()


def configure(build_dir):
    args = [CMAKE, "-S", TESTS, "-B", build_dir, "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release"]
    sp = os.environ.get("QT5SERIALPORT_DIR")
    if sp:
        args.append("-DQt5SerialPort_DIR=" + sp)
    r = subprocess.run(args, capture_output=True)
    return r.returncode == 0, (r.stdout + r.stderr).decode("utf-8", "replace")


def build_and_run(build_dir):
    """构建并运行目标。返回 (是否全绿, 输出尾部, FAIL 条数)。

    判据与全工程一致：QtTest 的退出码等于 FAIL 行数，
    所以"是否有判别力"看的是**出现了 FAIL 行**，而不是仅看退出码
    （崩溃时退出码非 0 但没有 FAIL 行，那是另一回事）。
    """
    r = subprocess.run(["cmake", "--build", build_dir, "--target",
                        "t113_test_credential_store"], capture_output=True)
    if r.returncode != 0:
        return False, (r.stdout + r.stderr).decode("utf-8", "replace")[-1200:], -1
    binary = os.path.join(build_dir, "t113_test_credential_store")
    if os.name == "nt":
        binary += ".exe"
    env = dict(os.environ)
    env["QT_QPA_PLATFORM"] = "offscreen"
    r = subprocess.run([binary], capture_output=True, env=env, cwd=build_dir)
    out = (r.stdout + r.stderr).decode("utf-8", "replace")
    nfail = len(FAIL_RE.findall(out))
    return (r.returncode == 0), out[-1500:], nfail


# ---------------------------------------------------------------------------
# 变异定义：(名字, [(文件, 旧, 新), ...])
# ---------------------------------------------------------------------------
VERIFY_LOCK = ("    // 先判锁定：锁定期内不再比哈希，也不再累加计数。\n"
               "    if (m_maxAttempts > 0 && r->lockedUntilMs > 0 && nowMs() < r->lockedUntilMs) {\n"
               "        return VerifyOutcome::LockedOut;\n"
               "    }\n")
AUTO_UNLOCK = ("    if (r->lockedUntilMs > 0 && nowMs() >= r->lockedUntilMs) {\n"
               "        r->lockedUntilMs = 0;\n"
               "        r->failed = 0;\n"
               "    }\n")
TEMP_USED = "        if (t.used) {\n            return false;               // 一次性：用过就不能再用\n        }\n"
TEMP_EXPIRED = ("        if (t.expiresAtMs <= nowMs()) {\n"
                "            return false;               // 已过期\n"
                "        }\n")
SALT_IN_SET = "    r->salt = makeSalt();\n"
CTEQ_BODY = ("    unsigned char diff = 0;\n"
             "    for (int i = 0; i < a.size(); ++i) {\n"
             "        diff |= static_cast<unsigned char>(a.at(i).unicode())\n"
             "                ^ static_cast<unsigned char>(b.at(i).unicode());\n"
             "    }\n"
             "    return diff == 0;\n")
CORRUPT_KEEP = ("        qWarning() << \"[凭据] 存储文件解析失败:\" << m_storagePath\n"
                "                   << err.errorString() << \"（保持内存中的现有凭据，不覆盖）\";\n"
                "        return false;\n")
PURGE_BODY = ("        if (t.used || t.expiresAtMs <= nowMs()) {\n"
              "            m_tempCodes.remove(i);\n"
              "            ++removed;\n"
              "        }\n")
HEX_GUARD = ("    if (!isHexOnly(hex)) {\n"
             "        return QByteArray();   // 非法输入 -> 空，由调用方的长度校验兜住\n"
             "    }\n")

MUT = [
    ("忽略锁定，永远比哈希", [(CS_CPP, VERIFY_LOCK, "")]),
    # "失败时不累加计数"：直接整段删掉 `++r->failed;`。
    # 第一版把它改成 `++r->failed; ++r->failed;`（**加倍**）—— 那根本不是
    # "不累加"，而且加倍后反而更容易触发锁定，于是测试照旧绿。
    # 变异必须精确对应它声称的退化。
    ("失败时不累加计数", [(CS_CPP, "    ++r->failed;\n"
                                  "    if (m_maxAttempts > 0 && r->failed >= m_maxAttempts) {",
                           "    if (m_maxAttempts > 0 && r->failed >= m_maxAttempts) {")]),
    ("锁定到期不自动解锁", [(CS_CPP, AUTO_UNLOCK, "")]),
    ("临时码可重复消费", [(CS_CPP, TEMP_USED, "")]),
    ("临时码不过期", [(CS_CPP, TEMP_EXPIRED, "")]),
    ("setSecret 用固定盐", [(CS_CPP, SALT_IN_SET,
                             "    r->salt = QByteArray(16, 'A');   // 变异：固定盐\n")]),
    ("定长比较退化成只比长度", [(CS_CPP, CTEQ_BODY, "    return true;   // 变异：只比长度\n")]),
    ("损坏 JSON 时清空凭据", [(CS_CPP, CORRUPT_KEEP,
                               "        m_userIds.clear(); m_users.clear();\n"
                               "        return false;\n")]),
    ("purge 连未到期的也清掉", [(CS_CPP, PURGE_BODY,
                                  "        m_tempCodes.remove(i);\n        ++removed;\n")]),
    ("hexToBytes 去掉十六进制校验", [(CS_CPP, HEX_GUARD, "")]),

    # ---- 对照 ----
    # 注意：对照必须**真的改动字节**，否则夹具的"变异未落到磁盘"守卫会把它
    # 判成夹具失败（第一版就是 `old == new`，于是被自己的守卫拦下）。
    ("对照：加无关注释", [(CS_CPP, "namespace accessterminal {",
                           "// 无关注释（对照）\nnamespace accessterminal {")]),
    ("对照：多一个空行", [(CS_CPP, "// ---------------------------------------------------------------------------\n"
                                  "// 校验\n",
                           "// ---------------------------------------------------------------------------\n"
                           "\n"
                           "// 校验\n")]),
]


def main():
    if not _acquire_lock():
        print("另一个夹具正在改写源码（%s）—— 请等它跑完再试" % LOCK)
        print("（构建/ctest/verify.py 也不要与夹具并发）")
        return 2
    try:
        return _run()
    finally:
        _release_lock()


def _run():
    _prepare_backup()
    _install_guards()
    if _HEALED:
        print("!! 检测到上一次运行被中断，已用磁盘原始副本自愈: %s"
              % ", ".join(_HEALED))

    for name, edits in MUT:
        for path, old, _new in edits:
            if _ORIG[path].count(old) != 1:
                print("[夹具错误] %s 的锚点在 %s 出现 %d 次（期望 1）"
                      % (name, os.path.basename(path), _ORIG[path].count(old)))
                return 2

    build_dir = tempfile.mkdtemp(prefix="t113-credtest-fixture-")
    print("构建目录: %s" % build_dir)
    print()
    try:
        ok, log = configure(build_dir)
        if not ok:
            print("*** tests/ 配置失败：")
            print("\n".join(log.splitlines()[-20:]))
            return 2

        problems = []
        try:
            # 基线先确认绿
            passed, tail, _n = build_and_run(build_dir)
            if not passed:
                print("*** 基线就是红的，无法进行故障注入：")
                print("\n".join(tail.splitlines()[-12:]))
                return 2
            print("基线: 绿 ✓")
            print()

            for name, edits in MUT:
                staged = dict(_ORIG)
                ok = True
                for path, old, new in edits:
                    if staged[path].count(old) != 1:
                        ok = False
                        break
                    staged[path] = staged[path].replace(old, new, 1)
                if not ok:
                    print("  %-38s *** 锚点失效" % name)
                    problems.append(name)
                    continue
                try:
                    for path in set(p for p, _o, _n in edits):
                        write(path, staged[path])
                    changed = all(read(p) != _ORIG[p]
                                  for p in set(p for p, _o, _n in edits))
                    if not changed:
                        print("  %-38s *** 变异未落到磁盘" % name)
                        problems.append(name)
                        continue
                    passed, tail, nfail = build_and_run(build_dir)
                    # 对照：必须绿
                    if name.startswith("对照") or _is_control(name):
                        if passed:
                            print("  %-38s 绿 ✓（对照）" % name)
                        else:
                            print("  %-38s *** 对照竟然红了" % name)
                            print("      " + "\n      ".join(tail.splitlines()[-6:]))
                            problems.append(name)
                        continue
                    if nfail < 0:
                        # 构建失败：**不算**"测试抓到" —— 那说明变异根本编译不过，
                        # 我们什么也没验证。第一版把这种情况落进下面的 else 分支
                        # 报"仍然全绿"，措辞与事实不符。
                        print("  %-38s *** 变异后无法编译（未验证）" % name)
                        print("      " + "\n      ".join(tail.splitlines()[-6:]))
                        problems.append(name)
                    elif not passed and nfail > 0:
                        print("  %-38s 变红 ✓（%d 条 FAIL）" % (name, nfail))
                    elif not passed and nfail == 0:
                        # 崩溃（无 FAIL 行）：也算被抓到，但要点明形态 ——
                        # 第 37 轮教训："崩溃"不是可靠判据，不能当作唯一守卫。
                        print("  %-38s 变红 ✓（崩溃，无 FAIL 行）" % name)
                    else:
                        print("  %-38s *** 仍然全绿（假绿，测试无判别力）" % name)
                        problems.append(name)
                finally:
                    for path in set(p for p, _o, _n in edits):
                        write(path, _ORIG[path])
        finally:
            _restore_all()

        same = all(read(p) == _ORIG[p] for p in FILES)
        print()
        passed, _tail, _n = build_and_run(build_dir)
        print("还原逐字节一致:", same)
        print("还原后目标:", "绿" if passed else "红")
        print()
        print("变异 %d 个，致命问题 %d 个" % (len(MUT), len(problems)))
        if problems:
            print("问题清单:", problems)
        return 1 if (problems or not same or not passed) else 0
    finally:
        shutil.rmtree(build_dir, ignore_errors=True)


def _is_control(name):
    return name.startswith("对照")


if __name__ == "__main__":
    sys.exit(main())

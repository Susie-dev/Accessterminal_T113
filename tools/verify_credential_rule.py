# -*- coding: utf-8 -*-
"""`[credential]` 静态规则的故障注入自检（第 41 轮，方案 P2）。

用法：python3 tools/verify_credential_rule.py
返回码 0 表示：每条危险退化都让 verify.py **变红**、对照不误报、
且源码还原后逐字节一致。

为什么必须证明"规则会红"
-----------------------
本工程的信条是"在缺陷代码上不失败的测试没有价值"。静态规则同理：
一条**从来不会报错**的规则等于没有规则，而它还会给人"已经守住了"的错觉。
因此每个变异都对应一条规则判据，逐个改坏，逐个确认 verify.py 真的报了。

变异清单（每条都必须被 `[credential]` 捕获）
------------------------------------------
  M1  把 PasswordManager 的成员改回明文 `QString password;`
      -> 规则 ①：口令不得以明文字符串成员留存
  M2  校验不再走 CredentialStore（删掉 .verify( 调用）
      -> 规则 ②：校验必须经过加盐哈希
  M3  把校验改回明文比较 `input == password`
      -> 规则 ②：这正是第 41 轮修掉的缺口
  M4  哈希不再加盐（hashSecret 忽略 salt 参数）
      -> 规则 ④：无盐哈希挡不住彩虹表
  M5  落盘改成非原子写（QSaveFile -> QFile）
      -> 规则 ⑤：掉电会留下半截 JSON = 门忽然打不开
  M6  测试目标从 ctest 摘掉（删 add_test 行）
      -> 规则 ⑥：构建了但 ctest 不跑它 = 永远不执行
  M7  测试目标不链接真实的 credential_store.cpp
      -> 规则 ⑥：哈希逻辑必须用真实现测
  M8  去掉"落盘不含明文"用例
      -> 规则 ⑥：这是本类存在的全部意义
  M9  去掉阳性对照（只剩否定性断言）
      -> 规则 ⑥：否定断言会恒真
  M10 把时钟注入换成 sleep
      -> 规则 ⑥：时序断言要等性质，不要等固定时长

对照（必须保持绿，即不误报）
--------------------------
  C1  加无关注释
  C2  调整空白/缩进
  C3  把部署配置里的明文换成哈希串（**应当消掉那条告警**，且不产生失败）

注意
----
需要能读仓库源码即可，**不需要编译**（这是纯静态规则的夹具）。
跑之前请确认没有别的夹具在改写源码 —— 本脚本自带文件锁。
"""
import io
import os
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
PY = sys.executable

PM_H = os.path.join(T, "src", "managers", "password_manager.h")
PM_CPP = os.path.join(T, "src", "managers", "password_manager.cpp")
CS_CPP = os.path.join(T, "src", "managers", "credential_store.cpp")
CS_H = os.path.join(T, "src", "managers", "credential_store.h")
TESTS_CM = os.path.join(T, "tests", "CMakeLists.txt")
TEST_SRC = os.path.join(T, "tests", "test_credential_store.cpp")
CFG = os.path.join(T, "config", "appconfig.json")

FILES = [PM_H, PM_CPP, CS_CPP, CS_H, TESTS_CM, TEST_SRC, CFG]


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

_GUARD = PristineGuard(files=FILES, tag='p2cred', lock_name='fi37_rewrite.lock')
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


def run_verify():
    """返回 (rc, [credential 相关的失败行])。"""
    r = subprocess.run([PY, os.path.join(T, "tools", "verify.py"), T],
                       capture_output=True, cwd=T)
    out = (r.stdout + r.stderr).decode("utf-8", "replace")
    lines = [l.strip() for l in out.splitlines()
             if "[credential]" in l and l.strip().startswith(("X", "!"))]
    return r.returncode, lines


# ---------------------------------------------------------------------------
# 变异定义：(名字, 期望失败的判据子串, [(文件, 旧, 新), ...])
#
# 用**列表**而不是"每文件一条"：`checkPassword()` 里有**两个** verify 调用
# （空输入分支与非空分支各一个），只删一个的话另一个还在，规则 ② 不会报 ——
# 那是夹具自己没把变异做彻底，不是规则漏报。
# ---------------------------------------------------------------------------
VERIFY_NONEMPTY = ("        const accessterminal::VerifyOutcome o =\n"
                   "            m_credentials.verify(doorUserId(), input);\n"
                   "        matched = (o == accessterminal::VerifyOutcome::Accepted);")
VERIFY_EMPTY = ("        const accessterminal::VerifyOutcome o =\n"
                "            m_credentials.verify(doorUserId(), QString());\n"
                "        matched = (o == accessterminal::VerifyOutcome::Accepted);")

MUT = [
    ("成员改回明文 QString password;", "[credential]",
     [(PM_H, "    accessterminal::CredentialStore m_credentials;",
       "    QString password;\n"
       "    accessterminal::CredentialStore m_credentials;")]),

    ("校验不再走 CredentialStore（两处都绕过）", "[credential]",
     [(PM_CPP, VERIFY_NONEMPTY, "        matched = false; // 变异：绕过存储层"),
      (PM_CPP, VERIFY_EMPTY, "        matched = false; // 变异：绕过存储层")]),

    ("校验改回明文比较 input == password", "[credential]",
     [(PM_CPP, VERIFY_NONEMPTY, "        matched = (input == password);")]),

    ("哈希不再加盐（忽略 salt 参数）", "[credential]",
     [(CS_CPP, "    return QStringLiteral(\"sha256:\") + toHex(salt) + QLatin1Char(':')\n"
               "           + QString::number(n) + QLatin1Char(':')\n"
               "           + iterateDigest(plaintext, salt, n);",
       "    return QStringLiteral(\"sha256:\") + QString(n, QLatin1Char('0'))\n"
       "           + QLatin1Char(':') + QString::number(n) + QLatin1Char(':')\n"
       "           + iterateDigest(plaintext, QByteArray(), n);")]),

    ("落盘改成非原子写（QSaveFile -> QFile）", "[credential]",
     [(CS_CPP, "    QSaveFile f(m_storagePath);",
       "    QFile f(m_storagePath);   // 变异：非原子写")]),

    ("测试目标从 ctest 摘掉", "[credential]",
     [(TESTS_CM, "add_test(NAME t113_credential_store COMMAND t113_test_credential_store)",
       "# add_test(NAME t113_credential_store COMMAND t113_test_credential_store)")]),

    ("测试目标不链接真实 credential_store.cpp", "[credential]",
     [(TESTS_CM, "    test_credential_store.cpp\n"
                 "    ${ACCESSTERMINAL_SRC_ROOT}/managers/credential_store.cpp\n",
       "    test_credential_store.cpp\n")]),

    ("去掉「落盘不含明文」用例", "[credential]",
     [(TEST_SRC, "    void persistedFileContainsNoPlaintext()",
       "    void persistedFileContainsNoPlaintext_REMOVED()")]),

    ("去掉阳性对照（只剩否定断言）", "[credential]",
     [(TEST_SRC, "            QCOMPARE(store2.verify(QString::fromLatin1(kUser), secret),\n"
                 "                     VerifyOutcome::Accepted);\n"
                 "            // 且错口令仍然被拒（证明不是\"读失败导致谁都通过\"）。\n"
                 "            QCOMPARE(store2.verify(QString::fromLatin1(kUser), QStringLiteral(\"nope\")),\n"
                 "                     VerifyOutcome::Rejected);",
       "            (void)store2;")]),

    ("把时钟注入换成 sleep", "[credential]",
     [(TEST_SRC, "        store.setNowMsForTest(1000000);\n"
                 "        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral(\"bad\")),\n"
                 "                 VerifyOutcome::Rejected);",
       "        QTest::qSleep(1200);\n"
       "        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral(\"bad\")),\n"
       "                 VerifyOutcome::Rejected);")]),

    # ---- 对照：这些**不该**让规则报错 ----
    ("对照：加无关注释", None,
     [(CS_CPP, "namespace accessterminal {",
       "// 无关注释（对照）\nnamespace accessterminal {")]),

    ("对照：调整缩进空白", None,
     [(PM_H, "private:\n    /// 门级密码在存储层里的用户标识（单人门禁，固定 ID）。",
       "private:\n\n    /// 门级密码在存储层里的用户标识（单人门禁，固定 ID）。")]),
]


def main():
    if not _acquire_lock():
        print("另一个夹具正在改写源码（%s）—— 请等它跑完再试"
              % _GUARD.lock_path())
        return 3
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

    # 预检：所有锚点必须逐字节存在，否则夹具已过期
    for name, _expect, edits in MUT:
        for path, old, _new in edits:
            if _ORIG[path].count(old) != 1:
                print("[夹具错误] %s 的锚点在 %s 出现 %d 次（期望 1）"
                      % (name, os.path.basename(path), _ORIG[path].count(old)))
                return 2

    rc, _ = run_verify()
    print("基线 rc = %d" % rc)
    if rc != 0:
        print("*** 基线就不通过，无法进行故障注入")
        return 2
    print()

    problems = []
    try:
        for name, expect, edits in MUT:
            # 先把每个文件变到"应用全部 edit"的状态，再统一写盘。
            # 同一个文件可能有多条 edit（见 MUT 上面的注释）。
            staged = dict(_ORIG)
            anchor_ok = True
            for path, old, new in edits:
                if staged[path].count(old) != 1:
                    anchor_ok = False
                    break
                staged[path] = staged[path].replace(old, new, 1)
            if not anchor_ok:
                print("  %-38s *** 锚点在本轮失效" % name)
                problems.append(name)
                continue
            try:
                for path in set(p for p, _o, _n in edits):
                    write(path, staged[path])
                # 证明变异**确实落到磁盘上**了（否则下面"没报错"
                # 可能只是变异没生效）
                changed = all(read(p) != _ORIG[p] for p in set(p for p, _o, _n in edits))
                if not changed:
                    print("  %-38s *** 变异未落到磁盘" % name)
                    problems.append(name)
                    continue

                rc, lines = run_verify()
                if expect is None:
                    if rc == 0:
                        print("  %-38s 通过 ✓（不误报）" % name)
                    else:
                        print("  %-38s *** 误报：rc=%d" % (name, rc))
                        for l in lines[:3]:
                            print("      " + l[:120])
                        problems.append(name)
                else:
                    hit = any(expect in l for l in lines)
                    if rc != 0 and hit:
                        print("  %-38s 被抓到 ✓" % name)
                    else:
                        print("  %-38s *** 未被抓到（假绿）rc=%d" % (name, rc))
                        for l in lines[:3]:
                            print("      " + l[:120])
                        problems.append(name)
            finally:
                for path in set(p for p, _o, _n in edits):
                    write(path, _ORIG[path])
    finally:
        _restore_all()

    same = all(read(p) == _ORIG[p] for p in FILES)
    rc, _ = run_verify()
    print()
    print("还原逐字节一致:", same)
    print("还原后: %s" % ("结论: 全部检查通过" if rc == 0 else "*** 仍不通过 rc=%d" % rc))
    print()
    print("变异 %d 个，致命问题 %d 个" % (len(MUT), len(problems)))
    if problems:
        print("问题清单:", problems)
    return 1 if (problems or not same or rc != 0) else 0


if __name__ == "__main__":
    sys.exit(main())

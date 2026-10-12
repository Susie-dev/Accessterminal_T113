# -*- coding: utf-8 -*-
"""`[resource-path]` 静态规则的故障注入自检（第 44 轮，方案 P5）。

用法：python3 tools/verify_resource_path_rule.py
返回码 0 表示：每条危险退化都让 verify.py **变红**、对照不误报、
且源码还原后逐字节一致。

被守的缺陷
----------
人脸分类器路径在**三处**互不一致（配置默认值 / CMake install / install.sh），
默认部署下 `cv::CascadeClassifier::load()` 静默失败，人脸识别一直是关的，
日志只有一行"加载失败"。现场会误判成摄像头坏了。

静态变异清单
------------
  A1  删掉 resolveCascadePath 的调用（类写了不接）
      -> 判据 A/C：写了不调用与没有它等价
  A2  失败时不打印候选清单
      -> 判据 C：只打印"加载失败"正是重构前无从排查的老毛病
  A3  不做"配置优先"（配置项被忽略，永远用回退路径）
      -> 判据 B：现场部署是"配置说什么就是什么"
  B1  用 QFileInfo::absolutePath()（平台相关解析）
      -> 判据 D：Windows 上 "/etc/x" -> "C:/etc/x"，主机与目标机算出的
         候选不同 —— 测试在主机过、上板却不一样
  C1  CMakeLists.txt 里的分类器文件名写错
      -> 判据 E：一处偏离就静默失灵
  C2  install.sh 里的分类器文件名写错
      -> 判据 E：同上
  C3  appconfig.json 默认值里的文件名写错
      -> 判据 E：同上
  D1  删掉「配置优先」用例
      -> 判据 F：那是"不改变现场行为"的承诺，没有测试=没有承诺
  D2  删掉「回退」用例
      -> 判据 F：修复本身必须被钉住
  E1  从 tests/CMakeLists.txt 注销测试目标
      -> 判据 F：测试写了却从不执行（README 第 78 条）

对照（必须保持绿）
------------------
  C1  加无关注释
  C2  调整空白
  C3  给局部变量改名

纯静态规则夹具，**不需要编译**（除 E1 之外都不动 CMake 结构）。
锁忙返回码 3（与其他夹具一致）。
"""
import io
import os
import subprocess
import sys

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
T = os.path.dirname(HERE)
PY = sys.executable

RP_H = os.path.join(T, "src", "config", "resource_paths.h")
RP_CPP = os.path.join(T, "src", "config", "resource_paths.cpp")
WIDGET = os.path.join(T, "src", "ui", "widget.cpp")
ROOT_CMAKE = os.path.join(T, "CMakeLists.txt")
TESTS_CMAKE = os.path.join(T, "tests", "CMakeLists.txt")
INSTALL_SH = os.path.join(T, "deploy", "install.sh")
CFG_JSON = os.path.join(T, "config", "appconfig.json")
TEST_RP = os.path.join(T, "tests", "test_resource_paths.cpp")

FILES = [RP_H, RP_CPP, WIDGET, ROOT_CMAKE, TESTS_CMAKE, INSTALL_SH, CFG_JSON,
         TEST_RP]

TAG = "[resource-path]"

sys.path.insert(0, HERE)
from fixture_pristine import PristineGuard  # noqa: E402

_GUARD = PristineGuard(files=FILES, tag='p5respath',
                       lock_name='fi37_rewrite.lock')
_ORIG = _GUARD.orig


def read(p):
    return io.open(p, encoding="utf-8", errors="replace").read()


def write(p, s):
    # 必须用 with 关闭文件。`io.open(p,"w").write(s)` 依赖引用计数在 CPython
    # 里"刚好"立即关闭 —— 写完马上要在**子进程**里读同一个文件，
    # 没有显式关闭就可能读到最后一次 flush 之前的旧内容（假绿/假红都可能）。
    # 第 44 轮第一版就是这样写的，出现了难以解释的不一致结果。
    with io.open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(s)


def apply_edits(edits, name, problems):
    """把一组编辑**累积**地写进各自文件；返回 (ok, 涉及的文件集合)。

    为什么必须累积：第一版对每个编辑都从 `_ORIG[path]` 重新取原始文本再写回，
    于是**对同一个文件的第二条编辑会覆盖掉第一条**。D1/D2 需要同时删掉
    「类内声明」与「函数定义」，就因此只删掉了后者（或前者），
    规则照样能在残留处匹配到 —— 夹具报"没抓到"，看起来像规则失效，
    实际是夹具自己没把变异做出来。

    编辑的旧串可以出现多次（例如 install.sh 里同一文件名出现 3 次）——
    那种情况下要求**全部替换**，因为"只改一处"不构成真实退化。
    """
    by_file = {}
    for path, old, new in edits:
        if path not in by_file:
            by_file[path] = _ORIG[path]
        src = by_file[path]
        n = src.count(old)
        if n < 1:
            print("  %-44s *** 锚点未找到（0 次）" % name)
            problems.append(name + "(锚点)")
            return False, set()
        by_file[path] = src.replace(old, new)   # 全部替换
    for path, text in by_file.items():
        write(path, text)
    return True, set(by_file)


def restore(paths):
    for path in paths:
        write(path, _ORIG[path])


def run_verify():
    r = subprocess.run([PY, os.path.join(T, "tools", "verify.py"), T],
                       capture_output=True, cwd=T)
    out = (r.stdout + r.stderr).decode("utf-8", "replace")
    lines = [l.strip() for l in out.splitlines()
             if TAG in l and l.strip().startswith(("X", "!"))]
    return r.returncode, lines


# 变异：(名字, 期望失败的判据子串, [(文件, 旧, 新), ...])
MUT = [
    # ---- A. 解析入口真的被调用，且失败可查 ----
    ("A1 不再调用解析入口（类写了不接）", "没有调用 resolveCascadePath",
     [(WIDGET,
       "    const QString cascadePath = accessterminal::resolveCascadePath(\n"
       "        cfg.faceCascadePath(), cfg.configPath());\n",
       "    const QString cascadePath = cfg.faceCascadePath();   // 变异\n")]),

    ("A2 失败时不打印候选清单", "没有打印候选清单",
     [(WIDGET,
       "        const QStringList tried = accessterminal::cascadeSearchPaths(\n"
       "            cfg.faceCascadePath(), cfg.configPath());\n"
       "        for (const QString& p : tried) {\n"
       "            qWarning() << \"    -\" << p;\n"
       "        }\n",
       "        // 变异：不再打印候选清单\n")]),

    ("A3 破坏「配置优先」（忽略配置项）", "没有把配置里显式写明的路径放进候选",
     [(RP_CPP,
       "    const QString cfg = configured.trimmed();\n"
       "    if (!cfg.isEmpty()) {\n"
       "        out << cfg;\n"
       "    }\n",
       "    // 变异：忽略配置里写的路径\n")]),

    # ---- B. 平台无关的路径解析 ----
    ("B1 用 absolutePath() 做平台相关解析", "absolutePath",
     [(RP_CPP,
       "        const int slash = cp.lastIndexOf(QLatin1Char('/'));\n"
       "        if (slash > 0) {                       // >0：排除 \"/appconfig.json\" 这种\n"
       "            out << cp.left(slash + 1) + name;  // 根目录下的情形（无目录可拼）\n"
       "        }\n",
       "        const QString d = QFileInfo(cp).absolutePath();   // 变异\n"
       "        if (!d.isEmpty()) {\n"
       "            out << d + QLatin1Char('/') + name;\n"
       "        }\n")]),

    # ---- C. 文件名常量必须处处一致 ----
    ("C1 根 CMakeLists.txt 里文件名写错", "CMakeLists.txt 里没有出现",
     [(ROOT_CMAKE,
       "install(FILES deploy/vendor/haarcascade_frontalface_default.xml",
       "install(FILES deploy/vendor/haarcascade_frontalface_DEFAULT.xml")]),

    ("C2 install.sh 里文件名写错", "deploy/install.sh 里没有出现",
     # 必须改**全部**出现处（第 54/55/56 行三处）。只改一行的话，规则
     # 仍能在其余两行找到正确文件名 —— 那样的"变异"没制造出真实退化，
     # 反而会让人误以为规则失效。真实的退化是"整份脚本里名字写错了"。
     [(INSTALL_SH,
       "haarcascade_frontalface_default.xml",
       "haarcascade_frontal_face_default.xml")]),

    ("C3 配置默认值里文件名写错", "config/appconfig.json 里没有出现",
     [(CFG_JSON,
       "\"face_cascade\": \"/root/haarcascade_frontalface_default.xml\"",
       "\"face_cascade\": \"/root/haarcascade_frontal_face.xml\"")]),

    # ---- D. 测试必须钉住两个关键性质 ----
    #
    # 注意这两条变异必须**声明与定义一起删**。第一版只改了定义的签名，
    # 类内**声明**还在，规则就照样能在声明里匹配到 —— 那不是"删掉了测试"，
    # 只是改了名字。真实的"覆盖被删除"退化是两者都不复存在。
    #
    # D1 还要删掉 `resolvePrefersConfiguredWhenItExists`：规则的判据是
    # "两个用例里**至少有一个**在"，因为两者都证明了"配置优先"这条性质。
    # 只删一个不算退化 —— 把两个都删掉才是真的没人守了。
    ("D1 删掉「配置优先」用例（两个都删）", "缺少「配置路径优先」用例",
     [(TEST_RP,
       "    void configuredPathIsAlwaysFirst();\n", ""),
      (TEST_RP,
       "    void resolvePrefersConfiguredWhenItExists();\n", ""),
      (TEST_RP,
       "/// 配置里写明的路径必须**永远**是第一候选 —— 这条兼容性性质是核心。\n"
       "void TestResourcePaths::configuredPathIsAlwaysFirst()\n{\n"
       "    const QString cfg = QStringLiteral(\"/opt/cascade.xml\");\n"
       "    const QStringList cands = cascadeSearchPaths(cfg, QString());\n"
       "    QVERIFY(!cands.isEmpty());\n"
       "    QCOMPARE(cands.first(), cfg);\n"
       "\n"
       "    // 即使配置路径与某个回退位置\"看起来相同\"，也不得被换掉。\n"
       "    const QStringList c2 = cascadeSearchPaths(\n"
       "        QStringLiteral(\"/root/haarcascade_frontalface_default.xml\"), QString());\n"
       "    QCOMPARE(c2.first(),\n"
       "             QStringLiteral(\"/root/haarcascade_frontalface_default.xml\"));\n"
       "}\n", ""),
      (TEST_RP,
       "void TestResourcePaths::resolvePrefersConfiguredWhenItExists()",
       "void TestResourcePaths::deletedConfiguredPreference()")]),

    ("D2 删掉「回退」用例（声明+定义）", "缺少「配置路径不存在时",
     [(TEST_RP,
       "    void resolveFallsBackWhenConfiguredIsMissing();\n", ""),
      (TEST_RP,
       "/// 这是本模块存在的理由：配置路径不存在时，必须回退到**真实存在**的落点。\n"
       "/// 重构前这里直接失败 -> 人脸识别静默关闭。\n"
       "void TestResourcePaths::resolveFallsBackWhenConfiguredIsMissing()\n{\n",
       "void TestResourcePaths::deletedFallbackCase()\n{\n")]),

    # ---- E. 目标必须真的注册 ----
    ("E1 注销测试目标（写了却从不执行）", "没有用 add_test 注册",
     [(TESTS_CMAKE,
       "add_test(NAME t113_resource_paths COMMAND t113_test_resource_paths)",
       "# 变异：不再注册")]),
]


# 对照：不该让规则报错
CTRL = [
    ("对照：加无关注释",
     [(RP_CPP, "QString cascadeFileName()",
       "// 无关注释（对照）\nQString cascadeFileName()")]),

    ("对照：调整空白",
     [(RP_CPP, "QStringList cascadeSearchPaths(",
       "QStringList cascadeSearchPaths\n(")]),

    ("对照：给局部变量改名",
     [(RP_CPP,
       "    QStringList uniq;\n"
       "    for (const QString& p : out) {\n"
       "        if (!uniq.contains(p)) {\n"
       "            uniq << p;\n"
       "        }\n"
       "    }\n"
       "    return uniq;",
       "    QStringList deduped;\n"
       "    for (const QString& q : out) {\n"
       "        if (!deduped.contains(q)) {\n"
       "            deduped << q;\n"
       "        }\n"
       "    }\n"
       "    return deduped;")]),
]


def main():
    if not _GUARD.acquire_lock():
        print("另一个夹具正在改写源码（%s）—— 请等它跑完再试"
              % _GUARD.lock_path())
        print("（构建/ctest/verify.py 也不要与夹具并发）")
        return 3
    try:
        return _run()
    finally:
        _GUARD.release_lock()


def _run():
    healed = _GUARD.prepare()
    _GUARD.install_guards()
    if healed:
        print("!! 检测到上一次运行被中断，已用磁盘原始副本自愈: %s"
              % ", ".join(os.path.basename(p) for p in healed))

    rc0, base = run_verify()
    print("基线 verify.py rc=%d，%s 失败 %d 条" % (rc0, TAG, len(base)))
    if rc0 != 0:
        print("*** 基线就不通过，无法进行故障注入：")
        for l in base[:5]:
            print("    " + l[:150])
        return 2
    print()

    problems = []

    for name, expect, edits in MUT:
        try:
            ok, touched = apply_edits(edits, name, problems)
            if ok:
                rc, lines = run_verify()
                hit = any(expect in l for l in lines)
                if rc != 0 and hit:
                    print("  %-44s 通过 ✓" % name)
                else:
                    print("  %-44s *** 失败  rc=%d 命中=%s" % (name, rc, hit))
                    for l in lines[:3]:
                        print("        " + l[:150])
                    problems.append(name)
        finally:
            restore({e[0] for e in edits})

    print()

    for name, edits in CTRL:
        try:
            ok, touched = apply_edits(edits, name, problems)
            if ok:
                rc, lines = run_verify()
                if rc == 0:
                    print("  %-44s 通过 ✓  （未误报）" % name)
                else:
                    print("  %-44s *** 误报  rc=%d" % (name, rc))
                    for l in lines[:3]:
                        print("        " + l[:150])
                    problems.append(name)
        finally:
            restore({e[0] for e in edits})

    for path in FILES:
        write(path, _ORIG[path])
    same = all(read(p) == _ORIG[p] for p in FILES)
    rc, lines = run_verify()

    print()
    print("还原逐字节一致:", same)
    print("还原后 verify.py rc=%d" % rc)
    print()
    print("变异 %d 个，对照 %d 个，致命问题 %d 个"
          % (len(MUT), len(CTRL), len(problems)))
    if problems:
        print("问题清单:", problems)
    return 1 if (problems or not same or rc != 0) else 0


if __name__ == "__main__":
    sys.exit(main())

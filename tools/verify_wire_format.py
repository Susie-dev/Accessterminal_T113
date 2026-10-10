# -*- coding: utf-8 -*-
"""check_wire_format_constants_are_pinned 的故障注入自检。

用法: verify_wire_format.py <verify.py> <本仓库根>

验证的要点：规则必须能区分"真的断言了"与"只是名字出现过/只是改了清单"。
"""
import importlib.util
import os
import shutil
import sys
import tempfile

VERIFY = os.path.abspath(sys.argv[1])
REPO = os.path.abspath(sys.argv[2])
FAILED = 0
TOTAL = 0


def load():
    spec = importlib.util.spec_from_file_location("v", VERIFY)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def run_case(name, expect_fail, build, patch_module=None):
    """在临时仓库上施加 build(root) 变形，然后运行规则。

    patch_module: 可选的 (module) -> None。用于直接修改**已加载的模块**
    （例如往 WIRE_FORMAT_CONSTANTS 里加一条探针常量）。
    注意不能靠改临时仓库里的 verify.py —— load() 导入的是原始 VERIFY 路径。
    """
    global FAILED, TOTAL
    TOTAL += 1
    tmp = tempfile.mkdtemp(prefix="wire_")
    try:
        root = os.path.join(tmp, "repo")
        shutil.copytree(REPO, root,
                        ignore=shutil.ignore_patterns("build", "build-*", ".git"))
        build(root)
        m = load()
        if patch_module:
            patch_module(m)
        m.FAIL.clear()
        m.WARN.clear()
        m.check_wire_format_constants_are_pinned(root, os.path.join(root, "src"))
        got = len(m.FAIL) > 0
        ok = got == expect_fail
        print("  [{}] {}  (fail={}, 期望fail={})".format(
            "PASS" if ok else "FAIL", name, got, expect_fail))
        if not ok:
            FAILED += 1
            for x in m.FAIL[:3]:
                print("        -> " + x[:170])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


CONSTS = ("kAudioSampleRate", "kAudioChannels", "kAudioBytesPerSample",
          "kAudioFramesPerPacket", "kSignalingPort", "kAudioPort",
          "kVideoPort", "kPreviewPort")


def strip_all_from_tests(root):
    tests = os.path.join(root, "tests")
    for fn in os.listdir(tests):
        if not fn.endswith((".cpp", ".h", ".hpp")):
            continue
        p = os.path.join(tests, fn)
        s = open(p, encoding="utf-8").read()
        for c in CONSTS:
            s = s.replace(c, "REMOVED_CONST")
        open(p, "w", encoding="utf-8", newline="\n").write(s)


def only_in_comments(root):
    strip_all_from_tests(root)
    p = os.path.join(root, "tests", "test_protocol.cpp")
    s = open(p, encoding="utf-8").read()
    s += "\n// 注释里提一下：" + " ".join(CONSTS) + "\n"
    open(p, "w", encoding="utf-8", newline="\n").write(s)


def add_undeclared_assert(root):
    """protocol.h 新增一个线上常量，测试不提及（探针由 patch_module 加进清单）。"""
    p = os.path.join(root, "src", "core", "net", "protocol.h")
    s = open(p, encoding="utf-8").read()
    i = s.find("kSignalTerminator")
    s = s[:i] + "constexpr quint16 kPreviewFpsProbe = 20;\n" + s[i:]
    open(p, "w", encoding="utf-8", newline="\n").write(s)


def add_declared_assert(root):
    p = os.path.join(root, "src", "core", "net", "protocol.h")
    s = open(p, encoding="utf-8").read()
    i = s.find("kSignalTerminator")
    s = s[:i] + "constexpr quint16 kCoveredProbe = 42;\n" + s[i:]
    open(p, "w", encoding="utf-8", newline="\n").write(s)
    # 同时在测试里断言它
    t = os.path.join(root, "tests", "test_audio_sender.cpp")
    ts = open(t, encoding="utf-8").read()
    ts += "\n// probe\nstatic const int kProbeCheck = protocol::kCoveredProbe;\n"
    open(t, "w", encoding="utf-8", newline="\n").write(ts)


def add_to_list(module, name):
    module.WIRE_FORMAT_CONSTANTS = tuple(module.WIRE_FORMAT_CONSTANTS) + (name,)


print("== check_wire_format_constants_are_pinned 故障注入自检 ==")
run_case("当前仓库(真阴性)", False, lambda r: None)
run_case("测试里完全没有断言(真阳性)", True, strip_all_from_tests)
run_case("只在注释里提及(真阳性)", True, only_in_comments)
run_case("新增线上常量却无测试(真阳性)", True, add_undeclared_assert,
         lambda m: add_to_list(m, "kPreviewFpsProbe"))
run_case("新增常量且测试已断言(真阴性)", False, add_declared_assert,
         lambda m: add_to_list(m, "kCoveredProbe"))
run_case("清单里的常量在 protocol.h 不存在(真阴性)",
         False, lambda r: None, lambda m: add_to_list(m, "kNotInProtocolAtAll"))

# 真实仓库必须通过
TOTAL += 1
m = load()
m.FAIL.clear()
m.check_wire_format_constants_are_pinned(REPO, os.path.join(REPO, "src"))
ok = len(m.FAIL) == 0
print("  [{}] 真实仓库实测  (fail={})".format("PASS" if ok else "FAIL", len(m.FAIL)))
if not ok:
    FAILED += 1

print("\n结果: {}/{} 通过".format(TOTAL - FAILED, TOTAL))
sys.exit(1 if FAILED else 0)

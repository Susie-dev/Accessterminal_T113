# -*- coding: utf-8 -*-
"""对 check_no_running_reset_in_run 做故障注入自检（纯 Python）。"""
import importlib.util
import os
import shutil
import sys
import tempfile

VERIFY = sys.argv[1]
ROOT = sys.argv[2]
FAILED = 0
TOTAL = 0


def load():
    spec = importlib.util.spec_from_file_location("v", VERIFY)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def run_case(name, files, expect_fail):
    global FAILED, TOTAL
    TOTAL += 1
    tmp = tempfile.mkdtemp(prefix="stopflag_")
    try:
        os.makedirs(os.path.join(tmp, "src", "core", "media"), exist_ok=True)
        for rel, text in files.items():
            p = os.path.join(tmp, rel.replace("/", os.sep))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
        m = load()
        m.FAIL.clear()
        m.WARN.clear()
        m.check_no_running_reset_in_run(tmp, os.path.join(tmp, "src"))
        got = len(m.FAIL) > 0
        ok = got == expect_fail
        print("  [{}] {}  (fail={}, 期望fail={})".format(
            "PASS" if ok else "FAIL", name, got, expect_fail))
        if not ok:
            FAILED += 1
            for f in m.FAIL[:3]:
                print("        -> " + f[:150])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


H_BAD = """
#include <QThread>
#include <atomic>
class CamPush : public QThread {
    Q_OBJECT
public:
    CamPush() : running(false) {}
    void stop();
protected:
    void run() override;
private:
    std::atomic<bool> running;
};
"""

C_BAD = """
#include "cam.h"
void CamPush::stop() { running = false; }
void CamPush::run() {
    running = true;
    while (running) { }
}
"""

H_GOOD = H_BAD.replace("running(false)", "running(true)")

C_GOOD = """
#include "cam.h"
void CamPush::stop() { running = false; }
void CamPush::run() {
    while (running) { }
}
"""

# 反例：run() 里出现 running = true 但那个名字不是停止标志
# （停止标志是其它名字），不应误报。
H_OTHER = """
#include <QThread>
#include <atomic>
class CamPush : public QThread {
    Q_OBJECT
public:
    CamPush() : running(true) {}
    void stop();
protected:
    void run() override;
private:
    std::atomic<bool> running;
    bool verbose = false;
};
"""

C_OTHER = """
#include "cam.h"
void CamPush::stop() { running = false; }
void CamPush::run() {
    verbose = true;
    while (running) { }
}
"""

# 反例：非 QThread 的类，即使有同名模式也不该被检查
H_PLAIN = """
#include <atomic>
class NotAThread {
public:
    NotAThread() : running(false) {}
    void stop();
private:
    std::atomic<bool> running;
};
"""

C_PLAIN = """
#include "plain.h"
void NotAThread::stop() { running = false; }
void NotAThread::run() {
    running = true;
}
"""

print("== check_no_running_reset_in_run 故障注入自检 ==")
run_case("run() 开头重置停止标志(真阳性)",
         {"src/media/cam.h": H_BAD, "src/media/cam.cpp": C_BAD}, True)
run_case("已修复:初值true且run不重置(真阴性)",
         {"src/media/cam.h": H_GOOD, "src/media/cam.cpp": C_GOOD}, False)
run_case("重置的是非停止标志(真阴性)",
         {"src/media/cam.h": H_OTHER, "src/media/cam.cpp": C_OTHER}, False)
run_case("非QThread类(真阴性)",
         {"src/media/plain.h": H_PLAIN, "src/media/plain.cpp": C_PLAIN}, False)
run_case("当前仓库已修复", {}, False)

# 当前仓库真实文件参与
TOTAL += 1
tmp = tempfile.mkdtemp(prefix="stopflag_real_")
try:
    shutil.copytree(os.path.join(ROOT, "src"), os.path.join(tmp, "src"))
    m = load()
    m.FAIL.clear()
    m.check_no_running_reset_in_run(tmp, os.path.join(tmp, "src"))
    ok = len(m.FAIL) == 0
    print("  [{}] 真实仓库 src/ 全量  (fail={})".format(
        "PASS" if ok else "FAIL", len(m.FAIL)))
    if not ok:
        FAILED += 1
        for f in m.FAIL[:3]:
            print("        -> " + f[:150])
finally:
    shutil.rmtree(tmp, ignore_errors=True)

print("\n结果: {} 个用例失败 (共 {})".format(FAILED, TOTAL))
sys.exit(1 if FAILED else 0)

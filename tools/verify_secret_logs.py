# -*- coding: utf-8 -*-
"""对 check_no_secret_in_logs 做故障注入自检（纯 Python，不经 PowerShell 文本往返）。"""
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


def run_case(name, mutate, expect_fail):
    """在临时副本上跑 check_core；mutate 接收 repo 根做改写。"""
    global FAILED, TOTAL
    TOTAL += 1
    tmp = tempfile.mkdtemp(prefix="seclog_")
    try:
        shutil.copytree(os.path.join(ROOT, "src"), os.path.join(tmp, "src"))
        mutate(tmp)
        m = load()
        m.FAIL.clear()
        m.WARN.clear()
        m.check_no_secret_in_logs.__wrapped__ if False else None
        getattr(m, "check_no_secret_in_logs")(tmp, os.path.join(tmp, "src"))
        got_fail = len(m.FAIL) > 0
        ok = got_fail == expect_fail
        print("  [{}] {}  (fail={}, 期望fail={})".format(
            "PASS" if ok else "FAIL", name, got_fail, expect_fail))
        if not ok:
            FAILED += 1
            for f in m.FAIL[:3]:
                print("        -> " + f[:140])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def case_original(root):
    """还原最初的泄密语句。"""
    write(os.path.join(root, "src", "managers", "password_manager.cpp"), '''
#include "managers/password_manager.h"
PasswordManager::PasswordManager(QObject* parent) : QObject(parent) {}
void PasswordManager::checkPassword(const QString& input)
{
    qDebug() << "[PasswordManager] checkPassword: input=" << input
             << ", password=" << password
             << ", inputHex=" << input.toUtf8().toHex();
    emit passwordChecked(input == password);
}
''')


def case_form(root):
    """还原输入窗体逐位 + 整串日志。"""
    write(os.path.join(root, "src", "ui", "form_passwd_x.cpp"), '''
#include "ui/form_passwd.h"
void Form_passwd::onDigitClicked()
{
    qDebug() << "[passwd] Digit clicked:" << btn->text();
}
void Form_passwd::on_btnok_clicked()
{
    qDebug() << "[passwd] OK clicked, inputBuffer=" << inputBuffer
             << ", hex=" << inputBuffer.toUtf8().toHex();
}
''')


def case_tag_only(root):
    """反例：敏感词只出现在日志前缀标签里，不应报错。"""
    write(os.path.join(root, "src", "ui", "form_passwd_y.cpp"), '''
#include "ui/form_passwd.h"
void Form_passwd::onDigitClicked()
{
    qDebug() << "[passwd] 数字键按下";
    qDebug() << "[PasswordManager] 密码校验完成，结果:" << ok;
}
''')


def case_length_only(root):
    """反例：只记录长度/是否为空，不应报错。"""
    write(os.path.join(root, "src", "ui", "form_passwd_z.cpp"), '''
#include "ui/form_passwd.h"
void Form_passwd::on_btnok_clicked()
{
    qDebug() << "[passwd] 已输入位数=" << inputBuffer.size();
    qDebug() << "[passwd] 空?" << password.isEmpty();
}
''')


def case_clean(root):
    """当前（已修复）仓库，应通过。"""
    return


print("== check_no_secret_in_logs 故障注入自检 ==")
run_case("原始泄密语句(真阳性)", case_original, True)
run_case("输入窗体逐位/整串日志(真阳性)", case_form, True)
run_case("敏感词仅作日志标签(真阴性)", case_tag_only, False)
run_case("仅记录长度/空判定(真阴性)", case_length_only, False)
run_case("当前仓库已修复", case_clean, False)

print("\n结果: {} 个用例失败 (共 {})".format(FAILED, TOTAL))
sys.exit(1 if FAILED else 0)

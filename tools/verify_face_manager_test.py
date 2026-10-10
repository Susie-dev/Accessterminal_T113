# -*- coding: utf-8 -*-
"""FaceManager 测试的故障注入自检（纯 Python，Windows 路径）。

用法：python tools/verify_face_manager_test.py
返回码 0 表示：每个门禁退化都被抓到，且两个对照未误报。

重要：多数变异是被**进程崩溃**抓到的，不是干净的 FAIL 行 ——
未训练的 LBPH predict() 抛 cv::Exception，异常逃出 Qt 事件循环
即 std::terminate。夹具因此必须同时检查 FAIL 行与退出码。

用法：python tools/verify_face_manager_test.py

要证明的命题：这组测试能否真的抓到**门禁退化**？每个变异都朝 face_manager.cpp
真实的危险退化去改，并要求对应用例失败或进程崩溃。

多数变异是被**进程崩溃**抓到的，不是干净的 FAIL 行 —— 未训练的 LBPH
predict() 会抛 cv::Exception，异常逃出 Qt 事件循环即 std::terminate。
夹具因此必须同时看 FAIL 行**与**退出码。
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


for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

if len(sys.argv) > 1:
    print("提示: 本脚本不需要参数，已忽略: %s" % " ".join(sys.argv[1:]))

T = r'C:\Users\user\Desktop\linux_ai\Accessterminal_T113'
FM = os.path.join(T, 'src', 'managers', 'face_manager.cpp')
MSYS = r'C:\Users\user\.dsh\toolchains\msys64'
CMAKE = os.path.join(MSYS, 'mingw64', 'bin', 'cmake.exe')
BUILD = os.path.join(os.environ['TEMP'], 'fm29win')
EXE = os.path.join(BUILD, 't113_test_face_manager.exe')

ENV = dict(os.environ)
ENV['PATH'] = (os.path.join(MSYS, 'mingw64', 'bin') + os.pathsep
               + os.path.join(MSYS, 'usr', 'bin') + os.pathsep + ENV['PATH'])

ADDGUARD = '    if (registeredFaces.size() >= collectTarget) return;'
# 守卫锚点直接从 face_manager.cpp 的真实字节构造（含内部注释行）。
# 手抄会漏掉注释，导致锚点 count=0 —— 本轮已踩过一次，夹具的
# 『锚点唯一』自证把它拦住了。
TRAINGUARD = '''    if (registeredFaces.size() < collectTarget) {
        // 发出 Qt 信号 trainFinished 通知其他对象。
        emit trainFinished(false);
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。'''
RECGUARD = '''    if (registeredFaces.size() < collectTarget) {
        // 发出 Qt 信号 recognizeResult 通知其他对象。
        emit recognizeResult(false, 0.0);
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。'''

MUT = [
    ("addFaceSample 不再在满额时丢弃",
     ADDGUARD,
     '    // 不再丢弃超额样本'),

    # 只替换守卫的**条件行**，保持括号完整；整块替换会漏掉结尾的 `}`，
    # 导致变异后括号不匹配、构建失败（被夹具误记成"构建失败"而非"抓不到"）。
    ("train 不再检查样本数（样本不足也去训练）",
     TRAINGUARD,
     TRAINGUARD.replace('registeredFaces.size() < collectTarget', 'false')),

    ("recognize 不再检查样本数（对未训练模型 predict）",
     RECGUARD,
     RECGUARD.replace('registeredFaces.size() < collectTarget', 'false')),

    ("阈值比较由严格 `<` 改成 `<=`",
     'bool success = (predictedLabel == 1 && confidence < confidenceThreshold);',
     'bool success = (predictedLabel == 1 && confidence <= confidenceThreshold);'),

    ("识别丢弃配置阈值、改回硬编码 90",
     'bool success = (predictedLabel == 1 && confidence < confidenceThreshold);',
     'bool success = (predictedLabel == 1 && confidence < 90);'),

    ("注册样本不做灰度转换（彩色直接存）",
     '    QImage gray = faceImg.convertToFormat(QImage::Format_Grayscale8);\n'
     '    // 执行本行语句完成对应变量、对象或业务状态处理。\n'
     '    // gray 是非 const 的 QImage',
     '    QImage gray = faceImg;\n'
     '    // 执行本行语句完成对应变量、对象或业务状态处理。\n'
     '    // gray 是非 const 的 QImage'),

    ("训练标签不再是 1（predictedLabel==1 恒真被打破）",
     '    std::vector<int> labels(collectTarget, 1);',
     '    std::vector<int> labels(collectTarget, 2);'),

    ("识别结果恒报拒绝",
     '    emit recognizeResult(success, confidence);',
     '    emit recognizeResult(false, confidence);'),

    ("addFaceSample 不再发进度信号",
     '    emit faceSampled(registeredFaces.size(), collectTarget);',
     '    // 不再发进度'),

    ("train 样本不足也报成功",
     TRAINGUARD,
     TRAINGUARD.replace('emit trainFinished(false);', 'emit trainFinished(true);')),

    ("clear() 不再清空样本",
     '    registeredFaces.clear();',
     '    // 不再清空'),

    ("对照：加无关注释",
     'FaceManager::FaceManager(QObject* parent)',
     '// 无关注释\nFaceManager::FaceManager(QObject* parent)', False),

    ("对照：改 qDebug 文案（本文件无 qDebug，改为改注释）",
     '// 引入系统或第三方头文件 <QDebug>。',
     '// 引入系统或第三方头文件 <QDebug>（注释改动，不影响语义）。', False),
]


def main():
    orig = io.open(FM, encoding='utf-8', errors='replace').read()
    for item in MUT:
        name, old = item[0], item[1]
        if orig.count(old) != 1:
            print('[夹具错误] %s 锚点出现 %d 次' % (name, orig.count(old)))
            return 2

    shutil.rmtree(BUILD, ignore_errors=True)
    r = subprocess.run([CMAKE, '-S', os.path.join(T, 'tests'), '-B', BUILD,
                        '-G', 'Ninja'], capture_output=True, env=ENV, cwd=T)
    print('configure rc=%d' % r.returncode)
    if r.returncode != 0:
        print(r.stderr.decode('utf-8', 'replace')[-1500:])
        return 2
    rb = subprocess.run([CMAKE, '--build', BUILD, '--target',
                         't113_test_face_manager'],
                        capture_output=True, env=ENV, cwd=T)
    print('初始构建 rc=%d' % rb.returncode)
    if rb.returncode != 0:
        print(rb.stdout.decode('utf-8', 'replace')[-1200:])
        return 2
    print()

    problems = []
    try:
        for item in MUT:
            name, old, new = item[0], item[1], item[2]
            expect_fail = item[3] if len(item) > 3 else True

            text = orig.replace(old, new, 1)
            io.open(FM, 'w', encoding='utf-8', newline='\n').write(text)
            if io.open(FM, encoding='utf-8', errors='replace').read() == orig:
                print('  [夹具错误] %-40s 文件未变' % name)
                problems.append(name)
                io.open(FM, 'w', encoding='utf-8', newline='\n').write(orig)
                continue

            t0 = os.path.getmtime(EXE) if os.path.exists(EXE) else 0
            rb = subprocess.run([CMAKE, '--build', BUILD, '--target',
                                 't113_test_face_manager'],
                                capture_output=True, env=ENV, cwd=T)
            if rb.returncode != 0:
                print('  [构建失败] %-40s %s'
                      % (name, rb.stdout.decode('utf-8', 'replace')[-200:]))
                problems.append(name)
                io.open(FM, 'w', encoding='utf-8', newline='\n').write(orig)
                continue
            t1 = os.path.getmtime(EXE) if os.path.exists(EXE) else 0

            rr = subprocess.run([EXE], capture_output=True, env=ENV, cwd=T)
            out = (rr.stdout + rr.stderr).decode('utf-8', 'replace')
            failed = re.findall(r'^FAIL!\s*:\s*\w+::(\w+)\(\)', out, re.M)
            crashed = rr.returncode not in (0, 1)

            if t1 <= t0:
                print('  [夹具错误] %-40s 可执行未重链' % name)
                problems.append(name)
            elif expect_fail:
                if failed or crashed:
                    tag = ('崩溃(%d)' % rr.returncode) if crashed else str(failed[:2])
                    print('  %-40s 被抓到 ✓ %s' % (name, tag))
                else:
                    print('  %-40s *** 未被抓到（假绿）rc=%d'
                          % (name, rr.returncode))
                    problems.append(name)
            else:
                if not failed and not crashed:
                    print('  %-40s 通过 ✓（不误报）' % name)
                else:
                    print('  %-40s *** 误报：%s' % (name, failed[:3]))
                    problems.append(name)

            io.open(FM, 'w', encoding='utf-8', newline='\n').write(orig)
    finally:
        io.open(FM, 'w', encoding='utf-8', newline='\n').write(orig)

    same = io.open(FM, encoding='utf-8', errors='replace').read() == orig
    print()
    print('还原逐字节一致:', same)
    subprocess.run([CMAKE, '--build', BUILD, '--target', 't113_test_face_manager'],
                   capture_output=True, env=ENV, cwd=T)
    rr = subprocess.run([EXE], capture_output=True, env=ENV, cwd=T)
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

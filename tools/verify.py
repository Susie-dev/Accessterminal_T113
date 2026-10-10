#!/usr/bin/env python3
"""
静态一致性校验：在没有编译器/Qt 的环境下，尽可能发现重构引入的机械性错误。

检查项：
  1. 所有本地 #include "..." 都能解析到实际文件（含 src/ 为根的搜索路径）
  2. CMakeLists.txt / .pro 的源文件列表与实际文件一一对应（无遗漏、无幽灵文件）
  3. res.qrc 引用的资源全部存在
  4. .ui 文件的 objectName 与代码中 ui->xxx 的引用一致
  5. 每个 .ui 都能被 Qt 的 uic 规则覆盖（列在 FORMS/APP_FORMS 中）
  6. 括号/花括号/引号粗平衡
  7. 检测重构前的旧标识符残留

用法：python verify.py <项目根目录>
"""
import os
import re
import sys
import xml.etree.ElementTree as ET

FAIL = []
WARN = []


def fail(msg):
    FAIL.append(msg)


def warn(msg):
    WARN.append(msg)


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def find_files(root, exts):
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        # 跳过构建目录与版本库
        dirnames[:] = [d for d in dirnames
                       if d not in (".git", "build", "build-t113", "docs", "deploy")]
        for fn in filenames:
            if os.path.splitext(fn)[1].lower() in exts:
                out.append(os.path.join(dirpath, fn))
    return out


def check_includes(root, src_root):
    """
    本地 #include 必须可解析：相对当前文件目录，或相对 src/ 根。
    ui_*.h 由 Qt 的 uic 在构建时生成，源码树中不存在，属正常情况。
    """
    pattern = re.compile(r'^\s*#include\s+"([^"]+)"', re.M)
    for path in find_files(src_root, {".cpp", ".h", ".hpp"}):
        text = read(path)
        base = os.path.dirname(path)
        for inc in pattern.findall(text):
            # uic/moc 生成物，构建期才出现
            if os.path.basename(inc).startswith(("ui_", "moc_", "qrc_")):
                continue
            cand = [os.path.join(base, inc), os.path.join(src_root, inc)]
            if not any(os.path.isfile(c) for c in cand):
                fail(f"[include] {os.path.relpath(path, root)} -> 无法解析 \"{inc}\"")


def check_build_lists(root, src_root):
    """CMake/pro 里列出的源文件必须存在；实际源文件必须被列出。"""
    cmake = os.path.join(root, "CMakeLists.txt")
    pro = os.path.join(root, "smartpro.pro")

    listed = set()
    if os.path.isfile(cmake):
        text = read(cmake)
        for m in re.finditer(r"^\s*(src/[\w/]+\.(?:cpp|h|ui|qrc))\s*$", text, re.M):
            listed.add(m.group(1).replace("/", os.sep))
        for m in re.finditer(r"^\s*(src/[\w/]+\.(?:cpp|h|ui|qrc))", text, re.M):
            listed.add(m.group(1).replace("/", os.sep))
    if os.path.isfile(pro):
        text = read(pro)
        for m in re.finditer(r"^\s*(src/[\w/]+\.(?:cpp|h|ui|qrc))\s*\\?$", text, re.M):
            listed.add(m.group(1).replace("/", os.sep))

    normalized = set()
    for item in listed:
        normalized.add(os.path.normpath(item))

    # 实际存在的、应当被列出的源文件
    actual = set()
    for path in find_files(src_root, {".cpp", ".h", ".ui", ".qrc"}):
        rel = os.path.relpath(path, root)
        normalized_rel = os.path.normpath(rel)
        actual.add(normalized_rel)

    missing_in_build = sorted(a for a in actual if a not in normalized)
    ghost = sorted(n for n in normalized if not os.path.isfile(os.path.join(root, n)))

    for m in missing_in_build:
        fail(f"[build] 源文件未列入构建脚本: {m}")
    for g in ghost:
        fail(f"[build] 构建脚本列出了不存在的文件: {g}")


def check_qrc(root, src_root):
    """res.qrc 引用的资源必须实际存在（相对 qrc 所在目录）。"""
    for qrc in find_files(src_root, {".qrc"}):
        base = os.path.dirname(qrc)
        text = read(qrc)
        for ref in re.findall(r"<file>([^<]+)</file>", text):
            ref = ref.strip()
            target = os.path.join(base, ref.replace("/", os.sep))
            if not os.path.isfile(target):
                fail(f"[qrc] {os.path.relpath(qrc, root)} 引用缺失资源: {ref}")


def check_ui_objectnames(root, src_root):
    """代码中的 ui->xxx 必须在对应 .ui 的 objectName 中定义。"""
    ui_files = find_files(src_root, {".ui"})
    for uipath in ui_files:
        try:
            tree = ET.parse(uipath)
        except ET.ParseError as e:
            fail(f"[ui] XML 解析失败 {os.path.relpath(uipath, root)}: {e}")
            continue

        names = set()
        for el in tree.iter():
            n = el.get("name")
            if n:
                names.add(n)

        stem = os.path.splitext(os.path.basename(uipath))[0]
        # 找到引用该 ui 的 cpp（同目录同名，或 include "ui_xxx.h"）
        for cpp in find_files(src_root, {".cpp"}):
            text = read(cpp)
            if f"ui_{stem}.h" not in text and os.path.splitext(os.path.basename(cpp))[0] != stem:
                continue
            # 仅在确实使用 Ui::<Stem> 时校验
            if f"Ui::{stem}" not in text and "setupUi" not in text:
                continue
            used = set(re.findall(r"\bui->([A-Za-z_]\w*)", text))
            # setupUi/retranslateUi 是 uic 生成的方法，不是控件
            used -= {"setupUi", "retranslateUi"}
            unknown = sorted(u for u in used if u not in names)
            for u in unknown:
                fail(f"[ui] {os.path.relpath(cpp, root)} 引用了 "
                     f"{os.path.basename(uipath)} 中不存在的控件: ui->{u}")


def check_balance(root, src_root):
    """粗粒度符号平衡（忽略字符串与注释会过度复杂，这里只做整体计数）。"""
    for path in find_files(src_root, {".cpp", ".h"}):
        text = read(path)
        # 去掉行注释与字符串字面量，减少误报
        stripped = re.sub(r"//[^\n]*", "", text)
        stripped = re.sub(r'"(?:\\.|[^"\\])*"', '""', stripped)
        stripped = re.sub(r"/\*.*?\*/", "", stripped, flags=re.S)
        for op, cl, label in (("{", "}", "花括号"), ("(", ")", "圆括号")):
            a, b = stripped.count(op), stripped.count(cl)
            if a != b:
                fail(f"[balance] {os.path.relpath(path, root)} {label}不平衡: {op}={a} {cl}={b}")


def strip_comments_and_strings(text):
    """
    去掉注释与字符串字面量，避免把注释里的示例当作真实代码。
    先处理块注释与字符串，最后处理行注释，避免相互干扰。
    """
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r'"(?:\\.|[^"\\])*"', '""', text)
    text = re.sub(r"'(?:\\.|[^'\\])*'", "''", text)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def check_legacy_identifiers(root, src_root):
    """重构后的源码不应再出现旧路径/旧标识符（忽略注释与字符串）。"""
    legacy = [
        (r'/home/meetyoo', "旧开发机绝对路径"),
        (r'camera/v4l2camera\.h', "旧 include 路径"),
        (r'camera_udp_server\.h', "旧 include 路径"),
        (r'camera_push_thread\.h', "旧 include 路径"),
        (r'#include\s+"appconfig\.h"', "旧配置头 appconfig.h"),
        (r'#include\s+"HailinFingerprint\.h"', "旧头文件名"),
        (r'QString::SkipEmptyParts', "Qt5-only API（应为 Qt::SkipEmptyParts）"),
        (r'QVector<[^>]+>::fromStdVector', "Qt6 已移除的 API"),
        (r'getFingerprint\(\)', "旧 Widget 访问器"),
        (r'getNextFingerId\(\)', "旧 Widget 访问器"),
    ]
    for path in find_files(src_root, {".cpp", ".h", ".pro"}):
        raw = read(path)
        text = strip_comments_and_strings(raw) if not path.endswith(".pro") else raw
        for pat, desc in legacy:
            if re.search(pat, text):
                fail(f"[legacy] {os.path.relpath(path, root)} 仍含{desc}: {pat}")


def check_qt5_portability(root, src_root):
    """
    Qt5 版本可移植性检查。

    目标 SDK（Tina Linux）常见为 Qt 5.9~5.12，而开发机可能是 5.15。
    仅在新版本存在的 API 会让交叉编译在旧 SDK 上失败，这里静态拦截。
    """
    # 这些符号在 Qt 5.15 才引入，旧 SDK 上不存在。
    too_new = [
        (r'\bQAbstractSocket::errorOccurred\b',
         "QAbstractSocket::errorOccurred 需要 Qt 5.15+，"
         "旧写法 QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error)"),
        (r'\bQTcpSocket::errorOccurred\b',
         "QTcpSocket::errorOccurred 需要 Qt 5.15+，请改用 QOverload 取 error 重载"),
        (r'\bQNetworkReply::errorOccurred\b',
         "QNetworkReply::errorOccurred 需要 Qt 5.15+"),
        (r'\bQSerialPort::errorOccurred\b',
         "QSerialPort::errorOccurred 需要 Qt 5.15+，请改用 QOverload 取 error 重载"),
        (r'\bQProcess::errorOccurred\b',
         "QProcess::errorOccurred 需要 Qt 5.15+"),
        (r'\bQStringView\b', "QStringView 需要 Qt 5.10+，旧 SDK 可能没有"),
        (r'\bQMetaType::fromType\b', "QMetaType::fromType 需要 Qt 6"),
        (r'\bQAudioSink\b|\bQAudioSource\b', "QAudioSink/QAudioSource 是 Qt 6 API，Qt5 用 QAudioOutput/QAudioInput"),
        (r'\bQMediaPlayer::PlaybackState\b', "QMediaPlayer::PlaybackState 是 Qt 6 API，Qt5 用 QMediaPlayer::State"),
        # Qt6 用 setSource(QUrl)，Qt5 是 setMedia(QMediaContent)；
        # 实际写法通常是 player->setSource(...)，故匹配成员调用形式。
        (r'(?:->|\.)setSource\s*\(', "QMediaPlayer::setSource 是 Qt 6 API，Qt5 用 setMedia"),
        (r'\bQLibraryInfo::path\b', "QLibraryInfo::path 是 Qt 6 API，Qt5 用 QLibraryInfo::location"),
        (r'\bQColorSpace\b', "QColorSpace 需要 Qt 5.14+"),
    ]
    # C++14/17 特性：本工程为 C++11
    cpp_too_new = [
        (r'^\s*inline\s+(?:constexpr|const)\s', "C++17 inline 变量"),
        (r'\bif\s+constexpr\b', "C++17 if constexpr"),
        # 结构化绑定形如 auto [a, b] = ...；必须带声明关键字，
        # 否则会把 lambda 捕获列表 [this, conn](...) 误判。
        (r'\b(?:const\s+)?auto\s*(?:&|&&)?\s*\[[^\]\n]+\]\s*[=(]',
         "C++17 结构化绑定"),
        (r'\bstd::optional\b', "C++17 std::optional"),
        (r'\bstd::string_view\b', "C++17 std::string_view"),
        (r'\bstd::make_unique\b', "C++14 std::make_unique"),
        (r'\bstd::filesystem\b', "C++17 std::filesystem"),
    ]

    for path in find_files(src_root, {".cpp", ".h"}):
        raw = read(path)
        text = strip_comments_and_strings(raw)
        rel = os.path.relpath(path, root)
        for pat, desc in too_new:
            if re.search(pat, text):
                fail(f"[qt5] {rel} 使用了过高版本的 Qt API: {desc}")
        for pat, desc in cpp_too_new:
            if re.search(pat, text, re.M):
                fail(f"[cxx11] {rel} 使用了 {desc}，本工程为 C++11")


def check_forward_declarations(root, src_root):
    """
    前置声明一致性检查。

    背景（真实缺陷）：重构后 widget.h 曾出现
        class VideoPushThread;
        class CameraPreviewServer;
    但工程里实际定义的类型是 CameraPushThread 与 CameraUdpServer。
    前置声明本身不报错，直到 widget.cpp 写
        m_cameraPushThread = new VideoPushThread(...);
    才报 "invalid use of incomplete type"，而且报错位置离根因很远，
    极易误判为缺少 include。

    做法：收集本工程所有 "class X;" 形式的前置声明，若 X 既不在本工程
    的类/结构体定义中，也不是已知的 Qt/标准库类型，则告警。
    仅检查本工程自有的类，Qt 类型（QTimer 等）不参与。
    """
    candidates = {}
    for path in find_files(src_root, {".h", ".cpp"}):
        raw = read(path)
        text = strip_comments_and_strings(raw)
        for m in re.finditer(r'^\s*class\s+([A-Za-z_]\w*)\s*;', text, re.M):
            name = m.group(1)
            # 跳过程序内定义的类
            if re.search(r'\b(?:class|struct)\s+' + re.escape(name) +
                         r'\b[^;{]*\{', text):
                continue
            candidates.setdefault(name, []).append(os.path.relpath(path, root))

    # 全局收集本工程实际定义的类型名
    defined = set()
    for path in find_files(src_root, {".h", ".cpp"}):
        text = strip_comments_and_strings(read(path))
        for m in re.finditer(r'\b(?:class|struct)\s+([A-Za-z_]\w*)\b\s*(?::[^;{]*)?\{',
                             text):
            defined.add(m.group(1))

    for name, where in sorted(candidates.items()):
        if name in defined:
            continue
        # Qt / 标准库类型不检查
        if name.startswith('Q') or name.startswith('Ui') or name in (
                'timeval', 'sockaddr', 'sockaddr_in', 'gpiod_chip',
                'gpiod_line', 'snd_pcm_t', 'snd_mixer_t', 'v4l2_buffer'):
            continue
        fail("[fwd] {} 前置声明了未在本工程定义的类型 '{}'；"
             "若这是一个真实使用的成员类型，编译期会报 incomplete type"
             .format(', '.join(sorted(set(where))), name))


def check_json_configs(root):
    """配置文件必须是合法 JSON，且字段类型正确。"""
    import json
    for name in ("config/appconfig.json",):
        path = os.path.join(root, name.replace("/", os.sep))
        if not os.path.isfile(path):
            fail(f"[json] 缺少配置文件: {name}")
            continue
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
        except json.JSONDecodeError as e:
            fail(f"[json] {name} 解析失败: {e}")
            continue
        # 关键字段的类型校验
        checks = [
            (("network", "server_ip"), str),
            (("network", "self_ip"), str),
            (("hardware", "door_gpio_line"), int),
            (("resources", "pcm_dir"), str),
            (("behavior", "frame_interval_ms"), int),
            (("security", "door_password"), str),
        ]
        for keys, typ in checks:
            node = data
            ok = True
            for k in keys:
                if not isinstance(node, dict) or k not in node:
                    ok = False
                    break
                node = node[k]
            if not ok:
                fail(f"[json] {name} 缺少字段: {'.'.join(keys)}")
            elif not isinstance(node, typ):
                fail(f"[json] {name} 字段 {'.'.join(keys)} 类型应为 {typ.__name__}，"
                     f"实为 {type(node).__name__}")


def check_deferred_session_guard(root, src_root):
    """
    延时任务必须保护“通话会话”有效性。

    背景（真实缺陷，曾存在于 baseline 与本工程）：
    widget.cpp 中音频播放线程是延迟 400ms 创建的，用于错开声卡争用：

        QTimer::singleShot(400, this, [this]() {
            m_audioReceiverThread = new AudioReceiverThread(m_audioSocketFd, this);
            ...
        });

    但 hangUpCall() 会 ::close(m_audioSocketFd) 并把它置为 -1。
    若在这 400ms 内通话被挂断（远端 recstop / 本地挂断 / 采集线程报
    connectionLost），这个已排队的 lambda 仍会执行，于是
    AudioReceiverThread 拿到 fd = -1；其 run() 随后对该 fd 调用
    fcntl()、FD_SET()，属于未定义行为。

    做法：找到 QTimer::singleShot(...) 的调用，检查其 lambda 体内是否
    引用了通话期资源（m_audioSocketFd / m_audioSenderThread /
    m_audioReceiverThread / m_cameraPushThread），若引用了，则要求
    同一 lambda 内出现会话守卫（m_callActive 或 m_callGeneration）。
    """
    # 这些成员只在通话期间有效，且会被 hangUpCall() 销毁或置为无效。
    guarded_members = (
        "m_audioSocketFd",
        "m_audioSenderThread",
        "m_audioReceiverThread",
        "m_cameraPushThread",
    )
    guard_tokens = ("m_callActive", "m_callGeneration")

    for path in find_files(src_root, {".cpp"}):
        raw = read(path)
        text = strip_comments_and_strings(raw)
        rel = os.path.relpath(path, root)

        # 定位每个 QTimer::singleShot( 的起始位置
        for m in re.finditer(r'QTimer\s*::\s*singleShot\s*\(', text):
            start = m.end() - 1                      # 指向 '('
            depth = 0
            i = start
            # 括号配平，取出整个调用表达式
            while i < len(text):
                ch = text[i]
                if ch == '(':
                    depth += 1
                elif ch == ')':
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            if depth != 0:
                continue
            call_text = text[start:i + 1]

            used = [name for name in guarded_members if name in call_text]
            if not used:
                continue
            if any(tok in call_text for tok in guard_tokens):
                continue

            line_no = text.count("\n", 0, m.start()) + 1
            fail("[deferred] {}:{} 延时任务引用了通话期资源 {}，"
                 "但未检查会话有效性（m_callActive / m_callGeneration）；"
                 "通话若在此之前挂断，这些资源已被关闭/销毁"
                 .format(rel, line_no, "/".join(used)))


def check_accept_gated_threads(root, src_root):
    """
    “音视频线程仅在 accept 之后创建”这一硬约束的结构化守卫。

    背景：这是跨设备契约里最容易被重构破坏、又最难靠运行测试发现的一条。
    空闲时零资源占用（不建线程、不占声卡与摄像头）是门口机的核心设计，
    一旦有人把 new CameraPushThread / new AudioSenderThread /
    new AudioReceiverThread 挪到别处（例如构造函数里“预建”），
    功能上可能仍然正常，但空闲功耗与资源占用立刻退化，且没有测试会报警。

    做法：
      1. 定位唯一允许创建这些线程的函数 startCallSession()，取函数体；
      2. 检查这三个线程的 new 表达式确实都在该函数体内；
      3. 检查 startCallSession() 只在 Signal::Accept 分支被调用。
    """
    widget_cpp = os.path.join(src_root, "ui", "widget.cpp")
    if not os.path.isfile(widget_cpp):
        return  # 该项目结构不同，跳过

    raw = read(widget_cpp)
    text = strip_comments_and_strings(raw)
    rel = os.path.relpath(widget_cpp, root)

    # ---- 1. 取出 startCallSession() 的函数体 ----
    m = re.search(r'\bWidget\s*::\s*startCallSession\s*\([^)]*\)\s*\{', text)
    if not m:
        fail("[gating] {} 找不到 Widget::startCallSession() 的定义；"
             "该函数是音视频线程的唯一创建点，不允许改名或删除".format(rel))
        return

    # 从 '{' 开始做花括号配平，得到函数体范围
    start = text.index('{', m.end() - 1)
    depth = 0
    i = start
    while i < len(text):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                break
        i += 1
    if depth != 0:
        fail("[gating] {} startCallSession() 花括号不配平，无法分析".format(rel))
        return
    body = text[start:i + 1]
    body_line0 = text.count("\n", 0, start)

    # ---- 2. 三个线程的 new 必须都在函数体内 ----
    for cls in ("CameraPushThread", "AudioSenderThread", "AudioReceiverThread"):
        # 允许的创建写法：new CameraPushThread(...)
        in_body = re.search(r'\bnew\s+' + cls + r'\b', body) is not None
        # 全文里的创建位置（排除头文件里的成员声明）
        all_sites = [mm.start() for mm in
                     re.finditer(r'\bnew\s+' + cls + r'\b', text)]
        if not in_body:
            line_no = text.count("\n", 0, all_sites[0]) + 1 if all_sites else 0
            where = "{}:{}".format(rel, line_no) if all_sites else "未找到创建点"
            fail("[gating] {} 创建了 {} 却不在 startCallSession() 内（{}）；"
                 "音视频线程只能在收到 accept 后创建，否则空闲时会有资源占用"
                 .format(rel, cls, where))
        # 函数体之外的创建点（若存在）同样有问题
        for pos in all_sites:
            if not (start <= pos <= i):
                fail("[gating] {}:{} {} 的创建点位于 startCallSession() 之外；"
                     "这破坏了“accept 之前不建流”的约定"
                     .format(rel, text.count("\n", 0, pos) + 1, cls))

    # ---- 3. startCallSession() 只能由 Signal::Accept 触发 ----
    call_sites = [mm.start() for mm in
                  re.finditer(r'\bstartCallSession\s*\(\s*\)', text)]
    # 去掉函数定义自身与头文件中的声明（本文件内定义处已排除）
    call_sites = [p for p in call_sites
                  if not (m.start() <= p <= m.end())]
    for pos in call_sites:
        line_no = text.count("\n", 0, pos) + 1
        # 往上看 6 行，应能看到 Accept 分支标记
        head = text[:pos]
        ctx_start = max(0, pos - 400)
        ctx = text[ctx_start:pos]
        if "Signal::Accept" not in ctx:
            fail("[gating] {}:{} 调用了 startCallSession()，但邻近上下文没有 "
                 "Signal::Accept 判断；该函数只允许在收到 accept 信令时调用"
                 .format(rel, line_no))
    if not call_sites:
        fail("[gating] {} 没有任何地方调用 startCallSession()；"
             "accept 将无法建立通话".format(rel))
    _ = body_line0


def main():
    if len(sys.argv) < 2:
        print("用法: python verify.py <项目根目录>")
        return 2
    root = os.path.abspath(sys.argv[1])
    src_root = os.path.join(root, "src")
    if not os.path.isdir(src_root):
        print(f"找不到 src 目录: {src_root}")
        return 2

    check_includes(root, src_root)
    check_build_lists(root, src_root)
    check_qrc(root, src_root)
    check_ui_objectnames(root, src_root)
    check_balance(root, src_root)
    check_legacy_identifiers(root, src_root)
    check_qt5_portability(root, src_root)
    check_forward_declarations(root, src_root)
    check_deferred_session_guard(root, src_root)
    check_accept_gated_threads(root, src_root)
    check_json_configs(root)

    print(f"校验目录: {root}")
    print("=" * 70)
    if WARN:
        print(f"\n警告 {len(WARN)} 项：")
        for w in WARN:
            print("  ! " + w)
    if FAIL:
        print(f"\n失败 {len(FAIL)} 项：")
        for f in FAIL:
            print("  X " + f)
        print(f"\n结论: 存在 {len(FAIL)} 项问题")
        return 1
    print("\n结论: 全部检查通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())

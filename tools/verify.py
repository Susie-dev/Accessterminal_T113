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


def check_subproject_cmake_is_selfcontained(root, src_root):
    """
    可独立构建的子工程 CMakeLists.txt（tests/、tests/interop/ 等）必须自带
    `cmake_minimum_required` / `project()`，并有 `enable_testing()` 与
    `CMAKE_AUTOMOC`（当测试文件 `#include "*.moc"` 时）。

    背景（真实缺陷，本轮修复）：
    `tests/CMakeLists.txt` 的文件头一直写着可以独立构建：

        cmake -S tests -B /tmp/t113-tests && cmake --build ... && ctest ...

    但它只有 `find_package` + `add_executable` + `add_test`，这三样原本由主
    CMakeLists.txt 提供。于是独立构建实际上**从来没有成功过**，且失败方式
    很有迷惑性：

      1. 缺 cmake_minimum_required -> 配置阶段直接失败
         "No cmake_minimum_required command is present"
      2. 缺 project() -> 同上（CMake 要求脚本模式之外必须有 project）
      3. 缺 enable_testing() -> **配置和构建都成功**，但 ctest 报
         "No tests were found!!!" —— 看起来构建通过了，实际一个用例都没跑
      4. 缺 AUTOMOC -> 测试文件末尾 `#include "test_xxx.moc"` 找不到

    第 3 种尤其危险：CI 里若只看构建退出码，会以为测试通过了。
    而"独立构建"恰恰是没有 ALSA/libgpiod 的开发机上唯一能跑单元测试的用法
    （主工程根 CMakeLists 需要 ALSA，开发机通常没有）。

    判定：子目录里的 CMakeLists.txt（排除顶层那一个）若含 add_test/add_executable
    却没有 cmake_minimum_required 或 project -> 报错；
    若含 add_test 却没有 enable_testing -> 报错；
    若同目录下的 .cpp 里出现 `#include "*.moc"` 却没有 AUTOMOC -> 报错。
    """
    top = os.path.join(root, "CMakeLists.txt")
    for dp, dns, fns in os.walk(root):
        if ".git" in dp.split(os.sep) or "build" in dp.split(os.sep):
            continue
        if "CMakeLists.txt" not in fns:
            continue
        p = os.path.join(dp, "CMakeLists.txt")
        if os.path.normpath(p) == os.path.normpath(top):
            continue  # 顶层工程由主 CMakeLists 负责
        rel = os.path.relpath(p, root)
        text = read(p)
        if "add_test" not in text and "add_executable" not in text:
            continue

        if not re.search(r"^\s*cmake_minimum_required\s*\(", text, re.M):
            fail("[cmake-selfcontained] {} 含 add_executable/add_test 却没有 "
                 "cmake_minimum_required()：独立执行 `cmake -S {}` 会直接报 "
                 "\"No cmake_minimum_required command is present\" 而无法配置。"
                 .format(rel, os.path.dirname(rel) or "."))
        if not re.search(r"^\s*project\s*\(", text, re.M):
            fail("[cmake-selfcontained] {} 缺少 project()：独立构建时 CMake 要求"
                 "顶层脚本必须有 project()，否则配置失败。".format(rel))
        if "add_test" in text and not re.search(
                r"^\s*enable_testing\s*\(", text, re.M):
            fail("[cmake-selfcontained] {} 注册了 add_test 却没有 "
                 "enable_testing()：独立构建会**配置成功、构建成功**，但 ctest "
                 "报 \"No tests were found!!!\" —— 一个用例都不会执行，"
                 "只看构建退出码会误判为通过。".format(rel))
        # 同目录 .cpp 里有 #include "*.moc" 就必须开 AUTOMOC
        has_moc_include = False
        for f in fns:
            if not f.endswith(".cpp"):
                continue
            if re.search(r'#include\s+"[\w]+\.moc"', read(os.path.join(dp, f))):
                has_moc_include = True
                break
        if has_moc_include and not re.search(r"AUTOMOC\s+ON", text):
            fail("[cmake-selfcontained] {} 同目录的测试文件里有 "
                 "#include \"*.moc\"，却没有 set(CMAKE_AUTOMOC ON)：会报 "
                 "fatal error: test_xxx.moc: No such file or directory。"
                 .format(rel))


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
            raw = read(cpp)
            if f"ui_{stem}.h" not in raw and os.path.splitext(os.path.basename(cpp))[0] != stem:
                continue
            # 仅在确实使用 Ui::<Stem> 时校验
            if f"Ui::{stem}" not in raw and "setupUi" not in raw:
                continue
            # 只看真实代码：注释/字符串里的 `ui->xxx`（例如说明文字、被注释掉的
            # 旧代码）不是对控件的引用，否则会产生误报。
            text = strip_comments_and_strings(raw)
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

    注意：被删除的片段若跨多行，必须**保留其中的换行符**，否则删除后的文本
    行号会整体前移，所有基于 `text.count("\\n", 0, pos)` 的报错行号都会指向
    错误位置。这里对块注释保留换行、其余字符换成空格；对字符串/字符字面量
    保留首尾定界符与换行，仅清空内容（与既有行为一致）。
    """
    def blank_keep_newlines(m):
        return re.sub(r"[^\n]", " ", m.group(0))

    def empty_string_keep_newlines(m):
        s = m.group(0)
        # 保留首尾定界符与内部换行，其余内容清空（与把字符串替换为 "" 等效）。
        return s[0] + re.sub(r"[^\n]", "", s[1:-1]) + s[-1]

    text = re.sub(r"/\*.*?\*/", blank_keep_newlines, text, flags=re.S)
    text = re.sub(r'"(?:\\.|[^"\\])*"', empty_string_keep_newlines, text)
    text = re.sub(r"'(?:\\.|[^'\\])*'", empty_string_keep_newlines, text)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def split_top_level_args(call):
    """
    按**顶层逗号**切分一个调用/参数列表文本，返回实参列表。

    为什么需要它：`connect()` 的实参里经常出现 `->` 与 `>`，
    `ui->addface, &QPushButton::clicked, this, &Cls::S`。

    一个常见的错误写法是把 `>` 也当成"需要配对的括号"：

        if ch in "(<[": depth += 1
        elif ch in ")>]": depth -= 1        # 错！`->` 里的 `>` 会误减

    这样每遇到一个 `->`，depth 就偏负一次，于是**顶层逗号再也分不出来**，
    整个实参列表被当成 1 个实参。后果是静默的：按索引取第 2/第 4 个实参的
    检查会因为 `len(args) < 2` 而直接 `continue`，**跳过所有首实参含 `->`
    的 connect**（例如 `connect(ui->addface, ...)`），既不报错也不提示。

    这里只让真正的 `(`/`)`、`[`/`]`、`{`/`}` 参与配对，`<`/`>` 仅在
    **成对尖括号模板**的常见形态下计数（用于 `QVector<int>` 这类逗号），
    并且要求 `<` 前是标识符、`>` 后不是 `>`（排除 `->` 与 `>>`）。
    """
    args, depth, cur = [], 0, ""
    i, n = 0, len(call)
    while i < n:
        ch = call[i]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == "<":
            # 仅把 `标识符<` 视作模板起始；`a < b` 会同时匹配，但这类比较在
            # connect 实参里几乎不出现，且退化为不计数也只是保守（少切分）。
            if i > 0 and (call[i - 1].isalnum() or call[i - 1] == "_"):
                depth += 1
        elif ch == ">":
            # `->` 与 `>>` 不参与配对
            if i > 0 and call[i - 1] == "-":
                pass
            elif i + 1 < n and call[i + 1] == ">":
                pass
            else:
                depth -= 1
        if ch == "," and depth == 0:
            args.append(cur)
            cur = ""
        else:
            cur += ch
        i += 1
    if cur.strip():
        args.append(cur)
    return args


def enclosing_call_text(text, open_paren_idx):
    """给定左括号下标，返回配对右括号之前的完整调用文本。"""
    depth, i = 0, open_paren_idx
    while i < len(text):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                break
        i += 1
    return text[open_paren_idx + 1:i]


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

        # 数值型字段还要做**取值**校验：随包发布的示例配置本身不能带
        # 0/负数这类会引发除零或忙循环的值（取值出口的兜底见
        # check_config_value_sanity()，两者互补）。
        # 注意 bool 是 int 的子类，需显式排除，否则 true 会被当成合法整数。
        ranges = [
            (("behavior", "frame_interval_ms"), 1, 10000),
            (("behavior", "face_detect_interval"), 1, 100000),
            (("behavior", "face_recognize_interval_ms"), 0, 3600000),
            (("behavior", "fingerprint_id_start"), 0, 65535),
            (("hardware", "door_gpio_line"), 0, 1024),
        ]
        for keys, lo, hi in ranges:
            node = data
            ok = True
            for k in keys:
                if not isinstance(node, dict) or k not in node:
                    ok = False
                    break
                node = node[k]
            if not ok or isinstance(node, bool) or not isinstance(node, (int, float)):
                continue  # 缺失/类型错误已在上面的 checks 中报过
            if not (lo <= node <= hi):
                fail(f"[json] {name} 字段 {'.'.join(keys)} 取值 {node} "
                     f"越界，允许范围 [{lo}, {hi}]")


def check_config_value_sanity(root, src_root):
    """
    数值型配置的取值出口必须做范围兜底。

    背景（真实缺陷，重构引入）：
    重构前 widget.cpp 里是硬编码常量

        const int detect_interval = 10;   // 每10帧检测一次

    重构后改成了从配置读取：

        if (++detectCounter % cfg.faceDetectInterval() == 0 && ...)

    而配置是不可信输入：
      - 环境变量经 QString::toInt() 解析，**非法文本静默返回 0**，例如
        ACCESSTERMINAL_BEHAVIOR_FACE_DETECT_INTERVAL=abc → 0；
      - JSON 文件里也可以直接写 0 或负数。
    于是在 widget.cpp:814 处触发整数除零（Linux 上为 SIGFPE，进程直接崩溃）。
    同类风险还有：QTimer 周期为 0 导致忙循环、LBPH 阈值为负导致所有人脸
    都通过、指纹 ID 超过 2 字节线格式溢出。

    做法分两步：
      A) 找出“用配置取值做除数”的取模/除法表达式，确认其配置访问器
         在 app_config.h 里被 clampInt()/clampDouble() 包住；
      B) 反向确认：所有 int/double 型配置访问器都必须经过 clamp。

    这样既覆盖了已发现的真实缺陷，也防止将来新增配置字段时重蹈覆辙。
    """
    header = os.path.join(src_root, "config", "app_config.h")
    if not os.path.isfile(header):
        fail("[config-sanity] 找不到 src/config/app_config.h")
        return
    htext = strip_comments_and_strings(read(header))

    # ---- 收集所有数值型访问器及其是否有 clamp 保护 ----
    # 形如:  int faceDetectInterval() const { return clampInt(m_x, 1, 100); }
    #
    # 必须匹配 `return clampInt(` / `return clampDouble(` 这种**调用**形态，
    # 而不是裸的 "clampInt(" 子串：后者会把 `return static_cast<quint16>(
    # clampInt(...))` 之外的情形也放过，也可能被注释/无关文本误命中。
    getter_re = re.compile(
        r'\b(?:int|double|quint16|qint16|qint32|quint32|long|short)\s+'
        r'(\w+)\s*\(\s*\)\s*const\s*\{([^{}]*)\}')
    clamp_call_re = re.compile(r'\breturn\b[^;]*\bclamp(?:Int|Double)\s*\(')
    numeric_getters = {}
    for name, body in getter_re.findall(htext):
        numeric_getters[name] = bool(clamp_call_re.search(body))

    if not numeric_getters:
        fail("[config-sanity] 未在 app_config.h 中解析到任何数值型访问器"
             "（检查器已失效，请更新正则）")
        return

    unprotected = sorted(n for n, ok in numeric_getters.items() if not ok)
    if unprotected:
        fail("[config-sanity] 这些数值型配置访问器未做范围兜底: "
             + ", ".join(unprotected)
             + "（配置值可为 0/负数，用作除数会 SIGFPE）")

    # ---- A) 用配置取值做除数的地方 ----
    # 匹配 ... % cfg.xxx()  或  ... / cfg.xxx()  或 % AppConfig::instance().xxx()
    divisor_re = re.compile(
        r'[%\/]\s*(?:cfg|AppConfig::instance\(\))\s*\.\s*(\w+)\s*\(\s*\)')

    for path in find_files(src_root, {".cpp", ".h"}):
        text = strip_comments_and_strings(read(path))
        for m in divisor_re.finditer(text):
            getter = m.group(1)
            line = text.count("\n", 0, m.start()) + 1
            rel = os.path.relpath(path, root)
            if getter not in numeric_getters:
                # 可能不是数值型访问器（例如 pcmFile()），只提示不判失败
                warn(f"[config-sanity] {rel}:{line} 用 cfg.{getter}() 做除数，"
                     f"但未在 app_config.h 找到数值型访问器，请人工确认")
                continue
            if not numeric_getters[getter]:
                fail(f"[config-sanity] {rel}:{line} 用未兜底的配置值 "
                     f"cfg.{getter}() 做除数，配置为 0 时会除零崩溃")


def check_config_knobs_are_used(root, src_root):
    """
    配置里公开的每个开关都必须真的生效，否则它就是「假旋钮」。

    背景（真实缺陷，重构引入）：
    config/appconfig.json 提供了 behavior.face_confidence_threshold，
    app_config.h 也提供了 faceConfidenceThreshold() 访问器，但
    FaceManager::recognize() 里仍然是重构前的硬编码：

        bool success = (predictedLabel == 1 && confidence < 90);

    即：现场把该配置从 90 调到 60，行为毫无变化 —— 配置被读取、被文档化，
    却从不生效。这类缺陷不会崩溃、不会告警，只会让「调参」永远无效，
    是最难被发现的一类重构退化。

    判据（要比「访问器有没有人调」更准确）：
    配置值的流向是  JSON/环境变量 -> m_xxx -> 使用者。因此一个开关
    「生效」的充要条件是：要么访问器 `xxx()` 在 app_config.* 之外被调用，
    要么它背后的成员 `m_xxx` 在加载代码之外被**读取**过。

    只看访问器调用点会误判：例如 pcmDir() 无人调用，但 pcmFile() 内部
    直接读了 m_pcmDir，该配置其实完全有效。故本检查两者都算。

    输出分两级：
      - 成员从未被读取  -> fail（真·假旋钮，配置改了没用）
      - 成员有读取但访问器无人调用 -> warn（冗余访问器，可清理）

    例外：ALLOWED_DEAD 里放「故意不读」的项，必须写明理由。
    """
    # 故意不读的访问器及理由。新增条目必须附理由，否则就是遗漏。
    ALLOWED_DEAD = {
        # 仅用于日志/排查，main.cpp 输出一次后即完成使命（desktop 侧使用）。
        "loadedFromFile",
    }
    # 生命周期/单例/工厂方法不是配置项，不参与本检查。
    NOT_KNOBS = {"instance", "load", "configPath", "AppConfig", "operator"}

    header = os.path.join(src_root, "config", "app_config.h")
    impl = os.path.join(src_root, "config", "app_config.cpp")
    if not os.path.isfile(header) or not os.path.isfile(impl):
        fail("[config-knobs] 找不到 src/config/app_config.{h,cpp}")
        return

    htext = strip_comments_and_strings(read(header))
    ctext = strip_comments_and_strings(read(impl))

    # 无参 const 访问器及其函数体：  QString serverIp() const { return m_serverIp; }
    getter_re = re.compile(
        r'^\s*(?:const\s+)?[\w:<>]+(?:\s*[*&])?\s+(\w+)\s*\(\s*\)\s*const\s*'
        r'([;{])', re.M)
    knobs = []          # (accessor_name, member_name or None)
    for m in getter_re.finditer(htext):
        name, brace = m.group(1), m.group(2)
        if name in NOT_KNOBS or name in ALLOWED_DEAD:
            continue
        member = None
        if brace == "{":
            # 取到匹配的右花括号，得到内联函数体
            depth, i = 0, m.end() - 1
            while i < len(htext):
                if htext[i] == "{":
                    depth += 1
                elif htext[i] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            body = htext[m.end() - 1:i + 1]
            found = re.findall(r'\b(m_\w+)\b', body)
            if len(set(found)) == 1:
                member = found[0]
        knobs.append((name, member))

    if not knobs:
        fail("[config-knobs] 未在 app_config.h 中解析到任何无参访问器"
             "（检查器已失效，请更新正则）")
        return

    # 收集 app_config.* 之外的访问器调用点
    called = set()
    for path in find_files(src_root, {".cpp", ".h"}):
        if os.path.basename(path) in ("app_config.h", "app_config.cpp"):
            continue
        body = strip_comments_and_strings(read(path))
        for name, _ in knobs:
            if re.search(r'\b' + name + r'\s*\(\s*\)', body):
                called.add(name)

    def strip_loading_code(text):
        """
        去掉 app_config.cpp 中的「加载代码」，只留下真正的消费者。

        为什么必须这么做：加载代码里也会**读**成员，但这些读并不代表配置
        生效。最典型的是把当前值当作回退实参传给 jsonInt/jsonDouble：

            m_faceConfidenceThreshold =
                jsonDouble(behavior, "face_confidence_threshold",
                           m_faceConfidenceThreshold);   // <- 这是读，但不是消费

        若不剔除，faceConfidenceThreshold 这种「假旋钮」就会被误判为生效
        （该缺陷最初就是这样漏掉的）。加载函数整体剔除后，剩下的
        m_xxx 读取才是真正的派生使用（例如 pcmFile() 读 m_pcmDir）。
        """
        # 需要剔除的加载入口。函数体按花括号配对截取。
        loading_fns = ("load", "applyDefaults", "applyEnvOverrides")
        out = text
        for fn in loading_fns:
            # 匹配 `void AppConfig::load(...)` 或 `bool AppConfig::load(...)`
            for m in re.finditer(
                    r'\b[\w:<>]+\s+AppConfig::' + fn + r'\s*\([^;{]*\)\s*\{',
                    out):
                depth, i = 0, m.end() - 1
                while i < len(out):
                    if out[i] == "{":
                        depth += 1
                    elif out[i] == "}":
                        depth -= 1
                        if depth == 0:
                            break
                    i += 1
                out = out[:m.start()] + "\n" * out[m.start():i + 1].count("\n") \
                    + out[i + 1:]
        return out

    consumed_text = strip_loading_code(ctext)

    def member_is_read(member):
        """成员是否在 app_config.cpp 的**加载代码之外**被读取（真正的消费）。"""
        t = consumed_text
        # 赋值语句 m_xxx = ...（排除 ==）：写入不算消费
        t = re.sub(r'\b' + member + r'\s*=(?!=)', '', t)
        return re.search(r'\b' + member + r'\b', t) is not None

    fake, redundant = [], []
    for name, member in knobs:
        if name in called:
            continue
        if member is not None and member_is_read(member):
            redundant.append("%s()（配置经 %s 仍生效）" % (name, member))
        else:
            fake.append("%s()" % name if member is None
                        else "%s()/%s" % (name, member))

    if fake:
        fail("[config-knobs] 这些配置开关从未被读取，属于「假旋钮」"
             "（配置可读可调但永不生效）: " + ", ".join(sorted(fake))
             + "；若确为有意保留，请加入 check_config_knobs_are_used 的 "
               "ALLOWED_DEAD 并写明理由")
    if redundant:
        warn("[config-knobs] 访问器无人调用，但配置经成员变量仍然生效"
             "（冗余访问器，可考虑清理）: " + ", ".join(sorted(redundant)))


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


def check_response_index_bounds(root, src_root):
    """
    串口/协议应答在被按下标取值前，必须先校验长度。

    背景（真实缺陷，来自 baseline，本轮修复）：
    `HailinFingerprint::autoIdentify()` 只写了

        QByteArray resp = readPacket(timeoutMs);
        if (resp.isEmpty()) { ... return false; }
        quint8 confirm = (quint8)resp[9];
        ...
        quint16 score = ((quint8)resp[13] << 8) | (quint8)resp[14];   // 越界

    定帧的 `FingerprintFrameParser` 只保证「长度字段自洽」，最短合法帧是
    9(包头+地址+标识+长度) + 2(校验和) = **11 字节**。也就是说 resp 完全
    可能是 11~14 字节的合法帧，此时 resp[11..14] 越界。
    `QByteArray::operator[]` 越界是**未定义行为**（不像 QString 返回空引用），
    只判 `isEmpty()` 挡不住这种“短但合法”的帧。

    判定规则：对形如 `resp[K]` / `resp.at(K)` 的常量下标访问，若所在函数内
    没有任何 `resp.size()` 与 K+1 的比较（且 K 大于包头长度 6），即报错。

    包头占 0..5，下标 6（包标识）恒在最短帧内，故从 7 起才要求校验。
    """
    for path in find_files(src_root, {".cpp"}):
        text = strip_comments_and_strings(read(path))
        rel = os.path.relpath(path, root)
        if ".size()" not in text and "resp[" not in text:
            continue

        # 按函数切分：用「函数定义行」作为分界
        funcs = list(re.finditer(
            r'^[\w:<>,\s\*&]+\b(\w+)\s*::\s*(\w+)\s*\([^;]*?\)\s*(?:const\s*)?\{',
            text, re.M))
        for fi, fm in enumerate(funcs):
            body_start = fm.end() - 1
            # 花括号配平取函数体
            depth = 0
            i = body_start
            while i < len(text):
                if text[i] == '{':
                    depth += 1
                elif text[i] == '}':
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            body = text[body_start:i + 1]
            body_line0 = text.count("\n", 0, body_start)

            # 该函数体内所有「<name>[K]」形式的常量下标访问
            max_need = {}
            for m in re.finditer(r'\b(\w+)\s*\[\s*(\d{1,4})\s*\]', body):
                var, idx = m.group(1), int(m.group(2))
                if idx <= 6:
                    continue  # 包头范围内，恒安全
                if var not in ("resp", "resp2", "resp3", "resp4", "buffer",
                               "datagram", "pkt", "frame"):
                    continue
                if idx > max_need.get(var, -1):
                    max_need[var] = idx
            if not max_need:
                continue

            for var, idx in sorted(max_need.items()):
                # 该函数内是否有 var.size() 与 (idx+1) 的比较
                need = idx + 1
                pat = (re.escape(var) + r'\s*\.\s*size\s*\(\s*\)\s*[<>]=?\s*(\d+)')
                ok = False
                for gm in re.finditer(pat, body):
                    bound = int(gm.group(1))
                    # 只接受「长度 guard 足够」的写法：
                    #   size() < need   /  size() <= need-1  /  size() >= need
                    snip = gm.group(0)
                    if '<=' in snip and bound >= need - 1:
                        ok = True
                    elif '<' in snip and not snip.strip().startswith(('>',)):
                        if bound >= need:
                            ok = True
                    elif '>=' in snip and bound >= need:
                        ok = True
                if not ok:
                    fail("[resp-bounds] {}:{} 函数内读取 {}[{}]（需至少 {} 字节），"
                         "但未见 {}.size() 与 {} 的长度校验；"
                         "QByteArray 越界是未定义行为，必须显式判断。"
                         .format(rel, body_line0 + 1, var, idx, need, var, need))


def check_unqueued_poll_timers(root, src_root):
    """
    轮询定时器不得以「无句柄」的局部变量创建。

    背景（真实缺陷，来自 baseline，已修复）：Form_finger::startFingerRegisterStep()
    里写的是

        QTimer* timer = new QTimer(this);      // 局部变量，没有任何成员保存它
        connect(timer, &QTimer::timeout, this, [=]{ ... });
        timer->start(500);

    这个轮询回调里做的是**阻塞串口读**且跑在 GUI 线程。只有“检测到手指”
    那一条路径会 stop 它；用户点『退出』（onAddQuitClicked 只调 close()）时
    定时器仍在跑。由于 Form_finger 没有设 WA_DeleteOnClose，close() 只是隐藏
    窗口、对象还活着，于是：每 500ms 在 GUI 线程上阻塞一次，界面永久卡顿。

    判定规则只针对「可被 close() 隐藏、但对象仍存活」的窗口类：
      - 该类必须继承 QWidget，并且代码里出现 close() 调用；
      - 该 QTimer 在创建后调用了 start(...)（成为周期定时器）；
      - 其句柄既不是类成员，也没有放进任何容器。
    此时除回调自身外无人能停止它，而窗口已经隐藏 —— 定时器会永久空转。

    为什么把范围收窄到 QWidget：像 FingerprintManager 这样随程序存活的
    QObject，其“等待手指”轮询本来就是设计上的常驻行为（识别流程结束时
    才 stop），并非缺陷。只有「承载它的界面可能先消失」时才构成问题。
    单次触发（singleShot / setSingleShot(true)）也会自己结束，不在范围内。

    注意：`check_blocking_poll_has_handle()` 覆盖了本规则的另一半 ——
    非 QWidget 的常驻轮询器（如 FingerprintManager）即使不会“空转拖死界面”，
    也必须持有句柄，否则无法在注册流程期间让路（串口互斥）。
    """
    for path in find_files(src_root, {".cpp"}):
        text = strip_comments_and_strings(read(path))
        rel = os.path.relpath(path, root)
        if "new QTimer" not in text:
            continue

        header = path[:-4] + ".h"
        if not os.path.isfile(header):
            continue
        htext = strip_comments_and_strings(read(header))

        # 只处理 QWidget 子类，且该文件里确实会 close()（说明能被隐藏而非销毁）
        if not re.search(r'class\s+\w+\s*:\s*public\s+QWidget\b', htext):
            continue
        if not re.search(r'\bclose\s*\(\s*\)', text):
            continue

        # 头文件里的成员变量名
        members = set(re.findall(r'\b(m_\w+)\s*(?:=|;)', htext))

        for m in re.finditer(r'\bQTimer\s*\*\s*(\w+)\s*=\s*new\s+QTimer\b', text):
            var = m.group(1)
            # 只关心“使能了周期性启动”的定时器
            if not re.search(re.escape(var) + r'\s*->\s*start\s*\(', text):
                continue
            if var in members:
                continue  # 有成员句柄，外部可停
            # 句柄是否被放进了某个容器（QList/QVector/append/<<）
            if re.search(r'\b\w+\s*(?:\.append|<<)\s*\(\s*' + re.escape(var), text):
                continue
            line = text.count("\n", 0, m.start()) + 1
            fail("[unguarded-timer] {}:{} 窗口类里的局部定时器 '{}' 被 start() 成"
                 "周期定时器却没有任何成员/容器保存句柄；除回调自身外无人能停止它。"
                 "该类会 close()（窗口被隐藏但对象仍存活，未设 WA_DeleteOnClose 时"
                 "尤其如此），定时器会在 GUI 线程上永久空转。"
                 "请改为成员变量并提供停止入口。"
                 .format(rel, line, var))


def check_blocking_poll_has_handle(root, src_root):
    """
    任何「周期性定时器 + 阻塞式读串口/读socket」的轮询器，其句柄必须是类成员。

    背景（真实缺陷，来自 baseline，本轮修复）：

        void FingerprintManager::startIdentify()
        {
            static bool waitingFinger = false;   // (1) 函数内 static
            if (waitingFinger) return;
            waitingFinger = true;
            QTimer* timer = new QTimer(this);    // (2) 局部变量，无成员句柄
            connect(timer, &QTimer::timeout, this, [=]() mutable {
                fingerprint->sendPacket(pkt);
                QByteArray resp = fingerprint->readPacket(500);  // (3) 阻塞读
                ...
            });
            timer->start(500);
        }

    为什么这是缺陷（两处独立问题）：

    (2) 句柄丢失 → 谁也停不下来。识别轮询与注册流程（Form_finger）读写的是
        **同一个串口**：

           识别轮询：  send(CMD_GET_IMAGE) → readPacket(500)   ← 抢应答
           注册流程：  send(CMD_GET_IMAGE) → readPacket(2000)  ← 抢应答

        两者互为对方之应答源。识别轮询一旦在跑，就会把注册流程正在等待的
        0x01/0x02/0x05/0x06 应答从串口缓冲里取走，注册流程拿到空字节数组，
        判为“采集图像失败”静默返回。反过来注册流程也会抢走识别的应答。
        要互斥就必须能**暂停识别轮询**，而暂停的前提是拿得到句柄。

    (3) 阻塞读在 GUI 线程 → 每 500ms 冻结界面 500ms（readPacket 的最坏耗时）。

    (1) 函数内 static 状态为所有实例共享，且不会随实例销毁而复位。

    判定规则：在 .cpp 中找到被 `start(数字)` 周期化的 QTimer，若其创建点
    捕获的变量不是类成员（m_xxx），且回调体内出现阻塞读调用
    （readPacket / waitForReadyRead / waitForBytesWritten / QSerialPort 阻塞读），
    则报错。要求改为成员句柄并提供停止入口。

    为什么需要这条规则（而不是靠 check_unqueued_poll_timers）：
    后者把范围收窄到「继承 QWidget 且会 close()」的类，理由是常驻 QObject
    的轮询属设计常态。但对**共用硬件资源**的常驻轮询器，句柄仍然是必需的
    ——不是为了防止空转，而是为了能与其他使用者互斥。两者互补。
    """
    blocking = (
        "readPacket", "waitForReadyRead", "waitForBytesWritten",
        "waitForBytesWritten", "waitForDisconnected", "waitForConnected",
    )
    for path in find_files(src_root, {".cpp"}):
        text = strip_comments_and_strings(read(path))
        rel = os.path.relpath(path, root)
        if "new QTimer" not in text:
            continue

        header = path[:-4] + ".h"
        if not os.path.isfile(header):
            continue
        htext = strip_comments_and_strings(read(header))
        members = set(re.findall(r'\b(m_\w+)\s*(?:=|;|\))', htext))

        for m in re.finditer(r'\bQTimer\s*\*\s*(\w+)\s*=\s*new\s+QTimer\b', text):
            var = m.group(1)
            # 必须是周期定时器
            if not re.search(re.escape(var) + r'\s*->\s*start\s*\(', text):
                continue
            if var in members:
                continue  # 有成员句柄，可被外部暂停
            # 单次触发会自己结束，不构成“永久轮询”
            if re.search(re.escape(var) + r'\s*->\s*setSingleShot\s*\(\s*true\s*\)',
                         text):
                continue

            # 取出该定时器对应的回调体（连接点之后的一段），判断是否有阻塞读
            conn = re.search(re.escape(var) + r'\s*,\s*&QTimer::timeout', text)
            if not conn:
                continue
            body = text[conn.end():conn.end() + 4000]
            # 只看到该 lambda 结束为止
            depth = 0
            started = False
            end = len(body)
            for i, ch in enumerate(body):
                if ch == '{':
                    depth += 1
                    started = True
                elif ch == '}':
                    depth -= 1
                    if started and depth == 0:
                        end = i
                        break
            body = body[:end]
            if not any(b in body for b in blocking):
                continue

            line = text.count("\n", 0, m.start()) + 1
            fail("[poll-handle] {}:{} 周期性定时器 '{}' 的回调里有阻塞读（每轮会冻结"
                 "调用线程一次），却没有成员句柄保存它 → 外部无法暂停/停止。"
                 "若该轮询与其他流程共用同一设备（如串口），对方将与之互相抢读应答。"
                 "请改为成员变量并提供 setSuspended()/stop 入口。"
                 .format(rel, line, var))


def check_deleted_on_close_members(root, src_root):
    """
    WA_DeleteOnClose 窗口不得用裸指针成员跟踪。

    背景（真实缺陷，本轮由重构引入并修复）：on_btnpsswd_clicked() 创建
    Form_passwd 并设 Qt::WA_DeleteOnClose，然后把裸指针存进成员
    m_passwdForm。该窗口在用户点“确定”或直接关闭时会被 Qt 自动销毁，
    m_passwdForm 立刻成为悬垂指针；而 PasswordManager::passwordChecked
    的回调里还会 `if (m_passwdForm) m_passwdForm->close();` ——
    对已释放对象调用成员函数，use-after-free。

    这类缺陷静态上很好识别：凡是在某处 `setAttribute(Qt::WA_DeleteOnClose)`
    的对象，若其句柄被赋给「裸指针类型的成员」，就需要改造成 QPointer
    （对象销毁后自动变空）。

    注意 baseline 里这段恰好不触发：原代码写的是
        Form_passwd* passwdForm = new Form_passwd(this);
    声明了同名【局部变量】遮蔽成员，成员永远为 nullptr，所以不会悬垂，
    但代价是“密码正确后窗口不会关闭”。重构把局部改为成员时修掉了那个
    功能缺陷，却引入了悬垂——这正是本检查要防住的回归。
    """
    for path in find_files(src_root, {".cpp"}):
        text = strip_comments_and_strings(read(path))
        rel = os.path.relpath(path, root)
        if "WA_DeleteOnClose" not in text:
            continue

        # 收集「被设置了 WA_DeleteOnClose 的变量名」
        marked = set()
        for m in re.finditer(r'(\w+)\s*->\s*setAttribute\s*\(\s*'
                             r'(?:Qt::)?WA_DeleteOnClose', text):
            marked.add(m.group(1))
        # 形如 setAttribute(Qt::WA_DeleteOnClose) 的隐式 this 用法不涉及成员名

        if not marked:
            continue

        # 在头文件里找裸指针类型的成员声明
        header = path[:-4] + ".h"
        if not os.path.isfile(header):
            continue
        htext = strip_comments_and_strings(read(header))
        # 形如:  Form_passwd* m_passwdForm = nullptr;
        #        Form_passwd *m_passwdForm;
        raw_members = {}
        for m in re.finditer(r'^\s*(?:[\w:]+)\s*\*\s*(m_\w+)\s*(?:=[^;]*)?;',
                             htext, re.M):
            raw_members[m.group(1)] = m.group(0).strip()

        for var in marked:
            # 该变量是否被赋给了某个裸指针成员
            # 形如  m_xxx = var;   或   m_xxx = new ...（new 形式也查）
            for mm in re.finditer(r'\b(m_\w+)\s*=\s*' + re.escape(var) + r'\s*;',
                                  text):
                member = mm.group(1)
                if member in raw_members:
                    line = text.count("\n", 0, mm.start()) + 1
                    fail("[dtor-close] {}:{} 把设了 WA_DeleteOnClose 的 '{}' 存入"
                         "裸指针成员 '{}'（{}）；该窗口关闭时会被 Qt 自动销毁，"
                         "成员随即悬垂。请改用 QPointer<...>。"
                         .format(rel, line, var, member,
                                 raw_members[member]))


def check_datagram_read_returns(root, src_root):
    """
    所有 UDP 收包都必须检查 readDatagram() / recvfrom() 的返回值。

    背景（真实缺陷，本轮修复）：
    `CallSession::onAudioDataReady()` 与 `onVideoDataReady()` 原本写成

        QByteArray datagram;
        datagram.resize(socket->pendingDatagramSize());
        socket->readDatagram(datagram.data(), datagram.size(), ...);   // 返回值被丢弃

    两个后果：
      1) `QByteArray::resize()` **不清零**新分配的字节。若实际读到的字节数
         少于 `pendingDatagramSize()`，尾部就是未初始化内存（或上一轮循环的
         残留字节）。音频路径下它会被 append 后直接喂给声卡（噪声），
         视频路径下会交给 `loadFromData()` 解码（垃圾数据/解码器崩溃）。
      2) 返回 -1 表示读取失败，此时 `readDatagram` 不会填充
         sender/senderPort 出参。音频路径若不判就赋值，会把“回传目标地址”
         改成未初始化/上一次的地址，导致上行语音发错地方。

    `pendingDatagramSize()` 返回 qint64 而非 int，因此这里统一要求：
      - 抓到变量名，检查同函数内该变量有 `< 0` / `== -1` 之类的失败分支；
      - 若读取时用了出参（&sender / &port），失败必须 `continue`/`return`
        而不能继续使用出参。

    只做“返回值是否被接收并判负”的结构检查，不推断语义。
    """
    for path in find_files(src_root, {".cpp"}):
        text = strip_comments_and_strings(read(path))
        rel = os.path.relpath(path, root)

        # 逐个函数体检查
        for fm in re.finditer(
                r'^[\w:<>,\s\*&]+\b\w+\s*::\s*(\w+)\s*\([^;]*?\)\s*(?:const\s*)?\{',
                text, re.M):
            b0 = fm.end() - 1
            depth, i = 0, b0
            while i < len(text):
                if text[i] == '{':
                    depth += 1
                elif text[i] == '}':
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            body = text[b0:i + 1]
            line0 = text.count("\n", 0, b0) + 1

            for m in re.finditer(
                    r'(?:(?:const\s+)?\w+\s+(\w+)\s*=\s*)?'
                    r'(\w+(?:->|\.)readDatagram\s*\(|(?<!\w)recvfrom\s*\()',
                    body):
                assignee, call = m.group(1), m.group(2)
                call_line = line0 + body.count("\n", 0, m.start())
                fn_name = fm.group(1)
                if not assignee:
                    fail("[datagram-ret] {}:{} 函数 {}() 调用了 {} 但丢弃了返回值；"
                         "QByteArray::resize() 不清零，尾部可能是未初始化字节，"
                         "且失败(-1)时出参未被填充。必须接收并检查返回值。"
                         .format(rel, call_line, fn_name, call.strip()))
                    continue
                # 该赋值变量在后续是否被判负
                tail = body[m.end():]
                if not re.search(re.escape(assignee) + r'\s*(?:<\s*0|==\s*-1|!=\s*\w*\s*[!=])',
                                 tail):
                    fail("[datagram-ret] {}:{} 函数 {}() 接收了 {} 的返回值 '{}' "
                         "但未见 <0 / ==-1 之类的失败判断。"
                         .format(rel, call_line, fn_name, call.strip(), assignee))


def check_connect_signal_is_signal(root, src_root):
    """
    connect() 的第二个实参必须是**信号**。

    背景（真实缺陷，本轮修复）：
    `Form_finger::addFingerRequested()` 被声明在 `public slots:` 区，却被当成
    connect 的信号源使用：

        connect(m_fingerForm, &Form_finger::addFingerRequested,
                this, &Widget::onAddFingerFromFingerForm);   // 永远连不上

    Qt5 的 PMF 版 connect 会把第二个实参（成员指针）交给
    `QObject::connectImpl()`，后者用 `QMetaObject::IndexOfMethod()` 反查信号索引，
    而 moc **只为 `signals:` 区的成员生成信号索引**（槽/普通成员索引为 -1）。
    因此该 connect 在运行期失败——Qt 打印
    `QObject::connect: signal not found in Form_finger` 并返回 false，
    槽 `Widget::onAddFingerFromFingerForm()` 永远不会被调用，其业务逻辑是死代码。

    注意：`connect(btn, &QPushButton::clicked, this, &Cls::someSlot)` 是完全正常的
    （槽只需可调用，不必是信号）。所以本检查**只看第二个实参**：只有当
    `&Cls::name` 中的 Cls 是本工程某个含 Q_OBJECT 的类、且 name 在该类的
    **非 signals 区**被显式声明时，才判定为错误。

    只看结构，不做语义推断；继承自基类的信号不予判定。
    """
    sig_re = r'^\s*(?:(?:public|private|protected)\s+)?(?:signals|Q_SIGNALS)\s*:'
    slot_re = r'^\s*(?:(?:public|private|protected)\s+)?(?:slots|Q_SLOTS)\s*:'
    acc_re = r'^\s*(?:public|private|protected)\s*:'
    mem_re = r'^(?:[\w:<>,\*&\s]+?\s)?~?(\w+)\s*\('
    skip = ("if", "for", "while", "switch", "return", "emit", "connect", "catch")

    # 1) 收集所有含 Q_OBJECT 的类的成员分区
    classes = {}
    for hpath in find_files(src_root, {".h", ".hpp"}):
        htext = strip_comments_and_strings(read(hpath))
        for cm in re.finditer(r'\bclass\s+(\w+)\s*(?::[^{;]*)?\{', htext):
            cname = cm.group(1)
            start = cm.end() - 1
            depth, i = 0, start
            while i < len(htext):
                if htext[i] == "{":
                    depth += 1
                elif htext[i] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            body = htext[start + 1:i]
            if "Q_OBJECT" not in body:
                continue
            signals, others = set(), set()
            cur = others
            for line in body.split("\n"):
                if re.match(sig_re, line):
                    cur = signals
                    continue
                if re.match(slot_re, line) or re.match(acc_re, line):
                    cur = others
                    continue
                m = re.match(mem_re, line.strip())
                if m and m.group(1) not in skip:
                    cur.add(m.group(1))
            classes[cname] = (signals, others)

    if not classes:
        return

    # 2) 扫描 connect() 的第二个实参
    for path in find_files(src_root, {".cpp"}):
        text = strip_comments_and_strings(read(path))
        rel = os.path.relpath(path, root)
        for m in re.finditer(r'\bconnect\s*\(', text):
            args = split_top_level_args(
                enclosing_call_text(text, m.end() - 1))
            if len(args) < 2:
                continue

            mm = re.search(r'&\s*(\w+)\s*::\s*(\w+)', args[1])
            if not mm:
                continue
            cname, name = mm.group(1), mm.group(2)
            if cname not in classes:
                continue
            signals, others = classes[cname]
            if name in signals or name not in others:
                continue
            line = text.count("\n", 0, m.start()) + 1
            fail("[connect-sig] {}:{} connect() 的第二个实参 &{}::{}() 不是信号"
                 "（它声明在非 signals 区）；moc 不会为它生成信号索引，运行期 "
                 "connect 必然失败并打印 \"signal not found in {}\"，"
                 "对应槽是死代码。"
                 .format(rel, line, cname, name, cname))


def check_declared_signal_is_emitted(root, src_root):
    """
    声明在 `signals:` 下的信号必须真的会被发射。

    背景（真实缺陷，本轮修复）：
    `AudioSenderThread::connectionLost()` 被声明、被 Widget 连接、并且那段
    connect 的注释明确写着用途——

        // 采集线程报告连接丢失时立即挂断，避免通话卡死。
        connect(m_audioSenderThread, &AudioSenderThread::connectionLost,
                this, &Widget::hangUpCall);

    ——但**全工程从未 emit 过它**（基线的 audiosenderthread.cpp 同样只
    emit errorMsg/finishedMsg）。于是这条兜底是死的：sendto 失败后采集循环
    break、线程结束，再无任何帧发往室内机，通话已单向静音，而界面依旧显示
    “通话中”，用户只能手动挂断。

    判定一个信号“真的会触发”只有两种途径：
      1. 被显式发射：`emit S(...)` 或直接调用 `S(...)`；
      2. 作为 connect 的**第 4 个实参**出现 —— 这是 Qt 的信号→信号链式连接
         （`connect(btn, &QPushButton::clicked, this, &Cls::S)`），源信号
         触发时 Qt 会自动发射 S。漏掉这一条会把 `Form_finger::
         addFaceRequested` 误判为死信号。

    不判定经由 `QMetaObject::invokeMethod("name")` 触发的槽（本工程无此用法），
    也不判定字符串形式的 `SIGNAL()`（本工程已全部清除）。继承自基类的信号
    不在声明集合内，天然不参与判定。
    """
    sig_re = r'^\s*(?:(?:public|private|protected)\s+)?(?:signals|Q_SIGNALS)\s*:'
    slot_re = r'^\s*(?:(?:public|private|protected)\s+)?(?:slots|Q_SLOTS)\s*:'
    acc_re = r'^\s*(?:public|private|protected)\s*:'
    mem_re = r'^(?:[\w:<>,\*&\s]+?\s)?~?(\w+)\s*\('

    # 1) 收集每个含 Q_OBJECT 的类在 signals: 区声明的信号
    declared = {}
    for hpath in find_files(src_root, {".h", ".hpp"}):
        htext = strip_comments_and_strings(read(hpath))
        for cm in re.finditer(r'\bclass\s+(\w+)\s*(?::[^{;]*)?\{', htext):
            cname = cm.group(1)
            start = cm.end() - 1
            depth, i = 0, start
            while i < len(htext):
                if htext[i] == "{":
                    depth += 1
                elif htext[i] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            body = htext[start + 1:i]
            if "Q_OBJECT" not in body:
                continue
            cur = False
            for line in body.split("\n"):
                if re.match(sig_re, line):
                    cur = True
                    continue
                if re.match(slot_re, line) or re.match(acc_re, line):
                    cur = False
                    continue
                if cur:
                    m = re.match(mem_re, line.strip())
                    if m:
                        declared.setdefault(
                            (cname, m.group(1)), os.path.relpath(hpath, root))

    if not declared:
        return

    # 2) 收集 emit 目标、以及 connect 第 4 实参构成的链式目标
    emitted, chained = set(), set()
    for path in find_files(src_root, {".cpp"}):
        text = strip_comments_and_strings(read(path))
        for m in re.finditer(r'\bemit\s+(\w+)\s*\(', text):
            emitted.add(m.group(1))
        # 无 emit 关键字的直接调用（信号也是普通成员函数，可以直呼）
        for (cname, sig) in list(declared):
            if re.search(r'(?<![\w:.>&])' + re.escape(sig) + r'\s*\(', text):
                emitted.add(sig)

        for m in re.finditer(r'\bconnect\s*\(', text):
            args = split_top_level_args(
                enclosing_call_text(text, m.end() - 1))
            # 只需第 4 个实参（索引 3）作为链式发射目标
            if len(args) >= 4:
                m4 = re.search(r'&\s*(\w+)\s*::\s*(\w+)', args[3])
                if m4:
                    chained.add((m4.group(1), m4.group(2)))

    # 3) 判定
    for (cname, sig), rel in sorted(declared.items()):
        if sig in emitted or (cname, sig) in chained:
            continue
        fail("[dead-signal] {}: 信号 {}::{}() 声明在 signals: 区，但全工程既未 "
             "emit 它，也不是任何 connect 的链式发射目标 —— 它是一个永远不会"
             "触发的死信号。若有代码 connect 到它，那条连接与对应槽都是死代码"
             "（本工程曾因此丢掉 \"采集线程连接丢失即挂断\" 的兜底）。"
             .format(rel, cname, sig))


def check_no_running_reset_in_run(root, src_root):
    """
    `run()` 内不得重置停止标志（否则会丢掉先到的 stop() 请求）。

    背景（真实缺陷，本轮修复）：
    `CameraPushThread` 的停止标志初值为 false，并在 `run()` 开头无条件
    `running = true`：

        CameraPushThread::CameraPushThread(...) : ..., running(false) {}
        void CameraPushThread::run() { running = true; ... while (running) {...} }

    而调用方是“先清理旧的、再 new + start 新的”：

        m_cameraPushThread = new CameraPushThread(...);
        m_cameraPushThread->start();

    在 start() 到 run() 真正执行之间存在时间窗口。若期间调用 stop()
    （用户瞬时挂断，或紧接着再次清理），running 被置 false，随后 run()
    又把它写回 true，**停止请求被吞掉**。线程于是长时间推流，导致：
      - 挂断流程 `wait(3000)` 超时；
      - `delete` 一个仍在运行的 QThread → Qt 打印
        `QThread: Destroyed while thread is still running` 并终止进程；
      - 该线程继续向已关闭的 socket 发送数据。

    正确做法（同工程 AudioSenderThread / AudioReceiverThread 已是如此）：
    停止标志初值置 true、`run()` 内不再重置，使 stop() 成为不可逆操作。

    判定：对每个 `class X : public QThread`，若其 `stop()` 里出现
    `NAME = false`、且 `X::run()` 内出现 `NAME = true`，则失败。
    """
    for hpath in find_files(src_root, {".h", ".hpp"}):
        htext = strip_comments_and_strings(read(hpath))
        for cm in re.finditer(r"\bclass\s+(\w+)\s*:\s*public\s+QThread\b", htext):
            cname = cm.group(1)
            # 取类体，找停止标志成员名
            start = htext.find("{", cm.end())
            if start < 0:
                continue
            depth, i = 0, start
            while i < len(htext):
                if htext[i] == "{":
                    depth += 1
                elif htext[i] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            body = htext[start + 1:i]
            flags = re.findall(r"\b(?:std::)?atomic\s*<\s*bool\s*>\s*(\w+)\b", body)
            if not flags:
                continue
            for flag in flags:
                for cpath in find_files(src_root, {".cpp"}):
                    ctext = strip_comments_and_strings(read(cpath))
                    rel = os.path.relpath(cpath, root)
                    if not re.search(r"\b" + cname + r"\s*::\s*run\s*\(", ctext):
                        continue
                    # run() 内把标志置 true
                    reset = re.search(
                        r"\b" + cname + r"\s*::\s*run\s*\([^)]*\)\s*\{",
                        ctext)
                    if not reset:
                        continue
                    rs = reset.end() - 1
                    d2, j = 0, rs
                    while j < len(ctext):
                        if ctext[j] == "{":
                            d2 += 1
                        elif ctext[j] == "}":
                            d2 -= 1
                            if d2 == 0:
                                break
                        j += 1
                    run_body = ctext[rs + 1:j]
                    m = re.search(r"\b" + flag + r"\s*=\s*true\s*;", run_body)
                    if m:
                        line = ctext.count("\n", 0, rs + 1 + m.start()) + 1
                        fail("[stop-flag] {}:{} {}::run() 在开头把停止标志 {} 重置为 "
                             "true；在 start() 与 run() 之间到达的 stop() 会被吞掉，"
                             "导致线程无法及时退出、wait() 超时后 delete 仍在运行的 "
                             "QThread（Qt 会终止进程）。应把 {} 的初值置为 true 并"
                             "在 run() 内不再重置（与 AudioSenderThread / "
                             "AudioReceiverThread 一致）。"
                             .format(rel, line, cname, flag, flag))
                    break


def check_abandoned_async_tasks(root, src_root):
    """
    用 `QtConcurrent::run` 启动、且把结果只赋给**单个变量**的异步任务，
    必须在同一函数里把 future 收集进容器，否则上一次任务会被"丢弃仍在运行"。

    背景（真实缺陷，本轮修复）：
    `AudioManager::playAsync()` 每次播放都做三件事：

        stop();                                       // 置旧标志
        interruptFlag = std::make_shared<std::atomic<bool>>(false);
        playFuture = QtConcurrent::run([=]() { ... 捕获 this ... });

    而 **`QFuture` 赋值不会等待上一个任务结束**（只转移内部 d 指针的引用，
    `~QFuture()` 也不阻塞）。于是上一次播放若尚未结束，它就成了"被丢弃但仍在
    运行"的任务：

      - 它捕获的 `flag` 已不再是 `interruptFlag` 指向的对象，`stop()` 再也
        打不断它（会把整段提示音播完，并继续持有静态 pcmMutex）；
      - 析构若只 `waitForFinished()` 最后一个 future，旧任务完全不被等待，
        `~AudioManager()` 返回后它仍在 emit 信号、读成员 —— use-after-free。

    判定规则：某函数内出现 `X = QtConcurrent::run(...)`，且整个函数体内
    **没有任何** future 收集动作（`isRunning()` 过滤后 `append` 到容器），
    即判定为缺陷。理由是这类"每次播放都覆盖同一个成员"的写法必然会在第二次
    调用时丢弃第一次的任务；只有把未完成的 future 收集起来，析构才可能等到
    全部任务（本工程修复后的写法正是如此）。

    不误伤"即发即忘"用法（`QtConcurrent::run(...)` 不赋给任何变量）：那类任务
    自带 QPointer 守卫，没有"被覆盖丢弃"的问题，见 widget.cpp 的人脸检测。
    """
    for path in find_files(src_root, {".cpp"}):
        text = strip_comments_and_strings(read(path))
        rel = os.path.relpath(path, root)
        if "QtConcurrent::run" not in text:
            continue

        funcs = list(re.finditer(
            r'^[\w:<>,\s\*&]+\b(\w+)\s*::\s*(\w+)\s*\([^;]*?\)\s*(?:const\s*)?\{',
            text, re.M))
        for fm in funcs:
            body_start = fm.end() - 1
            depth, i = 0, body_start
            while i < len(text):
                if text[i] == '{':
                    depth += 1
                elif text[i] == '}':
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            body = text[body_start:i + 1]
            if "QtConcurrent::run" not in body:
                continue

            assigned = re.findall(
                r'\b(\w+)\s*=\s*QtConcurrent\s*::\s*run\s*\(', body)
            if not assigned:
                continue  # 即发即忘，不判定

            # 收集动作：先把已完成的滤掉，再把仍在运行的收进容器。
            collects = bool(re.search(r'\.\s*append\s*\(', body)) and \
                bool(re.search(r'\bisRunning\s*\(', body))
            if collects:
                continue

            line = text.count("\n", 0, fm.start()) + 1
            fail("[abandoned-task] {}:{} {}() 里由 QtConcurrent::run 启动的异步"
                 "任务被直接赋给 \"{}\" 且未收集进容器。QFuture 赋值不会等待"
                 "上一个任务，故上一次仍在运行的任务会被丢弃：它捕获的 this 在"
                 "析构时不被等待会变成 use-after-free，它捕获的中断标志也不再受"
                 "stop() 影响。应把未完成的 future 收集起来，并在析构时逐个 "
                 "waitForFinished()。"
                 .format(rel, line, fm.group(2), "/".join(sorted(set(assigned)))))


def check_no_secret_in_logs(root, src_root):
    """
    禁止把密码/密钥等敏感值打进日志。

    背景（真实缺陷，本轮修复）：
    门口机的密码校验路径把**已存储的正确密码**写进了 qDebug：

        qDebug() << "[PasswordManager] checkPassword: input=" << input
                 << ", password=" << password
                 << ", inputHex=" << input.toUtf8().toHex()
                 << ", pwdHex=" << password.toUtf8().toHex();

    以及输入窗体里逐位打印按键、并打印整串输入：

        qDebug() << "[passwd] Digit clicked:" << btn->text();
        qDebug() << "[passwd] OK clicked, inputBuffer=" << inputBuffer << ...;

    后果：任何一次密码校验（含输错）都会把门禁密码明文写进 stderr /
    systemd journal。门口机是实体门禁设备，密码即物理准入凭据；日志可被
    任何有读取权限的人拿到，且跨重启留存。逐位日志与整串日志叠加，还能
    完整还原用户输入过程。

    判定规则：日志语句（qDebug/qWarning/qInfo/qCritical）中不得出现敏感
    标识符（password/passwd/pwd/secret/token/credential）作为**被输出的值**。
    - 例外一：仅出现在日志的**前缀字符串**里（如 "[passwd]" 标签）不算，
      否则会误伤正常的模块标签。
    - 例外二：仅取长度/是否为空等非内容信息（.size()/.length()/.isEmpty()）
      不算泄露。

    只做词法判定，不做数据流分析。
    """
    # 规则 1：日志中直接输出了名字本身就像凭据的标识符。
    secret_name = r"(?:password|passwd|pwd|secret|token|credential)"
    rule1 = re.compile(
        r"<<[^;]*?\b" + secret_name + r"\b\s*"
        r"(?:\.\w+\s*\([^()]*\))?\s*(?=[,;<])",
        re.IGNORECASE)
    # 规则 2：日志中输出了“被当作密码传出去的”那个变量。
    #   emit passwordEntered(inputBuffer);
    #   connect(..., &Form_passwd::passwordEntered, mgr, &PasswordManager::checkPassword);
    # 前者把 inputBuffer 标记为凭据变量，之后 log 里出现它就报错。
    #
    # 注意必须排除“结果型”名字：`passwordChecked(matched)` 里的 matched 是
    # 一个 bool 结果，不是密码本身；把它的实参也标成凭据会造成误报。
    # 故只认名字含 password 且**不含** checked/result/valid/ok/success/failed
    # 的被调方（如 passwordEntered / checkPassword / setPassword）。
    taint_call = re.compile(
        r"\b(?:emit\s+)?(?!\w*(?:checked|result|valid|success|failed|ok)\w*)"
        r"\w*password\w*\s*\(([^()]*)\)",
        re.IGNORECASE)
    # 规则 3：在涉及密码的文件里打印按钮文本（逐位记录用户输入的数字）。
    button_text = re.compile(r"\b\w+\s*->\s*text\s*\(\s*\)")
    auth_file = re.compile(secret_name, re.IGNORECASE)
    safe_re = re.compile(r"\.(size|length|count)\s*\(\s*\)|\.isEmpty\s*\(\s*\)")
    log_re = re.compile(r"\bq(?:Debug|Warning|Info|Critical)\s*\(\s*\)")

    for path in find_files(src_root, {".cpp", ".h", ".hpp"}):
        raw = read(path)
        text = strip_comments_and_strings(raw)
        rel = os.path.relpath(path, root)

        # 收集本文件内被当作凭据传递的标识符
        tainted = set()
        for tm in taint_call.finditer(text):
            for arg in tm.group(1).split(","):
                a = arg.strip()
                if re.fullmatch(r"[A-Za-z_]\w*", a):
                    tainted.add(a)

        is_auth = bool(auth_file.search(text))

        for m in log_re.finditer(text):
            end = text.find(";", m.end())
            if end < 0:
                continue
            stmt = text[m.end():end]
            if safe_re.search(stmt):
                continue  # 只看长度/空判定，不泄露内容

            hit = None
            if rule1.search(stmt):
                hit = "密码/密钥类标识符"
            else:
                # 规则 2：语句里出现了被标记的凭据变量
                for v in tainted:
                    if re.search(r"\b" + re.escape(v) + r"\b", stmt):
                        hit = "被当作密码传递的变量 " + v
                        break
                if hit is None and is_auth and button_text.search(stmt):
                    hit = "密码输入界面的按键文本"

            if hit is None:
                continue
            line = text.count("\n", 0, m.start()) + 1
            fail("[secret-log] {}:{} 日志中输出了{}；门口机为实体门禁设备，"
                 "日志可被他人读取且跨重启留存，应只记录“校验是否通过”或"
                 "输入长度等非内容信息。"
                 .format(rel, line, hit))


def check_hex_parse_is_validated(root, src_root):
    """
    `QByteArray::fromHex()` 的输入必须先逐字符校验为纯十六进制。

    背景（真实缺陷，本轮修复）：
    `parseHexBytes()` 原来只检查"清洗后非空"与"长度为偶数"就调用 fromHex。

    而 fromHex() **不会**因非法字符而失败 —— 它**跳过**非法字符，把剩下的
    合法字符**重新两两配对**。已用真实 Qt 5.15.19 实测确认（见
    tests/test_app_config.cpp 中的可执行断言）：

        "FD0G4C12045DDF" (14, 偶数) -> 0f d0 4c 12 04 5d df   （内容全错）
        "XX034C12045DDF" (14, 偶数) ->    03 4c 12 04 5d df   （错位，少一字节）

    于是只要字符数是偶数，解码总是"成功"，`!bytes.isEmpty()` 完全拦不住，
    一条写错的灯控指令会被原样写进串口，灯只表现为"没反应"。

    判定规则：若某函数把**变量**（而非字符串字面量）传给 fromHex，且该函数
    体内没有显式的十六进制字符校验（isHexOnly 之类的调用，或逐字符比较
    '0'~'9'/'a'~'f'/'A'~'F'），即报错。

    传给 fromHex 的**字面量**（如 applyDefaults() 里的
    `QByteArray::fromHex("FD034C12045DDF")`）不受影响：字面量的合法性由
    人眼与测试保证。本工程当前唯一的变量用法是 parseHexBytes()。
    """
    # 显式校验的形态。两点约束来自故障注入自检：
    #  1. strip_comments_and_strings() 会把字符字面量清空（'0' -> ''），
    #     所以**不能**用 QChar('0') / c >= '0' 这类基于字符字面量的模式。
    #  2. 不能只写死少数几个函数名，否则换个名字（如 validateHexOnly）就
    #     漏判；这里用"名字里含 hex 的校验型函数调用"这一宽模式。
    hexcheck_re = re.compile(
        r"\b\w*(?:is|check|validate|verify|ensure|assert)\w*hex\w*\s*\("
        r"|\b\w*hex\w*(?:is|check|validate|verify|only|ok|valid)\w*\s*\("
        r"|\bisxdigit\b", re.I)
    for path in find_files(src_root, {".cpp"}):
        text = strip_comments_and_strings(read(path))
        rel = os.path.relpath(path, root)
        if "fromHex" not in text:
            continue
        # 同时覆盖 "Class::method(...)" 与**自由函数** "ret name(...)"。
        # 真实缺陷就在匿名命名空间里的自由函数 parseHexBytes()，若只匹配
        # Class::method 便完全漏判 —— 这正是本检查首版自检抓出来的问题。
        funcs = list(re.finditer(
            r'^[^\n{};#]*?\b(\w+)\s*\([^;{}]*\)\s*(?:const\s*)?\{',
            text, re.M))
        for fm in funcs:
            if fm.group(1) in ("if", "while", "for", "switch", "catch",
                               "else", "do", "return"):
                continue
            start = fm.end() - 1
            depth, i = 0, start
            while i < len(text):
                if text[i] == '{':
                    depth += 1
                elif text[i] == '}':
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            body = text[start:i + 1]
            if "fromHex" not in body:
                continue
            if hexcheck_re.search(body):
                continue
            # 提取 fromHex 的实参时**必须按括号配平扫描**，不能用
            # `fromHex\s*\(([^()]*)\)`：真实写法是
            #     QByteArray::fromHex(compact.toLatin1())
            # 实参里含有嵌套括号 `toLatin1()`，`[^()]*` 根本匹配不到右括号，
            # 于是整个 fromHex 调用被静默跳过、缺陷漏判。
            # （本检查首版正是如此，靠故障注入自检才暴露。）
            for m in re.finditer(r"fromHex\s*\(", body):
                open_idx = m.end() - 1
                depth, j = 0, open_idx
                while j < len(body):
                    if body[j] == '(':
                        depth += 1
                    elif body[j] == ')':
                        depth -= 1
                        if depth == 0:
                            break
                    j += 1
                arg = body[open_idx + 1:j].strip()
                if not arg:
                    continue
                # 字面量：内容由代码显式写出，不判定
                if arg.startswith('"'):
                    continue
                line = text.count("\n", 0, fm.start()) + 1
                fail("[hex-parse] {}:{} {}() 把变量 \"{}\" 直接传给 "
                     "QByteArray::fromHex 而未先校验其为纯十六进制。"
                     "fromHex 遇到非法字符不会报错，而是跳过非法字符并把剩余"
                     "字符重新配对——偶数长度下总是\"成功\"、内容全错，"
                     "`!bytes.isEmpty()` 拦不住。请先用逐字符校验"
                     "（如 isHexOnly）拒绝含非十六进制字符的输入。"
                     .format(rel, line, fm.group(1), arg[:40]))


def check_load_state_reset(root, src_root):
    """
    配置载入函数必须在**每次调用开头**复位"是否已载入"状态。

    背景（真实缺陷，本轮修复）：
    `AppConfig` 是单例且 `load()` 可被多次调用。原实现只在成功分支里写
    `m_loadedFromFile = true`，却在函数末尾 `return m_loadedFromFile;`。
    applyDefaults() 只重置各字段，**不复位该标志**，于是：

        load(好文件) -> true
        load(坏文件) -> 解析失败走 continue，标志仍是 true -> **返回 true**
        load(不存在) -> 同样返回 true，configPath() 还指向早已失效的旧路径

    实测（真实 Qt）确认：load(bad) 返回 true 且 configPath 仍是上一个文件。
    调用方会据此认为"配置已按文件生效"，实际用的是内置默认值。

    判定规则：若某函数名形如 load/readConfig/parseConfig，体内有
    "<成员> = true/false" 这类标志赋值，且**末尾 return 该成员**，但函数开头
    （首个赋值之前）没有把它复位为 false，即报错。

    门口机侧使用**函数局部** `bool fileLoaded = false;`，天然无此问题 ——
    正是这个不对称让室内机的缺陷一直没被发现，故两仓库都保留本检查。
    """
    for path in find_files(src_root, {".cpp"}):
        text = strip_comments_and_strings(read(path))
        rel = os.path.relpath(path, root)
        funcs = list(re.finditer(
            r'^[^\n{};#]*?\b(\w+)\s*\([^;{}]*\)\s*(?:const\s*)?\{', text, re.M))
        for fm in funcs:
            name = fm.group(1)
            if name.lower() not in ("load", "readconfig", "parseconfig",
                                    "reload", "loadconfig"):
                continue
            start = fm.end() - 1
            depth, i = 0, start
            while i < len(text):
                if text[i] == '{':
                    depth += 1
                elif text[i] == '}':
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            body = text[start:i + 1]
            line = text.count("\n", 0, fm.start()) + 1
            # 末尾 return 的是某个成员（m_xxx）
            mret = re.search(r'return\s+(m_\w+)\s*;', body)
            if not mret:
                continue
            member = mret.group(1)
            # 该成员被赋过值
            assigns = list(re.finditer(
                r'\b' + re.escape(member) + r'\s*=\s*(true|false)\s*;', body))
            if not assigns:
                continue
            # 以**首个赋值为 true** 的位置为界，看它之前有没有复位为 false。
            # 注意不能拿"第一个赋值"当界——那样复位语句本身就成了界，
            # head 恒为空，规则会把正确代码也判为缺陷（首版正是这个错误，
            # 由故障注入的"开头复位(真阴性)"用例抓出）。
            set_true = None
            for a in assigns:
                if a.group(1) == "true":
                    set_true = a
                    break
            if set_true is None:
                continue  # 从未置 true，不存在残留问题
            head = body[:set_true.start()]
            if re.search(r'\b' + re.escape(member) + r'\s*=\s*false\s*;', head):
                continue
            fail("[load-state] {}:{} {}() 末尾 return {}，但函数开头没有把它"
                 "复位为 false。该成员是单例状态，多次调用 load() 时失败路径"
                 "不会清零它，于是 load(坏文件) 会返回 true（上一次成功的残留），"
                 "调用方误以为配置已生效。请在函数开头显式复位"
                 "（并一并清理 configPath 之类的状态）。"
                 .format(rel, line, name, member))


def check_narrowing_cast_of_config(root, src_root):
    """
    配置整数转窄类型前必须先夹取，不能直接 static_cast。

    背景（真实缺陷，本轮修复）：
    `m_signalingPort = static_cast<quint16>(jsonInt(..., "signaling_port", ...))`
    —— `static_cast` 到 quint16 是**取模截断**而不是夹取。实测（真实 Qt）：

        65536  -> 0      （=> QTcpServer 绑定随机临时端口）
        999999 -> 16959
        -1     -> 65535
        -5     -> 65531

    用户写 65536/-1 这类"明显想表达边界"的值时，得到的是能成功监听的**错误
    端口**：门口机连不上，而日志显示监听正常。必须在 cast 之前用 clampInt。

    判定规则：`static_cast<quint16|quint8|qint16|qint8|char|short>(...)` 的实参
    里若直接调用 jsonInt/toInt/toUShort/toUInt 等**未夹取**的读取函数，即报错。
    传入 clampInt(...) 的写法不受影响。
    """
    narrow = r'(?:quint16|quint8|qint16|qint8|uint16_t|uint8_t|int16_t|int8_t|char|short|unsigned\s+short)'
    readfn = r'(?:jsonInt|jsonDouble|toInt|toUInt|toUShort|toULongLong|toLongLong)'
    for path in find_files(src_root, {".cpp"}):
        text = strip_comments_and_strings(read(path))
        rel = os.path.relpath(path, root)
        for m in re.finditer(r'static_cast\s*<\s*' + narrow + r'\s*>\s*\(', text):
            open_idx = m.end() - 1
            depth, j = 0, open_idx
            while j < len(text):
                if text[j] == '(':
                    depth += 1
                elif text[j] == ')':
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            arg = text[open_idx + 1:j]
            if not re.search(r'\b' + readfn + r'\s*\(', arg):
                continue
            if re.search(r'\bclampInt\s*\(|\bclampDouble\s*\(', arg):
                continue
            line = text.count("\n", 0, m.start()) + 1
            fail("[narrow-cast] {}:{} static_cast 到窄整型前没有夹取：{}"
                 "static_cast 是**取模截断**而非夹取（65536 -> 0、-1 -> 65535、"
                 "999999 -> 16959），越界配置会变成\"能用的错误值\"，比直接报错"
                 "更难发现。请先 clampInt(..., lo, hi) 再转换。"
                 .format(rel, line, text[m.start():j + 1][:90]))


# 决定"线上/设备上到底是什么字节"的协议常量。它们一旦被改动，现场症状是
# 声音/画面不对而不是报错，因此必须有**可执行测试**把取值钉住。
WIRE_FORMAT_CONSTANTS = (
    "kAudioSampleRate",
    "kAudioChannels",
    "kAudioBytesPerSample",
    "kAudioFramesPerPacket",
    "kSignalingPort",
    "kAudioPort",
    "kVideoPort",
    "kPreviewPort",
)


def check_wire_format_constants_are_pinned(root, src_root):
    """
    影响线上字节格式的协议常量必须在 tests/ 里被断言。

    背景（真实的测试盲区，本轮补上）：
    AudioSenderThread 决定 UDP 12345 的**字节格式**（裸 PCM、S16_LE、
    16000 Hz、单声道、每包 320 帧 = 640 字节、**无包头**），室内机按包直接
    喂给 ALSA，所以包长或字节序错一点，现场只表现为"声音不对/有杂音"，
    **不报任何错误**。

    它此前从未被真正验证过：tools/devstubs 的 sendto() 只"返回 len"、不留
    痕迹，其 inet_pton() 还**永远返回 0**，导致 run() 在建连阶段就 return，
    根本走不到发送代码——测试即使"绿"着，发送逻辑一行都没执行。

    所以本检查要求：这些常量名必须出现在 tests/ 的源码里。它不能证明断言
    写得好，但能保证"改了这些值而测试毫无反应"这种情况不会悄悄发生。
    """
    proto = os.path.join(src_root, "core", "net", "protocol.h")
    if not os.path.isfile(proto):
        FAIL.append(f"[wire-format] 找不到 {proto}")
        return

    text = strip_comments_and_strings(read(proto))
    declared = set()
    for m in re.finditer(r"constexpr\s+[\w:]+\s+(k\w+)\s*=", text):
        declared.add(m.group(1))

    tests_root = os.path.join(root, "tests")
    if not os.path.isdir(tests_root):
        FAIL.append("[wire-format] 找不到 tests 目录，无法验证协议常量是否被钉住")
        return

    test_text = []
    for path in find_files(tests_root, (".cpp", ".h", ".hpp", ".c")):
        test_text.append(strip_comments_and_strings(read(path)))
    blob = "\n".join(test_text)

    missing = [c for c in WIRE_FORMAT_CONSTANTS if c in declared and c not in blob]
    if missing:
        FAIL.append(
            "[wire-format] 下列协议常量决定了线上字节格式，却没有任何测试断言它们："
            + ", ".join(missing)
            + " —— 改动它们不会让任何测试失败（详见 README 第 22 条）"
        )


def top_level_function_ranges(text):
    """
    返回 [(函数体起止下标, 函数体文本)]，只认**花括号深度为 0** 的函数定义。

    为什么不用"由内向外回溯"：那条路要往前找上一个 `;`/`}`/`{` 作为签名起点，
    而 `for (int i = 0; i < N; ++i)` 这类**头部含分号**的控制语句会把签名截断成
    `++i)`，于是被误判成函数签名、函数体只截到内层块。第 18 轮的
    `snd_pcm_writei` 正好包在这样一个 for 里，导致规则整处漏报（自检 4/6）。

    只看深度 0 的块就能彻底避开这个问题：函数体一定是一个深度 0 的块，
    块内嵌套的 for/if/lambda 都在深度 >= 1，不会被误认。
    """
    out = []
    depth = 0
    i = 0
    opens = []
    while i < len(text):
        c = text[i]
        if c == "{":
            if depth == 0:
                opens.append(i)
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0 and opens:
                start = opens.pop()
                out.append((start, i + 1, text[start:i + 1]))
            if depth < 0:
                depth = 0
        i += 1
    return out


def enclosing_function_body(text, pos):
    """返回包含 text[pos] 的**最内层顶层函数体**；找不到返回 None。"""
    best = None
    for start, end, body in top_level_function_ranges(text):
        if start <= pos < end:
            if best is None or start > best[0]:
                best = (start, end, body)
    return best[2] if best else None


def check_nonblock_pcm_handles_eagain(root, src_root):
    """
    以 SND_PCM_NONBLOCK 打开的 PCM 句柄，必须把 -EAGAIN 与真错误区分开。

    背景（真实缺陷，第 18 轮修复）：
    `AudioReceiverThread::run()` 里

        snd_pcm_open(&pcm_handle, PCM_DEVICE,
                     SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);

    设备是**非阻塞**打开的，因此 `snd_pcm_writei` 返回 -EAGAIN（播放缓冲区
    暂时满）是**完全可达的**正常情况，不是错误。原实现只特判 -EPIPE，其余
    负值一律走 `rcw < 0` 分支 → emit errorMsg + 结束播放线程。
    现场表现是**通话中途突然单向无声**，而 errorMsg 只打日志、界面毫无察觉。
    对照实验（探针，600 ms 窗口）：修复前 connectionLost 发射 3410 次且线程
    仍在运行；修复后恰好 1 次且线程结束。

    反向也危险：`AudioSenderThread` 以同样方式打开**采集**句柄，
    `snd_pcm_readi` 在缓冲区暂时没数据时同样返回 -EAGAIN；若把它当致命错误，
    上行音频会随录音间隙中断（该线程已有处理，见 tests/test_audio_sender.cpp）。

    判定：对每个 `snd_pcm_readi/writei` 调用，取其**所属函数体**；若该函数体内
    以 SND_PCM_NONBLOCK 打开了 PCM 句柄，则该函数体内必须出现 EAGAIN 或
    EWOULDBLOCK 字样。这是"有没有区分"的下限检查，不判断处理是否正确。
    """
    for path in find_files(src_root, (".cpp", ".cc", ".cxx")):
        rel = os.path.relpath(path, root)
        text = strip_comments_and_strings(read(path))
        if "SND_PCM_NONBLOCK" not in text:
            continue
        for m in re.finditer(r"\b(snd_pcm_(?:readi|writei))\s*\(", text):
            fn = m.group(1)
            body = enclosing_function_body(text, m.start())
            if body is None:
                continue
            if "SND_PCM_NONBLOCK" not in body:
                continue
            if "EAGAIN" in body or "EWOULDBLOCK" in body:
                continue
            fail("[nonblock-pcm] {} 里以 SND_PCM_NONBLOCK 打开 PCM 句柄后调用 {}，"
                 "却没有在同一个函数体内处理 -EAGAIN/-EWOULDBLOCK。"
                 "非阻塞句柄在缓冲区满/空时**必然**返回 -EAGAIN，把它当致命错误会"
                 "让通话中途单向无声（下行）或随录音间隙中断（上行）。"
                 "该值是可恢复的，应重试或丢弃本包后继续，而不是结束线程。"
                 .format(rel, fn))


def check_cross_repo_test_targets_are_unique(root, extra_root):
    """
    两个仓库的测试目标名必须互不冲突，否则**超级工程无法配置**。

    背景（真实缺陷，本轮修复）：
    门口机与室内机各自有 tests/CMakeLists.txt，目标名原本都是 test_protocol、
    test_app_config。单独构建时看不出问题，但把两个仓库作为子目录放进同一个
    CMake 工程（跨仓库集成、或 IDE 一次性载入）时，CMake 直接报错：

        add_executable cannot create target "test_protocol" because another
        target with the same name already exists.

    实测：修复前 configure_rc=1，两个 add_executable 全都失败；加仓库前缀后
    5/5 用例通过。ctest 的**用例名**同理（add_test(NAME ...)）：重名会让
    `ctest -R` 一次选中两个，输出也难以区分。

    判定规则：分别收集两个仓库 tests/ 下的 add_executable 与 add_test 名字，
    有交集即报错。
    """
    if not extra_root:
        return
    roots = [("门口机", root), ("室内机", os.path.abspath(extra_root))]

    collected = {}
    for label, repo in roots:
        p = os.path.join(repo, "tests", "CMakeLists.txt")
        if not os.path.isfile(p):
            continue
        text = strip_comments_and_strings(read(p))
        collected[label] = (
            set(re.findall(r'add_executable\s*\(\s*([\w.-]+)', text)),
            set(re.findall(r'add_test\s*\(\s*NAME\s+([\w.-]+)', text)))

    if len(collected) < 2:
        return

    items = list(collected.items())
    for i in range(len(items)):
        for j in range(i + 1, len(items)):
            la, (ta, ca) = items[i]
            lb, (tb, cb) = items[j]
            dup_t = ta & tb
            if dup_t:
                fail("[cross-target] {} 与 {} 的测试可执行目标重名: {}。"
                     "把两个仓库放进同一个 CMake 工程时会直接配置失败"
                     "（add_executable cannot create target ...）。"
                     "请给目标名加仓库前缀（如 t113_/desktop_）。"
                     .format(la, lb, ", ".join(sorted(dup_t))))
            dup_c = ca & cb
            if dup_c:
                fail("[cross-target] {} 与 {} 的 ctest 用例名重名: {}。"
                     "重名会让 ctest -R 一次选中两个、输出难以区分。"
                     "请给用例名加仓库前缀。".format(la, lb, ", ".join(sorted(dup_c))))


def main():
    if len(sys.argv) < 2:
        print("用法: python verify.py <项目根目录> [另一仓库根目录]")
        return 2
    root = os.path.abspath(sys.argv[1])
    src_root = os.path.join(root, "src")
    if not os.path.isdir(src_root):
        print(f"找不到 src 目录: {src_root}")
        return 2

    check_includes(root, src_root)
    check_build_lists(root, src_root)
    check_subproject_cmake_is_selfcontained(root, src_root)
    check_qrc(root, src_root)
    check_ui_objectnames(root, src_root)
    check_balance(root, src_root)
    check_legacy_identifiers(root, src_root)
    check_qt5_portability(root, src_root)
    check_forward_declarations(root, src_root)
    check_deferred_session_guard(root, src_root)
    check_accept_gated_threads(root, src_root)
    check_deleted_on_close_members(root, src_root)
    check_unqueued_poll_timers(root, src_root)
    check_blocking_poll_has_handle(root, src_root)
    check_datagram_read_returns(root, src_root)
    check_response_index_bounds(root, src_root)
    check_connect_signal_is_signal(root, src_root)
    check_json_configs(root)
    check_config_value_sanity(root, src_root)
    check_config_knobs_are_used(root, src_root)
    check_no_running_reset_in_run(root, src_root)
    check_declared_signal_is_emitted(root, src_root)
    check_abandoned_async_tasks(root, src_root)
    check_hex_parse_is_validated(root, src_root)
    check_load_state_reset(root, src_root)
    check_narrowing_cast_of_config(root, src_root)
    check_wire_format_constants_are_pinned(root, src_root)
    check_nonblock_pcm_handles_eagain(root, src_root)
    check_cross_repo_test_targets_are_unique(
        root, sys.argv[2] if len(sys.argv) > 2 else None)
    check_no_secret_in_logs(root, src_root)

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

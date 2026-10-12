# -*- coding: utf-8 -*-
"""`strip_comments_and_strings()` / `strip_comments_only()` 的**纯单元**自检。

用法：python3 tools/verify_stripper_robustness.py

为什么需要它（第 41 轮的真实事故）
---------------------------------
这两个剥离器是 `tools/verify.py` 里**全部 50 条规则**的共同底座：
规则都是"先剥注释与字符串，再在剩下的代码文本上匹配"。底座错了，
**所有**规则一起错 —— 而且是**静默**地错。

第 41 轮加 `credential_store.cpp` 时，`[credential]` 报"盐没有参与哈希构造"，
而代码里 `toHex(salt)` 写得清清楚楚。查下去是底座的问题：

    旧顺序：块注释 → 字符串 → **字符字面量** → 行注释
                                            ^^^^^^^^^^ 最后

    源文件第 48 行（`//` 注释里）：
        //  undefined reference to `accessterminal::CredentialStore::kMinIterations'
                                                                               ^
    这个**孤立的撇号**与几千行之后 `QLatin1Char('0')` 的开引号配对，
    字符字面量正则一次吃掉整段代码：

        剥之前 20184 字节 → 剥之后 4349 字节
        `toHex` / `iterateDigest` / `salt` 全部消失

于是依赖它们的规则全部误报。任何英文注释里的 `don't` / `it's` / `` `Foo' ``
都会触发 —— 这不是某个文件的问题，是每个 `.cpp` 都可能踩到的定时炸弹。

修法：把**行注释提前到字符字面量之前**（此时字符串内容已清空，
所以 `"http://..."` 里的 `//` 不会被误当注释）。

本脚本钉住的正是这件事。**纯文本处理，不需要编译器/Qt。**

判据取向
-------
- 每条"不该出现"都配"该出现"的阳性对照（否则剥离器"全吃光"也能通过）；
- 保留行号：剥离后换行数必须不变（否则所有报错行号都会偏）。
"""
import io
import os
import sys

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import verify  # noqa: E402  （被测对象就是本仓库的 verify.py）

APOS = "'"
FAILS = []


def check(name, cond, detail=""):
    if cond:
        print("  [PASS] %s" % name)
    else:
        print("  [FAIL] %s  %s" % (name, detail))
        FAILS.append(name)


def strip(text):
    return verify.strip_comments_and_strings(text)


# ---------------------------------------------------------------------------
# 1. 行注释里的孤立撇号**不得**吃掉后面的代码（本轮修的那个 bug）
# ---------------------------------------------------------------------------
print("=== 1. 行注释里的孤立撇号（第 41 轮的真实事故）===")

SRC = (
    "int before() { return 1; }\n"          # 撇号之前的代码
    "// undefined reference to `Foo::kBar'\n"   # 孤立的撇号
    + "\n" * 40 +                                # 中间隔一大段
    "int after() { return QLatin1Char('0'); }\n"
)

s = strip(SRC)
check("撇号之前的代码保留", "before" in s)
check("撇号之后的代码保留", "after" in s,
      "行注释里的孤立撇号吃掉了后面的代码（行注释必须早于字符字面量处理）")
check("QLatin1Char 调用保留", "QLatin1Char" in s)
check("该注释本身被剥掉", "undefined reference" not in s)

# 阳性对照：不带撇号时也必须全都保留（证明上面前提成立，不是"碰巧")
s2 = strip(SRC.replace("`Foo::kBar'", "`Foo::kBar`"))
check("对照：无撇号时同样保留", "before" in s2 and "after" in s2)


# ---------------------------------------------------------------------------
# 2. 常见英文缩写不得触发同类问题
# ---------------------------------------------------------------------------
print()
print("=== 2. 英文缩写（don't / it's / won't）===")

for phrase in ("don't do this", "it's fine", "won't happen", "the user's key"):
    src = ("int head() { return 1; }\n"
           "// %s\n" % phrase
           + "\n" * 20 +
           "int tail() { return QLatin1Char('a'); }\n")
    s = strip(src)
    check("注释含 %-16s 时 tail 保留" % ('"%s"' % phrase),
          "tail" in s and "QLatin1Char" in s)


# ---------------------------------------------------------------------------
# 3. 字符串里的 `//` 不得被当成注释
# ---------------------------------------------------------------------------
print()
print("=== 3. 字符串里的 // 不得被当成行注释 ===")

src = (
    'const char* url = "http://example.com/x";\n'
    "int keepMe() { return 2; }\n"
)
s = strip(src)
check("字符串后的代码保留", "keepMe" in s)
check("URL 内容被清空", "example.com" not in s)


# ---------------------------------------------------------------------------
# 4. 块注释里的撇号 / 引号不得影响后续解析
# ---------------------------------------------------------------------------
print()
print("=== 4. 块注释里的撇号与引号 ===")

src = (
    "/* 这里有一个撇号 don't 和一个引号 \"unclosed */\n"
    "int afterBlock() { return QLatin1Char('z'); }\n"
)
s = strip(src)
check("块注释后的代码保留", "afterBlock" in s and "QLatin1Char" in s)
check("块注释内容被剥掉", "afterBlock" in s and "撇号" not in s)


# ---------------------------------------------------------------------------
# 5. 换行数必须不变（否则所有报错行号都会偏）
# ---------------------------------------------------------------------------
print()
print("=== 5. 剥离后换行数不变（行号不能漂）===")

src = 'a(); // 注释\nb("str");\n/* c1\nc2 */\nd();\n'
s = strip(src)
check("换行数一致", s.count("\n") == src.count("\n"),
      "剥离后 %d 个换行，原 %d 个" % (s.count("\n"), src.count("\n")))


# ---------------------------------------------------------------------------
# 6. strip_comments_only 保留字符串（第三种剥离器的既有语义）
# ---------------------------------------------------------------------------
print()
print("=== 6. strip_comments_only 保留字符串字面量 ===")

src = 'x("keep this string"); // 注释\n'
sc = verify.strip_comments_only(src)
check("字符串内容保留", "keep this string" in sc)
check("注释被剥掉", "注释" not in sc)


# ---------------------------------------------------------------------------
# 7. 真实仓库：两个仓库里**所有**源文件都必须"剥不瘦"
# ---------------------------------------------------------------------------
print()
print("=== 7. 真实仓库全量回归（剥离不应吃掉大段代码）===")

T = os.path.dirname(HERE)
checked = 0
worst = []
for dp, _dn, fn in os.walk(os.path.join(T, "src")):
    for f in fn:
        if not f.endswith((".cpp", ".h")):
            continue
        p = os.path.join(dp, f)
        try:
            raw = io.open(p, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        checked += 1
        st = strip(raw)
        # 换行数必须一致（这是硬性质：行号不漂）
        if st.count("\n") != raw.count("\n"):
            worst.append((os.path.relpath(p, T).replace("\\", "/"),
                          "换行数 %d -> %d" % (raw.count("\n"), st.count("\n"))))
        # 剥离后不应比原文"瘦"得离谱 —— 注释/字符串占比不会超过 ~75%
        if len(raw) > 400 and len(st) < len(raw) * 0.25:
            worst.append((os.path.relpath(p, T).replace("\\", "/"),
                          "长度 %d -> %d（疑似整段被吃）" % (len(raw), len(st))))

check("已扫描 %d 个源文件" % checked, checked > 30)
check("没有源文件被整段吃掉", not worst, "; ".join("%s: %s" % w for w in worst[:5]))


print()
if FAILS:
    print("失败 %d 项：%s" % (len(FAILS), FAILS))
    sys.exit(1)
print("结论: 全部通过")
sys.exit(0)

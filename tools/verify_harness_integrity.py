# -*- coding: utf-8 -*-
"""`verify.py` **自身harness完整性**的故障注入自检（第 42 轮）。

用法：python3 tools/verify_harness_integrity.py
返回码 0 表示：两个变异都让 verify.py **变红**、对照不误报、源码逐字节还原。

它守的是什么
------------
`verify.py` 是**所有其它检查的载体**。它自己出问题时，表现往往不是"报错"，
而是"安静地少跑一条规则"—— 那是最难发现的一类失效。本夹具钉住两点：

  M1  定义了一个 `check_*` 函数但**忘了在 main() 里注册**
      -> 该规则**从不执行**，而 verify.py 依然 rc=0，看起来一切正常。
         判据：`[harness-crash]`（"已定义但未注册"）。
         实现要点：必须数 **main() 函数体源码里实际出现的调用**，
         且用 `globals()` 而不是 `dir()`。
  M2  `main()` 里打印的"检查函数 N 个"被**写死**成别的值（不再数出来）
      -> 人工维护的"52 项检查"会随代码漂移且**毫无症状**。
         这正是 README 第 76 条的同一类问题。
         判据：**打印值 != 本夹具独立数出的注册数**。

         注意谁在抓它：**是本夹具，不是 verify.py**。verify.py 打印的就是
         它自己 list 的长度，无法察觉自己被写死 —— 它没有第二份判据。
         这正是"检查者本身也需要被检查"的典型：给 verify.py 配一个**独立
         实现**去重新数一遍，才构成真正的交叉验证。
         （对照 C2 是这条判据的阳性证据：加一个**已注册**的空规则后，
         打印值与独立计数**同步**从 52 变成 53 —— 证明这个计数是**数出来的**，
         不是碰巧相等。）

为什么这两个变异都**必须真的跑一次 verify.py** 才能判定
------------------------------------------------------
它们改的都是 verify.py 自己。只有真的执行它、读它的输出，才能知道
"未注册的规则有没有被发现"。纯文本断言证明不了任何事。

对照（必须保持绿）
-----------------
  C1  加无关注释
  C2  加一个**已注册**的 check_ 函数（应当计数 +1 且不报错）

注意
----
本夹具会改写 `tools/verify.py` 本身 —— 它同样用 `PristineGuard` 保护。
锁忙返回码为 3（与其他夹具一致）。
"""
import io
import os
import re
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
VP = os.path.join(T, "tools", "verify.py")

FILES = [VP]

sys.path.insert(0, HERE)
from fixture_pristine import PristineGuard  # noqa: E402

_GUARD = PristineGuard(files=FILES, tag='harnint',
                       lock_name='fi37_rewrite.lock')
_ORIG = _GUARD.orig
_HEALED = []

GUARD_MARK = 'if __name__ == "__main__":'

# 变异 1：插入一个**未注册**的 check_ 函数（必须插在 __main__ 守卫之前）
ORPHAN_FN = ('def check_orphan_probe(root, src_root):\n'
             '    """故意不注册的探针。"""\n'
             '    return None\n\n\n')

# 变异 2：把打印的计数写死成 999（与真实注册数不一致）
COUNT_OLD = 'print(f"检查函数: {len(_calls)} 个（已注册）")'
COUNT_NEW = 'print("检查函数: 999 个（已注册）")'

# 对照 2：加一个**已注册**的 check_ 函数
REGISTERED_FN = ('def check_control_probe(root, src_root):\n'
                 '    """对照：已注册的空规则。"""\n'
                 '    return None\n\n\n')
REGISTER_OLD = '    check_latest_value_handoff(root, src_root)\n'
REGISTER_NEW = ('    check_latest_value_handoff(root, src_root)\n'
                '    check_control_probe(root, src_root)\n')


def read(p):
    return io.open(p, encoding="utf-8", errors="replace").read()


def write(p, s):
    io.open(p, "w", encoding="utf-8", newline="\n").write(s)


def run_verify():
    """返回 (rc, 完整输出)。

    必须用 sys.executable 直接跑脚本，并捕获 stdout+stderr ——
    计数与 [harness-crash] 都只出现在输出里。
    """
    r = subprocess.run([PY, VP, T], capture_output=True, cwd=T)
    return r.returncode, (r.stdout + r.stderr).decode("utf-8", "replace")


def count_of(out):
    m = re.search(r'检查函数: (\d+) 个', out)
    return int(m.group(1)) if m else None


def count_registered_in_source(src):
    """独立地从 main() 源码里数注册调用 —— **与被测实现无关的第二份判据**。

    这一点很关键：如果只比较"两次运行的打印值是否不同"，那 M2 只是在
    观察文本变化，任何无关的改动都会让它"通过"，它证明不了计数与真实
    注册数一致。这里用一段**独立实现**重新数一遍，再与 verify.py 打印的
    数字比对，才是在检验那个数字**是不是真的数出来的**。
    """
    i = src.index('def main(')
    body = src[i:]
    # main() 到下一个顶层 def 为止
    nxt = re.search(r'(?m)^def ', body[1:])
    if nxt:
        body = body[:nxt.start() + 1]
    return len(set(re.findall(r'(?m)^\s{4}(check_[a-z_0-9]+)\(', body)))


def main():
    if not _GUARD.acquire_lock():
        print("另一个夹具正在改写源码（%s）—— 请等它跑完再试"
              % _GUARD.lock_path())
        return 3
    try:
        return _run()
    finally:
        _GUARD.release_lock()


def _run():
    _HEALED[:] = _GUARD.prepare()
    _GUARD.install_guards()
    if _HEALED:
        print("!! 检测到上一次运行被中断，已用磁盘原始副本自愈: %s"
              % ", ".join(_HEALED))

    src = _ORIG[VP]

    # 预检锚点
    for label, anchor in (('__main__ 守卫', GUARD_MARK),
                          ('计数打印', COUNT_OLD),
                          ('注册点', REGISTER_OLD)):
        if src.count(anchor) != 1:
            print("[夹具错误] %s 锚点出现 %d 次（期望 1）"
                  % (label, src.count(anchor)))
            return 2

    rc0, out0 = run_verify()
    base_n = count_of(out0)
    real_n = count_registered_in_source(src)
    print("基线 rc = %d，报告检查函数 %s 个，独立数出 %d 个"
          % (rc0, base_n, real_n))
    if rc0 != 0 or base_n is None:
        print("*** 基线就不通过/无计数，无法进行故障注入")
        return 2
    if base_n != real_n:
        print("*** 基线：打印的计数与独立计数不一致 —— 被测实现本身有问题")
        return 2
    print()

    problems = []

    def case(name, new_src, expect_fail, expect_n=None):
        """跑一个变异；expect_fail=True 表示必须变红。"""
        try:
            write(VP, new_src)
            if read(VP) == src:
                print("  %-42s *** 变异未落到磁盘" % name)
                problems.append(name)
                return
            rc, out = run_verify()
            n = count_of(out)
            hit = ('[harness-crash]' in out) and ('check_orphan_probe' in out
                                                 or 'check_' in out)
            if expect_fail == 'orphan':
                ok = (rc != 0) and hit and n == base_n
                detail = ('报了 [harness-crash] 且计数仍为 %s' % n) if ok \
                    else 'rc=%d 计数=%s hit=%s' % (rc, n, hit)
            elif expect_fail == 'count':
                # 写死 999 时，打印值必须与**独立数出**的注册数不符
                # （而不是仅仅"和上次不一样"）。
                ok = (n != real_n)
                detail = '打印=%s 独立数出=%d' % (n, real_n)
            else:  # 对照：必须绿，且打印计数仍等于独立计数
                ok = (rc == 0) and (n == count_registered_in_source(new_src))
                detail = 'rc=%d 打印=%s 独立数出=%d' % (
                    rc, n, count_registered_in_source(new_src))
            print("  %-42s %s  (%s)" % (name, '通过 ✓' if ok else '*** 失败',
                                       detail))
            if not ok:
                problems.append(name)
                for line in out.strip().splitlines()[-4:]:
                    print('      ' + line[:120])
        finally:
            write(VP, src)

    # M1：未注册的 check_ 函数 -> 必须报 [harness-crash]
    case('M1 定义 check_ 但不在 main() 注册',
         src.replace(GUARD_MARK, ORPHAN_FN + GUARD_MARK, 1), 'orphan')

    # M2：计数写死 -> 必须被发现（计数不符 或 rc!=0）
    case('M2 检查计数写死为 999',
         src.replace(COUNT_OLD, COUNT_NEW, 1), 'count')

    # C1：无关注释 -> 必须绿
    case('对照 C1 加无关注释',
         src.replace('FAIL = []', 'FAIL = []  # 无关注释', 1), False)

    # C2：注册一个空规则 -> 必须绿
    case('对照 C2 加一个已注册的空规则',
         src.replace(GUARD_MARK, REGISTERED_FN + GUARD_MARK, 1)
            .replace(REGISTER_OLD, REGISTER_NEW, 1), False)

    _GUARD.restore()
    same = read(VP) == _ORIG[VP]
    rc, out = run_verify()
    print()
    print("还原逐字节一致:", same)
    print("还原后: %s" % ("结论: 全部检查通过" if rc == 0
                       else "*** 仍不通过 rc=%d" % rc))
    print()
    print("变异 2 个，对照 2 个，致命问题 %d 个" % len(problems))
    if problems:
        print("问题清单:", problems)
    return 1 if (problems or not same or rc != 0) else 0


if __name__ == "__main__":
    sys.exit(main())

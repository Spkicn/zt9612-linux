#!/usr/bin/env python3
"""C 注释结构检查：找出"没闭合的块注释"和"被注释吞掉的代码行"。

**公开副本**：位于 `ci/`，不在 .gitignore 的排除范围内，会随公开仓库发布，
公开 CI 上一定跑得到它（见 `.github/workflows/docs.yml`）。

为什么需要它（真实事故）：重写块注释时漏掉结尾的 `*/`，后面的代码会被整段吞进注释里，
编译器给出的却是完全不相干的信息 ——
  * 文件头注释漏 `*/` ⇒ `module_param()` 被吞 ⇒ `error: expected ')' before 'int'`；
  * mac80211 段头漏 `*/` ⇒ `zt_band_2ghz` 被吞 ⇒ `error: 'zt_band_2ghz' undeclared`。
这两次都是 CI 的**编译作业**最后发现的（文档机检不解析 C 语法）。本脚本把这类错误提前，
并且能在公开 CI 上跑 —— 它只需要 `driver/*.c` 文本，不需要内核头文件。

判据（只报"能确证"的）：
  1. 块注释在文件结束前**没有闭合**；
  2. 一行**明显是代码**（预处理指令、`static`/`struct`/`module_param`…、单独一个 `}`）
     却落在块注释区间里 —— 这类几乎总是漏写 `*/` 造成的。

已知不覆盖：注释与字符串混排的极端写法、`#if 0` 里的代码块。宁可少报，不要误报。

用法：
    python ci/check_comments.py                # 默认检查 driver/zt9612.c、driver/zt9612_fw.c
    python ci/check_comments.py FILE...        # 指定文件

只读脚本。退出码：0 = 结构正常；1 = 发现问题。
"""
from __future__ import annotations

import re
import sys

DEFAULT = ["driver/zt9612.c", "driver/zt9612_fw.c"]
# 一眼就是代码的行（出现在块注释里就可疑）
CODE_HINT = re.compile(
    r"^\s*(#\s*\w|static\b|struct\b|union\b|enum\b|typedef\b|"
    r"module_param|MODULE_|EXPORT_SYMBOL|int\s+\w+\s*\(|\}\s*;?\s*$)"
)


def scan(path: str) -> list[str]:
    text = open(path, encoding="utf-8", errors="replace").read()
    problems: list[str] = []

    # 1) 状态机走一遍，找未闭合的块注释 / 未结束的字符串
    i, n, line, state, block_start = 0, len(text), 1, "code", 0
    while i < n:
        ch = text[i]
        if ch == "\n":
            line += 1
            if state == "line":
                state = "code"
        if state == "code":
            if text.startswith("//", i):
                state, i = "line", i + 2
                continue
            if text.startswith("/*", i):
                state, block_start, i = "block", line, i + 2
                continue
            if ch == '"':
                state = "string"
            elif ch == "'":
                state = "char"
            i += 1
            continue
        if state == "block":
            if text.startswith("*/", i):
                state, i = "code", i + 2
                continue
            i += 1
            continue
        if state in ("string", "char"):
            if ch == "\\":
                i += 2
                continue
            if (state == "string" and ch == '"') or (state == "char" and ch == "'"):
                state = "code"
            i += 1
            continue
        i += 1

    if state == "block":
        problems.append(f"{path}: 第 {block_start} 行开始的块注释**没有闭合**（一直吃到文件末尾）")
    elif state in ("string", "char"):
        kind = "字符串" if state == "string" else "字符常量"
        problems.append(f"{path}: 文件结束时仍有未关闭的{kind}")

    # 2) 找"像代码却在块注释里"的行
    in_block = False
    for idx, src_line in enumerate(text.splitlines(), start=1):
        if in_block:
            if "*/" in src_line:
                in_block = False
                continue
            if CODE_HINT.match(src_line) and not src_line.strip().startswith("*"):
                problems.append(f"{path}:{idx}: 这一行像代码，却处在块注释里 —— {src_line.strip()[:70]}")
            continue
        stripped = src_line.strip()
        if stripped.startswith("/*") and "*/" not in src_line.split("/*", 1)[1]:
            in_block = True
    return problems


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    targets = sys.argv[1:] or DEFAULT
    checked: list[str] = []
    all_problems: list[str] = []
    for path in targets:
        try:
            all_problems += scan(path)
            checked.append(path)
        except FileNotFoundError:
            continue
    if not checked:
        print(f"没有可检查的文件（尝试过：{', '.join(targets)}）", file=sys.stderr)
        return 1
    if all_problems:
        print(f"发现 {len(all_problems)} 个注释结构问题。块注释必须闭合：漏掉 `*/` 会把后面的代码"
              f"整段吞进注释，报错信息却指向别处。", file=sys.stderr)
        for item in all_problems:
            print(f"  {item}", file=sys.stderr)
        return 1
    print(f"OK：{'、'.join(checked)} 的块注释都闭合，且没有明显的代码被吞进注释")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

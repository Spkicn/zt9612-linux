#!/usr/bin/env python3
"""Markdown 渲染检查：防止 `~` 被解析成删除线。

**公开副本**：本文件位于 `ci/`，不在 .gitignore 的排除范围内，会随公开仓库发布，
公开 CI 上一定跑得到它（见 `.github/workflows/docs.yml`）。

背景（真实事故）：GitHub 的 markdown 把**单个** `~` 也当删除线分隔符，不只是 `~~`。
于是"上传 3.7~4.7 Mbit/s，下载 7~13 Mbit/s"这样的区间写法里，第一个 `~` 与第二个 `~`
配成一对，整段被划上删除线（release 页面上已经出现过多次）。

规则：`.md` 文件里，**代码围栏与行内代码之外**不允许出现单个 `~`。
- 区间用 en dash：`3.7–4.7 Mbit/s`（U+2013）或 `3.7-4.7`；
- 约数用"约"：`约 15 Mbit/s`，不要写 `~15 Mbit/s`；
- 家目录、`~/.bashrc` 这类必须用 `~` 的地方，放进行内代码（反引号）或代码块里。

用法：
    python ci/markdown_check.py            # 只报问题；有发现时退出码 1
    python ci/markdown_check.py -v         # 打印扫过的文件数
    python ci/markdown_check.py --root DIR # 手动指定仓库根
    python ci/markdown_check.py FILE...    # 额外检查指定文件（例如 Release 正文草稿，
                                           # 它们不在仓库里，CI 覆盖不到）

只读脚本，不修改任何文件。退出码：0 = 干净；1 = 发现问题。
"""
from __future__ import annotations

import argparse
import os
import re
import sys

SKIP_DIRS = {".git", "re", "firmware_from_cd", ".workbuddy", "__pycache__", "node_modules"}

# 代码围栏（``` 或 ~~~）与行内代码（`...`，允许多个反引号成对）
FENCE_RE = re.compile(r"^(\s*)(`{3,}|~{3,})")
INLINE_CODE_RE = re.compile(r"`+[^`]*`+")
# 成对的删除线标记 `~~...~~` 是合法的：先把 `~~` 两个码位整体去掉，
# 剩下的单个 `~` 才会与别的 `~` 配成删除线，是本次要挡的问题。
STRIKE_PAIR_RE = re.compile(r"~~")
TILDE_RE = re.compile(r"~")


def find_root(start: str) -> str:
    if os.path.isfile(os.path.join(start, "dkms.conf")) or os.path.isdir(os.path.join(start, "driver")):
        return start
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def iter_markdown(root: str):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in sorted(filenames):
            if name.lower().endswith((".md", ".markdown")):
                yield os.path.join(dirpath, name)


def scan(path: str) -> list[tuple[int, str]]:
    """返回 [(行号, 该行去掉代码后的内容)]，只保留仍有单个 ~ 的行。"""
    hits: list[tuple[int, str]] = []
    fence: str | None = None
    with open(path, encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, start=1):
            stripped = line.rstrip("\n")
            m = FENCE_RE.match(stripped)
            if m:
                marker = m.group(2)[0]
                if fence is None:
                    fence = marker
                elif fence == marker:
                    fence = None
                continue
            if fence is not None:
                continue
            code_free = INLINE_CODE_RE.sub("CODE", stripped)
            # 合法的 ~~删除线~~ 会把 `~~` 两两用掉；剩下的裸 `~` 才是隐患。
            leftover = STRIKE_PAIR_RE.sub("", code_free)
            if TILDE_RE.search(leftover):
                hits.append((lineno, stripped))
    return hits


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")

    parser = argparse.ArgumentParser(description="markdown 渲染检查（~ 删除线）")
    parser.add_argument("-v", "--verbose", action="store_true", help="打印扫过的文件数")
    parser.add_argument("--root", default=os.getcwd(), help="仓库根（默认当前目录）")
    parser.add_argument("files", nargs="*", help="额外检查的文件（Release 正文等，不在仓库里）")
    args = parser.parse_args()

    root = find_root(args.root)
    problems: list[str] = []
    scanned = 0

    for path in iter_markdown(root):
        rel = os.path.relpath(path, root)
        scanned += 1
        for lineno, line in scan(path):
            problems.append(f"{rel}:{lineno}: 行内代码之外出现单个 ~（会被渲染成删除线）：{line.strip()}")

    for path in args.files:
        if not os.path.isfile(path):
            problems.append(f"{path}: 文件不存在")
            continue
        for lineno, line in scan(path):
            problems.append(f"{path}:{lineno}: 行内代码之外出现单个 ~（会被渲染成删除线）：{line.strip()}")

    if args.verbose:
        print(f"已扫描 {scanned} 个 markdown 文件（根：{root}）")

    if problems:
        print(f"发现 {len(problems)} 处裸露的 ~。区间请用 en dash '–'，约数请写「约 15 Mbit/s」，"
              f"必须用 ~ 时放进反引号里的行内代码。", file=sys.stderr)
        for item in problems:
            print(f"  {item}", file=sys.stderr)
        return 1

    print("OK：markdown 中没有会被误解析成删除线的单个 ~")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

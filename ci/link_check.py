#!/usr/bin/env python3
"""链接检查：公开文档里的**相对链接**必须指向仓库里真实存在的文件。

**公开副本**：本文件位于 `ci/`，不在 .gitignore 的排除范围内，会随公开仓库发布，
公开 CI 上一定跑得到它（见 `.github/workflows/docs.yml`）。

为什么只查相对链接：外部 URL 会随对方站点、网络与地域抖动，把它做成阻塞门会让 CI
无谓变红（同类项目多用第三方 action 做全量外链检查，本仓库的既有惯例是"stdlib-only +
只查自己能负责的东西"）。相对链接坏了 100% 是我们的问题 —— 拆分/改名文档时最容易断。

规则：
- `[文字](目标)` / `![alt](目标)` 里的**相对路径**必须存在（`#锚点` 只校验文件部分）；
- `http(s)://`、`mailto:`、`#锚点`、`/` 开头的站点绝对路径一律跳过；
- 代码围栏与行内代码里的内容不解析；
- `--external` 额外对 http(s) 链接做一次 HEAD 探测，**只提示不判失败**（需要联网，默认关闭）。

用法：
    python ci/link_check.py              # 只查相对链接；有问题退出码 1
    python ci/link_check.py -v           # 打印扫过的 markdown 文件数
    python ci/link_check.py --external   # 另做外链探测（信息性）
    python ci/link_check.py --root DIR   # 手动指定仓库根

只读脚本。退出码：0 = 全部可解析；1 = 有相对链接指向不存在的文件。
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import urllib.parse
import urllib.request

SKIP_DIRS = {
    ".git", "re", "firmware_from_cd", ".workbuddy", ".mimic-cache",
    "__pycache__", "node_modules",
}
FENCE_RE = re.compile(r"^(\s*)(`{3,}|~{3,})")
INLINE_CODE_RE = re.compile(r"`+[^`]*`+")
LINK_RE = re.compile(r"!?\[[^\]]*\]\(\s*([^)\s]+?)(?:\s+\"[^\"]*\")?\s*\)")
SKIP_PREFIXES = ("http://", "https://", "mailto:", "#", "/", "ftp://")


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


def strip_code(text: str) -> list[tuple[int, str]]:
    """去掉代码围栏与行内代码，返回 [(行号, 处理后的行)]。"""
    out: list[tuple[int, str]] = []
    fence: str | None = None
    for lineno, line in enumerate(text.splitlines(), start=1):
        m = FENCE_RE.match(line)
        if m:
            marker = m.group(2)[0]
            if fence is None:
                fence = marker
            elif fence == marker:
                fence = None
            continue
        if fence is not None:
            continue
        out.append((lineno, INLINE_CODE_RE.sub("CODE", line)))
    return out


def external_ok(url: str) -> bool:
    req = urllib.request.Request(url, method="HEAD", headers={"User-Agent": "zt9612-link-check"})
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return 200 <= resp.status < 400
    except Exception:
        return False


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")

    parser = argparse.ArgumentParser(description="相对链接检查")
    parser.add_argument("-v", "--verbose", action="store_true", help="打印扫过的文件数")
    parser.add_argument("--external", action="store_true", help="额外探测外链（只提示）")
    parser.add_argument("--root", default=os.getcwd(), help="仓库根（默认当前目录）")
    args = parser.parse_args()

    root = find_root(args.root)
    problems: list[str] = []
    external: list[tuple[str, str]] = []
    scanned = 0
    checked = 0

    for path in iter_markdown(root):
        rel = os.path.relpath(path, root)
        scanned += 1
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
        for lineno, line in strip_code(text):
            for target in LINK_RE.findall(line):
                if target.startswith(SKIP_PREFIXES):
                    if args.external and target.startswith(("http://", "https://")):
                        external.append((rel, target))
                    continue
                local = urllib.parse.unquote(target.split("#", 1)[0])
                if not local:
                    continue
                checked += 1
                resolved = os.path.normpath(os.path.join(os.path.dirname(path), local))
                if not os.path.exists(resolved):
                    problems.append(f"{rel}:{lineno}: 相对链接指向不存在的文件：{target}")

    if args.verbose:
        print(f"已扫描 {scanned} 个 markdown 文件，校验 {checked} 条相对链接（根：{root}）")

    if args.external:
        bad: list[tuple[str, str]] = []
        for rel, url in external:
            if not external_ok(url):
                bad.append((rel, url))
        if bad:
            print(f"外链探测：{len(bad)}/{len(external)} 条不可达（仅提示，不判失败）")
            for rel, url in bad:
                print(f"  提示：{rel}: {url}")

    if problems:
        print(f"发现 {len(problems)} 条坏链接。相对链接必须指向仓库里真实存在的文件"
              f"（改名/拆文档时最容易断）。", file=sys.stderr)
        for item in problems:
            print(f"  {item}", file=sys.stderr)
        return 1

    print("OK：所有相对链接都能解析到真实文件")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

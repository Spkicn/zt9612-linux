#!/usr/bin/env python3
"""emoji 检查：仓库文档与代码注释里不允许出现 emoji。

**公开副本**：本文件位于 `ci/`，不在 .gitignore 的排除范围内，会随公开仓库发布，
公开 CI 上一定跑得到它（见 `.github/workflows/docs.yml`）。

理由（仓库约定，见本地 `docs/14-GitHub仓库规范.md` §2.4）：
文档与代码注释一律用中文，告警用 GitHub alert 语法（`> [!WARNING]`）或文字（「注意」）
表达；emoji 在终端、diff、邮件与补丁里表现不一致，会污染 `grep` 结果，也让文档显得不严肃。

检查范围：默认扫描仓库根下的文本文件，跳过二进制、版本控制目录与本地保留的大体量数据
（`re/`、`firmware_from_cd/`、`.workbuddy/`）。公开 CI 树上没有这些目录，判断逻辑不变。

用法：
    python ci/emoji_check.py            # 只报问题；有 emoji 时退出码 1
    python ci/emoji_check.py -v         # 打印扫过的文件数与命中详情
    python ci/emoji_check.py --root DIR # 手动指定仓库根

只读脚本，不修改任何文件。退出码：0 = 干净；1 = 发现 emoji 或扫描失败。
"""
from __future__ import annotations

import argparse
import os
import re
import sys

# 覆盖常见 emoji：星形平面（U+1F000 起）、杂项符号与装饰符号（U+2600–U+27BF）、
# 杂项符号与箭头（U+2B00–U+2BFF），以及变体选择符 U+FE0F（警告符号的第二个码位）。
EMOJI_RE = re.compile(
    "[\U0001F000-\U0001FAFF]"
    "|[\u2600-\u27BF]"
    "|[\u2B00-\u2BFF]"
    "|\uFE0F"
)

# 文本文件后缀（含无后缀的常见文本文件名）。
TEXT_EXT = {
    ".md", ".markdown", ".txt", ".c", ".h", ".sh", ".bash", ".py", ".ps1",
    ".conf", ".yml", ".yaml", ".rules", ".json", ".gitignore", ".gitattributes",
    ".editorconfig", ".checkpatch.conf",
}
TEXT_NAMES = {"Makefile", "MAINTAINERS", "LICENSE", "dkms.conf", "supported-device-IDs"}

# 跳过：版本控制、本地保留的大体量数据与二进制目录、缓存。
SKIP_DIRS = {".git", "re", "firmware_from_cd", ".workbuddy", "__pycache__", "node_modules"}
SKIP_EXT = {".bin", ".pcap", ".ko", ".o", ".zst", ".png", ".jpg", ".jpeg", ".gif", ".webp", ".pdf"}


def find_root(start: str) -> str:
    if os.path.isfile(os.path.join(start, "dkms.conf")) or os.path.isdir(os.path.join(start, "driver")):
        return start
    parent = os.path.dirname(os.path.abspath(__file__))
    return os.path.dirname(parent)


def is_text(path: str) -> bool:
    name = os.path.basename(path)
    ext = os.path.splitext(name)[1].lower()
    if ext in SKIP_EXT:
        return False
    return ext in TEXT_EXT or name in TEXT_NAMES


def walk(root: str):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in sorted(filenames):
            path = os.path.join(dirpath, name)
            if is_text(path):
                yield path


def main() -> int:
    # Windows 控制台默认不是 UTF-8，日志里的中文会变乱码；CI（Linux）不受影响。
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")

    parser = argparse.ArgumentParser(description="emoji 检查（文档与代码注释）")
    parser.add_argument("-v", "--verbose", action="store_true", help="打印扫过的文件数")
    parser.add_argument("--root", default=os.getcwd(), help="仓库根（默认当前目录）")
    args = parser.parse_args()

    root = find_root(args.root)
    problems: list[str] = []
    scanned = 0

    for path in walk(root):
        rel = os.path.relpath(path, root)
        try:
            with open(path, encoding="utf-8") as fh:
                lines = fh.readlines()
        except (UnicodeDecodeError, OSError):
            continue  # 二进制或不可读：不是本检查的对象
        scanned += 1
        for lineno, line in enumerate(lines, start=1):
            hits = EMOJI_RE.findall(line)
            if hits:
                got = " ".join(sorted({h for h in hits}))
                problems.append(f"{rel}:{lineno}: 含 emoji（{got}）")

    if args.verbose:
        print(f"已扫描 {scanned} 个文本文件（根：{root}）")

    if problems:
        print(f"发现 {len(problems)} 处 emoji。仓库约定：文档与代码注释一律不用 emoji，"
              f"告警改用 `> [!WARNING]` / `> [!IMPORTANT]` 或文字。", file=sys.stderr)
        for item in problems:
            print(f"  {item}", file=sys.stderr)
        return 1

    print("OK：文档与代码注释中未发现 emoji")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

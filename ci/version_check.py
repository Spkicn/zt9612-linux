#!/usr/bin/env python3
"""版本一致性检查（版本号只有一个真值，任何一处漂移都要被抓住）。

**公开副本**：本文件位于 `ci/`，不在 .gitignore 的排除范围内，会随公开仓库发布，
因此公开 CI 上一定跑得到它。本地另有 `tools/version_check.py` —— 那只是一个转发入口
（`import` 本文件后调 `main()`），逻辑以本文件为唯一实现，改这里即可，不会有两份逻辑漂移。

**只读公开文件**：dkms.conf、driver/zt9612.c、CHANGELOG.md、README.md。
本文件**不**引用 docs/、re/、tools/，也不含任何测试机信息 / 凭据，可以安全公开。
`docs/` 相关的检查（内部文档的参数表、默认值、CRLF 等）在 `tools/doc_lint.py`，那是本地脚本。

用法：
    python ci/version_check.py             # 只报问题，有问题时退出码 1
    python ci/version_check.py -v          # 额外打印每条版本声明
    python ci/version_check.py --root DIR  # 手动指定仓库根

核对对象：
  1. `dkms.conf` 的 `PACKAGE_VERSION="x.y.z"` —— 作为**基准**
     （它决定 `dkms install zt9612/<版本>` 装的是哪个版本）；
  2. `driver/zt9612.c` 的 `MODULE_VERSION("x.y.z")`；
  3. `driver/zt9612.c` 文件头注释里若写了版本（`vX.Y.Z`），也算一条声明；
  4. `CHANGELOG.md` 最新**已发布**小节 `## [x.y.z] - 日期`（`[Unreleased]` 不算）；
  5. `README.md` 里"当前版本 / 最新发布"类声明 —— README 允许同时写
     "最新发布 X / 开发中 vY"（前瞻版本），这种只当提示，不作硬约束。

仓库根定位：优先用当前工作目录（CI 里就是 checkout 根）；当该目录里既没有 dkms.conf
也没有 driver/zt9612.c 时，回退到**本脚本父目录的上一级**（`ci/` 的上一级 = 仓库根）
—— 这样"只有公开文件的树"（例如 `git archive HEAD | tar -x -C <临时目录>` 出来的树）
里也能直接跑。

只读脚本，不修改任何文件。退出码：0 = 全部一致；1 = 有不一致或缺少核对对象。
"""
from __future__ import annotations

import io
import os
import re
import sys

DKMS_CONF = "dkms.conf"
DRIVER = os.path.join("driver", "zt9612.c")
CHANGELOG = "CHANGELOG.md"
README = "README.md"

# 版本号形态统一按 x.y.z（本仓库 0.x 里程碑版本都是三段）
VERSION_RE = re.compile(r"v?(\d+\.\d+\.\d+)")
# README 里算硬约束的措辞
README_HARD = ("当前版本", "当前发布", "最新发布", "最新版本", "发布版本")
# 前瞻版本（下一里程碑），只提示不阻塞
README_SOFT = ("开发中", "下一里程碑", "下一个里程碑", "计划发布")
# 文件头注释里的版本（vX.Y.Z，或"版本 X.Y.Z"）
HEAD_VERSION_RE = re.compile(r"(?:版本|version|Version)[^\n]{0,12}?v?(\d+\.\d+\.\d+)|v(\d+\.\d+\.\d+)")

problems: list[str] = []
notes: list[str] = []


class Claim:
    """一条版本声明：哪个文件、第几行、什么措辞、写了哪个版本。"""

    __slots__ = ("value", "path", "lineno", "label")

    def __init__(self, value: str, path: str, lineno: int, label: str) -> None:
        self.value = value
        self.path = path
        self.lineno = lineno
        self.label = label

    def where(self, root: str) -> str:
        return f"{rel(root, self.path)}:{self.lineno}"


def rel(root: str, path: str) -> str:
    return os.path.relpath(path, root).replace(os.sep, "/")


def read_lines(path: str) -> list[str]:
    with open(path, "rb") as fh:
        return fh.read().decode("utf-8", errors="replace").splitlines()


def find_root() -> str:
    """仓库根：当前工作目录优先，认不出仓库再回退到脚本父目录的上一级（ci/ → 仓库根）。"""
    cwd = os.getcwd()
    if os.path.isfile(os.path.join(cwd, DKMS_CONF)) or os.path.isfile(os.path.join(cwd, DRIVER)):
        return cwd
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def collect_dkms(root: str) -> Claim | None:
    path = os.path.join(root, DKMS_CONF)
    if not os.path.isfile(path):
        problems.append(f"[版本] {DKMS_CONF}: 文件不存在，无法核对 `PACKAGE_VERSION`")
        return None
    for lineno, line in enumerate(read_lines(path), 1):
        match = re.search(r"^\s*PACKAGE_VERSION\s*=\s*[\"']?" + VERSION_RE.pattern, line)
        if match:
            return Claim(match.group(1), path, lineno, "`PACKAGE_VERSION`")
    problems.append(f"[版本] {DKMS_CONF}: 找不到 `PACKAGE_VERSION=\"x.y.z\"`（版本基准缺失）")
    return None


def collect_driver(root: str, claims: list[Claim]) -> None:
    path = os.path.join(root, DRIVER)
    if not os.path.isfile(path):
        problems.append(f"[版本] {rel(root, path)}: 文件不存在，无法核对 `MODULE_VERSION()`")
        return
    lines = read_lines(path)
    for lineno, line in enumerate(lines, 1):
        match = re.search(r"MODULE_VERSION\s*\(\s*[\"']" + VERSION_RE.pattern, line)
        if match:
            claims.append(Claim(match.group(1), path, lineno, "`MODULE_VERSION()`"))
    # 文件头注释（第一个 */ 之前）里若写了版本，同样当声明
    head: list[str] = []
    for line in lines:
        head.append(line)
        if "*/" in line:
            break
    head_text = "\n".join(head)
    match = HEAD_VERSION_RE.search(head_text)
    if match:
        value = match.group(1) or match.group(2)
        lineno = head_text[: match.start()].count("\n") + 1
        claims.append(Claim(value, path, lineno, "文件头注释版本"))
    else:
        notes.append(f"[版本·提示] {rel(root, path)} 文件头注释没写版本（跳过该项）")


def collect_changelog(root: str) -> Claim | None:
    path = os.path.join(root, CHANGELOG)
    if not os.path.isfile(path):
        problems.append(f"[版本] {CHANGELOG}: 文件不存在，无法核对最新已发布小节")
        return None
    for lineno, line in enumerate(read_lines(path), 1):
        # `## [Unreleased]` 与 `## [x.y.z] - 日期`；只认后者，第一条即最新已发布
        match = re.match(r"##\s*\[(\d+\.\d+\.\d+)\]\s*-\s*\S+", line)
        if match:
            return Claim(match.group(1), path, lineno, "最新已发布小节")
    problems.append(f"[版本] {CHANGELOG}: 找不到最新已发布小节 `## [x.y.z] - 日期`")
    return None


def collect_readme(root: str, claims: list[Claim]) -> None:
    path = os.path.join(root, README)
    if not os.path.isfile(path):
        problems.append(f"[版本] {README}: 文件不存在，无法核对版本声明")
        return
    hard: dict[tuple[int, str], Claim] = {}
    soft: dict[tuple[int, str], Claim] = {}
    for lineno, line in enumerate(read_lines(path), 1):
        for bucket, words, label in ((hard, README_HARD, "「{w}」声明"), (soft, README_SOFT, "「{w}」")):
            for word in words:
                pos = line.find(word)
                if pos < 0:
                    continue
                match = VERSION_RE.search(line, pos + len(word))
                if match:
                    bucket.setdefault((lineno, match.group(1)),
                                      Claim(match.group(1), path, lineno, label.format(w=word)))
    claims.extend(hard.values())
    for item in soft.values():
        notes.append(f"[版本·提示] {item.where(root)} {item.label} 写了 {item.value}（前瞻版本，不作硬约束）")
    if not hard:
        notes.append(f"[版本·提示] {README} 里没找到「当前版本 / 最新发布」类声明（跳过该项）")


def main() -> int:
    root, verbose = parse_args(sys.argv[1:])
    claims: list[Claim] = []
    dkms = collect_dkms(root)
    if dkms:
        claims.append(dkms)
    collect_driver(root, claims)
    changelog = collect_changelog(root)
    if changelog:
        claims.append(changelog)
    collect_readme(root, claims)

    # 基准：dkms.conf 的 PACKAGE_VERSION（没有它才退而求其次取多数票）
    base = dkms
    if base is None and claims:
        votes: dict[str, int] = {}
        for item in claims:
            votes[item.value] = votes.get(item.value, 0) + 1
        winner = max(votes, key=lambda key: votes[key])
        base = next(item for item in claims if item.value == winner)

    for item in claims:
        if base is not None and item is not base and item.value != base.value:
            problems.append(
                f"[版本] {item.where(root)} {item.label} 写 {item.value}，"
                f"与基准 {base.where(root)} {base.label} 的 {base.value} 不一致")

    out = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    if problems:
        print(f"发现 {len(problems)} 个问题：", file=out)
        for item in problems:
            print("  " + item, file=out)
    else:
        value = base.value if base else "?"
        sources = "、".join(item.where(root) for item in claims)
        print(f"OK：版本一致（{value}），{len(claims)} 处声明全部相同：{sources}", file=out)
    if verbose:
        print(f"\n核对到的版本声明 {len(claims)} 处：", file=out)
        for item in claims:
            print(f"  · {item.where(root)} {item.label} = {item.value}", file=out)
        if notes:
            print(f"\n提示 {len(notes)} 条：", file=out)
            for item in notes:
                print("  " + item, file=out)
    out.flush()
    return 1 if problems else 0


def parse_args(argv: list[str]) -> tuple[str, bool]:
    root, verbose = None, False
    index = 0
    while index < len(argv):
        if argv[index] == "--root" and index + 1 < len(argv):
            root = os.path.abspath(argv[index + 1])
            index += 2
            continue
        if argv[index] in ("-v", "--verbose"):
            verbose = True
        index += 1
    return root or find_root(), verbose


if __name__ == "__main__":
    sys.exit(main())

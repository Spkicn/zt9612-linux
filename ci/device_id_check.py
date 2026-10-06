#!/usr/bin/env python3
"""USB ID 一致性检查：驱动里的 `usb_device_id` 表与公开文档必须对得上。

**公开副本**：本文件位于 `ci/`，不在 .gitignore 的排除范围内，会随公开仓库发布，
公开 CI 上一定跑得到它（见 `.github/workflows/docs.yml`）。

为什么需要它：驱动里那张表（`zt_id_table` + `MODULE_DEVICE_TABLE`）决定内核会不会
把设备绑到这个驱动，而 `supported-device-IDs` 与 README 决定**用户能不能判断自己手里的卡
受不受支持**。两边一旦漂移，表现是"文档说支持、内核不绑"或反过来 —— 两者都很难现场排查。
同类 out-of-tree 驱动（如 aircrack-ng/rtl8812au）也在 CI 里做同类卫生检查。

检查项：
  1. `driver/zt9612.c` 里能解析出 VID/PID 与至少一条 `USB_DEVICE(...)`；
  2. 表里**没有重复**的 vid:pid（重复条目通常意味着抄错 ID）；
  3. `MODULE_DEVICE_TABLE(usb, ...)` 存在（缺了 `modinfo` 就没有 alias，udev 不会自动绑定）；
  4. 每条 id 的 `vvvv:pppp` 小写形式都出现在 `supported-device-IDs` 与 `README.md` 里。

用法：
    python ci/device_id_check.py           # 有问题退出码 1
    python ci/device_id_check.py -v        # 打印解析到的 ID
    python ci/device_id_check.py --root DIR

只读脚本。退出码：0 = 一致；1 = 有漂移或缺文件。
"""
from __future__ import annotations

import argparse
import os
import re
import sys

DRIVER = os.path.join("driver", "zt9612.c")
DOC_IDS = "supported-device-IDs"
DOC_README = "README.md"

DEFINE_RE = re.compile(r"^\s*#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(0[xX][0-9a-fA-F]+)\s*$", re.M)
USB_DEVICE_RE = re.compile(r"USB_DEVICE(?:_AND_INTERFACE_INFO)?\s*\(\s*([^,]+?)\s*,\s*([^,)]+?)\s*[,)]")
MODULE_TABLE_RE = re.compile(r"MODULE_DEVICE_TABLE\s*\(\s*usb\s*,")


def find_root(start: str) -> str:
    if os.path.isfile(os.path.join(start, "dkms.conf")) or os.path.isdir(os.path.join(start, "driver")):
        return start
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def resolve(token: str, macros: dict[str, int]) -> int | None:
    token = token.strip()
    if token in macros:
        return macros[token]
    if re.fullmatch(r"0[xX][0-9a-fA-F]+", token):
        return int(token, 16)
    return None


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")

    parser = argparse.ArgumentParser(description="USB ID 一致性检查")
    parser.add_argument("-v", "--verbose", action="store_true", help="打印解析到的 ID")
    parser.add_argument("--root", default=os.getcwd(), help="仓库根（默认当前目录）")
    args = parser.parse_args()

    root = find_root(args.root)
    problems: list[str] = []

    driver_path = os.path.join(root, DRIVER)
    if not os.path.isfile(driver_path):
        print(f"[ID] 找不到 {DRIVER}，无法核对", file=sys.stderr)
        return 1
    with open(driver_path, encoding="utf-8", errors="replace") as fh:
        src = fh.read()

    macros: dict[str, int] = {}
    for match in DEFINE_RE.finditer(src):
        macros[match.group(1)] = int(match.group(2), 16)

    ids: list[tuple[int, int]] = []
    for match in USB_DEVICE_RE.finditer(src):
        vid = resolve(match.group(1), macros)
        pid = resolve(match.group(2), macros)
        if vid is None or pid is None:
            problems.append(f"[ID] {DRIVER}: 无法解析 USB_DEVICE({match.group(1).strip()}, {match.group(2).strip()})")
            continue
        ids.append((vid, pid))

    if not ids:
        problems.append(f"[ID] {DRIVER}: 没解析到任何 USB_DEVICE 条目")

    if len(set(ids)) != len(ids):
        problems.append(f"[ID] {DRIVER}: USB_DEVICE 表里有重复条目（{len(ids)} 条 / {len(set(ids))} 个唯一）")

    if not MODULE_TABLE_RE.search(src):
        problems.append(f"[ID] {DRIVER}: 缺少 MODULE_DEVICE_TABLE(usb, ...) —— modinfo 不会有 alias，udev 不会自动绑定")

    wanted = {f"{vid:04x}:{pid:04x}" for vid, pid in ids}
    for doc in (DOC_IDS, DOC_README):
        path = os.path.join(root, doc)
        if not os.path.isfile(path):
            problems.append(f"[ID] 找不到 {doc}")
            continue
        with open(path, encoding="utf-8", errors="replace") as fh:
            text = fh.read().lower()
        for one in sorted(wanted):
            if one not in text:
                problems.append(f"[ID] {doc} 里没有出现 {one}（驱动 ID 表里有；文档与表已漂移）")

    if args.verbose:
        print(f"驱动 ID 表（{len(ids)} 条）：" + ", ".join(f"{v:04x}:{p:04x}" for v, p in ids))

    if problems:
        print(f"发现 {len(problems)} 个 ID 漂移问题。", file=sys.stderr)
        for item in problems:
            print(f"  {item}", file=sys.stderr)
        return 1

    print(f"OK：USB ID 表与 {DOC_IDS} / {DOC_README} 一致（{', '.join(sorted(wanted))}）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# 一键收集排障信息，产出一个可以直接贴进 GitHub issue 的文本文件。
#
# 为什么有这个脚本：issue 模板（.github/ISSUE_TEMPLATE/bug_report.md）要求的是
# lsusb / uname / dmesg / 固件校验 / 模块参数这一组信息，手工收集容易漏；
# 同类 out-of-tree 驱动仓库普遍提供同类脚本（如 morrownr 系列的 save-log.sh），
# 这里按本仓库要查的东西自己写一份。
#
# 用法：
#   ./scripts/collect-debug-info.sh              # 输出到当前目录
#   sudo ./scripts/collect-debug-info.sh /tmp    # 输出到 /tmp（dmesg 需要权限时用 sudo）
#
# 隐私提醒：dmesg 与 ip/iw 输出里可能带真实 MAC、SSID 与主机名。
# 提交到公开 issue 前请自己过一遍，必要时打码（厂商固件/凭据不要贴）。
#
# 只读脚本：不加载/卸载模块，不改任何配置，只把命令输出写进一个文件。
set -u

OUT_DIR=${1:-.}
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="$OUT_DIR/zt9612-debug-$STAMP.txt"

if ! touch "$OUT" 2>/dev/null; then
	echo "无法写入 $OUT —— 换个目录（或加 sudo）再试。" >&2
	exit 1
fi

# 打一个标题段
section() {
	printf '\n===== %s =====\n' "$1" >>"$OUT"
}

# 跑一条命令，把 stdout+stderr 都写进报告，失败也不中断
collect() {
	label=$1
	cmd=$2
	section "$label"
	sh -c "$cmd" >>"$OUT" 2>&1 || printf '(命令失败：%s)\n' "$cmd" >>"$OUT"
}

section "zt9612 排障信息（$(date -Is 2>/dev/null || date)）"
printf '生成命令：%s\n' "$0 $*" >>"$OUT"

collect "系统" 'uname -a; cat /etc/os-release 2>/dev/null | head -5'
collect "USB 设备（350b:）" 'lsusb 2>/dev/null | grep -i 350b || echo "(没有 350b: 设备)"'
collect "模块信息" 'command -v modinfo >/dev/null && modinfo zt9612 2>&1 | head -30 || echo "(modinfo 不可用)"'
collect "已加载模块" 'lsmod 2>/dev/null | grep -E "^(zt9612|mac80211|cfg80211|usbcore) " || echo "(没有加载)"'
collect "模块版本" 'cat /sys/module/zt9612/version 2>/dev/null || echo "(模块未加载)"'
collect "固件校验（应与 firmware/README.md 一致）" \
	'sha256sum /lib/firmware/zt9612_fw.bin /lib/firmware/zt9612_settings.bin 2>&1'
collect "接口状态" 'ip -br link 2>/dev/null; command -v iw >/dev/null && iw dev 2>/dev/null'
collect "/dev 通道" 'ls -l /dev/zt9612 2>&1 || echo "(没有 /dev/zt9612)"'
collect "模块参数当前值" 'for f in /sys/module/zt9612/parameters/*; do [ -e "$f" ] && printf "%s = %s\n" "$(basename "$f")" "$(cat "$f")"; done 2>/dev/null || echo "(模块未加载)"'
collect "Secure Boot" 'command -v mokutil >/dev/null && mokutil --sb-state 2>&1 || echo "(mokutil 不可用)"'
collect "dmesg（最近 200 行）" 'dmesg 2>/dev/null | tail -200 || sudo -n dmesg 2>/dev/null | tail -200 || echo "(读不到 dmesg：需要 root 或 kernel.dmesg_restrict=0)"'

section "提交前请检查"
printf '%s\n' \
	'- 上面的 dmesg / ip / iw 输出可能含真实 MAC、SSID、主机名，贴 issue 前先过一遍；' \
	'- 不要贴厂商固件本体与任何凭据；' \
	'- 顺带写上：期望发生什么、实际发生什么、是否每次都能复现。' >>"$OUT"

printf '已生成：%s\n' "$OUT"
printf '下一步：把它附到 issue 上（隐私内容先自己过一遍）。\n'

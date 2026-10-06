#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# 一次性配置：插上 ZT9612U 时自动从「驱动光盘模式」(350b:f179)
# 切到「网卡模式」(350b:9612)。
#
# 为什么需要它：芯片切模式后 ROM 只留出**秒级**的固件接收窗口，
# 手动切完再 insmod 常常已经错过；udev 自动切换能让 probe 紧跟枚举。
#
# 用法: sudo ./scripts/setup-mode-switch.sh
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
RULES_DST=/etc/udev/rules.d/99-zt9612-autoswitch.rules
MS_DIR=/etc/usb_modeswitch.d
MS_DST=$MS_DIR/350b:f179

if [ "$(id -u)" -ne 0 ]; then
    echo "请用 sudo 运行"; exit 1
fi

if ! command -v usb_modeswitch >/dev/null 2>&1; then
    echo "未安装 usb-modeswitch，请先：sudo apt install usb-modeswitch"
    exit 1
fi

install -m 644 "$HERE/udev/99-zt9612-autoswitch.rules" "$RULES_DST"
install -d "$MS_DIR"
install -m 644 "$HERE/usb_modeswitch.d/350b-f179.conf" "$MS_DST"

udevadm control --reload-rules
udevadm trigger --subsystem-match=usb

echo "已安装："
echo "  $RULES_DST"
echo "  $MS_DST"
echo
echo "现在拔插一次网卡，lsusb 应显示 350b:9612。"

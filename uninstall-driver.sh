#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# ZT9612U 驱动卸载脚本
#
# 默认只删除模块文件与配置，**不做 rmmod**：本项目观察到反复卸载/加载会让机器
# 变成"能 ping 不能 SSH"（见 README「已知问题」）。模块若已加载，重启即可消失。
#
# 用法:
#   sudo ./uninstall-driver.sh              # 删除文件，提示重启
#   sudo ./uninstall-driver.sh --force-rmmod  # 先尝试卸载已加载模块（有风险）
set -u

DRV_NAME=zt9612
SRC_DIR=$(cd "$(dirname "$0")" && pwd)
KVER=$(uname -r)
MODDESTDIR=/lib/modules/$KVER/extra
DKMS_DEST=/lib/modules/$KVER/updates/dkms
BLACKLIST=/etc/modprobe.d/$DRV_NAME-blacklist.conf

FORCE_RMMOD=0
for arg in "$@"; do
	case "$arg" in
		--force-rmmod) FORCE_RMMOD=1 ;;
		-h|--help) sed -n '3,15p' "$0"; exit 0 ;;
		*) echo "未知参数: $arg（用 --help 看用法）"; exit 1 ;;
	esac
done

if [ "$(id -u)" -ne 0 ]; then
	echo "请用 sudo 运行"; exit 1
fi

removed=0

if command -v dkms >/dev/null 2>&1 && [ -f "$SRC_DIR/dkms.conf" ]; then
	# shellcheck disable=SC1091
	. "$SRC_DIR/dkms.conf"
	if dkms status "$PACKAGE_NAME/$PACKAGE_VERSION" 2>/dev/null | grep -q .; then
		echo "dkms remove $PACKAGE_NAME/$PACKAGE_VERSION --all"
		dkms remove "$PACKAGE_NAME/$PACKAGE_VERSION" --all && removed=1
	fi
fi

for f in "$MODDESTDIR/$DRV_NAME.ko" "$DKMS_DEST/$DRV_NAME.ko"; do
	if [ -f "$f" ]; then
		rm -f "$f" && echo "已删除 $f" && removed=1
	fi
done

if [ -f "$BLACKLIST" ]; then
	rm -f "$BLACKLIST" && echo "已删除 $BLACKLIST" && removed=1
fi

/sbin/depmod -a "$KVER"

if [ "$removed" -eq 0 ]; then
	echo "没有找到已安装的 $DRV_NAME（可能本来就没装）"
fi

if lsmod | grep -q "^$DRV_NAME"; then
	if [ "$FORCE_RMMOD" -eq 1 ]; then
		echo
		echo "⚠️  正在 rmmod $DRV_NAME ...（若命令长时间不返回，说明命中了已知的卸载死锁）"
		rmmod "$DRV_NAME" && echo "已卸载" || echo "rmmod 失败"
	else
		echo
		echo "注意：模块当前已加载，本脚本默认不 rmmod（已知卸载路径有挂机风险）。"
		echo "      重启机器即消失；确实要现在卸载可加 --force-rmmod。"
	fi
fi

echo
echo "完成。建议重启机器让状态彻底干净。"

#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# ZT9612U (ZTOP / ACEV100) USB Wi-Fi 驱动安装脚本
#
# 当前驱动能力（务必先读）：
#   已实现并实机验证：固件装载（M1）、IPC 初始化 + /dev/zt9612 通道（M2）、
#   mac80211 接口（M3.1）、双频扫描（M3.2/M3.5）、关联（M3.3）、数据面（M3.4）、
#   WPA2-PSK 加密（CCMP 由 mac80211 软件加解密）
#   ⇒ 装完手动加载后可以得到一个 managed 模式的无线接口（名字按 MAC 生成，
#     形如 wlx + 12 位十六进制），能扫描、关联、DHCP、ping 外网。
#   实测吞吐约 4.2~4.7 Mbit/s（注意：`iw link` 报的 1.0 Mbit/s **不是**真实发射速率，
#   速率由固件内部的 ARRM 模块自主决定、主机侧改不动，见 README「已知问题」）。
#   本脚本只负责把模块正确编译、签名、安装，并把已知风险（见 README「已知问题」）挡住。
#
# 用法:
#   sudo ./install-driver.sh                # 默认：编译 + 安装，保留 blacklist（不自动加载）
#   sudo ./install-driver.sh --enable-autoload   # 允许开机/插卡自动加载（有已知风险）
#   sudo ./install-driver.sh --dkms          # 用 DKMS 安装（内核升级自动重建）
set -u

DRV_NAME=zt9612
SRC_DIR=$(cd "$(dirname "$0")" && pwd)
FW_DIR=/lib/firmware
FW_SHA=8562003c3558c7f608d5ff926d368d6de021c1ae12ac833978ea7c4e5a9ebe2d
SET_SHA=58c2d73a4d2a1c1eb10a606a6e0bf6a201fb8f288fce1cbe0ce5364f7271fb46

ENABLE_AUTOLOAD=0
USE_DKMS=0

for arg in "$@"; do
	case "$arg" in
		--enable-autoload) ENABLE_AUTOLOAD=1 ;;
		--dkms)            USE_DKMS=1 ;;
		-h|--help)
			sed -n '3,20p' "$0"; exit 0 ;;
		*)
			echo "未知参数: $arg（用 --help 看用法）"; exit 1 ;;
	esac
done

if [ "$(id -u)" -ne 0 ]; then
	echo "请用 sudo 运行"; exit 1
fi

KVER=$(uname -r)
KDIR=/lib/modules/$KVER/build
MODDESTDIR=/lib/modules/$KVER/extra
BLACKLIST=/etc/modprobe.d/$DRV_NAME-blacklist.conf

echo "=============================================================="
echo " ZT9612U 驱动安装"
echo "--------------------------------------------------------------"
echo " 源码目录   : $SRC_DIR"
echo " 内核版本   : $KVER"
echo " 架构       : $(uname -m)"
echo " 内存/核心  : $(LC_ALL=C free -m | awk '/Mem:/ {print $2" MB"}') / $(nproc) cores"
echo " 编译器     : $(gcc --version 2>/dev/null | head -1)"
echo " DKMS       : $(dkms --version 2>/dev/null || echo '未安装')"
echo " SecureBoot : $(mokutil --sb-state 2>/dev/null || echo '未知（mokutil 未安装）')"
echo " 设备       : $(lsusb 2>/dev/null | grep -i '350b' || echo '未检测到 350b:xxxx 设备')"
echo "=============================================================="

# ---- 1. 依赖检查 -----------------------------------------------------------
missing=""
for tool in make gcc; do
	command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
[ -d "$KDIR" ] || missing="$missing linux-headers-$KVER"
if [ -n "$missing" ]; then
	echo "缺少依赖:$missing"
	echo "  sudo apt install -y build-essential linux-headers-\$(uname -r)"
	exit 1
fi

# ---- 2. 固件检查（本仓库不附带固件，见 firmware/README.md）-----------------
fw_ok=0
if [ -f "$FW_DIR/${DRV_NAME}_fw.bin" ]; then
	got=$(sha256sum "$FW_DIR/${DRV_NAME}_fw.bin" | cut -d' ' -f1)
	if [ "$got" = "$FW_SHA" ]; then
		fw_ok=1
	else
		echo "警告：$FW_DIR/${DRV_NAME}_fw.bin 的 SHA-256 与已知版本不符"
		echo "  期望 $FW_SHA"
		echo "  实际 $got"
	fi
fi
if [ "$fw_ok" -ne 1 ]; then
	echo
	echo "缺少固件：$FW_DIR/${DRV_NAME}_fw.bin（+ ${DRV_NAME}_settings.bin）"
	echo "本仓库不附带厂商固件（版权原因，见 firmware/README.md）。请自行取得后："
	echo "  sudo $SRC_DIR/scripts/install-firmware.sh <固件所在目录>"
	echo
	printf "仍要继续安装模块吗？[y/N] "
	read -r ans
	case "$ans" in y|Y) ;; *) echo "已取消"; exit 1 ;; esac
fi

# ---- 3. 切模式（光盘模式 -> 网卡模式）--------------------------------------
if lsusb 2>/dev/null | grep -qi '350b:f179'; then
	echo "设备处于驱动光盘模式（350b:f179），尝试切换 ..."
	"$SRC_DIR/scripts/switch-to-wifi-mode.sh" || true
fi

# ---- 4. Secure Boot 签名准备 ----------------------------------------------
SB=$(mokutil --sb-state 2>/dev/null || true)
if [ -n "$SB" ] && echo "$SB" | grep -qi 'enabled'; then
	if [ ! -f "$SRC_DIR/driver/MOK.priv" ] || [ ! -f "$SRC_DIR/driver/MOK.pem" ]; then
		echo
		echo "Secure Boot 已启用，但没有找到 MOK 密钥（driver/MOK.priv, driver/MOK.pem）。"
		printf "现在生成一对新密钥并导入 MOK？(需要重启并在 MOK Manager 里确认) [y/N] "
		read -r ans
		case "$ans" in
		y|Y)
			openssl req -new -x509 -newkey rsa:2048 \
				-keyout "$SRC_DIR/driver/MOK.priv" \
				-outform DER -out "$SRC_DIR/driver/MOK.der" \
				-nodes -days 36500 -subj "/CN=zt9612 module signing/"
			openssl x509 -inform DER -in "$SRC_DIR/driver/MOK.der" \
				-out "$SRC_DIR/driver/MOK.pem"
			echo "已生成 driver/MOK.{priv,der,pem}"
			echo "接下来会调用 mokutil --import，请设置一个一次性密码，重启时在"
			echo "蓝色的 MOK Manager 界面里选择 Enroll MOK 并输入它。"
			mokutil --import "$SRC_DIR/driver/MOK.der" || true
			echo
			echo "请重启机器完成 MOK 注册，然后重新运行本脚本。"
			exit 0
			;;
		*) echo "跳过签名；未签名模块在 Secure Boot 机器上无法加载。"; exit 1 ;;
		esac
	fi
	echo "Secure Boot 已启用：将使用 driver/MOK.{priv,pem} 签名模块"
fi

# ---- 5. 编译 / 签名 / 安装 -------------------------------------------------
cd "$SRC_DIR/driver" || exit 1

if [ "$USE_DKMS" -eq 1 ]; then
	if ! command -v dkms >/dev/null 2>&1; then
		echo "--dkms 需要先安装 dkms：sudo apt install dkms"; exit 1
	fi
	# shellcheck disable=SC1091
	. "$SRC_DIR/dkms.conf"
	echo "DKMS 安装 $PACKAGE_NAME/$PACKAGE_VERSION ..."
	dkms remove "$PACKAGE_NAME/$PACKAGE_VERSION" --all >/dev/null 2>&1 || true
	dkms install "$SRC_DIR" || { echo "DKMS 安装失败，日志："; exit 1; }
else
	if [ -n "$SB" ] && echo "$SB" | grep -qi 'enabled'; then
		make -j"$(nproc)" sign || exit 1
	else
		make -j"$(nproc)" || exit 1
	fi
	make install || exit 1
fi

# ---- 6. 自动加载策略（默认关闭，见 README「已知问题」）---------------------
if [ "$ENABLE_AUTOLOAD" -eq 1 ]; then
	rm -f "$BLACKLIST"
	echo
	echo "注意：已启用自动加载。插上网卡（或开机）就会自动 probe 并装载固件。"
	echo "      曾观察到这条路径让机器启动后无法登录，根因是驱动漏设 wiphy 的父设备"
	echo "      （缺少 SET_IEEE80211_DEV()），已在 v0.2.0 修复，但「干净开机自动加载」"
	echo "      这条完整路径尚未重新复验过（见 README 的「已知问题」）。"
	echo "      如果开机后机器异常，请参考 README 中的恢复步骤。"
else
	printf 'blacklist %s\n' "$DRV_NAME" > "$BLACKLIST"
	echo
	echo "已写入 $BLACKLIST：插卡/开机**不会**自动加载本驱动（规避已知风险）。"
	echo "手动加载：sudo modprobe $DRV_NAME        # 想看日志先 sudo dmesg -C"
	echo "需要自动加载时重跑本脚本并加 --enable-autoload"
fi

# ---- 7. 手动加载一次并收集日志 --------------------------------------------
echo
echo "--------------------------------------------------------------"
echo "现在手动加载一次（约 7 秒，其中 MM_START 要等约 6.5 秒）..."
dmesg -C
if modprobe "$DRV_NAME" 2>/dev/null || insmod "$MODDESTDIR/$DRV_NAME.ko"; then
	sleep 8
	dmesg | tail -30
	echo
	echo "设备节点：$(ls -l /dev/$DRV_NAME 2>/dev/null || echo '未出现（看上面的 dmesg）')"
	echo "网络接口：$(ls /sys/class/net 2>/dev/null | grep -E '^(wlx|wlan)' | head -1 || echo '未出现（看上面的 dmesg；接口名按 MAC 生成，不一定是 wlan0）')"
else
	echo "加载失败，请把上面的 dmesg 贴到 issue 里"
fi

echo
echo "完成。收尾建议："
echo "  * 不要反复 rmmod/insmod（已知会把机器搞成无法登录，见 README「已知问题」）"
echo "  * 需要换模块时：重启机器"
echo "  * 卸载：sudo ./uninstall-driver.sh"

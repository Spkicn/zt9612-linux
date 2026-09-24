#!/bin/bash
# ZT9612U (ZTop / 兆通微 ACEV100) 无线网卡：从"驱动光盘模式"切到"网卡模式"
# 机制已实测确认：与 Windows 上厂商 Auto_eject.exe 完全相同 —— 标准 SCSI 媒体弹出
#   Windows: IOCTL_STORAGE_EJECT_MEDIA
#   Linux  : STANDARD SCSI EJECT (eject / sg_start --eject / usb_modeswitch -K)
set -e

found=""
for d in /dev/sr*; do
    [ -e "$d" ] || continue
    vid=$(udevadm info -q property -n "$d" 2>/dev/null | sed -n 's/^ID_VENDOR_ID=//p')
    pid=$(udevadm info -q property -n "$d" 2>/dev/null | sed -n 's/^ID_MODEL_ID=//p')
    if [ "$vid" = "350b" ] && [ "$pid" = "f179" ]; then
        found="$d"
        break
    fi
done

if [ -z "$found" ]; then
    echo "没找到处于光驱模式的 350b:f179 设备（没插 / 已切换 / 被 usb-storage 占用）"
    echo "当前 USB 中的 350b 设备："
    lsusb | grep -i '350b' || true
    exit 0
fi

echo "找到 $found，发送弹出命令把它切到网卡模式 ..."
eject "$found" || usb_modeswitch -K -W -v 350b -p f179
sleep 3
echo "切换后应该是 ID 350b:9612："
lsusb | grep -i '350b' || true

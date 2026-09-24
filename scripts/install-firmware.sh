#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# 把固件与配置块安装到 /lib/firmware/，并校验 SHA-256。
#
# 本仓库**不包含**厂商固件（版权原因）。请先自行取得这两个文件：
#   zt9612_fw.bin        219076 字节  来自网卡自带驱动光盘（运行 auto_load.exe 后从安装目录取出）
#   zt9612_settings.bin     212 字节  同上（或由 rwnx_settings.ini 提取）
#
# 用法:
#   sudo ./scripts/install-firmware.sh <目录或文件...>
#   sudo ./scripts/install-firmware.sh ~/zt9612_fw.bin ~/zt9612_settings.bin
set -e

FW_SHA=8562003c3558c7f608d5ff926d368d6de021c1ae12ac833978ea7c4e5a9ebe2d
SET_SHA=58c2d73a4d2a1c1eb10a606a6e0bf6a201fb8f288fce1cbe0ce5364f7271fb46
DEST=${DEST:-/lib/firmware}

if [ "$#" -eq 0 ]; then
    echo "用法: sudo $0 <包含固件的目录或文件...>"
    exit 1
fi

# 收集候选文件：目录参数展开为目录下的 .bin
cands=()
for arg in "$@"; do
    if [ -d "$arg" ]; then
        while IFS= read -r f; do cands+=("$f"); done < <(find "$arg" -maxdepth 2 -name '*.bin' -type f)
    elif [ -f "$arg" ]; then
        cands+=("$arg")
    else
        echo "跳过（不存在）: $arg"
    fi
done

if [ "${#cands[@]}" -eq 0 ]; then
    echo "没有找到任何 .bin 文件"
    exit 1
fi

install_one() {
    local name=$1 want_sha=$2 want_size=$3 src=""
    for f in "${cands[@]}"; do
        [ "$(basename "$f")" = "$name" ] || continue
        src=$f
        break
    done
    if [ -z "$src" ]; then
        # 名字不匹配时按大小+哈希猜
        for f in "${cands[@]}"; do
            if [ "$(stat -c %s "$f")" = "$want_size" ]; then src=$f; break; fi
        done
    fi
    if [ -z "$src" ]; then
        echo "缺少 $name（未在参数中找到）"
        return 1
    fi

    got=$(sha256sum "$src" | cut -d' ' -f1)
    if [ "$got" != "$want_sha" ]; then
        echo "错误：$src 的 SHA-256 不匹配"
        echo "  期望 $want_sha"
        echo "  实际 $got"
        echo "（可能取到了别的型号/版本的固件；请确认来源）"
        return 1
    fi

    install -m 644 "$src" "$DEST/$name"
    echo "已安装 $DEST/$name  ($(stat -c %s "$src") 字节, sha256 校验通过)"
}

mkdir -p "$DEST"
install_one zt9612_fw.bin       "$FW_SHA"  219076
install_one zt9612_settings.bin "$SET_SHA"    212

echo
echo "完成。驱动 probe 时会 request_firmware() 这两个文件。"

#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# 供 dkms.conf 调用：按可用内存限制并行编译数，避免小内存机器 OOM。
# （不要在终端里直接跑，它是给 DKMS 用的。）
#
# 设计参考：morrownr 系列驱动仓库的 dkms-make.sh（GPL-2.0）做法。

SMEM=$(LC_ALL=C free | awk '/Mem:/ { print $2 }')
sproc=$(nproc)

if [ "$sproc" -gt 1 ]; then
	if [ "$SMEM" -lt 1400000 ]; then sproc=2; fi
	if [ "$SMEM" -lt 700000 ];  then sproc=1; fi
fi

kernelver=${kernelver:-$(uname -r)}

exec make -j"$sproc" -C driver KVER="$kernelver" KDIR="/lib/modules/$kernelver/build" modules

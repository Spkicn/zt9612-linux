#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""ZT9612U /dev/zt9612 IPC 往返自测（M2 验收工具）

前提：驱动已加载（sudo modprobe zt9612），且 /dev/zt9612 存在。
只依赖 Python 标准库，不需要 pyusb。

用法:
    sudo python3 scripts/zt9612-devtest.py              # 两条往返测试
    sudo python3 scripts/zt9612-devtest.py --seconds 20 # 之后持续探测稳定性（心跳是否在跑）
"""
import argparse
import os
import select
import struct
import sys
import time

DEV = "/dev/zt9612"

# 主机 -> 设备的 MM 段消息（编号与开源 rwnx 的 MM 枚举一致，仅列常用项）
MM_REQ = {
    0x0000: "MM_RESET_REQ",
    0x0002: "MM_START_REQ",
    0x0004: "MM_VERSION_REQ",
    0x0006: "MM_ADD_IF_REQ",
    0x0010: "MM_SET_CHANNEL_REQ",
    0x0020: "MM_SET_SLOTTIME_REQ",
    0x0022: "MM_SET_IDLE_REQ",
}
# 设备 -> 主机的确认
MM_CFM = {mid + 1: name.replace("_REQ", "_CFM") for mid, name in MM_REQ.items()}
MM_CFM[0x0101] = "vendor(0x0101) MAC"
MM_CFM[0x0200] = "boot notify"


def name(mid):
    return MM_REQ.get(mid) or MM_CFM.get(mid) or "?"


def frame(mid, params=b"", dest=0, src=100, typ=0x0100):
    """构造一条 WLAN 帧：magic + hlen + type + lmac_msg"""
    msg = struct.pack("<HHHH", mid, dest, src, len(params)) + params
    return b"WLAN" + struct.pack("<HH", len(msg), typ) + msg


def read_frame(fd, timeout):
    r, _, _ = select.select([fd], [], [], timeout)
    if not r:
        return None
    return os.read(fd, 2048)


def roundtrip(fd, mid, params, want, timeout=3.0):
    """发一条、等对应 CFM，返回 (成功?, 描述)"""
    os.write(fd, frame(mid, params))
    end = time.time() + timeout
    while time.time() < end:
        f = read_frame(fd, 0.5)
        if not f or len(f) < 16 or f[:4] != b"WLAN":
            continue
        rmid = struct.unpack_from("<H", f, 8)[0]
        plen = struct.unpack_from("<H", f, 14)[0]
        payload = f[16:16 + plen]
        if rmid == want:
            return True, "-> %s (%#06x)\n   <- %s (%#06x) len=%d %s" % (
                name(mid), mid, name(rmid), rmid, plen, payload[:24].hex())
        # 其它消息（事件 0x0300 等）顺带打印，便于定位问题
        print("   ... 顺带收到 %#06x len=%d" % (rmid, plen))
    return False, "-> %s (%#06x)\n   <- 无响应（超时 %.1fs）" % (name(mid), mid, timeout)


def main():
    ap = argparse.ArgumentParser(description="/dev/zt9612 IPC 往返自测")
    ap.add_argument("--seconds", type=int, default=0,
                    help="往返测试后继续每隔 5 秒探测一次，持续这么多秒（检查心跳/稳定性）")
    args = ap.parse_args()

    if not os.path.exists(DEV):
        print("找不到 %s —— 驱动是否已加载？(sudo modprobe zt9612; sudo dmesg | tail -30)" % DEV)
        return 1

    fd = os.open(DEV, os.O_RDWR)
    print("已打开 %s" % DEV)

    tests = [
        (0x0004, b"", 0x0005, "固件版本"),
        (0x0100, b"\x00", 0x0101, "厂商私有：取 MAC"),
    ]
    ok = 0
    for mid, params, want, label in tests:
        print("\n[%s]" % label)
        good, detail = roundtrip(fd, mid, params, want)
        print("   " + detail)
        ok += 1 if good else 0

    print("\n往返成功 %d/%d" % (ok, len(tests)))

    if args.seconds > 0:
        print("\n继续探测 %d 秒（每 5 秒一条 MM_VERSION_REQ，能回说明固件没被看门狗复位）..." % args.seconds)
        end = time.time() + args.seconds
        n = alive = 0
        while time.time() < end:
            time.sleep(5)
            n += 1
            good, _ = roundtrip(fd, 0x0004, b"", 0x0005, timeout=2.0)
            alive += 1 if good else 0
            print("   [%2d] %s" % (n, "OK" if good else "无响应（异常）"))
        print("稳定性：%d/%d 次有响应" % (alive, n))

    os.close(fd)
    return 0 if ok == len(tests) else 1


if __name__ == "__main__":
    sys.exit(main())

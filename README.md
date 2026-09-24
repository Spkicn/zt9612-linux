<div align="center">

# zt9612 — ZT9612U (ZTOP / ACEV100) USB Wi-Fi 6 driver for Linux

Linux 内核驱动，用于 **VID:PID `350b:9612`** 的 ZT9612U / ACEV100 USB 无线网卡

[![build](https://github.com/Spkicn/zt9612-linux/actions/workflows/build.yml/badge.svg)](https://github.com/Spkicn/zt9612-linux/actions/workflows/build.yml)
[![checkpatch](https://github.com/Spkicn/zt9612-linux/actions/workflows/checkpatch.yml/badge.svg)](https://github.com/Spkicn/zt9612-linux/actions/workflows/checkpatch.yml)
[![License: GPL-2.0-only](https://img.shields.io/badge/License-GPL--2.0--only-blue.svg)](LICENSE)

</div>

> ## ⚠️ 项目状态：实验性半成品（0.1.0）
>
> | 里程碑 | 状态 |
> |---|---|
> | 硬件识别 / 协议逆向 | ✅ 完成 |
> | **M1 内核态固件装载** | ✅ 已实现，实机验证通过 |
> | **M2 IPC 初始化 + `/dev/zt9612`** | ✅ 已实现，实机验证通过（IPC 往返成功） |
> | **M3.1 mac80211 注册 / `wlan0`** | 🟡 代码已就绪，**尚未实机验证** |
> | M3.2 扫描 / M3.3 关联 / M3.4 数据面 | ⬜ 未开始 |
>
> **现在加载本驱动不会得到可用的 Wi-Fi 网络接口。** 它做的是：把固件装进芯片、
> 完成同步初始化、建立 `/dev/zt9612` 原始 IPC 通道。距离"能上网"还差 M3.2~M3.4。
>
> 这块卡在 Windows 上是正常工作的；本项目是为了在 Linux 上把它跑起来而做的
> 逆向 + 驱动实现。**厂商没有公开 Linux 驱动**（有内部版本，需向厂商索取）。

## 硬件

| 项目 | 值 |
|---|---|
| 厂商 | ZTOP / 山东兆通微电子（Zhaotong Micro） |
| 模组 | **ZT9612U V1.0**（FCC ID `2BOBE-ZT9612UV10`） |
| 芯片平台 | **ACEV100**（自研 Wi-Fi 6 2T2R SoC，非 Realtek / MediaTek 方案） |
| 网卡模式 | `350b:9612`，bcdDevice `0x0200`，`802.11ax 2x2 WLAN Adapter` |
| 光盘模式 | `350b:f179`（USB CD-ROM，卷标 "Wi-Fi6 Adapter"） |
| 接口 | 1 个，class `FF/FF/FF`，5 个 512 字节批量端点（EP4-IN / EP5~8-OUT） |
| 固件 | `zt9612_fw.bin`（219,076 B）+ `zt9612_settings.bin`（212 B） |

支持范围与 ID 判定方法见 [`supported-device-IDs`](supported-device-IDs)。
**同厂其它型号（`350b:9101`、`350b:9611` 等）不适用。**

## 现在能做什么 / 不能做什么

**能：**
- 把设备从"驱动光盘模式"切到"网卡模式"（标准 SCSI 弹出，非厂商私有命令）
- 完整复现固件下载协议：`hello` 握手 → 488B/块写入 → 末块整段 XOR16 校验 →
  配置块 → RUN（实机验证：449 块，校验 `0x2d14`）
- 完成同步 IPC 初始化（`MM_RESET` → `MM_VERSION` → 厂商私有段 → `MM_START`（约 6.5 s）
  → `MM_SET_IDLE` / `MM_ADD_IF` / `MM_SET_SLOTTIME` / `MM_SET_CHANNEL`）
- 5 秒心跳维持固件存活；在 `/dev/zt9612` 上收发原始 `WLAN` 帧

**不能：**
- ❌ 出现 `wlan0`（M3.1 未验证）
- ❌ 扫描 / 关联 / 传输数据（M3.2~M3.4）
- ❌ 5 GHz、AP 模式、监听模式、蓝牙（都不在范围内）

## 兼容性

| 项目 | 值 |
|---|---|
| **已实机验证** | `7.0.0-31-generic`（Ubuntu 24.04.5 LTS，x86_64）—— M1/M2 通过 |
| **已验证可编译** | `6.17.0-1022-azure`（CI，ubuntu-24.04 runner，**零告警**）；`modinfo` 正确生成 `alias: usb:v350Bp9612d*` |
| **编译下限** | **6.12**（`dkms.conf` 的 `BUILD_EXCLUSIVE_KERNEL`）：驱动使用 6.12 才引入的 `linux/unaligned.h`；CI 实测 6.8 内核因缺该头文件编译失败 |
| 未验证区间 | 6.12~6.16 能否正确工作未验证；mac80211 ops 签名只在 6.17+/7.0 上确认匹配 |
| 需要 | `build-essential`、`linux-headers-$(uname -r)` |

> 本驱动使用较新的 mac80211 ops 签名（例如
> `config(struct ieee80211_hw *, int radio_idx, u32 changed)`、
> `tx(struct ieee80211_hw *, struct ieee80211_tx_control *, struct sk_buff *)`）。
> 老内核上需要适配，欢迎提 PR（附完整报错与 `uname -a`）。

## 安装

### 0) 准备：固件（必须自备，仓库不附带）

固件版权属于厂商，本仓库不再分发。请从你自己的网卡配套驱动盘里取
（Windows 下运行 `auto_load.exe` 后在安装目录找 `zt9612_fw.bin` / `zt9612_settings.bin`），
或向厂商索取。取到后：

```bash
sudo ./scripts/install-firmware.sh /path/to/firmware-dir    # 会校验 SHA-256
```

细节（文件大小、哈希、来源）见 [`firmware/README.md`](firmware/README.md)。

### 1) 切模式（设备默认是"光驱"，不是网卡）

```bash
sudo ./scripts/setup-mode-switch.sh     # 一次性配置 udev 自动切换（推荐）
# 手动切一次：
sudo eject /dev/sr0
# 或 sudo usb_modeswitch -K -W -v 350b -p f179
```

切换后 `lsusb` 应显示 `350b:9612`。**切完要尽快加载驱动** —— 芯片 ROM 只留出秒级窗口
接受固件下载，这正是 udev 自动切换存在的意义。

### 2) 安装驱动

```bash
sudo ./install-driver.sh                       # 编译 +（必要时签名）+ 安装 + 手动加载一次
sudo ./install-driver.sh --dkms                # 走 DKMS，内核升级自动重建
sudo ./install-driver.sh --enable-autoload     # 允许插卡/开机自动加载（见「已知问题」）
```

也可以直接用 DKMS（`dkms.conf` 在仓库根目录，版本 `0.1.0`）：

```bash
sudo dkms install .                            # add + build + install 一步到位
dkms status                                    # 查看状态
sudo dkms remove zt9612/0.1.0 --all            # 移除
```

默认行为：安装模块到 `/lib/modules/$(uname -r)/extra/`，并写入
`/etc/modprobe.d/zt9612-blacklist.conf`（**只挡自动加载，手动 `modprobe` 仍可用**），
然后手动加载一次并打印 dmesg。

### 2') 手动编译（等价流程）

```bash
cd driver
make                       # 编译 zt9612.ko
make sign                  # Secure Boot 机器：用 MOK 密钥签名
sudo make install          # 安装到 /lib/modules/$(uname -r)/extra/ + depmod
sudo modprobe zt9612
```

可用目标见 `make help`；可覆盖变量：`KVER` / `KSRC`（或 `KERNEL_SRC`）/ `JOBS`。

### 3) Secure Boot 签名

Secure Boot 打开时内核处于 `lockdown=integrity`，会拒绝未签名模块。二选一：

```bash
# A. 关闭 Secure Boot（BIOS 里最简单）
# B. 自签 + 注册 MOK（一次性）
cd driver
openssl req -new -x509 -newkey rsa:2048 -keyout MOK.priv -outform DER -out MOK.der \
    -nodes -days 36500 -subj "/CN=zt9612 module signing/"
openssl x509 -inform DER -in MOK.der -out MOK.pem
sudo mokutil --import MOK.der     # 设一次性密码
sudo reboot                       # 开机蓝屏：Enroll MOK → Continue → 输入密码
make sign
```

`install-driver.sh` 检测到 Secure Boot 且没有密钥时会引导你走这一步。
`MOK.*` 已被 `.gitignore` 排除，**永远不要提交**。

## 验收：加载后应该看到什么

```bash
sudo dmesg -C
sudo modprobe zt9612            # 约 7 秒（其中 MM_START 要等约 6.5 秒）
sudo dmesg | tail -30
```

正常输出（实机实测时间线）：

```
zt9612 1-9:1.0: probing 350b:9612 (iface 0)
zt9612 1-9:1.0: hello ack: yes (try 1)
zt9612 1-9:1.0:   wrote addr=0x61070000 len=219048 (449 blocks) cs=0x2d14
zt9612 1-9:1.0:   wrote addr=0x210ce700 len=212 (1 blocks) cs=0x0001
zt9612 1-9:1.0: RUN (1 sections)
zt9612 1-9:1.0: boot notify: type=0x0100 len=16
zt9612 1-9:1.0: firmware loaded
zt9612 1-9:1.0: === IPC init sequence ===
zt9612 1-9:1.0: MAC = 00:b4:01:1a:00:12
zt9612 1-9:1.0: MM_START_REQ (rf init, waiting ~6.5s)
zt9612 1-9:1.0: MM_START_CFM received (firmware up)
zt9612 1-9:1.0: M1+M2 done: firmware running, /dev/zt9612 ready
zt9612 1-9:1.0: mac80211 registered (M3.1) - wlan0 should appear
```

然后跑 IPC 往返自测（M2 验收）：

```bash
sudo python3 scripts/zt9612-devtest.py
# 期望：往返成功 2/2（MM_VERSION_REQ → CFM；厂商 0x0100 → 返回 MAC）
sudo python3 scripts/zt9612-devtest.py --seconds 20    # 顺带验证心跳稳定性
```

`wlan0` 是否出现是 M3.1 的验收点 —— 目前**尚未确认**，欢迎反馈你的结果。

## 已知问题（重要，务必读）

### 1. 不要反复 `rmmod` / `insmod`

连续卸载/加载曾两次把整机搞成"能 ping、能连 22 端口，但 SSH 永远读不到 banner"
（内核活着、用户态起不来），只能硬重启。卸载路径已加固
（`usb_poison_urb()` + 2 秒上限，超时故意泄漏实例而不无界阻塞），但硬件/控制器层面
是否仍会被拖住没有证据排除。**换模块请重启机器。** `uninstall-driver.sh` 默认不 `rmmod`。

### 2. 开机自动加载有未定位的风险

把模块放进 `/lib/modules/$(uname -r)/extra/` 后，内核会通过 USB modalias 在开机时
自动加载它；本项目观察到这条路径导致开机后失联（同上的半死症状），且该次没有任何
`rmmod`/`insmod` 动作，所以与"卸载死锁"无关。可疑方向：xhci 枚举抖动、probe 在 udev
worker 中耗时约 7 秒、mac80211 注册后 NetworkManager 的动作。**根因尚未定位。**

因此 `install-driver.sh` 默认写 `blacklist zt9612`。确认你的机器安全后再
`--enable-autoload`。若机器已经失联：硬重启 → 物理拔插网卡 → 保留 blacklist。

### 3. 芯片挂死后只能物理拔插

`-110` / `can't set config #1` / `Entity not found` 表示芯片已挂死，软件复位无效。
触发原因通常是：固件运行中又灌了一次固件、切模式后拖太久、不等 CFM 连发 IPC 导致
固件断言崩溃（事件里能看到 `macif.c` 字样）。**拔下等 10 秒再插。**

### 4. 设备会自己变回光驱（`350b:f179`）

固件看门狗复位：主机超过 5 秒没发心跳。内核驱动用 `delayed_work` 每 5 秒发一次；
如果你在写自己的用户态脚本，请自行维持心跳。

### 5. 其他

- 驱动源码中部分中文注释在早期编辑中损坏成乱码（不影响编译），待清理
- 只支持 2.4 GHz 与 STA 模式；无蓝牙 / AP / 监听
- `0.x` 版本的接口（模块参数、`/dev` 协议）可能变化

## 调试

```bash
sudo dmesg -C && sudo modprobe zt9612 && sudo dmesg | tail -30
ls -l /dev/zt9612                        # M2 的原始 IPC 通道
cat /sys/module/zt9612/parameters/*      # 模块参数当前值
```

模块参数（详见 [`zt9612.conf`](zt9612.conf)）：

| 参数 | 默认 | 说明 |
|---|---|---|
| `do_init` | 1 | 固件装载后是否执行同步初始化序列；`0` = 只装载固件 |
| `do_boot` | 1 | 是否下载固件；`0` = 复用已在运行的固件，只做 IPC 初始化 |

`/dev/zt9612` 接口：`write()` 传入完整 `WLAN` 帧（`"WLAN" + u16 hlen + u16 type + payload`）
原样发到 EP8；`read()` 返回一条设备发来的完整帧（阻塞、3 秒超时，支持 `O_NONBLOCK`）。

> **内核 taint 提示（不是 bug）**：任何外部（out-of-tree）模块加载后，内核都会被打上
> `O` 标记；未签名模块还会加 `E`。所以 `cat /proc/sys/kernel/tainted` 非 0、以及
> `dmesg` 里的 `loading out-of-tree module taints kernel` 都是预期行为。
> 只有 `P`（专有模块）才代表许可证问题 —— 本驱动是 GPL 兼容的，不会出现。

找不到答案先看 [`FAQ.md`](FAQ.md)。

## 路线图

| 步骤 | 目标 | 卡在哪 |
|---|---|---|
| M3.1 | `wlan0` 出现 | 代码已就绪，只差实机验证 |
| M3.2 | `iw dev wlan0 scan` 能扫到 AP | **RX 数据路径格式未知**：扫描结果以什么帧从 EP4-IN 上来 |
| M3.3 | `wpa_supplicant` 关联 | 认证/关联消息的参数布局 |
| M3.4 | 能 ping 通 | **TX 描述符格式未知**（TXQ / host desc 怎么填） |
| — | 协议收尾 | `0x020c` / `0x0104` / `0x0105` / `0x050e` 的精确语义 |
| — | 可选 | 5 GHz 频段、蓝牙（同芯片 BT 功能，属复合接口） |

最省力的替代路线：**向厂商索取官方 Linux 驱动**
（`ZTOP_ACEV100_Android_wifi_bt_*.tar.gz`，内含 `build_linux.sh`）。
拿到的话，本仓库可以转为适配/维护角色。

## 实现要点（给审阅者）

- **线协议**：所有主机↔设备消息走批量端点，统一帧格式
  `"WLAN" + u16 hlen + u16 type + payload[hlen]`；主机→设备 **EP8-OUT**，设备→主机 **EP4-IN**
- **消息 ABI**：CEVA RivieraWaves rwnx 血统，
  `struct lmac_msg { u16 id; u16 dest; u16 src; u16 param_len; u32 param[]; }`，
  主机侧 `src_id = 100`；MM 段消息编号与开源 rwnx 枚举一致
- **固件容器**：`"ZT"` magic + `u16 pid(0x9612)` + `u8 count` + `count × 19B` 段表；
  校验是**对整段数据做 XOR16**（不是每块）
- **同步语义**：RUN 之后必须等固件启动通知（`type=0x0100`）再发 `MM_RESET_REQ`，
  否则消息丢失、永远等不到 CFM；每条 IPC 都要等到对应 CFM 再发下一条
- 厂商私有消息段（`0x01xx` / `0x02xx` / `0x05xx`）不在开源枚举里，
  实现策略是**按实测抓包原样重放**，不猜结构

## 参与贡献

见 [`CONTRIBUTING.md`](CONTRIBUTING.md)。最需要的是**实机测试反馈**与 M3 未知项的突破。

```
Fork 本仓库 → 开分支 → 改动 → make checkpatch → 实机验证 → 附 dmesg → 提交 PR
```

## 法律与来源说明

- 本驱动是**独立实现**：只依据设备对外可观测的行为（USB 描述符、总线上的请求/响应
  序列）与公开的 CEVA RivieraWaves rwnx 消息框架知识编写；仓库中**不含任何厂商源码、
  反编译产物或厂商二进制**
- 开发期的内部协议笔记、设备调试记录与测试资料**不在本仓库中**（保留在开发者本地）。
  源码注释里若出现 `re/`、`docs/` 之类的路径引用，指的就是那些未公开的内部资料
- 与芯片厂商（ZTOP / 兆通微）**无隶属、无背书关系**；厂商名与型号仅用于说明兼容性
- 厂商固件与 Windows 驱动**未随仓库分发**（见 [`firmware/README.md`](firmware/README.md)），
  请从你自己设备的配套材料中取得
- 参考了开源 AIC8800（rwnx 系）驱动的分层与消息框架思路，以及 morrownr / lwfinger
  系列驱动仓库的构建、DKMS 与文档组织方式
- 内核接口与规范以 [kernel.org 文档](https://docs.kernel.org/) 为准

## 许可

[GPL-2.0-only](LICENSE) —— 与内核模块的要求一致（本驱动调用 `ieee80211_*` 等
GPL-only 符号）。

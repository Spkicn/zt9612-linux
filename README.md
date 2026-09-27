# zt9612: ZT9612U (ZTOP / ACEV100) USB Wi-Fi 6 driver for Linux

[![build](https://github.com/Spkicn/zt9612-linux/actions/workflows/build.yml/badge.svg)](https://github.com/Spkicn/zt9612-linux/actions/workflows/build.yml)
[![checkpatch](https://github.com/Spkicn/zt9612-linux/actions/workflows/checkpatch.yml/badge.svg)](https://github.com/Spkicn/zt9612-linux/actions/workflows/checkpatch.yml)
[![License: GPL-2.0-only](https://img.shields.io/badge/License-GPL--2.0--only-blue.svg)](LICENSE)

Linux 内核驱动，对应 VID:PID 为 `350b:9612` 的 ZT9612U / ACEV100 USB 无线网卡。

## 项目状态

当前版本 **0.3.2**。得到的是一个 managed 模式的无线接口：双频扫描、WPA2-PSK 关联与加密、
DHCP 与外网访问都已实机验证。

| 里程碑 | 状态 |
|---|---|
| 硬件识别与协议分析 | 完成 |
| M1 内核态固件装载 | 完成，已实机验证 |
| M2 IPC 初始化与 `/dev/zt9612` | 完成，已实机验证 |
| M3.1 mac80211 注册（网络接口） | 完成，已实机验证（接口名按 MAC 生成） |
| M3.2 扫描 | 完成，已实机验证（双频，一轮 40 个以上 BSS，含真实信号强度） |
| M3.3 关联 | 完成，已实机验证（`assoc=1`、接口 `LOWER_UP`） |
| M3.4 数据面 | 完成，已实机验证（DHCP 租约、`ping` 网关与公网） |
| M3.5 5 GHz | 完成（0.3.1 起才真正可用，见「版本要点」） |
| WPA2-PSK 加密 | 完成，已实机验证（四次握手 `PTK=CCMP GTK=CCMP`，CCMP 由 mac80211 软件加解密） |
| 吞吐 | 见下表；速率由固件内部决定，主机侧无法影响 |

实测吞吐（同一张卡，HTTPS，10 s × 3 取中位数）：

| 方向 | 2.4 GHz | 5 GHz |
|---|---|---|
| 下载 | 约 4 Mbit/s | **9–13 Mbit/s** |
| 上传 | — | 3.7–4.7 Mbit/s |

> `iw link` 里的速率数字（1.0 Mbit/s 之类）**不是**真实发射速率，只是主机侧记账，
> 不要用它衡量性能。

### 版本要点

- **0.3.0**：修掉 RX 缓冲过小导致"HTTP 200 之后收不到数据"的缺陷
- **0.3.1**：修掉扫描时 5 GHz 信道的 band 标志写死成 0 —— **5 GHz 在此之前一直不可用**
  （`iw scan` 里那些"5G BSS"其实是频点被错误标注的 2.4 GHz 帧）。修好后首次完成
  5 GHz 关联，吞吐约为 2.4 GHz 的 2–3 倍
- **0.3.2**：修掉 TX 侧 988 字节的帧长上限 —— **上传在此之前基本不可用**
  （满尺寸 TCP 段全部被驱动静默丢弃）。修复后 `ping -M do` 1472 正常、上传可用

改动细节与实测数据见 [CHANGELOG.md](CHANGELOG.md)。

## 硬件

| 项目 | 值 |
|---|---|
| 厂商 | ZTOP / 山东兆通微电子（Zhaotong Micro） |
| 模组 | ZT9612U V1.0（FCC ID `2BOBE-ZT9612UV10`） |
| 芯片平台 | ACEV100（自研 Wi-Fi 6 2T2R SoC，非 Realtek / MediaTek 方案） |
| 网卡模式 | `350b:9612`，bcdDevice `0x0200`，iProduct `802.11ax 2x2 WLAN Adapter` |
| 光盘模式 | `350b:f179`（USB CD-ROM，卷标 "Wi-Fi6 Adapter"） |
| 接口 | 1 个，class `FF/FF/FF`，5 个 512 字节批量端点（EP4-IN / EP5-8-OUT） |
| 固件 | `zt9612_fw.bin`（219,076 字节）+ `zt9612_settings.bin`（212 字节） |

支持范围与 ID 判定方法见 [supported-device-IDs](supported-device-IDs)。同厂其它型号
（`350b:9101`、`350b:9611` 等）不适用。

## 功能范围

已实现：

- 把设备从驱动光盘模式切到网卡模式（标准 SCSI 弹出，不是厂商私有命令）
- 完整复现固件下载协议：hello 握手、488 字节分块写入、末块整段 XOR16 校验、配置块、RUN
- 同步 IPC 初始化：`MM_RESET`、`MM_VERSION`、厂商私有段、`MM_START`（约 6.5 秒）、
  `MM_SET_IDLE`、`MM_ADD_IF`、`MM_SET_SLOTTIME`、`MM_SET_CHANNEL`
- 5 秒心跳维持固件存活；`/dev/zt9612` 上收发原始 `WLAN` 帧
- mac80211：managed 接口、**2.4 GHz 14 个信道（含 2484）+ 5 GHz 25 个信道**、
  `hw_scan`（双频）、关联、WPA2-PSK、数据面（TCP/UDP/IPv6 均正常）
- 扫描结果带**真实信号强度**（RX 描述符里的 int8 dBm，实测 −94 至 −23 dBm）

尚未实现：

- WPA2-Enterprise（802.1X）：代码路径未验证（当前实验环境没有 802.1X AP）
- 主动扫描默认关闭（`scan_probe=0` 为被动）；`scan_probe=2` 的厂商 probe 回放已验证可用
- AP 模式、蓝牙、40/80 MHz 带宽
- 提高实测吞吐：TX 速率由固件内部决定，主机侧改不了（见「实现要点」）

这块网卡在 Windows 下工作正常。厂商没有公开发布 Linux 驱动，但存在内部版本，
**向厂商索取是更省力的路线**。

## 兼容性

| 项目 | 值 |
|---|---|
| 已实机验证 | `7.0.0-31-generic`（Ubuntu 24.04.5 LTS，x86_64） |
| 已验证可编译 | `6.17.0-1022-azure`（CI，ubuntu-24.04 runner，无告警）；`modinfo` 正确生成 `alias: usb:v350Bp9612d*` |
| 编译下限 | 6.12（见 `dkms.conf` 的 `BUILD_EXCLUSIVE_KERNEL`）：驱动包含 6.12 才引入的 `linux/unaligned.h` |
| 未验证区间 | 6.12–6.16 能否正常工作未验证；mac80211 ops 签名只在 6.17 及以上确认匹配 |
| 构建依赖 | `build-essential`、`linux-headers-$(uname -r)` |

驱动使用了较新的 mac80211 ops 签名，例如
`config(struct ieee80211_hw *, int radio_idx, u32 changed)` 和
`tx(struct ieee80211_hw *, struct ieee80211_tx_control *, struct sk_buff *)`。
在较老内核上需要适配，欢迎提 PR 并附完整报错与 `uname -a`。

## 安装

### 1. 准备固件

固件版权属于厂商，本仓库不分发。请从自己网卡的配套驱动盘中取得
（Windows 下运行 `auto_load.exe` 后，在安装目录中找 `zt9612_fw.bin` 与
`zt9612_settings.bin`），或向厂商索取。取得后执行：

```bash
sudo ./scripts/install-firmware.sh /path/to/firmware-dir
```

脚本会校验 SHA-256 后才安装。文件大小、哈希与来源说明见
[firmware/README.md](firmware/README.md)。

### 2. 切换设备模式

设备出厂默认枚举为光驱，不是网卡。可以做一次性配置让插入时自动切换：

```bash
sudo ./scripts/setup-mode-switch.sh
```

也可以手动切换一次：

```bash
sudo eject /dev/sr0
sudo usb_modeswitch -K -W -v 350b -p f179
```

切换后 `lsusb` 应显示 `350b:9612`。芯片 ROM 只在切换后的数秒内接受固件下载，
因此切完应尽快加载驱动，这也是配置 udev 自动切换的原因。

### 3. 安装驱动

```bash
sudo ./install-driver.sh                     # 编译、按需签名、安装，并手动加载一次
sudo ./install-driver.sh --dkms              # 通过 DKMS 安装，内核升级后自动重建
sudo ./install-driver.sh --enable-autoload   # 允许插卡或开机自动加载，见「已知问题」
```

默认行为是安装模块到 `/lib/modules/$(uname -r)/extra/`，并写入
`/etc/modprobe.d/zt9612-blacklist.conf`。该文件只阻止自动加载，手动 `modprobe`
仍然可用。

也可以直接用 DKMS：

```bash
sudo dkms install .                          # add、build、install 一步完成
dkms status
sudo dkms remove zt9612/0.3.2 --all
```

手动编译的等价流程：

```bash
cd driver
make                       # 生成 zt9612.ko
make sign                  # Secure Boot 机器：用 MOK 密钥签名
sudo make install          # 安装到 /lib/modules/$(uname -r)/extra/ 并 depmod
sudo modprobe zt9612
```

可用目标见 `make help`；可覆盖变量为 `KVER`、`KSRC`（或 `KERNEL_SRC`）、`JOBS`。

### 4. Secure Boot 签名

Secure Boot 打开时内核处于 `lockdown=integrity`，会拒绝未签名模块。有两种办法：
在 BIOS 中关闭 Secure Boot，或生成 MOK 密钥并注册（一次性）：

```bash
cd driver
openssl req -new -x509 -newkey rsa:2048 -keyout MOK.priv -outform DER -out MOK.der \
    -nodes -days 36500 -subj "/CN=zt9612 module signing/"
openssl x509 -inform DER -in MOK.der -out MOK.pem
sudo mokutil --import MOK.der     # 设置一次性密码
sudo reboot                       # 开机蓝屏界面选 Enroll MOK，输入该密码
make sign
```

`install-driver.sh` 检测到 Secure Boot 且没有密钥时，会引导完成上述步骤。
`MOK.*` 已被 `.gitignore` 排除，不要提交。

**DKMS 已知坑**：dkms 3.0.11 即使配好了 `mok_signing_key` / `mok_certificate`，
也**不会给压缩后的 `.ko.zst` 签名**，`modprobe` 会以 `Key was rejected by service` 失败。
就地补签即可：

```bash
KVER=$(uname -r); KO=/lib/modules/$KVER/updates/dkms/zt9612.ko.zst
sudo zstd -d -f $KO -o /tmp/zt9612.ko
sudo $KVER/build/scripts/sign-file sha256 MOK.priv MOK.pem /tmp/zt9612.ko   # 密钥路径按实际调整
sudo zstd -f -q /tmp/zt9612.ko -o $KO
sudo depmod -a && sudo modprobe zt9612
modinfo -k $KVER zt9612 | grep signer      # 应显示已注册的签名者
```

补签后 `dkms status` 会提示 `WARNING! Diff between built and installed module!`，属预期现象；
下一次 `dkms install` 会再次覆盖成未签名版本，需要重复这一步。

## 验收

```bash
sudo dmesg -C
sudo modprobe zt9612            # 约 7 秒，其中 MM_START 需等待约 6.5 秒
sudo dmesg | tail -30
```

正常输出如下（实测时间线）：

```
zt9612 1-9:1.0: probing 350b:9612 (iface 0)
zt9612 1-9:1.0:   ep 0x84 IN
zt9612 1-9:1.0:   ep 0x05 OUT
zt9612 1-9:1.0: hello ack: yes (try 1)
zt9612 1-9:1.0:   wrote addr=0x61070000 len=219048 (449 blocks) cs=0x2d14
zt9612 1-9:1.0:   wrote addr=0x210ce700 len=212 (1 blocks) cs=0x0001
zt9612 1-9:1.0: RUN (1 sections)
zt9612 1-9:1.0: boot notify: type=0x0100 len=16
zt9612 1-9:1.0: firmware loaded
zt9612 1-9:1.0: === IPC init sequence ===
zt9612 1-9:1.0: fw 0x0101: status=0 mac=XX:XX:XX:XX:XX:XX
zt9612 1-9:1.0: MM_START_REQ (rf init, waiting 约 6.5s)
zt9612 1-9:1.0: MM_START_CFM received (firmware up)
zt9612 1-9:1.0: MM_ADD_IF_CFM: status=0 inst_nbr=0
zt9612 1-9:1.0: M1+M2 done: firmware running, /dev/zt9612 ready
zt9612 1-9:1.0: mac80211 registered (M3.1) - wlan0 should appear
zt9612 1-9:1.0 wlxXXXXXXXXXXXX: renamed from wlan0
```

> 上面的 MAC 与接口名已用 `XX` 隐去（驱动实际打印的是内核 `%pM` 格式的地址）。
> 接口名是 systemd 按 MAC 生成的可预测名（`wlx` + 12 位十六进制），
> 所以**不要**假设它叫 `wlan0`。

再做 IPC 往返自测：

```bash
sudo python3 scripts/zt9612-devtest.py
# 期望：往返成功 2/2（MM_VERSION_REQ 收到 CFM；厂商 0x0100 返回 MAC）
sudo python3 scripts/zt9612-devtest.py --seconds 20    # 顺带检查心跳稳定性
```

接口与无线验收：

```bash
IFACE=$(ls /sys/class/net | grep -E '^(wlx|wlan)' | head -1)
iw dev ; iw phy "$(iw dev "$IFACE" info | awk '/wiphy/{print "phy"$2}')" info | head -40
ethtool -i "$IFACE" | head -2                 # 期望 driver: zt9612
sudo iw dev "$IFACE" scan | grep -c '^BSS'    # 期望 40 个以上（双频）
```

## 已知问题

- **反复 `rmmod` / `insmod` 会泄漏实例**：卸载时设备不应答在途 bulk-IN，驱动按设计
  "宁可泄漏也不 UAF"。实测 12 次卸载里 11 次如此。**换模块优先重启机器。**
- **默认不自动加载**：`install-driver.sh` 默认写 `blacklist zt9612`。原因是历史上
  "开机自动加载"曾导致机器失联（根因是漏设 wiphy 父设备，已在 v0.2.0 修复），
  但**"插卡 + 开机自动加载"这条完整路径还没有重新复验过**。确认可用后加
  `--enable-autoload` 即可。
- **上传吞吐低于下载（已结案到"下一步方向"，不是描述符常量）**：TX 上传约 3.7 Mbit/s，而同一张卡
  在厂商 Windows 驱动下是 **95.3 Mbit/s**（同链路、同一 BSS、出口已核对）。抓包确认厂商对
  **数据帧**用的描述符常量与我们（抄自管理帧/probe）不同，但**照抄不成立、且已用功能判据证伪**：
  改描述符后微基准读数暴涨 27 倍，而 ARP 判据显示网关**收不到任何请求**（帧根本没上过空口），
  DHCP 也随之断掉。静态逆向进一步表明那两个可疑值（`0x1312`/`0x7540`）在驱动里不是立即数、
  在 IPC 里也不存在 ⇒ 无依据可抄。**厂商那条快路径依赖我们尚未复现的其它状态**。
  详见 CHANGELOG 与 `re/EXPERIMENT_TX_RATE.md` §5~7。
- **做 TX 实验必须带功能判据**：`usb_bulk_msg` 完成得快**不等于**帧发出去了
  （本项目自己踩过这个坑）。请用 `tools/arp_oracle.py`（发 ARP 请求看网关回不回）作为必跑项。
- **运行期改 MTU 会让接口短暂失去关联**（丢 1–2 个 `ping`，NetworkManager 会重配）；
  请勿在运行期反复改。
- **芯片挂死后需要物理拔插**：出现 `-110`、`can't set config #1` 表示芯片已挂死，
  软件复位无效。拔下等 10 秒再插上。
- **设备可能自行回到光盘模式**：固件看门狗复位（主机超过 5 秒没有发心跳）。内核驱动用
  `delayed_work` 每 5 秒发一次；自写用户态脚本需要自己维持心跳。
- **WPA2-Enterprise（802.1X）未验证**：实验环境没有 802.1X AP。
- 驱动源码中部分中文注释在早期编辑中损坏成乱码，不影响编译，待清理。
- 0.x 阶段的接口（模块参数、`/dev` 协议）可能变化。

已修复的历史问题（过程与数据见 [CHANGELOG.md](CHANGELOG.md)）：RX 缓冲过小导致下载停滞、
5 GHz 扫描 band 标志错误导致 5 GHz 完全不可用、TX 帧长上限导致上传不可用、
`max_mtu` 曾被设成 900 的弯路、开机自动加载的空指针崩溃。

## 调试

```bash
sudo dmesg -C && sudo modprobe zt9612 && sudo dmesg | tail -30
ls -l /dev/zt9612                        # 原始 IPC 通道
cat /sys/module/zt9612/parameters/*      # 模块参数当前值
```

模块参数说明见 [zt9612.conf](zt9612.conf)：

| 参数 | 默认 | 说明 |
|---|---|---|
| `do_init` | 1 | 固件装载后是否执行同步初始化序列；`0` 表示只装载固件 |
| `do_boot` | 1 | 是否下载固件；`0` 表示复用已在运行的固件，只做 IPC 初始化 |
| `scan_probe` | 0 | 扫描是否主动发 probe request：`0` 被动（默认）、`1` 自建、`2` 逐字节重放厂商 probe（已验证）。主动探测会跳过 `NO_IR`/`RADAR` 信道 |
| `tx_ep` | 5 | TX 端点号（实验开关） |
| `tx_prep` | 1 | 切换信道前是否重放厂商的使能序列 |
| `tx_variant` | 0 | TX 帧变体实验开关 |
| `rx_debug` | 0 | 诊断：写 N 即记录接下来 N 个接收 USB 传输的长度（`journalctl -k \| grep rxdbg`） |
| `tx_status` | 0 | 实验开关：把发送结果上报给 mac80211（实测不提吞吐，只让记账更真实） |
| `tx_status_probe` | 100 | 开启上报时每 N 帧标 1 帧为"已 ACK"，其余如实报失败 |
| `ht_cap_enable` | 0 | 实验开关：给 5 GHz band 声明 HT。**实测无收益且会触发 mac80211 聚合 WARNING**，见 CHANGELOG |
| `tx_desc_mask` | 0 | 实验开关：加密单播数据帧按位采用**厂商数据帧的描述符字段值**（bit1 = `+0x08`，就是那个会大幅改变微基准读数的字段）。**实测会断 DHCP，默认必须为 0**；微基准读数不等于空口吞吐，别被它骗 |
| `ntf_log` | 0 | 实验开关：打印 EP2-IN 通知内容。实测本设备没有 EP2-IN（只有 EP4-IN + EP5-8-OUT），该通道恒为空 |

`/dev/zt9612` 的接口约定（调试通道，非数据面）：

| 操作 | 语义 |
|---|---|
| `write()` | 传入完整 `WLAN` 帧（`"WLAN" + u16 hlen + u16 type + payload`），原样发往 TX 端点 |
| `read()` | 返回一条设备发来的完整帧，阻塞等待且 3 秒超时，支持 `O_NONBLOCK`；保证整帧 |
| `poll()` | 支持 `select()`/`poll()` 等待可读 |
| ioctl `ZT_IOC_TXRAW` | 把一条完整 `WLAN` 帧直接交给 TX 端点，用于**不重载模块**就试验描述符与时序 |
| debugfs `zt9612/tx_raw` | 同上，走写入方式（`lockdown=integrity` 下 debugfs 不可写，通常用 ioctl） |

看 RX 帧长分布（写参数不能用 `echo N | sudo tee`，tee 会吞掉 stdin；
`kernel.dmesg_restrict=1` 时用 `journalctl -k` 读日志）：

```bash
sudo sh -c 'echo 2000 > /sys/module/zt9612/parameters/rx_debug'
# 期间做一次下载，然后：
sudo journalctl -k --since '-2 min' | grep rxdbg
```

内核 taint 提示：加载外部模块后内核会被标记 `O`，未签名模块再加 `E`。因此
`cat /proc/sys/kernel/tainted` 非 0、`dmesg` 里出现 `loading out-of-tree module taints kernel`
都是预期现象。只有 `P`（专有模块）才代表许可证问题，本驱动是 GPL 兼容的，不会出现。

其它问题先看 [FAQ.md](FAQ.md)。

## 路线图

| 步骤 | 目标 | 状态 |
|---|---|---|
| M3.1–M3.5 | 接口 / 双频扫描 / 关联 / 数据面 / 5 GHz | 完成并实机验证（5 GHz 自 0.3.1 起真正可用） |
| v0.3 | 吞吐判定 | 完成：速率由固件内 ARRM 自主决定，主机侧改不了；过程中修掉 RX 缓冲缺陷 |
| v0.3.1 / v0.3.2 | 5 GHz 与上传修复 | 完成（见「版本要点」） |
| 待办 | 上传吞吐的天花板 | 已量化：TX 字节率上限约 6.2–7.2 Mbit/s（非软件串行化，URB 流水线无用）。剩下的是"厂商驱动上传是多少"的对照 |
| 下一步 | 速率控制反馈 | 补 `ampdu_action` 与真实 TX 反馈，让速率控制能爬升（当前停在 MCS 0） |
| 待办 | 干净开机自动加载复验 | 通过后才能把 `install-driver.sh` 的默认 blacklist 去掉 |
| 待办 | WPA2-Enterprise（802.1X） | 代码路径未验证 |
| 收尾 | 协议细节 | `0x0104`、`0x0105`、`0x050e` 的精确语义（`0x020c` 已定案，可忽略） |
| 可选 | 扩展 | AP 模式、40/80 MHz、蓝牙 |

更省力的替代路线是向厂商索取官方 Linux 驱动
（`ZTOP_ACEV100_Android_wifi_bt_*.tar.gz`，内含 `build_linux.sh`）。取得后本仓库可以转为
适配与维护的角色。

## 实现要点

- 线协议：所有主机与设备间的消息都走批量端点，统一帧格式为
  `"WLAN" + u16 hlen + u16 type + payload[hlen]`；主机到设备走 EP8-OUT，设备到主机走 EP4-IN
- 消息 ABI：CEVA RivieraWaves rwnx 血统，
  `struct lmac_msg { u16 id; u16 dest; u16 src; u16 param_len; u32 param[]; }`，
  主机侧 `src_id` 固定为 100；MM 段消息编号与开源 rwnx 枚举一致
- 固件容器：`"ZT"` magic、`u16 pid`（`0x9612`）、`u8 count`，后接 `count` 个 19 字节段表项；
  校验是对整段数据做 XOR16，不是每块
- 同步语义：RUN 之后必须等到固件启动通知（`type=0x0100`）再发 `MM_RESET_REQ`，否则消息
  丢失且永远等不到 CFM；每条 IPC 都要等到对应 CFM 才能发下一条
- **TX 速率由固件决定**：28 字节 TX 描述符里没有速率/MCS 字段，厂商驱动也从不发速率消息，
  因此主机侧补速率表、上报 TX status、声明 HT/VHT 都不改变实测吞吐
- 厂商私有消息段（`0x01xx`、`0x02xx`、`0x05xx`）不在开源枚举中，实现上按实测序列
  原样重放，不推测结构

## 参与贡献

见 [CONTRIBUTING.md](CONTRIBUTING.md)。目前最需要的是实机测试反馈（不同主板、不同内核，
失败的 dmesg 同样有价值），以及向厂商索取官方 Linux 驱动包。

## 来源与许可

- 本驱动是独立实现，只依据设备对外可观测的行为（USB 描述符、总线上的请求与响应序列）
  以及公开的 CEVA RivieraWaves rwnx 消息框架知识编写。仓库中不含任何厂商源码、
  反编译产物或厂商二进制
- 开发期的内部协议笔记、设备调试记录与测试资料不在本仓库中，保留在开发者本地。源码
  注释中若出现 `re/`、`docs/` 之类的路径引用，指向的是那些未公开的内部资料
- 与芯片厂商（ZTOP / 兆通微）无隶属关系，也未获其背书；厂商名与型号仅用于说明兼容性
- 厂商固件与 Windows 驱动未随仓库分发，见 [firmware/README.md](firmware/README.md)
- 参考了开源 AIC8800（rwnx 系）驱动的分层与消息框架思路，以及 morrownr、lwfinger 系列
  驱动仓库在构建、DKMS 与文档组织上的做法
- 内核接口与规范以 [kernel.org 文档](https://docs.kernel.org/) 为准

本项目使用 [GPL-2.0-only](LICENSE)，与内核模块的许可要求一致。

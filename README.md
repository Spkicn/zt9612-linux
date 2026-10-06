# zt9612: ZT9612U (ZTOP / ACEV100) USB Wi-Fi 6 driver for Linux

[![build](https://github.com/Spkicn/zt9612-linux/actions/workflows/build.yml/badge.svg)](https://github.com/Spkicn/zt9612-linux/actions/workflows/build.yml)
[![checkpatch](https://github.com/Spkicn/zt9612-linux/actions/workflows/checkpatch.yml/badge.svg)](https://github.com/Spkicn/zt9612-linux/actions/workflows/checkpatch.yml)
[![License: GPL-2.0-only](https://img.shields.io/badge/License-GPL--2.0--only-blue.svg)](LICENSE)

Linux 内核驱动，对应 VID:PID 为 `350b:9612` 的 ZT9612U / ACEV100 USB 无线网卡。

## 项目状态

最新发布 **0.3.2**；`[Unreleased]` 里是 v0.4（上传快路径：固件站点会话 + A-MPDU 聚合 +
WPA2 密钥路径；行为开关**默认关闭**，默认联网行为与 0.3.2 一致）。得到的是一个 managed
模式的无线接口：双频扫描、WPA2-PSK 关联与加密、DHCP 与外网访问都已实机验证。

**v0.4 的三大组件（`sta_add_fmt=7` 会话、`key_en=2` 加密、A-MPDU 聚合）也已实机走通，
但都不提升吞吐**；截至 2026-10-05（r42~r44）的端到端数字与结论见下表。历史上
"TX 死锁 / TXQ 计量 / 35 倍差 / 固件消费衰减 / 毒物块"等多版归因均已证伪或更正，
过程与证据见 [CHANGELOG.md](CHANGELOG.md) `[Unreleased]`。

| 能力 | 状态 |
|---|---|
| 固件装载 · IPC 初始化 · `/dev/zt9612` | 完成，已实机验证 |
| mac80211：接口 · 双频扫描 · 关联 · 数据面 · 5 GHz | 完成，已实机验证（5 GHz 自 0.3.1 起真正可用） |
| WPA2-PSK 加密 · DHCP · 外网访问 | 完成，已实机验证（CCMP 由 mac80211 软件加解密） |
| 上传快路径（会话 / 密钥 / A-MPDU 聚合） | 已实机走通，开关默认关；聚合可长跑但**不提速** |
| WPA2-Enterprise · AP 模式 · 蓝牙 · 80 MHz | 未实现 |

实测吞吐（**端到端**，对端为另一台设备上的 TCP sink，接收侧逐字节核对；2026-10-05 r42/r42d）：

| 方向 | 2.4 GHz | 5 GHz |
|---|---|---|
| 上传 | 6.5–7.5 Mbit/s | **12.7–14.9 Mbit/s** |
| 下载 | — | **18.5–20.4 Mbit/s** |

> [!WARNING]
> 早先这张表记的是"2.4G 约 4 / 5G 上传 3.7–4.7 Mbit/s"，那是**旧口径**（不同仪器、
> 且当时 TX 侧还有 988 字节帧长上限等已修缺陷）——数字以本表为准。
> **别拿厂商"95.3 Mbit/s"对比**：那是近端 sink 口径，不可比；见「已知问题」。
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

尚未实现 / 仍属实验：

- WPA2-Enterprise（802.1X）：代码路径未验证（当前实验环境没有 802.1X AP）
- 主动扫描默认关闭（`scan_probe=0` 为被动）；`scan_probe=2` 的厂商 probe 回放已验证可用
- **上传快路径**：固件站点会话（`sta_add_en`，默认关）、A-MPDU 聚合（`ampdu_en`，默认关）
  与 WPA2 密钥路径（`key_en`，默认 2 = mac80211 软件加解密）都已实现且实机走通；三条路径
  **都不提升实测吞吐**（速率由固件内部决定）。⚠️ 早期两版归因**均已作废**：①「`MM_STA_ADD`
  登记后 8~40s 定时死亡」由 `sta_add_fmt=7` 拆除（r26）；②「连续聚合有限次数后打挂设备」
  由 `agg_min_len=1549` + `agg_stop` 修掉（r40），并在真实 TCP 下长跑 10,930 次零断言（r42）
  —— 详见 CHANGELOG `[Unreleased]`
- **带宽协商**：`ht_width40=1`（HT40）实测**更慢且致死**（不采纳，默认关）；A-MSDU 大单帧
  因两个 AP 都报 `amsdu=0` 无法协商，路线挂起
- AP 模式、蓝牙、80 MHz 带宽
- 提高实测速率：**发射速率**由固件内部决定，主机侧改不了（见「实现要点」与「已知问题」）

这块网卡在 Windows 下工作正常。厂商没有公开发布 Linux 驱动，但存在内部版本，
**向厂商索取是更省力的路线**。

## 兼容性

| 项目 | 值 |
|---|---|
| 已实机验证 | `7.0.0-31-generic`（Ubuntu 24.04.5 LTS，x86_64）：功能全通、0 Oops、0 WARNING |
| 功能已验证 | `7.0.0-34-generic`（2026-09-27 复测）：固件装载、IPC 初始化、双频扫描、5 GHz 关联、DHCP、外网 `ping`、`ping -M do 1472` 全部通过，`dmesg` **0 WARNING**（首帧 TX 曾在 `net/mac80211/tx.c:3832` 触发一次，已修：进程上下文改用 `ieee80211_tx_dequeue_ni()`） |
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
`/etc/modprobe.d/zt9612-blacklist.conf`，内容是**一行 `blacklist zt9612`** ——
它**只挡住插卡/开机的自动加载，不挡手动 `modprobe`**（`--enable-autoload` 会删掉这个文件）：

```bash
sudo modprobe zt9612
```

> [!IMPORTANT]
> **手工加过守卫的机器**上是另一种情况：守卫文件里除了 `blacklist` 还可能有
> `install zt9612 /bin/true`。那种环境下 `sudo modprobe zt9612` 会**静默返回 0 且什么都不做**
> （实测 2026-09-27，内核 7.0.0-34），必须用：
>
> ```bash
> sudo modprobe --ignore-install zt9612
> ```
>
> 手工加过守卫的机器上，`--enable-autoload` 会**整个删除**该 blacklist 文件（连同守卫）。

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

- **上传吞吐上限约 15 Mbit/s，主机侧无法再提**：端到端实测见「项目状态」；UDP（无拥塞
  控制）容量上限 **15.0 Mbit/s**，TCP 已达其 94% ⇒ 瓶颈不是 TCP 拥塞退避。同一张卡的
  下载为 18.5–20.4 Mbit/s ⇒ **上限在固件 TX 侧，不是链路容量**。主机侧七个杠杆已逐一
  证否（描述符速率字 / 速率消息 / `MM_STA_ADD_REQ` 的 RC 块 / A-MPDU 打包 / 换更强 AP /
  声明 HT40 / A-MSDU），速率由固件内部 ARRM 决定，而该模块不在 219 KB 镜像内 ——
  再往上需要厂商固件文档、第二个固件段，或一台支持 A-MSDU 的 AP。
- **聚合默认关闭；UDP 无流控满压会打死设备，需同时开 `agg_pred_us>0`**：`ampdu_en=0` 是
  默认值。打开后（须同时 `ht_cap_enable=1`，且 `agg_min_len=1549` 与 `agg_stop` 一起用）
  在真实 TCP 负载下可长跑（连续 10,930 次聚合传输零断言、USB 设备号不变，真正的变量是
  **有无背压**），但**不带来吞吐增益**（12.5 vs 单帧 14.15 Mbit/s）；UDP 无流控持续满压
  仍会断言并 USB 重枚举 ⇒ 必须开拥塞预测（r44 A/B：不开 = USB 重枚举死亡；开 = 存活零
  bulk 失败）。
- **设备挂死后的唯一可靠复位 = 冷启动（S5 冷关机 + RTC 唤醒），不是热重启**：出现 `-110`、
  `can't set config #1`、`Entity not found`，或 `dmesg` 反复 `hello ack timeout` /
  `firmware boot failed: -110`，就表示芯片已挂死。热 `systemctl reboot` 不断 USB VBUS，
  芯片 ROM 的秒级固件接收窗口不会重开，之后任何 `insmod` 都会失败。已逐一实测无效：
  端口 `disable` 往返（仅逻辑重枚举）、设备级/驱动级 unbind-bind、`USBDEVFS_RESET`、
  `authorized` 往返、`xhci_hcd` unbind→bind、`usb_modeswitch -R`、D3cold。

  ```bash
  sudo rtcwake -m no -s 180      # 设唤醒闹钟
  sudo systemctl poweroff        # 等 ping 100% 丢包 = 真的断电
  # RTC 自动唤醒后，udev 规则会紧跟枚举自动加载驱动（本机 WoL 在 S5 下无效）
  ```

  改代码后必须把新 `.ko` 装进 `/lib/modules/$(uname -r)/extra/`，并 **移走 DKMS 的
  `updates/dkms/zt9612.ko.zst`**（`updates/` 优先级更高，否则加载的是旧代码）。详见
  [FAQ.md](FAQ.md)。

- **反复 `rmmod` / `insmod` 会泄漏实例**：卸载时设备不应答在途 bulk-IN，驱动按设计
  "宁可泄漏也不 UAF"。实测 12 次卸载里 11 次如此。**换模块优先重启机器。**
- **默认不自动加载**：`install-driver.sh` 默认写 `blacklist zt9612`。原因是历史上
  "开机自动加载"曾导致机器失联（根因是漏设 wiphy 父设备，已在 v0.2.0 修复），
  但**"插卡 + 开机自动加载"这条完整路径还没有重新复验过**。确认可用后加
  `--enable-autoload` 即可。
- **做 TX 实验必须带功能判据**：`usb_bulk_msg` 完成得快**不等于**帧发出去了。用
  `ping -M do 1472`、DHCP，或"在开放网络上发 ARP 看网关是否应答"这类 A 级判据。
  注意**未加密的裸帧判据在 WPA/WPA2 链路上无效**：那些帧会被 AP 直接丢弃，而同一链路上的
  普通 `ping` 与 ARP 一切正常 —— 本项目在这里踩过坑（`ZT_IOC_TXRAW` 恒判"没发出"）。
- **运行期改 MTU 会让接口短暂失去关联**（丢 1–2 个 `ping`，NetworkManager 会重配）；
  请勿在运行期反复改。
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
| `ht_cap_enable` | 0 | 实验开关：给 5 GHz band 声明 HT（HT20/SGI、MCS0-15）。**它是 A-MPDU 聚合的前提**（mac80211 的 `ieee80211_aggr_check()` 在对端链路没有 HT 能力时直接返回，实测：不开时 `start_tx_ba_session()` 永不触发）；单独声明 HT 不改变实测吞吐 |
| `ht_width40` | 0 | 实验开关：把 5 GHz 的 HT 能力从 **HT20 放宽到 HT40**（`SUP_WIDTH_20_40` + `SGI_40`，`rx_highest` 144→300）；需 `ht_cap_enable=1`。**r43 实测否证、不采纳**：同一手机热点（HT40/VHT80）下 HT20 为 14.88/13.91/15.51 Mbit/s，HT40 反而 **10.51/8.34**，且第三轮 bulk OUT 失败 ×3 → **USB 重枚举**（零 assert）⇒ 更慢且致死。**保持默认 0。** |
| `tx_desc_mask` | 0 | 实验开关：加密单播数据帧按位采用**厂商数据帧的描述符字段值**（bit1 = `+0x08`，就是那个会大幅改变微基准读数的字段）。**实测会断 DHCP，默认必须为 0**；微基准读数不等于空口吞吐，别被它骗 |
| `ntf_log` | 0 | 实验开关：打印 EP2-IN 通知内容。实测本设备没有 EP2-IN（只有 EP4-IN + EP5-8-OUT），该通道恒为空 |
| `sta_add_en` | 0 | 实验开关：关联后给固件发 `MM_STA_ADD_REQ`，并把固件分配的 `sta_idx` 写进**加密单播数据帧**的描述符 `+0x09`（其余帧保持"无站点"）。载荷已被固件接受（`STA_ADD_CFM status=0 sta_idx=N`）；**但只带 `sta_idx` 未见吞吐提升**（上传 0.38 vs 0.33 Mbit/s，噪声内），默认仍为 0。载荷格式的默认值已由 5 换成 7（宽切除载荷），见 `sta_add_fmt` 行 |
| `tx_staidx` | 1 | 实验开关：`1`（默认）= 加密单播数据帧描述符写入 `sta_idx`；`0` = 一律走"无站点" `0xff` 路径（r20 重连聋隔离臂） |
| `tx_staid4b` | 0 | 实验开关（r22）：`1` = 描述符头 4 字节写成 `00 staid 05 00`（厂商**关联期 46B 形态**，`+0x01=staid` 随 `STA_ADD_CFM` 演化）；`0`（默认）= 厂商**稳态** `0xffffffff`。r22 首臂实测无差异 |
| `sta_del_en` | 0 | 实验开关：`1` = 断开时发 `MM_STA_DEL_REQ` 归还固件站点槽位；`0`（默认）= 保留固件会话跨重连 —— r20 实测固件对断开后的会话清理不可逆，发 STA_DEL 后重连的加密单播 TX 全聋，会话复用是唯一可用的重连路径 |
| `sta_reuse` | 1 | 实验开关：`1`（默认）= 同 BSSID 会话已存在时跳过重发 `MM_STA_ADD`（r20 验证两次重连 0% 丢包）；`0` = 强制重发拿新 `sta_idx`（r21 观测：环境劣化后同样救不回来） |
| `sta_add_fmt` | 7 | `MM_STA_ADD_REQ` 的 RC 块格式：`7`（默认，2026-10-04 r26 起转正）= **宽切除载荷**（保留速率核 + AID + BSSID，三个未知掩码块清零）；`5` = 厂商实抓真载荷；`0` = legacy 模板基线；`1`/`2` = 历史 format=2 构造（r17/r19 实测**当场打死固件**，勿用）；`3`/`4` = fmt0+rate_map 历史臂；`6` = 历史臂；`9`/`10` = r38 安全块回填臂；**`11`/`12` = 块二分臂**（r42：只回填 A 块 `+0x04..05`（`rate_map`）/ 只回填 B-C 块 `+0x18..1b`（`he_max_ampdu` 掩码））；**`13`/`14` = 速率探测臂**（r42：最低档位图 / 最高档位图）。**r42 判决（2026-10-05）**：`7`/`11`/`12`/`8`/`5` 五臂**全部存活**、吞吐同为约 12 Mbit/s ⇒ r25 的「毒物块」模型退役（掩码块无罪）；`13` 被固件**直接拒绝**（`STA_ADD: no CFM`）、`14` 被接受但**破坏数据面**（0.37 Mbit/s）⇒ **RC 块会被固件校验，但不是速率杠杆**，提速不能靠本参数 |
| `vendor_seq_en` | 0 | 实验开关（r21）：`1` = 关联后在 `STA_ADD_CFM` 之后补发厂商配置序列（0x001a×4、0x0054、0x001e、0x0020、0x005a，载荷逐字节来自实抓）；`0`（默认）= 行为不变 |
| `key_en` | 2 | WPA 密钥路径：`2`（默认，r20 定案）= **mac80211 软件加解密**（WPA2 `ping` 0% 丢包）；`1` = 固件密钥路径（`MM_KEY_ADD_REQ` 0x24，44B 载荷已被接受，但**实测固件拿到密钥也不做 TX 加密**，保留用于逆向）；`0` = 关闭 |
| `key_rx_en` | 0 | 实验开关：装密钥后的 RX 解密声明方式。`0`（默认）= 收到的帧原样上交（用 `/dev/zt9612` dump 抓加密帧）；`1` = 剥 16B CCMP/TKIP IV+MIC 并声明 `DECRYPTED\|IV_STRIPPED\|MMIC_STRIPPED`；`2`/`3` = 只加声明不剥字节的历史臂 |
| `ampdu_en` | 0 | 实验开关：A-MPDU 聚合。声明 `IEEE80211_HW_AMPDU_AGGREGATION`、TX 缓冲提到 16 KB、实现 `.ampdu_action`（`MM_BA_ADD_REQ`/`MM_BA_DEL_REQ`）。**组帧在驱动里做**：mac80211 一次只给一个 MPDU，把多个 MPDU 拼进**同一次** bulk 传输是驱动的事（厂商抓包实测 EP5 传输长度 1578/3200/4808 = 1/2/3 个单元，非末尾单元固定 1608 字节槽位）。BA 会话已建立（`BA_ADD_CFM status=0`，前提 `ht_cap_enable=1`）。**2026-10-05 r42/r44 更新**：连续聚合**不再会**打挂设备——真实 TCP 下实测长跑 10,930 次聚合传输零断言、USB 设备号不变（早期"有限次数后必挂"是 r40 之前短帧断言缺陷所致）；但**打包不带来吞吐增益**（12.5 vs 单帧 14.15 Mbit/s）；**UDP 无流控满压需同时开 `agg_pred_us>0`**（r44 A/B：不开 = USB 重枚举死亡，开 = 存活零 bulk 失败）。另：早期"固件 TX 死锁"归因已证伪（那是主机未上报 TX status 的记账问题）⇒ 默认 0 |
| `agg_stride` | 1608 | 实验开关：聚合时**非末尾单元**的固定槽位（厂商实测下一个单元恒在 +1608；0 = 紧凑拼接） |
| `agg_dump` | 0 | 实验开关：hexdump 前 N 次聚合传输，用于与厂商抓包逐字节对比 |
| `vendor_tmpl` | 0 | 实验开关：加密单播数据帧改用**厂商数据帧描述符模板**（`+0x02..+0x1b` 全部常量），并把 `+0x0E` 写成帧自己的 802.11 序列号。默认关闭 |
| `agg_block` | 0 | 实验开关：保留 BA 会话但**禁止多 MPDU 打包**（实测该组合完全稳定，用于隔离"会话"与"聚合传输"） |
| `agg_max_xfers` | 1 | 安全阀：最多尝试打包 N 次，之后退回"一次 bulk 一个 MPDU"（`0` = 不限；默认 1，避免打包缺陷把设备打成 crash-loop） |
| `tx_dump` | 0 | 诊断：打印前 N 个交给设备的 TX 帧（长度/帧控制位/是否可聚合/序列号） |
| `tx_diag` | 0 | 诊断：打印两条 TX 路径的帧数（`legacy=` 传统 `.tx` 路径 / `txq=` TXQ 路径）与聚合计数、`txq_wakes`/`txq_works`。**r34 修正**：本内核数据帧走 **TXQ** 路径（驱动无 `.sta_add` ⇒ `sta->uploaded=true`），`legacy` 计数只收管理帧/广播/EAPOL；早期"数据帧走 legacy ⇒ 聚合不会发生"是 D19（TXQ 未排空）修复前的现场，已作废 |
| `agg_min_len` | 1549 | 只有 MPDU **≥** 此长度才参与聚合，更小的帧走单帧路径。**2026-10-04 r40 定案**：非末尾单元的声明长度 `hlen = 28 + MPDU` 若 **≤ 1576（MPDU ≤ 1548）**，固件必断言 `macif.c:1019`（raw 通道单变量二分：1576 致命 / 1577 无害；驱动侧同构复现）；厂商抓包里非末尾单元**恒为 MPDU 1550（hlen 1578）**。默认 1549 即"只聚合满尺寸数据帧"，短帧（如 ICMP 1464 → MPDU 1542）一律单帧 |
| `agg_max_units` | 3 | 一次 bulk 最多装几个单元。默认取**厂商抓包的上限 3**（厂商实抓 96,107 次 EP5 传输里最多 3 单元）。**历史记录**：装 8 个（12832 字节）曾跑出 76 Mbit/s（**微基准，非空口吞吐**）后崩于第 21 次、3 与 2 单元崩于第 44 次——那是 **r40 之前**的短帧断言缺陷；「累计次数必死」的归因已作废（r40 修掉触发条件、r42 长跑 10,930 次零断言） |
| `agg_gap_us` | 0 | 两次聚合传输之间的最小间隔（微秒）。**固定节流已被更优方案取代**：1 ms 固定间隔在旧缺陷下压不住（仍在第 44 次崩），而**自适应的 `agg_pred_us`**（按 bulk 耗时提前停打包）在 r44 实测有效 ⇒ 本参数保留作实验旋钮，默认 0 |
| `agg_pred_us` | 0 | 实验开关：**聚合拥塞预测**。某次聚合 bulk 的 USB 耗时 ≥ 此值（µs）时，判定设备侧排队在变深（r34 实测死前 113 µs→2.7 ms），**立即停止打包** `agg_pred_cool_ms` 毫秒，期间全部走已证稳定的单帧路径，到期自动恢复（区别于 `agg_stop` 那种要等新 BA 会话的硬停）。**r44 实测有效**：手机热点 UDP 满压 1500 pps 下，预测器关 = USB 重枚举（005→007，8 次 bulk 失败，3614 次聚合后死）；预测器开（3000 µs）= **无重枚举、无 bulk 失败**（4157 次聚合传输、619 次冷却）。默认 0 = 关闭，行为与之前一致 |
| `agg_pred_cool_ms` | 100 | 实验开关：拥塞预测器触发后的冷却时长（毫秒）。冷却期内不打包、全部走单帧路径，到期自动恢复打包。仅在 `agg_pred_us > 0` 时有意义 |
| `ba_add_fmt` | 0 | r38 实验开关：`MM_BA_ADD_REQ`(0x28) 载荷布局。`0`（默认）= 8 字节 aic8800 SDK 布局 `{type,sta,tid,pad,A:u16,B:u16}`；`1` = 厂商实抓 7 字节布局 `{sta,tid,pad,A:u16,B:u16}`。**实测（r38，干净设备两臂）**：7B 布局本身不破坏数据面（禁打包时 ping 0% 丢包），但**打包时 `macif.c:1019` 断言照出** ⇒ 布局不是断言门控，维持 8B |

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

| 事项 | 状态 |
|---|---|
| 聚合的工程化收口 | 待办：`agg_pred_us` 的 3000 µs 是单点取值，值得扫一次；A-MSDU 需一台支持它的对端 |
| 干净开机自动加载复验 | 待办：通过后才能去掉 `install-driver.sh` 的默认 blacklist |
| WPA2-Enterprise（802.1X） | 待办：代码路径未验证（实验环境没有 802.1X AP） |
| 协议细节 | 收尾：`0x0104`、`0x0105`、`0x050e` 的精确语义 |
| AP 模式 / 80 MHz / 蓝牙 | 范围外或可选 |

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

**遇到问题去哪里问**：先查 [FAQ.md](FAQ.md)；仍未解决就到
[Issues](https://github.com/Spkicn/zt9612-linux/issues) 反馈，请附 `lsusb -d 350b:`、`uname -a`
与 `dmesg` 全段（模板见 [.github/ISSUE_TEMPLATE/bug_report.md](.github/ISSUE_TEMPLATE/bug_report.md)）。
无线网卡驱动的问题几乎都能从这三样里看出来；缺信息时只能来回问，效率很低。

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

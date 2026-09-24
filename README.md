# zt9612: ZT9612U (ZTOP / ACEV100) USB Wi-Fi 6 driver for Linux

[![build](https://github.com/Spkicn/zt9612-linux/actions/workflows/build.yml/badge.svg)](https://github.com/Spkicn/zt9612-linux/actions/workflows/build.yml)
[![checkpatch](https://github.com/Spkicn/zt9612-linux/actions/workflows/checkpatch.yml/badge.svg)](https://github.com/Spkicn/zt9612-linux/actions/workflows/checkpatch.yml)
[![License: GPL-2.0-only](https://img.shields.io/badge/License-GPL--2.0--only-blue.svg)](LICENSE)

Linux 内核驱动，对应 VID:PID 为 `350b:9612` 的 ZT9612U / ACEV100 USB 无线网卡。

## 项目状态

当前版本 0.2.0：固件装载、IPC 初始化、mac80211 接口、双频扫描、关联、WPA2-PSK 加密与
端到端联网（DHCP + `ping` 外网）均已实机验证。

| 里程碑 | 状态 |
|---|---|
| 硬件识别与协议分析 | 完成 |
| M1 内核态固件装载 | 完成，已实机验证 |
| M2 IPC 初始化与 `/dev/zt9612` | 完成，已实机验证（IPC 往返成功） |
| M3.1 mac80211 注册（网络接口） | 完成，已实机验证（接口按真实 MAC 命名为 `wlx` + MAC 十六进制） |
| M3.2 扫描 | 完成，已实机验证（`iw scan` 实测 40+ 个 BSS） |
| M3.3 关联 | 完成，已实机验证（`iw connect` 关联开放 AP，`assoc=1`、接口 `LOWER_UP`） |
| M3.4 数据面 | 完成，已实机验证（关联后广播 ARP 出网并收到网关应答、单播回包正常回收）；IP 地址取决于 AP 是否提供 DHCP |
| M3.5 5GHz | 完成，已实机验证（`iw phy info` 列出 2.4G 14 信道 + 5G 25 信道；`iw scan` 59 个 BSS 中 7 个在 5GHz） |
| WPA2 关联与加密 | 完成，已实机验证（WPA2-PSK 热点：关联 226 ms、四次握手 344 ms、`PTK=CCMP GTK=CCMP`、DHCP 拿到租约、`ping` 外网 3/3） |

加载驱动后可以得到一个 managed 模式的无线接口（名字按 MAC 生成，例如 `wlxb4011aXXXXXX`），
`iw dev`、`iw scan`、`iw connect`、`ethtool -i`、`/dev/zt9612` 都可用。扫描覆盖 2.4 GHz 与
5 GHz，能列出周围 AP（含 SSID、信道、加密、HT/VHT 能力与**真实信号强度**）。关联、加密与
数据面均已走通：WPA2-PSK 热点下四次握手到 `COMPLETED`（`PTK=CCMP GTK=CCMP`，CCMP 由
mac80211 软件加解密），DHCP 能拿到租约，`ping` 网关与公网均通。WPA2-Enterprise（802.1X）
的代码路径**未验证**——当前实验环境里没有任何 802.1X AP。

> 扫描支持被动与主动两种：模块参数 `scan_probe` 控制是否由驱动自己发 probe request
> （`0`=被动，`1`=自建 probe，`2`=逐字节重放厂商 probe）；mac80211 触发的扫描走同一条 TX 路径。

这块网卡在 Windows 下工作正常。本项目的目标是在 Linux 下把它跑起来。厂商没有公开
发布 Linux 驱动，但存在内部版本，向厂商索取是更省力的路线。

## 硬件

| 项目 | 值 |
|---|---|
| 厂商 | ZTOP / 山东兆通微电子（Zhaotong Micro） |
| 模组 | ZT9612U V1.0（FCC ID `2BOBE-ZT9612UV10`） |
| 芯片平台 | ACEV100（自研 Wi-Fi 6 2T2R SoC，非 Realtek / MediaTek 方案） |
| 网卡模式 | `350b:9612`，bcdDevice `0x0200`，iProduct `802.11ax 2x2 WLAN Adapter` |
| 光盘模式 | `350b:f179`（USB CD-ROM，卷标 "Wi-Fi6 Adapter"） |
| 接口 | 1 个，class `FF/FF/FF`，5 个 512 字节批量端点（EP4-IN / EP5~8-OUT） |
| 固件 | `zt9612_fw.bin`（219,076 字节）+ `zt9612_settings.bin`（212 字节） |

支持范围与 ID 判定方法见 [supported-device-IDs](supported-device-IDs)。同厂其它型号
（`350b:9101`、`350b:9611` 等）不适用。

## 功能范围

已实现：

- 把设备从驱动光盘模式切到网卡模式（标准 SCSI 弹出，不是厂商私有命令）
- 完整复现固件下载协议：hello 握手、488 字节分块写入、末块整段 XOR16 校验、
  配置块、RUN（实机验证：449 块，校验值 `0x2d14`）
- 同步 IPC 初始化：`MM_RESET`、`MM_VERSION`、厂商私有段、`MM_START`（约 6.5 秒）、
  `MM_SET_IDLE`、`MM_ADD_IF`、`MM_SET_SLOTTIME`、`MM_SET_CHANNEL`
- 5 秒心跳维持固件存活；`/dev/zt9612` 上收发原始 `WLAN` 帧
- 注册 mac80211：出现 managed 模式的无线接口，**2.4 GHz 14 个信道（含 2484）+ 5 GHz 25 个信道**，
  `iw dev`、`iw phy`、`ethtool -i` 均可正常读取
- **扫描**：实现 `hw_scan` 与 RX 数据路径，mac80211 会把一次 `iw scan` 按频段拆成两段
  （2.4 GHz 14 信道 + 5 GHz 25 信道）；实机实测一轮 59 个 BSS，其中 7 个在 5 GHz
- **关联与数据面**：`iw connect` 关联开放 AP 成功，TX/RX 双向正常，`iw link` 计数增长
- **WPA2-PSK 加密**：四次握手到 `wpa_state=COMPLETED`（`PTK=CCMP GTK=CCMP`），DHCP 拿到
  租约并 `ping` 通网关与公网；驱动不实现 `set_key`，CCMP 由 mac80211 走软件加解密

尚未实现：

- WPA2-Enterprise（802.1X）：代码路径未验证（实验环境无 802.1X AP；`wpa_supplicant`
  的 EAP 配置与 PSK 路径共用同一条数据面）
- 主动扫描默认关闭（`scan_probe=0` 为被动）：`scan_probe=2` 的厂商 probe 回放已实测能收到
  probe response，且主动探测会跳过 cfg80211 标记 `NO_IR`/`RADAR` 的信道
- 吞吐优化：2.4 GHz 频段只声明 4 个 CCK 速率，且驱动不上报 TX status，
  实测速率恒定 1 Mbit/s（功能正常，吞吐偏低）
- AP 模式、蓝牙、40/80 MHz 带宽

## 兼容性

| 项目 | 值 |
|---|---|
| 已实机验证 | `7.0.0-31-generic`（Ubuntu 24.04.5 LTS，x86_64）：M1–M3.5 + WPA2-PSK 全部通过（固件装载、IPC、mac80211、双频扫描、关联、DHCP 与公网 `ping`） |
| 已验证可编译 | `6.17.0-1022-azure`（CI，ubuntu-24.04 runner，无告警）；`modinfo` 正确生成 `alias: usb:v350Bp9612d*` |
| 编译下限 | 6.12（见 `dkms.conf` 的 `BUILD_EXCLUSIVE_KERNEL`）：驱动包含 6.12 才引入的 `linux/unaligned.h`，CI 上 6.8 内核即因缺该头文件失败 |
| 未验证区间 | 6.12~6.16 能否正常工作未验证；mac80211 ops 签名只在 6.17 及以上确认匹配 |
| 构建依赖 | `build-essential`、`linux-headers-$(uname -r)` |

驱动使用了较新的 mac80211 ops 签名，例如
`config(struct ieee80211_hw *, int radio_idx, u32 changed)` 和
`tx(struct ieee80211_hw *, struct ieee80211_tx_control *, struct sk_buff *)`。
在较老内核上需要适配，欢迎提 PR 并附完整报错与 `uname -a`。

## 安装

### 1. 准备固件

固件版权属于厂商，本仓库不再分发。请从自己网卡的配套驱动盘中取得
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
sudo dkms remove zt9612/0.2.0 --all
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

- 在 BIOS 中关闭 Secure Boot
- 生成 MOK 密钥并注册（一次性）

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

**DKMS + Secure Boot 实测注意（2026-09-24，dkms 3.0.11 / Ubuntu 24.04 / 模块压缩为 `.ko.zst`）**：
`dkms add/build/install` 本身正常（`dkms status` 显示 `zt9612/0.2.0, <kernel>, x86_64: installed`），
但在 `/etc/dkms/framework.conf` 里指定已注册的 `mok_signing_key` / `mok_certificate` 之后，
DKMS 虽然打印 `Signing module …`，**安装到 `/lib/modules/$(uname -r)/updates/dkms/zt9612.ko.zst`
的产物仍然没有签名**，`modprobe` 会以 `Key was rejected by service` 失败。就地补签即可：

```bash
KVER=$(uname -r); KO=/lib/modules/$KVER/updates/dkms/zt9612.ko.zst
sudo zstd -d -f $KO -o /tmp/zt9612.ko
sudo $KVER/build/scripts/sign-file sha256 MOK.priv MOK.pem /tmp/zt9612.ko   # 密钥路径按实际调整
sudo zstd -f -q /tmp/zt9612.ko -o $KO
sudo depmod -a && sudo modprobe zt9612
modinfo -k $KVER zt9612 | grep signer      # 应显示已注册的签名者
```

补签后 `dkms status` 会提示 `WARNING! Diff between built and installed module!`（安装产物被重签过），
属预期现象；下一次 `dkms install` 会再次覆盖成未签名版本，需要重复上面这一步。

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
zt9612 1-9:1.0: MAC = XX:XX:XX:XX:XX:XX
zt9612 1-9:1.0: MM_START_REQ (rf init, waiting ~6.5s)
zt9612 1-9:1.0: MM_START_CFM received (firmware up)
zt9612 1-9:1.0: MM_ADD_IF_CFM: status=0 inst_nbr=0
zt9612 1-9:1.0: M1+M2 done: firmware running, /dev/zt9612 ready
zt9612 1-9:1.0: mac80211 registered (M3.1) - wlan0 should appear
zt9612 1-9:1.0 wlxXXXXXXXXXXXX: renamed from wlan0
zt9612 1-9:1.0: mac80211: start
zt9612 1-9:1.0: mac80211: add_interface type=2 addr=XX:XX:XX:XX:XX:XX
```

> 上面的 MAC 与接口名已用 `XX` 隐去（驱动实际打印的是内核 `%pM` 格式的地址）。
> 接口名是 systemd 按 MAC 生成的可预测名（`wlx` + 12 位十六进制），
> 所以**不要**假设它叫 `wlan0`。早期版本读 MAC 时错位（少读一个 status 字节），
> 打印出来的是整体左移一字节、末字节被 0 顶掉的值。若你看到的是那样的值，
> 说明模块来自 v0.2.0 之前的代码：此时 TX 帧会被固件静默丢弃、单播 RX 也会被过滤掉
> （见 CHANGELOG 的 Fixed 一节）。

再做一次 IPC 往返自测（M2 验收）：

```bash
sudo python3 scripts/zt9612-devtest.py
# 期望：往返成功 2/2（MM_VERSION_REQ 收到 CFM；厂商 0x0100 返回 MAC）
sudo python3 scripts/zt9612-devtest.py --seconds 20    # 顺带检查心跳稳定性
```

M3.1 已在 `7.0.0-31-generic` 上实机验证：`iw dev` 能看到 managed 接口，`iw phy phy0 info`
列出 2.4 GHz **14** 个信道（含 2484）+ 5 GHz **25** 个信道（5180–5825），`ethtool -i`
返回 `driver: zt9612`。
注意接口名不一定叫 `wlan0`：systemd 会按 MAC 生成可预测名（形如 `wlxb4011aXXXXXX`），
用 `ls /sys/class/net | grep -E '^(wlx|wlan)'` 或 `iw dev` 查实际名字。

## 已知问题

### 反复 rmmod / insmod 会泄漏实例（已实测，可重载）

2026-09-24 的稳定性回归（v0.2.0，10 轮 rmmod/insmod）实测：**12 次卸载里 11 次**出现
`rx urb 2s 未回收（设备可能已挂死），泄漏该实例以避免 UAF` +
`disconnected (teardown incomplete, instance leaked)`（唯一一次干净卸载紧跟在另一次干净
卸载之后，设备还没进入"不再应答在途 URB"的状态）。

- 原因不是死锁：卸载时设备不再回应在途的 bulk-IN，`usb_poison_urb()` 之后 2 秒内收不到
  completion，驱动按 D9 设计**故意泄漏该实例**（宁可泄漏也不 use-after-free）。
- 之后 USB 会自行重新枚举（`350b:f179` 光驱 → `350b:9612` 网卡，约 8–11 秒），
  所以重新加载必须等 ~20 秒；本轮 10/10 轮重新加载成功，接口与扫描全部恢复。
- 同一轮回归的其他项目全部干净：20 次接口 up/down + 每次 `iw scan`（rc=0，51–58 个 BSS）、
  10 分钟 NetworkManager 长跑（含自动扫描）、3 轮热点断连重连（3/3 重新 `COMPLETED` 并
  拿到 DHCP 租约、`ping` 通网关）——全程 **0 Oops / 0 BUG / 0 WARNING**，
  `rx.c:5475` 始终为 0，也没有再出现「能 ping 不能 SSH」的失联。
- 代价：每次卸载泄漏一个实例（KB 级）；阶段 1 的 Slab 从 424.3 MB 涨到 425.5 MB
  （含扫描等噪声，未做严格归因）。

结论：**可以重载（等 20 秒），但每次卸载都有泄漏**。彻底消除需要在卸载前让固件停下来
或更可靠地回收 URB，留作后续任务；在此之前「换模块优先重启」仍是更保险的做法，
`uninstall-driver.sh` 也继续默认不执行 `rmmod`。

### 开机自动加载曾导致失联，根因已定位并修复

这一条此前是未定位问题：模块经 USB modalias 在开机时自动加载后，机器会变成「能 ping、
能连 22 端口，但 SSH 读不到 banner」，只能硬重启。现在原因已经查清，是驱动的缺陷：

驱动注册 wiphy 时没有设置父设备（缺少 `SET_IEEE80211_DEV()`），于是 `wiphy_dev(wiphy)`
为 `NULL`。无线接口出现后 NetworkManager 立即通过 `SIOCETHTOOL` 查询驱动信息，
`cfg80211_get_drvinfo()` 解引用空指针，在关闭中断的状态下崩溃，用户态随之卡死。
实测崩溃现场：

```
BUG: kernel NULL pointer dereference, address: 0000000000000068
CPU: 16 PID: 1127 Comm: NetworkManager  Tainted: G  O  7.0.0-31-generic
RIP: 0010:cfg80211_get_drvinfo+0x27/0x1c0 [cfg80211]
Call Trace: ethtool_get_drvinfo → __dev_ethtool → dev_ioctl
note: NetworkManager[1127] exited with irqs disabled
```

修复是在 `ieee80211_register_hw()` 之前加上 `SET_IEEE80211_DEV(hw, &z->intf->dev)`。
修复后已在实机复测：`ethtool -i` 正常返回，NetworkManager、sshd 全程存活，`dmesg`
无 oops。

`install-driver.sh` 仍然默认写入 `blacklist zt9612`，因为「干净开机自动加载」这条路径
还没有重新跑过一次完整验证。确认没问题后可以用 `--enable-autoload`。

### 芯片挂死后需要物理拔插

出现 `-110`、`can't set config #1`、`Entity not found` 表示芯片已挂死，软件复位无效。
常见触发原因：固件运行期间重复灌固件、切模式后拖延过久、不等 CFM 就连发 IPC 导致固件
断言崩溃（事件中会出现 `macif.c` 字样）。处理办法是拔下网卡等待 10 秒再插上。

### 设备可能自行回到光盘模式

固件看门狗复位，原因是主机超过 5 秒没有发送心跳。内核驱动用 `delayed_work` 每 5 秒发送
一次；如果自行编写用户态脚本，需要自己维持心跳。

### 其它

- 扫描结果的 `signal` 是**描述符里的真值**（int8 dBm，实测 −94…−23 dBm；同 AP 同信道组内
  极差 ≤2 dB），`iw scan` 可直接用于判断远近
- 单帧上限约 1000 字节，较大的 802.11 帧会被截断
- 驱动源码中部分中文注释在早期编辑中损坏成乱码，不影响编译，待清理
- 支持 2.4 GHz + 5 GHz、STA 模式；AP 模式与蓝牙未实现
- 2.4 GHz 频段只声明 4 个 CCK 速率且驱动不上报 TX status，实测速率恒定 1 Mbit/s（吞吐偏低）
- 0.x 阶段的接口（模块参数、`/dev` 协议）可能变化

## 调试

```bash
sudo dmesg -C && sudo modprobe zt9612 && sudo dmesg | tail -30
ls -l /dev/zt9612                        # M2 的原始 IPC 通道
cat /sys/module/zt9612/parameters/*      # 模块参数当前值
```

模块参数说明见 [zt9612.conf](zt9612.conf)：

| 参数 | 默认 | 说明 |
|---|---|---|
| `do_init` | 1 | 固件装载后是否执行同步初始化序列；`0` 表示只装载固件 |
| `do_boot` | 1 | 是否下载固件；`0` 表示复用已在运行的固件，只做 IPC 初始化 |
| `scan_probe` | 0 | 扫描时是否主动发送 probe request：`0` 被动（默认），`1` 自建 probe（尚未单独复验），`2` 逐字节重放厂商 probe（**已验证能收到 probe response**）。主动探测会跳过 cfg80211 标记 `NO_IR`/`RADAR` 的信道 |
| `tx_ep` | 5 | TX 端点号（实验开关，一般不用改） |
| `tx_prep` | 1 | 切换信道前是否重放厂商的使能序列 |
| `tx_variant` | 0 | TX 帧变体实验开关 |

`/dev/zt9612` 的接口约定（调试通道，非数据面）：

| 操作 | 语义 |
|---|---|
| `write()` | 传入完整 `WLAN` 帧（`"WLAN" + u16 hlen + u16 type + payload`），原样发往 EP8 |
| `read()` | 返回一条设备发来的完整帧，阻塞等待且 3 秒超时，支持 `O_NONBLOCK`；保证整帧，半帧不会错位 |
| `poll()` | 支持 `select()`/`poll()` 等待可读 |
| ioctl `ZT_IOC_TXRAW` | 把一条完整 `WLAN` 帧直接交给 TX 端点，用于**不重载模块**就试验 TX 描述符与时序 |
| debugfs `zt9612/tx_raw` | 同上，走写入方式（Secure Boot 的 `lockdown=integrity` 下 debugfs 不可写，通常用 ioctl） |

内核 taint 提示：任何外部模块加载后内核都会被标记 `O`，未签名模块再加 `E`。
因此 `cat /proc/sys/kernel/tainted` 非 0，以及 `dmesg` 中的
`loading out-of-tree module taints kernel`，都是预期现象。只有 `P`（专有模块）才代表
许可证问题，本驱动是 GPL 兼容的，不会出现。

其它问题先看 [FAQ.md](FAQ.md)。

## 路线图

| 步骤 | 目标 | 状态 |
|---|---|---|
| M3.1 | 注册 mac80211 并出现无线接口 | 已完成并实机验证 |
| M3.2 | `iw dev <iface> scan` 能扫到 AP | 已完成并实机验证（双频，一轮 59 个 BSS） |
| M3.3 | 关联（开放 AP 与 WPA2-PSK） | 已完成并实机验证 |
| M3.4 | 数据面与联网 | 已完成并实机验证（DHCP 租约 + `ping` 网关与公网） |
| M3.5 | 5 GHz 频段 | 已完成并实机验证 |
| 下一步 | 速率与吞吐 | 2.4 GHz 速率表与 TX status 上报（当前恒定 1 Mbit/s） |
| 下一步 | WPA2-Enterprise（802.1X） | 代码路径未验证（实验环境无 802.1X AP） |
| 收尾 | 协议细节 | `0x020c`、`0x0104`、`0x0105`、`0x050e` 的精确语义 |
| 可选 | 扩展 | AP 模式、40/80 MHz、蓝牙（同芯片 BT 功能，属复合接口） |

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
- 厂商私有消息段（`0x01xx`、`0x02xx`、`0x05xx`）不在开源枚举中，实现上按实测序列
  原样重放，不推测结构

## 参与贡献

见 [CONTRIBUTING.md](CONTRIBUTING.md)。目前最需要的是实机测试反馈，以及 M3 未知项的突破。

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

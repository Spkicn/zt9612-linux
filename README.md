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
但都不提升吞吐**；截至 2026-10-05（r42–r44）的端到端数字与结论见下表。历史上
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
  **都不提升实测吞吐**（速率由固件内部决定）。早期两版归因**均已作废**：①「`MM_STA_ADD`
  登记后 8–40s 定时死亡」由 `sta_add_fmt=7` 拆除（r26）；②「连续聚合有限次数后打挂设备」
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
| 已验证可编译 | `6.17.0-1022-azure`（CI 阻塞作业，无告警）；CI 内核矩阵另在 **`6.12.112` / `6.14.11` / `6.16.12`**（Ubuntu mainline headers）上编译通过；`modinfo` 正确生成 `alias: usb:v350Bp9612d*` |
| 编译下限 | 6.12（见 `dkms.conf` 的 `BUILD_EXCLUSIVE_KERNEL`）：驱动包含 6.12 才引入的 `linux/unaligned.h`；6.17 给 `config` 等 op 加了 `radio_idx`，驱动内有按 `LINUX_VERSION_CODE` 选择的转发，因此 6.12–6.16 也能编译 |
| 未验证区间 | 6.12–6.16 的**运行**行为未验证（只做了编译取证，没有这些内核的实机）；6.12 以下不支持（缺 `linux/unaligned.h`） |
| 构建依赖 | `build-essential`、`linux-headers-$(uname -r)` |

驱动使用较新的 mac80211 ops 签名（`config(struct ieee80211_hw *, int radio_idx, u32 changed)`、
`tx(struct ieee80211_hw *, struct ieee80211_tx_control *, struct sk_buff *)`）。其中 `config` 在
6.17 才加上 `radio_idx`，6.12–6.16 是 `(hw, u32 changed)`，所以驱动里有一层按
`LINUX_VERSION_CODE` 选择的转发来保住 6.12 下限（详见 CHANGELOG `[Unreleased]`）。
更老的内核需要另加适配，欢迎提 PR 并附完整报错与 `uname -a`。

## 安装与验收

**完整步骤（含 Secure Boot / MOK 签名与 DKMS 补签）见 [INSTALL.md](INSTALL.md)。** 最简路径：

```bash
sudo ./scripts/install-firmware.sh /path/to/firmware-dir   # 固件自备；脚本校验 SHA-256
sudo ./scripts/setup-mode-switch.sh                        # 插卡时自动从光驱模式切到网卡
sudo ./install-driver.sh                                   # 编译、按需签名、安装并加载一次
sudo dmesg | tail -30                                      # 期望 firmware loaded → M1+M2 done
sudo python3 scripts/zt9612-devtest.py                     # 期望「往返成功 2/2」
```

> [!IMPORTANT]
> **手工加过守卫的机器**上是另一种情况：守卫文件里除了 `blacklist` 还可能有
> `install zt9612 /bin/true`。那种环境下 `sudo modprobe zt9612` 会**静默返回 0 且什么都不做**
> （实测 2026-09-27，内核 7.0.0-34），必须用 `sudo modprobe --ignore-install zt9612`。

## 已知问题

完整清单、处置步骤与反馈渠道见 [TROUBLESHOOTING.md](TROUBLESHOOTING.md)。最要紧的三条：

- **上传吞吐上限约 15 Mbit/s，主机侧无法再提**：七个提速杠杆已逐一实测证否，速率由固件内部
  决定（见「项目状态」）。
- **A-MPDU 聚合默认关闭**（`ampdu_en=0`）：打开后在真实 TCP 下可长跑（10,930 次零断言）但
  **不提速**；UDP 无流控满压需同时开 `agg_pred_us>0`，否则会断言并 USB 重枚举。
- **设备挂死后只能冷启动复位**：S5 关机 + RTC 唤醒；热重启无效，也不必物理拔插。

## 文档

| 文档 | 内容 |
|---|---|
| [INSTALL.md](INSTALL.md) | 安装、Secure Boot / MOK 签名、DKMS、验收命令与期望输出 |
| [TROUBLESHOOTING.md](TROUBLESHOOTING.md) | 已知问题完整清单、挂死恢复、反馈渠道 |
| [PARAMETERS.md](PARAMETERS.md) | 模块参数（36 个）与 `/dev/zt9612` 接口 |
| [FAQ.md](FAQ.md) | 常见问题（Secure Boot、固件、挂死、rmmod 风险） |
| [CHANGELOG.md](CHANGELOG.md) | 变更日志，含每轮实验的结论与更正 |
| [CONTRIBUTING.md](CONTRIBUTING.md) | 贡献规范、DCO、AI 政策、禁提交清单 |
| [zt9612.conf](zt9612.conf) | `/etc/modprobe.d` 参数模板（逐参数取值说明） |
| [supported-device-IDs](supported-device-IDs) | USB ID 判定与不支持的同厂型号 |

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

**遇到问题**：先查 [TROUBLESHOOTING.md](TROUBLESHOOTING.md) 与 [FAQ.md](FAQ.md)；仍未解决就到
[Issues](https://github.com/Spkicn/zt9612-linux/issues) 反馈 —— 请附 `lsusb -d 350b:`、
`uname -a` 与 `dmesg` 全段（模板见 [.github/ISSUE_TEMPLATE/bug_report.md](.github/ISSUE_TEMPLATE/bug_report.md)）；
这些信息可以一条命令收齐：`sudo ./scripts/collect-debug-info.sh`。

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

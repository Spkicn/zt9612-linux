# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概览

ZT9612U（ZTOP / 兆通微 ACEV100）USB Wi-Fi 6 网卡的**独立实现** Linux 驱动（VID:PID `350b:9612`）。厂商没有公开发布 Linux 驱动；本驱动完全依据设备对外可观测的行为（USB 描述符、总线请求/响应序列）与公开的 CEVA RivieraWaves rwnx 消息框架知识编写。当前发布 **0.3.2**；`[Unreleased]` 的 v0.4 三大组件（固件站点会话 `sta_add_fmt=7`、WPA2 密钥路径 `key_en=2`、A-MPDU 聚合 `ampdu_en`）**均已实机走通，行为开关默认关，默认联网行为与 0.3.2 一致**。已实机验证：固件装载、IPC 初始化、双频扫描、关联、WPA2-PSK、DHCP、外网访问。**吞吐真值（r42–r44，端到端、对端接收侧逐字节核对）**：5 GHz 上传 **12.7–14.9 Mbit/s**、下载 **18.5–20.4 Mbit/s**、UDP 容量 15.0 Mbit/s；厂商**同口径**（经出口核对）13.37–18.21 ⇒ **同量级**（曾被引用的"95.3 Mbit/s / 差 26 倍"是**近端 sink 口径、不可比**，已退役）。聚合经 r40 修掉 `macif.c:1019` 断言、r42 在真实 TCP 下长跑 10,930 次零断言，但**不提速**；UDP 满压需 `agg_pred_us>0`（r44）。**主机侧七个提速杠杆已逐一证否** ⇒ 约 15 Mbit/s 是固件 TX 侧上限。设备挂死用 **S5 冷启动 + RTC 唤醒**远程复位（`docs/06` §2.4），**不必物理拔插**。状态结论迭代频繁，**引用前以 `CHANGELOG.md` `[Unreleased]` 顶部与 `README.md`「项目状态」为准**。

项目文档以中文为主，本文件与所有公开文档保持一致使用中文。**全仓库禁止 emoji**（文档与代码注释都不允许；机检 `python ci/emoji_check.py`，公开 CI 每次 push 都跑），需要强调时用 `> [!WARNING]` 这类 alert 语法或文字。**markdown 里也不要裸用 `~`**（GitHub 把单个 `~` 渲染成删除线，区间用 `–`、约数写「约 N」；机检 `python ci/markdown_check.py`，Release 正文用同脚本单独查）。

## 常用命令

驱动是 out-of-tree 内核模块，**必须在 Linux 目标机上编译**（Windows 开发机上没有内核头文件）。开发机上通过 `tools/rsh.py` 把源码上传到测试机再编译（见「开发工作流」）。编译下限内核 **6.12**（驱动 include `linux/unaligned.h`，`dkms.conf` 用 `BUILD_EXCLUSIVE_KERNEL` 声明；实机验证内核 7.0.0-31 / 7.0.0-34）。

```bash
# 编译 / 安装（在 Linux 上，driver/ 目录内）
cd driver
make                       # 生成 zt9612.ko
make sign                  # Secure Boot 机器：用 MOK 密钥签名（= make + sign-file）
make install               # 安装到 /lib/modules/$(uname -r)/extra/ 并 depmod
make checkpatch            # 内核风格检查（.checkpatch.conf 参数：--no-tree --strict --max-line-length=100）
make help                  # 列出全部目标；可覆盖 KVER / KSRC / JOBS

# DKMS
sudo dkms install .        # add + build + install 一步完成
dkms status

# 加载（开发期用 insmod，不要先进 extra/）
sudo dmesg -C && sudo insmod zt9612.ko && sudo dmesg | tail -30
sudo modprobe --ignore-install zt9612    # 有守卫文件时（普通 modprobe 会静默返回 0 且不加载）

# 用户态自测
sudo python3 scripts/zt9612-devtest.py   # 期望「往返成功 2/2」

# 文档 / 版本一致性机检（本地完整树）
python ci/version_check.py               # 公开脚本，CI 每次 push 都跑；四处版本号必须一致
python tools/doc_lint.py                 # 本地保留：CRLF / 表格 / 参数表 / 默认值 / 行数引用
```

`ci/version_check.py` 校验 `dkms.conf` 的 `PACKAGE_VERSION` / `driver/zt9612.c` 的 `MODULE_VERSION()` / `CHANGELOG.md` 最新已发布小节 / `README.md`「最新发布」四处一致。**改版本号或 README 状态后必跑。**

**没有自动化运行时测试**：CI 只保证编译通过（`build.yml`，ubuntu-24.04 阻塞 + 一个仅信息性的 old-kernel 对照作业验证下限声明）、风格（`checkpatch.yml`）、脚本静态检查（`shellcheck.yml`）、版本一致与 emoji 检查（`docs.yml`）。回归 100% 靠真机人工验证。

## 架构

驱动是**单文件**实现 `driver/zt9612.c`（约 4200 行），分三个逻辑层，自顶向下：

1. **固件装载层（M1）** — `zt_boot()`：hello 握手 → 488 字节分块写入固件 → 末块整段 XOR16 校验 → 配置块 → RUN → 等启动通知（`type=0x0100`）。
2. **IPC / 消息层（M2）** — `zt_cmd()` / `zt_send()` / `zt_recv()`：所有主机↔设备消息走 bulk 端点，统一帧格式 `"WLAN" + u16 hlen + u16 type + payload`（端点分工：IPC 主机→设备 **EP8-OUT**、设备→主机 **EP4-IN**；数据面 TX 另走 **EP5-OUT**，另有 EP2-IN 通知通道）。消息 ABI 为 rwnx 血统 `struct lmac_msg { u16 id; u16 dest; u16 src; u16 param_len; u32 param[]; }`，主机 `src_id` 固定 100。`zt_run_init()` 执行 12 步同步初始化（`MM_RESET`/`MM_START` 约 6.5s 等）。另有 `/dev/zt9612` 字符设备（misc）与 debugfs `zt9612/tx_raw` 作调试通道。
3. **mac80211 层（M3）** — 文件下半部分 `/* ==== mac80211` 之后：managed 接口、双频 band（2.4G 14 ch + 5G 25 ch）、`hw_scan`、关联、软件加解密。**`.tx` 与 `.wake_tx_queue` 只登记不发送**，真正的 `usb_bulk_msg()` 在 `zt_tx_work()`（进程上下文，可睡眠）里做。

关键数据流：
- **TX**：mac80211 → `zt_mac_tx`/`zt_mac_wake_tx_queue`（登记到 `z->txq`）→ `zt_tx_work`（进程上下文）→ `zt_tx_build()`（构造 28 字节描述符）→ `usb_bulk_msg()`。聚合时 `zt_tx_agg_send()` 把多个 MPDU 拼进同一次 bulk（**组帧在驱动里做**，mac80211 一次只给一个 MPDU）。
- **RX**：中断 URB → kfifo → `zt_rx_inject()` 注入 mac80211。
- **心跳**：`delayed_work` 每 5 秒发 `0x05c2`，否则固件看门狗复位、设备退回光盘模式。

**卸载路径**（`zt_disconnect`）刻意防死锁：先置 `z->alive=false` 拒绝一切 USB IO，再 `usb_poison_urb()` + 最多等 2 秒，**超时故意泄漏实例而非 use-after-free**。新增收尾代码必须沿用这个模式，禁止 `usb_kill_urb()` 的无界等待。

`driver/zt9612_fw.c` 是 M1 阶段的独立固件装载模块，**不参与编译**（`Makefile` 只编 `zt9612.o`），保留供对照。**`zt9612.c` 里有历史乱码注释**（双重编码，如 `閿涙…`）：属已知问题，待清理清单与判据登记在 `docs/07` §9 / `docs/11` P2-4，**不要整文件批量重编码**（会把合法中文注释一起破坏）。

## 关键的硬件与实验纪律（会挂机的坑）

详见 `CONTRIBUTING.md` §5、`docs/03` §5、`docs/12` §7。最要紧的：

1. **禁止在运行中的机器上反复 `rmmod`/`insmod`** —— 已两次把整机搞成「能 ping 不能 SSH」，只能硬重启。换模块靠重启机器。
2. **开发期保持 blacklist 生效**，不让模块开机自动加载（`install-driver.sh` 的默认行为）。
3. **每次实验前 `sudo dmesg -C`，结束后落盘** `sudo dmesg > ~/dmesg_$(date +%H%M%S).txt` —— 整机失联时 `/var/log` 不保证留下。
4. **IPC 必须一问一答**：发一条 → 等对应 CFM → 再发下一条。连发会让固件断言崩溃。
5. **不要调 `usb_set_interface()`**（单 alt setting，调用后设备行为异常）。
6. **芯片挂死（`-110` / `Entity not found` / 反复 `hello ack timeout`）的唯一可靠复位 = S5 冷启动 + RTC 闹钟唤醒**（`rtcwake -m no -s 180` + `systemctl poweroff`，脚本 `tools/r42_coldboot.sh` + `tools/r42_after_boot.sh`）：只有真正断 VBUS 才会重开芯片 ROM 的秒级固件接收窗口。**r42c 实测已证无效**：热 `systemctl reboot`、端口 `disable` 往返、设备级/驱动级 unbind-bind、`USBDEVFS_RESET`、`authorized` 往返、`xhci_hcd` unbind→bind、`usb_modeswitch -R`、D3cold。旧的软件恢复阶梯 `re/r40_recover.py`（r40 曾记"零拔插恢复"）对 `-110` 深挂死**不再推荐**，**不要**只靠它。详见 `docs/06` §2.4。
7. **重灌固件前必须断电复位**（切一次模式、拔插，或 S5 冷启动）；同一上电周期重复装载会失败。
8. **TX 实验必须带功能判据**：`usb_bulk_msg` 完成得快 ≠ 帧发出去了。用 `tools/arp_oracle.py`（仅开放网络有效）或 `ping -M do 1472` / DHCP 等 A 级判据。
9. **打流前必须绑定接口**（`SO_BINDTODEVICE`，再用 `ip route get` 复核）——测试机有线/无线双网卡同网段，不绑定的测试流量全走有线口，曾造成数周「35 倍差」误判（r34 终判）。

## 开发工作流

开发在 Windows 上，实测在 Linux 测试机（`docs/03` §1 有测试机信息，其余在本地保留目录）。惯例是**先用户态原型验证协议改动，再搬进内核**，避免反复把设备搞挂。

```bash
python tools/rsh.py "<远程命令>"          # 在测试机执行一条命令
python tools/rsh.py --script tools/x.sh   # 把本地脚本当 bash 在远端跑
python tools/rsh.py --put <本地> <远端>    # 上传文件
```

实验工具全部经 `rsh.py` 从开发机调用：A/B 臂标准跑法 `tools/run_arm.sh`（等接口 → 关 NetworkManager 托管 → 手动关联指定 BSSID → 测量），固件侧观测 `tools/zt_arm_tool.py`（census / kick / rawtx / mempeek / memdump，经 `/dev/zt9612`），高风险设备实验走 `re/r40_safe_probe.sh`（健康门 + 单发 + 冷却 + 硬失败即停）；设备挂死后用 **S5 冷启动**复位（`tools/r42_coldboot.sh` + `tools/r42_after_boot.sh`，旧的 `re/r40_recover.py` 阶梯对 `-110` 深挂死已证无效）。

改动分级（`docs/12` §2，**不许降级**）：L0 文档/注释 → L1 脚本/工具 → L2 驱动非 TX 路径（需实机编译 + M1/M2 + M3.1–M3.4 回归 + checkpatch 无新增告警）→ L3 协议/固件交互/TX-RX 数据路径（另加功能判据 + 单臂 A/B + 动手前预登记回滚条件 + 一变量一轮）。

**新功能一律 opt-in 模块参数（默认关）**，默认值 = 与上一发布版本行为一致，保证运行时可回退、可 A/B。加载期参数，换臂只能重启。

## 仓库内容边界（公开 vs 本地保留）

驱动由 `.gitignore` 严格切分：**公开的只有驱动源码、构建/安装脚本与开源规范文档**。

- **公开**：`driver/`（`zt9612.c`/`Makefile`/`zt9612_fw.c`）、`scripts/`、`install-driver.sh`、`uninstall-driver.sh`、`dkms-make.sh`、`dkms.conf`、`README.md`、`INSTALL.md`、`TROUBLESHOOTING.md`、`PARAMETERS.md`、`CONTRIBUTING.md`、`CHANGELOG.md`、`FAQ.md`、`AGENTS.md`、`CLAUDE.md`、`zt9612.conf`、`supported-device-IDs`、`ci/`、`.github/`（`firmware/` 目录只有占位 README，二进制由用户自备）
- **本地保留（已 gitignore，公开仓库不含）**：`docs/`（内部交接文档，含测试机信息/凭据）、`re/`（逆向报告、抓包、用户态原型）、`tools/`（运维脚本 + 明文凭据 `ssh/pass.txt`）、`firmware_from_cd/`、`vendor-request.txt`、`.workbuddy/`

提交前做脱敏检查（`docs/07` §8）：`git grep` 查凭据、测试机 IP、真实 MAC / 接口名 / SSID。**厂商固件、抓包、反编译产物、密钥、凭据一律不入库。**

## 文档与维护约定

文档分工（`docs/12` §1 是总入口）：`docs/12` = 「以后怎么干活」唯一入口；`docs/02` = 协议规格；`docs/03` = 上机环境与铁律；`docs/04` = 踩坑记录（Dxx）；`docs/07` = 公开边界与发版流程；`docs/14` = 对外呈现规范（README / Release / commit 写法）；`docs/11`/`13` = 已收口的里程碑计划（只作历史）；`re/REPORT_*.md` 与 `CHANGELOG.md` = 结论。

**改驱动行为 / 新增模块参数**时，同一批提交里必须同步：`PARAMETERS.md`（公开的参数表）、`zt9612.conf`、`docs/03` §7、`docs/05`、`docs/08` **五处参数表** + `CHANGELOG.md` 的 `[Unreleased]`（机检：`tools/doc_lint.py` 按 `PARAMETERS.md` 与三份内部文档对账）。结论被推翻时不要静默改写历史，加更正横幅并写进 `CHANGELOG` 新小节。

提交规范（`docs/07` §4）：区域前缀 `driver`/`scripts`/`docs`/`ci`/`build`，标题祈使句 ≤75 字符，正文 75 列折行，一个提交只做一件事（可 bisect），`git commit -s` 加 DCO 签名。AI 辅助需加 `Assisted-by: LLM <工具名>`，但 **AI 不得添加 `Signed-off-by`**（DCO 是法律声明）。

**注意**：文档中引用的源码行号会随编辑漂移，永远以现场 `grep -n` 为准，不要照抄文档里的旧行号。

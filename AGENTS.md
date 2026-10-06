# AGENTS.md

ZT9612U（ZTOP / ACEV100，`350b:9612`）USB Wi-Fi 6 网卡的独立实现 Linux 驱动。
**仓库文档、代码注释一律用中文，且一律不使用 emoji**（终端 / diff / 邮件里表现不一致，还会
污染 `grep`；机检 `python ci/emoji_check.py`，已接入公开 CI，见 `.github/workflows/docs.yml`）。
**markdown 里也不要裸用 `~`**：GitHub 会把单个 `~` 渲染成删除线（`3.7~4.7` 与 `7~13` 这种写法
会让整段被划掉）；区间用 `–`，约数写「约 15 Mbit/s」，必须用时放进反引号（机检 `python ci/markdown_check.py`）。
**提交信息**无强制语言要求（历史上中英混用，近轮以英文为主）。
状态结论迭代很快（数天内多次反转）：真值以 `README.md`「项目状态」与
`CHANGELOG.md` `[Unreleased]` 顶部为准，不要照抄旧文档的结论或行号 —— 行号一律 `grep -n` 现场确认。

## 构建与验证

驱动是 out-of-tree 内核模块，**必须在 Linux 目标机上编译**（Windows 开发机没有内核头文件），
编译下限内核 6.12（`dkms.conf` 的 `BUILD_EXCLUSIVE_KERNEL`；驱动 include 的是 6.12 才改名的
`linux/unaligned.h`）。**6.17 给 `config` 等 op 加了 `radio_idx`，驱动内有 `LINUX_VERSION_CODE`
兼容转发；CI 的 `build-kernel-matrix` 对 6.12/6.14/6.16 做编译取证（运行行为仍以实机为准）。**
Windows 侧把源码/脚本传到测试机，在那边编译、加载、取证：

```bash
python tools/rsh.py "<远程命令>"           # 在测试机执行一条命令（凭据 tools/ssh/pass.txt，已 gitignore）
python tools/rsh.py --script tools/x.sh    # 把本地脚本当 bash 在远端跑（注入 ZT_SUDO_PASS）
python tools/rsh.py --put <本地> <远端>     # 上传文件
```

```bash
cd driver
make                 # 生成 zt9612.ko；可覆盖 KVER / KSRC(KERNEL_SRC) / JOBS
make checkpatch      # --no-tree --strict --max-line-length=100；新增代码不得引入新告警
make sign            # Secure Boot 机器：MOK 签名；make sign-install = sign + install
make help

sudo dmesg -C && sudo modprobe --ignore-install zt9612 && sudo dmesg | tail -30
sudo python3 scripts/zt9612-devtest.py     # 期望「往返成功 2/2」

python ci/version_check.py   # 改版本号或 README 状态后必跑：dkms.conf / MODULE_VERSION /
                             # CHANGELOG 最新已发布小节 / README 最新发布 四处必须一致
```

**没有自动化运行时测试**：CI 只做编译（build.yml，ubuntu-24.04 阻塞）、checkpatch（只有
ERROR 阻塞，WARNING/CHECK 是存量告警不阻塞）、shellcheck、版本一致 + emoji + markdown +
相对链接 + USB ID + C 注释结构检查（docs.yml）；
回归 100% 靠真机人工验证。`python tools/doc_lint.py` 只在本地完整树可用
（参数表 / 默认值 / CRLF / 行数引用），公开树没有 `tools/` 与 `docs/`。

固件不入库（厂商版权，见 `firmware/README.md`）：驱动用 `request_firmware()` 取
`zt9612_fw.bin` + `zt9612_settings.bin`；装到 `/lib/firmware/` 用 `scripts/install-firmware.sh`
（自带 SHA-256 校验），缺这两个文件时 M1 装载失败。

**开发惯例：先在用户态把协议改动原型验证，再搬进内核** —— 反复把设备搞挂的代价远高于写原型。
实验工具链（均在测试机上跑，经 `rsh.py` 调用）：A/B 臂 `tools/run_arm.sh`（等接口 → 关
NetworkManager 托管 → 手动指定 BSSID 关联 → 测量）、固件侧观测 `tools/zt_arm_tool.py`
（`census`/`kick`/`rawtx`/`mempeek`/`memdump`/`sysstat`/`trace`，走 `/dev/zt9612`）、
高风险设备实验 `re/r40_safe_probe.sh`（健康门 + 单发 + 冷却 + 连续两次不健康即停）。

## 架构

`driver/zt9612.c` 是唯一参与编译的源文件（`zt9612_fw.c` 不参与，仅留作对照），自上而下三层：
M1 固件装载 `zt_boot()`；M2 IPC `zt_cmd()`/`zt_send()`/`zt_recv()` 与 `zt_run_init()` 同步初始化；
M3 mac80211（文件内 `==== mac80211` 之后的区段）。

- 线协议：`"WLAN" + u16 hlen + u16 type + payload`；IPC 主机→设备 **EP8-OUT**、设备→主机
  **EP4-IN**，数据面 TX 走 **EP5-OUT**。消息 ABI 为 rwnx 血统 `struct lmac_msg`，主机 `src_id=100`。
- TX：`.tx`/`.wake_tx_queue` 只登记 → `zt_tx_work()`（进程上下文，可睡眠）→ `zt_tx_build()`
  构造 28B 描述符 → `usb_bulk_msg()`；`zt_tx_agg_send()` 负责把多个 MPDU 拼进同一次 bulk。
- RX：中断 URB → kfifo → `zt_rx_inject()` 注入 mac80211。
- 心跳：每 5 秒发 `0x05c2` 的 `delayed_work`，否则固件看门狗复位、设备退回光盘模式。
- `zt_disconnect()` 防死锁模式：先置 `z->alive=false` 拒绝一切 USB IO，再 `usb_poison_urb()`
  + 最多等 2 秒，超时故意泄漏实例而非 UAF。新增收尾代码沿用此模式，禁止 `usb_kill_urb()` 无界等待。
- `zt9612.c` 里有历史乱码注释（双重编码），属已知问题，**不要整文件批量重编码**（会破坏合法中文注释）。

## 实机纪律（会挂机的坑）

1. 禁止在运行中的机器上反复 `rmmod`/`insmod`（已两次把整机搞成「能 ping 不能 SSH」）；换模块靠重启机器。
2. 开发期保持 `blacklist zt9612` 生效；有守卫文件时普通 modprobe 会静默返回 0，必须 `modprobe --ignore-install`。
3. 每次实验前 `sudo dmesg -C`，结束后 `sudo dmesg > ~/dmesg_$(date +%H%M%S).txt` 落盘（整机失联时 `/var/log` 不保证留下）。
4. IPC 必须一问一答：发一条 → 等对应 CFM → 再发下一条；连发会让固件断言。
5. 不要调 `usb_set_interface()`（单 alt setting，调用后设备行为异常）。
6. 挂死（`-110`/`Entity not found`/反复 `hello ack timeout`）的唯一可靠复位 = **S5 冷启动 + RTC 唤醒**
   （`rtcwake -m no -s 180` + `systemctl poweroff`，脚本 `tools/r42_coldboot.sh` + `tools/r42_after_boot.sh`），
   不必物理拔插；热 reboot / 端口 disable / 设备级与驱动级 unbind-bind / `USBDEVFS_RESET` /
   `authorized` 往返 / `xhci_hcd` unbind→bind / `usb_modeswitch -R` / D3cold 均已实测无效
   （旧的 `re/r40_recover.py` 阶梯对深挂死不再推荐）。重灌固件前必须断电复位（切模式、拔插或 S5 冷启动）。
7. TX 实验必须带功能判据：`usb_bulk_msg` 完成得快 ≠ 帧发出去了。用 `tools/arp_oracle.py`
   （仅开放网络有效）或 `ping -M do 1472`/DHCP。打流前用 `SO_BINDTODEVICE` 绑定接口并
   `ip route get` 复核（测试机有线/无线同网段，不绑定曾造成数周「35 倍差」误判）。

## 改动分级与模块参数

- 分级**不许降级**（细节在本地 `docs/12`）：L0 文档/注释 → L1 脚本/工具 → L2 驱动非 TX 路径
  （实机编译 + M1/M2 + M3 回归 + checkpatch 无新增告警）→ L3 协议/固件/TX-RX 数据路径
  （另加功能判据 + 单臂 A/B + 动手前预登记回滚条件 + 一变量一轮）。
- 新功能一律 opt-in 模块参数（默认关）；默认值 = 与上一发布版本行为一致。加载期参数，换臂只能重启。
- 改驱动行为/新增参数时，同一批提交同步 `PARAMETERS.md`（公开参数表）、`zt9612.conf`、`docs/03` §7、
  `docs/05`、`docs/08` 五处参数表 + `CHANGELOG.md` 的 `[Unreleased]`。

## 仓库边界与文档地图

**公开树只有**：`driver/`（`zt9612.c` / `Makefile` / 不参与编译的 `zt9612_fw.c`）、`scripts/`、
`install-driver.sh` / `uninstall-driver.sh` / `dkms-make.sh` / `dkms.conf`、`ci/`、`.github/`、
`README.md` / `INSTALL.md` / `TROUBLESHOOTING.md` / `PARAMETERS.md` / `CHANGELOG.md` /
`CONTRIBUTING.md` / `FAQ.md` / `SECURITY.md` / `MAINTAINERS`、
`AGENTS.md` / `CLAUDE.md`、`zt9612.conf`、`supported-device-IDs`、`firmware/`（仅占位 README）。

**本地保留（已 gitignore，公开仓库不存在，但本地开发时要读）**：`docs/`（内部文档）、`re/`（逆向
报告、抓包、用户态原型）、`tools/`（运维/实验脚本 + 明文凭据 `tools/ssh/pass.txt`）、
`.workbuddy/`（任务卡模板）、`firmware_from_cd/`、`vendor-request.txt`。

- `docs/12-后续开发规范.md` —— **「以后怎么干活」的唯一入口**：任务分级与证据门槛、七步闭环、
  收口检查表。开工前读 §2/§3，收工前跑 §5。
- `docs/02` 协议规格、`docs/03` 上机环境与铁律、`docs/04` 踩坑（Dxx）、`docs/06` 现场恢复卡、
  `docs/07` 代码/仓库/发版/公开边界、`docs/14` 对外呈现规范（README / Release / **commit 写法**，
  含调研出处）。`docs/11`、`docs/13` 是**已收口的里程碑计划**，别当现状读。
- 新会话接手顺序：`docs/12` → `README.md`「项目状态」/`CHANGELOG.md` `[Unreleased]` 顶部 → `docs/03` §5 → `docs/07` §9。
- 结论只在 `CHANGELOG.md` `[Unreleased]` 与 `re/REPORT_*.md`；`re/DRIVER_PROGRESS.md` 是停在
  补记的历史日志。结论被推翻时**不要静默改写历史**，加更正横幅并写进 `CHANGELOG` 新小节。

## 提交与脱敏

- 提交前做脱敏检查（`docs/07` §8）：凭据、测试机 IP、真实 MAC / 接口名 / SSID 一律不进提交；
  厂商固件、抓包、反编译产物、密钥、凭据**不入库**（`firmware_from_cd/`、`vendor-request.txt` 同属本地保留）。
- 提交信息：区域前缀 `driver`/`scripts`/`docs`/`ci`/`build`，标题祈使句 ≤75 字符，正文 75 列折行，
  一个提交只做一件事（可 bisect）；`git commit -s` 加 DCO 签名。AI 辅助加
  `Assisted-by: LLM <工具名>`，但 **AI 不得添加 `Signed-off-by`**（DCO 是法律声明）。
- 分支从 `main` 拉（`fix/xxx`、`feat/xxx`）；涉及驱动行为变更必须附 dmesg 证据，未实机验证要写明。
- 根目录 `CLAUDE.md` 是同一指导的 Claude Code 入口；改仓库约定时与本文件保持同步。
- 实验本身走「七步闭环」（`docs/12` §3：立卡 → 前置核查 → 单变量 → 取证 → 收口/回滚 →
  文档同步 → 归档），任务卡与记录模板在 `.workbuddy/templates/`；回滚条件必须**动手前**登记。

# Changelog

本文件记录所有值得注意的变更。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/)。

> 说明：`0.x` 阶段用于表达驱动能力里程碑（M1/M2/M3），而不是"可用的产品版本"。

## [Unreleased]

### Planned
- M3.2 扫描：`iw dev wlan0 scan` 触发逐信道扫描并上报 AP
- M3.3 关联：`wpa_supplicant` 关联成功
- M3.4 数据面：TX 描述符 / RX 数据路径（当前最大的未知项）
- 清理 `driver/zt9612.c` 中历史遗留的乱码注释（需与编译验证一起做）

## [0.1.0] - 2026-09-24

首个公开快照：内核态固件装载与 IPC 通信已在实机验证，mac80211 注册代码就绪但未验证。

### Added
- **M1 内核态固件装载**（`driver/zt9612.c`）：`"ZT"` 容器解析、hello 握手、
  488 字节分块写入、末块整段 XOR16 校验、配置块、RUN 与启动通知等待
- **M2 IPC 初始化 + 用户态通道**：按抓包原样同步执行的初始化序列
  （`MM_RESET` → `MM_VERSION` → 厂商私有段 → `MM_START`（约 6.5 秒）→
  `MM_SET_IDLE` / `MM_ADD_IF` / `MM_SET_SLOTTIME` / `MM_SET_CHANNEL`）；
  `misc` 设备 `/dev/zt9612`（`write()` 发原始 `WLAN` 帧，`read()` 取一帧）
- **5 秒心跳**（`0x05c2`）：`delayed_work` 实现，缺少它固件看门狗会复位
- **模块参数**：`do_init=0`（只装载固件）、`do_boot=0`（复用运行中的固件）
- **M3.1 mac80211 注册**（代码就绪，未实机验证）：wiphy / 2.4G band（13 信道、4 速率）、
  `start/stop/add_interface/remove_interface/config/tx/configure_filter/wake_tx_queue`
  与 `ieee80211_emulate_*` chanctx
- **卸载路径加固**：`alive` 存活标志 + `usb_poison_urb()` + 2 秒上限，
  超时故意泄漏实例而不是无界阻塞（规避已知的卸载死锁）
- 构建与安装：`Makefile`（`modules` / `sign` / `sign-install` / `install` /
  `uninstall` / `checkpatch` / `clean` / `help`）、`dkms.conf` + `dkms-make.sh`、
  `install-driver.sh` / `uninstall-driver.sh`
- 设备切换：`scripts/switch-to-wifi-mode.sh`、`scripts/setup-mode-switch.sh`
  （udev 自动切模式）、`scripts/install-firmware.sh`（SHA-256 校验后安装固件）
- 文档：`README.md`、`FAQ.md`、`supported-device-IDs`、`firmware/README.md`、
  `CONTRIBUTING.md`、`zt9612.conf`
- CI：`.github/workflows/` —— `build`（ubuntu-24.04 阻塞门禁 + 低于编译下限的对照作业）、
  `checkpatch`（仅 ERROR 阻塞）、`shellcheck`。首次运行的实测结论：
  6.17.0-1022-azure 上**零告警编译通过**并正确生成 `alias: usb:v350Bp9612d*`；
  6.8.0-1064-azure 因缺 `linux/unaligned.h` 失败 ⇒ 编译下限为 **6.12**

### Known issues
- **M3.1 未验证**：`wlan0` 是否出现尚未在实机确认；当前驱动不提供可用的网络接口
- **开机自动加载有未知风险**：模块放入 `extra/` 后由 USB modalias 自动加载时，
  观察到机器启动后"能 ping 不能 SSH"（根因未定位）。`install-driver.sh` 默认
  写入 `blacklist zt9612` 规避
- **不要反复 `rmmod`/`insmod`**：曾两次导致整机半死（`uninstall-driver.sh` 默认不 `rmmod`）
- **固件不在仓库内**：需自备（见 `firmware/README.md`）
- `driver/zt9612.c` 中部分中文注释在早期编辑中损坏成乱码（不影响编译），待清理
- 只支持 2.4G 频段、STA 模式；无蓝牙、无 AP / 监听模式

[Unreleased]: https://github.com/Spkicn/zt9612-linux/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/Spkicn/zt9612-linux/releases/tag/v0.1.0

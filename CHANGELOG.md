# Changelog

本文件记录所有值得注意的变更。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/)。

> 说明：`0.x` 阶段用于表达驱动能力里程碑（M1/M2/M3），而不是"可用的产品版本"。

## [Unreleased]

### Added
- **M3.4 TX 数据路径（侦查 + 初步实现）**：从 Windows 抓包还原出 TX 走 **EP5-OUT**，
  线上格式为 `WLAN 头 + 28 字节描述符 + 802.11 帧`（描述符 +0x04 是 802.11 帧长，
  +0x0e 是逐帧递增序号，与同族 AIC8800 的 `txdesc_host` 同源），传输长度按
  `align8(8+hlen)` 补零。驱动侧新增 `zt_tx_frame()` 与 `zt_scan_probe()`，
  扫描时可主动发 probe request，由模块参数 `scan_probe`（**0=被动，1=自建 probe，
  2=逐字节重放厂商 probe**）控制；扫描结束日志新增 `tx= / probe= / beacon= /
  probe-resp=`、RX 类型分布、设备消息表等观测打点
- **M3.2 扫描**：实现 `hw_scan`（被动扫描）与 RX 数据路径。实机验证：
  `iw dev <iface> scan` 返回 **42 个 BSS**（含 SSID、信道、WPA2、HT/VHT 能力），
  13 个信道约 1.7 秒，`dmesg` 无 oops
  - RX 数据路径：EP4-IN 上 `type=0x0000/0x0004` 的帧 = 每帧描述符（48 / 52 字节）+ 802.11 帧，
    跳过描述符后经 `ieee80211_rx_irqsafe()` 交给 mac80211
  - 扫描期间 RX URB 不能停，改用 `zt_cmd_fifo()` 从 URB 填充的 kfifo 取 CFM，避免与 URB
    抢同一个 IN 端点
- M3.1 实机验证通过：无线接口注册成功（`wlan0` 按 MAC 命名为 `wlx00b4011a0012`），
  `iw dev`、`iw phy phy0 info`、`ethtool -i` 均正常；实测环境
  Ubuntu 24.04 / 内核 `7.0.0-31-generic`

### Fixed
- **`dest_id` 必须等于消息编号里的 task 字段（`id >> 10`）**：抓包实测 MM 段为 `dest=0`，
  而厂商段 `0x050e`（×4，疑似 RF/PHY 配置）与心跳 `0x05c2` 都是 `dest=1`。此前驱动把
  `dest` 写死为 0，等于把这 4 条配置消息投递给了错误的固件任务。心跳的参数序号（前 4 字节）
  也改为递增，与厂商一致
- TX 帧必须带 8 字节 `WLAN` 头：首版 `zt_tx_frame()` 直接把 28 字节描述符当帧头发送，
  固件解析失败并断言，设备复位回 ROM 模式重枚举（现象为扫描中止 + 设备掉线）。
  补上 `WLAN` + `hlen = 28 + 帧长` + `type = 0x0000` 后设备不再崩溃
- **修复「开机自动加载后整机失联」的根因（原 D10）**：注册 wiphy 时缺少
  `SET_IEEE80211_DEV()`，`wiphy_dev(wiphy)` 为 NULL。无线接口出现后 NetworkManager
  立即通过 `SIOCETHTOOL` 查询驱动信息，`cfg80211_get_drvinfo()` 空指针崩溃并带关中断
  退出，导致用户态卡死（能 ping、22 端口能连、SSH 无 banner）。崩溃现场见上一版
  日志：`BUG: kernel NULL pointer dereference, address: 0x68`，`Comm: NetworkManager`，
  `RIP: cfg80211_get_drvinfo`。修复后 `ethtool -i` 返回 `driver: zt9612`，
  NetworkManager 与 sshd 全程存活，`dmesg` 无 oops
- 停止 RX 时也唤醒回收等待者：原实现只在 `alive==0` 时 `complete()`，设备正常但 RX 空闲时
  会让 `zt_rx_stop()` 白等满 2 秒并走到"泄漏实例"分支（此修复尚未单独复验）

### Changed
- 文档统一去除 emoji，README、CONTRIBUTING、FAQ 重写为更平实的措辞；
  原有内容与结论未变（本地交接文档同步做了同样的清理）
- README 与 FAQ 更新 D10 的根因说明：原先怀疑的 xhci 枚举抖动、udev worker 阻塞均不成立

### Planned
- M3.4 收尾：把 TX 描述符字段语义完全解码（目前只是逐字节重放），让主动扫描收到
  probe response；厂商抓包里有 94 个 probe response，说明设备的 RX 路径会放行
- M3.3 关联：`wpa_supplicant` 关联成功（需要关联过程的 RX 事件解析，依赖 TX 路径）
- 解码 RX 描述符里的 RSSI 字段，让扫描结果的信号强度可信
- 清理 `driver/zt9612.c` 中历史遗留的乱码注释（需与编译验证一起做）
- 复验卸载路径：`rmmod` 只在"设备未绑定"场景验证过 0 秒返回，已绑定场景待验（原 D9）

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
- CI：`.github/workflows/` 下的 `build`（ubuntu-24.04 阻塞门禁，外加低于编译下限的对照
  作业）、`checkpatch`（仅 ERROR 阻塞）与 `shellcheck`。首次运行的实测结论：
  6.17.0-1022-azure 上零告警编译通过并正确生成 `alias: usb:v350Bp9612d*`；
  6.8.0-1064-azure 因缺 `linux/unaligned.h` 失败，因此编译下限为 6.12

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

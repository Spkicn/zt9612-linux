# Changelog

本文件记录所有值得注意的变更。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/)。

> 说明：`0.x` 阶段用于表达驱动能力里程碑（M1/M2/M3），而不是"可用的产品版本"。

## [Unreleased]

> 下一站 **v0.3 吞吐**。这里的条目在实机验证通过前不要写"已完成"。

### Changed
- 修正文档与代码不一致的历史遗留：`zt9612.conf` 补全全部 6 个模块参数并更新
  `scan_probe` 说明（`2` 重放厂商 probe 已验证能收到 probe response，`1` 自建 probe 尚未单独复验）、
  `install-driver.sh` 的能力说明改为 v0.2.0 的真实状态（不再 grep `wlan0`，
  改为按实际接口名查找，因为接口名由 MAC 生成）、README 验收输出改用当前的 MAC 读取路径
- 公开文件（README、CHANGELOG）不再出现本机真实 MAC、AP BSSID 与 SSID，改用占位符；
  设备唯一信息只留在本地未入库的交接文档与进度日志里

### Added
- **TX status 上报（实验开关，默认关闭）**：新增模块参数 `tx_status`（0=关闭，非 0=开启）
  与 `tx_status_probe`（默认 100），把发送结果交给 mac80211：
  `ieee80211_tx_status_irqsafe()`，速率沿用 mac80211 当初选的那个，不编造。
  因为**无法得知帧是否真的被 ACK**（USB 写入成功 ≠ 空口成功，固件也没有确认通道），
  所以 `ack` 只在每 `tx_status_probe` 帧里乐观探测一次，其余如实报 false ——
  宁可显得慢，也不给速率控制灌假数据。关闭时行为与 v0.2.0 完全一致。
  实测（2026-09-26）：开启后 `station dump` 的 `tx retries` 从 18 涨到 **6532**，
  证明上报确实进了 mac80211 的速率控制；全程 `dmesg` 无 Oops/WARNING，
  吞吐无回归。**默认不开**，因为下面的判定结论还不足以证明它有益。

### 阶段 B 判定结论（2026-09-26，实机）

- **`iw link` 的 tx bitrate 不可当作真实发射速率**：它只是主机侧的速率掩码/记账。
  证据：`iw dev <iface> set bitrates legacy-2.4 {1|11}` 之后 `iw link` 跟着显示
  1.0 / 11.0 Mbit/s，而**实测吞吐完全不跟着变**（各条件 3~4.7 Mbit/s）；
  且实测吞吐本身就是"1 Mbit/s 链路"的 3~4.7 倍，物理上不可能是按 1 Mbit/s 在发。
- **主机选速对吞吐没有可观测影响 ⇒ 固件自行选速**（docs/09 §2 的 U1）。
- **补充证据（2026-09-26，静态分析，把上面的"倾向"提级为"确证"）**：
  厂商 Windows 驱动在整个抓包（46,182 包）里 host→设备**只发过 15 种消息 id**，
  其中**没有任何速率相关消息**——`MM_SET_BASIC_RATES_REQ/CFM`(22/23) 0 次、
  `MM_STA_RC_UPDATE_REQ/CFM`(105/106) 0 次、`DBG_RRM_RATE_INFO_CFG_REQ/CFM`(145/146)
  及其余 `DBG_RRM_*` 配置 0 次（对照：`MM_SET_CHANNEL_REQ` 81 次、心跳 6 次，
  说明统计完整、解析正确）。复现：`python re/vendor_msgs.py`。
  另：固件镜像里与速率相关的字符串只有两条（`SET_BASIC_RATES`、`RRM_CFG_RATE_INFO`），
  说明协议里**存在**速率配置能力，但**厂商驱动不使用它**。
  三条独立事实同向：① 描述符无速率字段；② 厂商不发速率消息；③ 主机改速率不影响吞吐
  ⇒ **数据速率由固件自主决定，主机侧改速率表/上报 TX status 都不会改变实际发射速率**。
  因此 v0.3 的"吞吐优化"不能靠补速率表实现；有意义的方向是
  提高固件自身选速的空间（HT/VHT 能力声明）或改善空口条件。
- **补充证据（固件静态分析，`re/REPORT_HOST_RATE.md`）**：
  * `WLAN` 帧处理函数（VA `0x6108916C`）逐指令覆盖 148/148 后，**11 处访存全部落在
    buf+0..+7（8 字节 WLAN 传输头）与 buf-0x20..buf-1（固件内部 32 字节结构）**，
    `buf+8..buf+0x23` 这段**描述符窗口零访问** ⇒ 固件整段忽略 28 字节描述符
    （**中-高**：该函数方向无法从静态代码确证，倾向主机→设备）。
  * 固件自带速率控制模块：`arrm_rc.c`、`arrm.c`、`arrm_txmode.c`、`arrm_tpc.c`、
    `rrm_rts.c`、`rrm_sr.c`、`rrm_common.c`、`rrm_cfg.c`、`phy_custom_rf.c`，
    以及整套 `RRM_CFG_RATE_INFO/FEC_CODING/NTX_ANTANSET/TPC_CODE/PROT_MODE/STBC/SR/AMPDU_DUR`
    （**确证**，字符串级）。固件内还有两张**自己的**短名表（MM 42 条、DBG 50 条），
    其中含 `UPDATE_STA_RC`（rank 38）与 `RRM_CFG_RATE_INFO`（rank 30）
    ⇒ 协议与固件两侧都有"主机可设速率"的入口，但**厂商从未使用**。
  * 镜像内**不存在**经典速率表（10/20/55/110、1/2/5.5/11、MCS 序列等）——命中全为 0。

### 主机能否重新影响速率：结论是「结构上封闭」（2026-09-26 实测）

用 `/dev/zt9612` 写了探针 `re/ipc_probe.py`（只发 `param_len=0` 的请求、每个 id 一次、
每 3 秒补心跳、按 0.4 秒窗口收响应），先跑**控制组**证明探针可靠：

| 发送 id | 收到的响应 | 说明 |
|---|---|---|
| `0x0004` | `0x0005 MM_VERSION_CFM` plen=32 | 命中 |
| `0x0006` | `0x0007 MM_ADD_IF_CFM` plen=2 | 命中 |
| `0x0010` | `0x0011 MM_SET_CHANNEL_CFM` plen=2（另有一条 `0x020c` 回显） | 命中 |
| `0x0022` | `0x0023 MM_SET_IDLE_CFM` plen=0 | 命中 |
| `0x0042/0x0043/0x0044/0x0045` | **无 IPC 响应** | 无效 id |

顺带确认了两件事：**固件对未知 id 完全不回应**（不会回错误码），所以"有没有 CFM"
就是干净的判据；`0x020c` 那条"刚离开的信道"回显也再次被现场看到。

**rank → wire id 的映射没有廉价解**：一度以为找到规律（`2×rank+8` 在 rank 12~21
上连续成立，与实测的 SET_SLOTTIME=0x20、SET_IDLE=0x22、SET_POWER=0x24 吻合），
但探针实测 `0x0054` 是 `MM_SET_P2P_OPPPS_REQ`（印证了固件表与厂商枚举**不同序**），
而 RRM 速率的候选位点 `0x0042~0x0045` **全部无效** ⇒ 公式作废，只能靠穷举探测，
而穷举要遍历大量**语义未知**的消息（部分会改信道/复位/断言），违反"不猜结构"的纪律。

**因此这条通道判定为结构性封闭**，两条理由：
1. **参数结构无从获得**：厂商驱动从未发送这两条消息（抓包里 0 次），所以没有可对照的
   样本；而包含 handler 的那部分固件代码**不在**我们手上的 219 KB 镜像里
   （指针表指向段外、`gp` 也越界）。即使穷举出 wire id，也仍然构造不出安全可用的参数。
2. **风险不对称**：猜参数的代价是固件断言 + 设备挂死（历史上盲改描述符就触发过，
   需要物理拔插），而收益只是"多一个可能影响速率的通道"。

**最终结论**：让主机重新影响速率的现实路径不存在（在本仓库现有资料范围内）。
v0.3 剩下**唯一**可能真正提升吞吐的驱动侧手段是**声明 HT/VHT 能力**（给固件内的
ARRM 更大选速空间），其次是改善空口条件（信道/距离/干扰）。

### 仍在未确定状态的（别当成结论）
- **速率控制算法本身**：`arrm_rc.c` 的代码**不在** `zt9612_fw.bin` 这 219 KB 里 ——
  镜像自己的指针表指向 `0x610D7xxx`/`0x610E8xxx`（1484 个代码段 u32 里 1370 个越界，
  连 `gp = 0x610CCC00` 都越界）。所以"表驱动 / 探测式 / 混合"无法从本文件判断。
- **固件短名表的 rank 到 wire id 的映射**：实证否证了"rank 即 id"与 `2×rank+8`
  两种规律，且固件表与厂商枚举不同序 ⇒ 无法静态推导（见上一节的封闭结论）。
  上游 [`re/REPORT_FW3_TXGATE.md`](../re/REPORT_FW3_TXGATE.md) §1.1 的
  "表项索引即 id" 与 [`re/REPORT_FW2_DEOBF.md`](../re/REPORT_FW2_DEOBF.md) §4.2 的
  "`0x19DAC` 是主机 TX 描述符解析点" 均已被本轮**推翻**，两份报告需带更正说明阅读。

### Planned
- **2.4 GHz 速率表补齐**：现在只有 4 个 CCK 速率（1/2/5.5/11 Mbps）。
  注意：据上面的结论，**补速率表本身不会提升实测吞吐**（固件自选速），
  但它能让 `iw` 侧的速率条目与真实能力一致、也让 `iw link` 的显示不再误导
- **HT/VHT 能力**：厂商 probe 帧里带 HT Capabilities，说明射频支持；最后再动 `band->ht_cap`，
  因为声明之后 mac80211 会开始使用聚合，对发送队列的要求更高
- 干净开机自动加载复验（D10 已修复，但"插卡 + 开机自动加载"这条完整路径尚未重跑）
- RX 描述符里的**速率**字段：描述符 `+0x10`/`+0x25` 是弱候选，
  USB 层抓包没有 radiotap 真值，宁缺勿猜
- 清理驱动源码中历史遗留的乱码注释（必须与实机编译验证一起做）

### Known limitations（已知限制，不是待办）
- 实测速率恒定 1 Mbit/s（链路报告值）：速率表只有 4 个 CCK，且速率控制拿不到 TX status
- **无线接收缓冲过小（已修，这是"大流量停滞"的真因）**：RX URB 缓冲原来只有 1024 字节
  （`MAX_FRAME`），装不下满尺寸的 802.11 数据帧（ping payload ≥900 字节的回复实测
  接口层 RX 增量为 0、空口却收到了），TCP/TLS 因此在大包上停摆。
  已把接收路径的缓冲与单帧上限提到 2048 字节。修复后实测（MTU 保持默认 1500）：
  HTTPS 下载中位数约 **4.2 Mbit/s**，而修复前同类测量是 "HTTP 200 之后收不到数据"
- 中间过程的一个错误结论（保留以免重犯）：曾把 1 KB 现象归因成"设备帧长上限"，
  并把 `ieee80211_hw.max_mtu` 设为 900。实测证明这既没必要也有害——
  MTU=900 时吞吐约 3.2 Mbit/s，**低于 1500 下的约 4.2 Mbit/s**，
  而且 MTU 上限低于 1280 会让 **IPv6 完全不可用**（IPv6 与 mac80211 都要求 ≥1280）。
  该上限已改回 1500
- **收帧上限 ~1024 字节（发送侧无此限制）**：禁止分片的 `ping`/UDP 归因实验
  （2026-09-26，`tools/udp_attrib.py`）显示：
  * **发送正常**：UDP payload 100→1472 字节，`tx_bytes` 增量始终与载荷+802.11 开销吻合；
  * **接收失败**：ICMP payload 900 字节时 `tx +1924 / rx +1884`，到 1000 字节变成
    `tx +2124 / rx +0`；ping 1400 期间接口层 `rx_packets` 只 +2 而 mac80211 层 +94
    （那 94 是广播流量）——**回复帧到了空口，却一个字节都没上栈**；
  * 阈值正好落在 802.11 帧 ≈1030 字节（payload 900 的回复帧实测 `usb_len=1038`，
    payload 1000 → 帧 1068 失败），而 RX URB 缓冲从 1024 提到 2048 只把窗口推了 16 字节
    ⇒ **限制在设备侧，不是主机的 URB 缓冲**；
  * **已确证丢弃方式**：用驱动的 `rx_debug` 参数（打印 USB 层收到的长度）取到硬证据 ——
    payload 900（能通）时窗口里出现 `usb_len=1038` 的回复帧；
    payload 1400（100% 丢包）时同长度窗口的 181 条记录里 `usb_len` 最大只有 **570**，
    全是背景/管理流量 ⇒ **超限帧被设备直接丢弃且不报错，不是截断**；
  * 影响面有限：**TCP 数据面不受影响**（大流量下载中位数约 4.2 Mbit/s 正常），
    受影响的只有单包 >905 字节的 ping/UDP 这类"大包 + 需要回复"的用法。
  * 复现/续查工具：`rx_debug` 模块参数（写 N 即记录接下来 N 帧的 USB 长度，默认关闭）。
    两个坑：写参数**不能用 `echo N | sudo tee`**（tee 会吞掉 stdin，N 进不去），
    读日志用 `journalctl -k`（本机 `kernel.dmesg_restrict=1`，`dmesg` 直读为空）。
- 运行期 `ip link set mtu` 会让接口短暂失去关联（实测丢 1~2 个 ping，NetworkManager 会重配）；
  另有一次观察到接口消失约 2 分钟后由驱动重新装载并自动重连。两次 dmesg 均无 Oops，
  具体机制未定位
- WPA2-Enterprise（802.1X）路径未验证：实验环境没有 802.1X AP
- 每次 `rmmod` 会泄漏一个实例（卸载时设备不应答在途 bulk-IN，驱动按设计宁可泄漏也不 UAF）；
  换模块优先重启机器

## [0.2.0] - 2026-09-24

> 第一个**功能可用**的版本：双频扫描、关联、数据面、WPA2-PSK 全部实机验证；
> 已知短板是吞吐（`iw link` 恒 1 Mbit/s）。

### Added
- **WPA2 关联、加密与端到端联网打通（M3 验收全部达成）**：连接 WPA2-PSK 热点实测
  关联 226 ms、四次握手 344 ms、`WPA: Key negotiation completed [PTK=CCMP GTK=CCMP]`，
  `wpa_cli status` 报 `wpa_state=COMPLETED`；随后 DHCP 拿到 `192.168.43.8/24`（租约 6 h），
  `ping` 网关 3/4、`ping 223.5.5.5` 3/3（48~69 ms）、`ping 8.8.8.8` 3/3（54~82 ms），
  全程 0 条 Oops/WARNING。**本项驱动零改动**：mac80211 自带默认加密套件表
  （`iw phy info` 列出 WEP/TKIP/CCMP/GCMP 共 7 个），且 `set_key` 为空时自动回退软件加密
  （`ieee80211_key_enable_hw_accel` 的 `je` 分支），因此无需注册 `cipher_suites`、也无需实现
  `set_key`——CCMP 由 mac80211 软件完成
- **M3.5 5GHz 支持**：注册 `NL80211_BAND_5GHZ`（5180..5825 共 25 个信道，与厂商扫描列表一致）
  与 OFDM 速率表；2.4G 表补上 `2484`(ch14) 并用 wiphy 的 `hw_value` 填 DS 参数（修掉 2484 被算成 15
  的老 bug）。probe 模板按频率分叉：2.4G 保留 DS 参数补丁，**5GHz 完全按厂商模板不改帧**
  （5G 帧里没有信道字节，原补丁会写坏 HT Capabilities）；主动探测跳过 `NO_IR`/`RADAR` 信道。
  实机：`iw phy info` 两个频段（14 + 25 信道）、`iw scan` 59 个 BSS 中 7 个 5GHz、关联与
  WARNING 检查均无回归
- **M3.4 数据面打通**：关联开放 AP 后，广播 ARP 请求能出网并收到网关应答，单播回包正常回收，
  IPv4 组播（mDNS/IGMP）与 IPv6（RS/MLD）数据帧均正常收发；主动扫描实测
  `probe=13 → probe-resp=22`。判定依据：同链路 ARP 完整往返，说明收发路径本身无缺陷，
  DHCP 未拿到地址是该 AP 的行为（其网段与其他 STA 使用的网段并存）
- **扫描日志与观测**：扫描开始清零计数，扫描结束打印 `tx/probe/beacon/probe-resp`、
  RX 类型分布、**RX 状态来源**（描述符频率 / 退回信道 / 描述符频率≠扫描信道 / RSSI 兜底）
- **RX 真实信号强度与信道**：`rx_status.signal` 取归一化描述符 `+0x0E`（int8 dBm，带
  -95..-20 兜底），`freq/band` 取 `+0x2A` 并在 wiphy 反查。实测 25 个不同取值、-94~-23 dBm、
  连扫两次平均差 0.53 dB、频率与各 AP 自报 DS IE 55/55 一致。**注意必须
  `ieee80211_hw_set(hw, SIGNAL_DBM)`**，否则 mac80211 不会把 signal 交给 cfg80211
- **M3.3 关联打通（认证 + 关联均成功）**：实机 `iw dev <iface> connect -w <SSID>` 返回
  `connected to <AP 的 BSSID>`，驱动侧 `bss_info ... assoc=1 aid=1`，接口进入 `LOWER_UP`；
  关联后 `iw dev <iface> link` 显示 `tx bitrate 1.0 MBit/s`、DTIM/beacon 间隔等来自真实 AP 的信息
  - **mac80211 TX 路径**：`.tx` 与 `wake_tx_queue` 只做登记，真正的 `usb_bulk_msg()` 提交放在
    `tx_work`（这两个回调可能在软中断上下文执行，不能睡眠）。本内核把 TXQ 路径定为必选：
    缺少 `wake_tx_queue` 时 `ieee80211_alloc_hw_nm()` 会 WARN 并返回失败（`main.c:800`）
  - **`config` 回调同步信道**：mac80211 切信道时向固件发 `MM_SET_CHANNEL`（12 字节，5 GHz
    首字段为 1）；切信道前重放厂商的使能序列（`SET_IDLE / 0x0104 / SET_FILTER`），
    可用新模块参数 `tx_prep=0` 关闭
  - `bss_info_changed` 打点（`assoc/aid/bssid`），用于观察关联过程
- **M3.4 调试通道**：`/dev/zt9612` 上的 ioctl `ZT_IOC_TXRAW`，用户态可以把一条完整 `WLAN`
  帧直接发到 TX 端点（用于在不重载模块的前提下批量试描述符与时序）。之所以不用 debugfs：
  Secure Boot 下内核处于 `lockdown=integrity`，会拒绝写 debugfs
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
- M3.1 实机验证通过：无线接口注册成功（`wlan0` 按 MAC 重命名为可预测名，形如 `wlx` + MAC 十六进制），
  `iw dev`、`iw phy phy0 info`、`ethtool -i` 均正常；实测环境
  Ubuntu 24.04 / 内核 `7.0.0-31-generic`

### Fixed
- **RX 注入被 `scan_active` 门控（关联一直超时的根因）**：原实现只在扫描进行中才把收到的帧
  交给 mac80211，扫描之外的认证响应/关联响应全部丢失，`iw connect` 必然超时；而
  `iw scan` 却完全正常，所以长期没有暴露。改为常开注入（仅在 `hw` 存在时）后关联立即成功。
  同批修复还包括：`assoc`/`aid` 在本内核已移到 `vif->cfg`，`bss_info_changed` 不能再用
  `bss_conf.assoc`
- **MAC 读取错位 + `MM_ADD_IF_REQ` 少一字节（TX 一直不发射的根因）**：`0x0101` 应答的参数是
  `{ u8 status; u8 mac[6]; }`（7 字节），旧代码把 `resp[0..5]` 当 MAC，于是 MAC 整体左移一字节、
  末字节被 0 顶掉（抓包中 95 个发往本机的单播帧 addr1 证实了正确的六个字节在 `resp[1..6]`）。
  同时 `MM_ADD_IF_REQ` 按 rwnx 布局是 `{ u8 type; u8 mac[6]; u8 p2p; }`（8 字节，
  抓包实测参数为 `00` + 本机 MAC + `00`），旧代码只发 7 字节。二者叠加使固件中的 vif 建在错误地址上：
  主机 TX 帧被静默丢弃、RX 单播被地址过滤，扫描只能看到 beacon。修复后实测
  `MM_ADD_IF_CFM status=0 inst_nbr=0`、主动扫描 `probe=13 / probe-resp=18`（M3.4 打通），
  接口按真实 MAC 命名，并新增 `MM_ADD_IF_CFM` 状态日志便于回归
- **`/dev/zt9612` 读路径可能长时间卡住**：原 `zt_read()` 只等"FIFO 里有 2 字节长度头"就返回，
  遇到半帧（URB 分片）会反复错位、每次白等 3 秒；实测把用户态扫描脚本挂死 10 分钟
  （内核栈停在 `zt_read`）。现在改为**等完整帧才返回**、脏长度头自动丢弃、
  缓冲不足时不消费长度头，并补上 `.poll` 让 `select()` 语义正确
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

### Known issues
- **吞吐偏低**：实测速率恒定 1 Mbit/s（2.4G 只声明 4 个 CCK 速率，且驱动不上报 TX status，
  速率控制拿不到反馈）。这是 v0.3 的目标，见 `[Unreleased]`
- **每次 `rmmod` 泄漏一个实例**：卸载时设备不应答在途 bulk-IN，驱动按设计宁可泄漏也不
  use-after-free；换模块优先重启机器
- **WPA2-Enterprise（802.1X）未验证**：实验环境没有 802.1X AP
- **干净开机自动加载未复验**：D10 的根因已修复，但"插卡 + 开机自动加载"这条完整路径
  还没有重新跑过，因此 `install-driver.sh` 默认仍写 `blacklist zt9612`
- `driver/zt9612.c` 部分中文注释在早期编辑中损坏成乱码（不影响编译），待清理

### Verified（实机验证记录，供发布说明引用）
- **阶段 A 吞吐基线（2026-09-26）**：无线接口绑定测速（`SO_BINDTODEVICE` + 显式绑源地址，
  不动默认路由）—— MTU=1500 时 TCP/TLS 完全跑不动；MTU=900 时 3.38 Mbit/s（HTTP 200）。
  路径 MTU 二分实测≈917 字节（有线对照 1000 字节可通）。全程 0 Oops / 0 WARNING
- **稳定性回归**：10 轮 `rmmod`/`insmod`（12 次卸载中 11 次走到"泄漏实例"分支，但 10/10
  次重新加载成功）、20 轮接口 up/down + 每轮 `iw scan`、NetworkManager 长跑 10 分钟、
  3 轮热点断连重连（3/3 重新 `COMPLETED` 并拿到 DHCP 租约）—— 全程 **0 Oops / 0 BUG /
  0 WARNING**，`rx.c:5475` 计数始终为 0
- **DKMS 实装**：`dkms add/build/install` + `dkms status` 显示
  `zt9612/0.2.0, <kernel>, x86_64: installed`；注意 Secure Boot 下 DKMS 3.0.11 即使配置了
  `mok_signing_key` 也**不会给 `.ko.zst` 签名**，需就地补签（见 README「Secure Boot 签名」）

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

[Unreleased]: https://github.com/Spkicn/zt9612-linux/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/Spkicn/zt9612-linux/releases/tag/v0.2.0
[0.1.0]: https://github.com/Spkicn/zt9612-linux/releases/tag/v0.1.0

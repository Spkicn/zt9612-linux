# Changelog

本文件记录所有值得注意的变更。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/)。

> 说明：`0.x` 阶段用于表达驱动能力里程碑（M1/M2/M3），而不是"可用的产品版本"。

## [Unreleased]

> 下一个里程碑的候选清单见下面的 Planned；公开侧路线图以本节与 README「路线图」为准。

> **发布线说明**：已发布版本是 **0.3.2**（三处一致：`dkms.conf` 的 `PACKAGE_VERSION`、
> `driver/zt9612.c` 的 `MODULE_VERSION()`、`README`「项目状态」的当前版本）。
> 本节按两条发布线分组：**0.3.3 候选/维护组** 只收 0.3.x 级的实验开关与工程收口；
> **v0.4 主线** 是上传快路径（固件站点会话 + A-MPDU 聚合 + WPA2 密钥路径）。
> 截至 2026-10-02（r19~r22）：`sta_add_en`/`ampdu_en`/`tx_staid4b` 等**行为开关仍默认 0**，
> 但三个参数的默认值已按实测改定 —— `sta_add_fmt=5`（厂商实抓真载荷）、`key_en=2`
> （mac80211 软件加解密）、`sta_reuse=1`（会话跨重连复用）。默认组合已能完成 WPA2 关联 +
> DHCP + ping。
> **2026-10-04（r26）：炸弹拆除** —— r25 定位的"登记后 8~40s 定时死亡"已由
> `sta_add_fmt=7`（宽切除修复载荷）实证拆除（重连恢复 + 12min 浸泡 + 21.4 Mbit/s
> 洪泛 30s 全存活），详见下方 r26 条目；`sta_add_fmt` 默认值已切换为 7。
> **2026-10-04（r34 静态判决）：剩余瓶颈重新归因** —— 对 mac80211 master 快照
> 逐门判定：AQL/airtime 计量对本驱动整体关闭（未声明 `NL80211_EXT_FEATURE_AQL`），
> 且数据帧从不走 mac80211 TXQ（无 `.sta_state` ⇒ `sta->uploaded` 恒 false）——
> 此前"TXQ 计量"表述全部作废；35 倍差的当前最优模型为"多单元 bulk 一次性楔死
> 固件 TX 推送控制器"，R34 段序判决臂（`re/_r34_segment.sh`）待上机执行，
> 详见 `re/REPORT_R34_TXQ_STATIC.md`。

## [Unreleased] r26（2026-10-04 凌晨）：炸弹拆除 + 聚合咬合 + 瓶颈定位

- **`sta_add_fmt=7`（宽切除修复载荷，新实验开关）**：fmt=5 厂商真载荷保留
  "速率核"（format/max-mcs/r_idx/0x0ff0 位图/NSS/AID/BSSID），清零三个未知
  掩码块（+0x04..05=0xfffa、+0x18..0x1f、+0x24..25=0xffff）。**三重验证**：
  断开重连恢复（r20 会话复用兼容）、12 分钟浸泡零死亡、21.4 Mbit/s UDP 洪泛
  30 秒全存活（旧 fmt=5 时代第 21/44 次传输即挂）。R13（无 STA_ADD 基线）
  同链路同速率对照成立。
- **二分过程（R11-R17，每臂判据预登记）**：R11（fmt=5）PRE 0% → round 2 死
  （tail `10 2C→90 2C`）＝炸弹复现；R12（fmt=0）出生即闸死（`00 04`、B≡0）；
  R13（ht=1 纯基线）5min 存活 ⇒ 毒在载荷。R14（固件 KEY_ADD 零材料注入）
  打断数据面（固件 key 状态调制 TX 管线实证）；R15（MCS 钳位 0b→07）无效
  ⇒ 速率档排除；R16（宽切除）存活 + R17（前半掩码回填）死亡 ⇒ **毒物锁定在
  fmt=5 的掩码块**。
- **健康链路发现（R9-R10）**：TP-LINK AP 断电重启后 5G 只拒 legacy 调制
  数据帧（assoc/4 次握手/加密 RX 全通、DHCP/ARP 零响应；2.4G 同卡全通；
  `ht_cap_enable=1` 绕过）——beacon 仍通告 legacy 基础速率，AP 行为与自播
  beacon 矛盾，机制未解。
- **聚合咬合达成（R20-R21b）**：旧"第 21/44 次传输打挂"在 fmt=7 会话上
  **未复现**（30s 洪泛全存活）；咬合元凶 = `agg_max_xfers` 默认值 1（第一次
  尝试后永久关闭打包），需设 0。
- **剩余问题定位（2026-10-04 r34 修正）**：35 倍差（agg 路径 ~55 帧/s vs 单帧路径
  1760 帧/s；airtime 94% 空闲；tx failed=0）。~~mac80211 TXQ 计量~~表述**作废**
  （r34 静态判决：AQL 未声明特性整链关闭 + 数据帧从不进 TXQ）。已排除：打印洪流、
  扫描打断、USB 速度（bulk 116us）、状态上报模式（free/全 ACK/同步）、
  速率回填、无锁旁路、mac80211 AQL/deficit、BA 会话 hold。当前最优模型 =
  **多单元 bulk 一次性楔死固件 TX push controller**（R24 预筛后 B/C 段主机侧
  逐字节等价而吞吐差 35 倍 ⇒ 毒在会话历史不在代码路径）；R34 段序判决臂
  （C1 干净基准 → A1 首聚合 → C2 楔死后再单帧 → C3 自愈观察）待上机，
  判据预登记见 `re/REPORT_R34_TXQ_STATIC.md` §5。
- **参考源码入库（本地 re/reference/，gitignored）**：aic8800_fdrv（CS 包
  GPL 源码树，SDK 同源）、aic8800d80 全套、ZTOP ZT9101 原厂源码
  （Codeberg，Realtek 系架构）、FCC 档案 3 份 PDF（模块 datasheet/内部
  照片/批准信）、mac80211 master 快照。跨参考报告：
  `re/REPORT_AIC_CROSSREF.md`、`re/REPORT_AIC_MIGRATION.md`。
- **新工具**：`re/_r3_rxdump.py`（chardev RX 帧分类 dump：EAPOL/单播定向/
  beacon——连接期抓握手必备）；`re/_r14_keyadd.py`（固件 KEY_ADD 注入器）。
- **方法坑入册**：跨 BSSID 切换不重载模块 = 固件会话陈旧（每臂必须全新
  模块）；写死网关 MAC 的 permanent neigh 遇 AP 重启必翻车；远端重编后
  必须 `make sign`；sshd 偶发拒绝连接加重试循环即可；聚合期 dev_info
  每帧打印会拖垮 TXQ 排水（打印门控修复）。

### Added (v0.4 主线：站点会话 + A-MPDU 聚合)
- **A-MPDU 推送语义已定案（2026-09-30）：A-MPDU 由驱动在传输层组帧**。
  厂商上传抓包（`re/captures/vendor_upload.pcap`，EP5 共 96107 条传输）显示：
  ① 传输长度只有 **1578 / 1888 / 3200 / 4152 / 4808** 几种众数，`>2000 字节 = 45582 条`
  ⇒ **一次 bulk = 1/2/3 个自描述单元**；② 每个单元是 `"WLAN" + u16 hlen + u16 type + 28B 描述符 + MPDU`，
  **非末尾单元占固定 1608 字节槽位**（下一个 `WLAN` 恒在 +1608；1608 = 8 + 28 + 1572），
  末尾单元才按 8 字节对齐；③ 单元之间**没有** 802.11 MPDU delimiter / 长度表；
  ④ 厂商数据帧描述符是**一套常量模板**（`+0x0a=0x1312`、`+0x0c=0x0040`、`+0x14=0x0200`、
  `+0x1a=0x7540`，`+0x02=0xffff`、`+0x06/+0x08=0`），其中 **`+0x0E` = 帧自己的 802.11 序列号**
  （4808 那条：0x0296/0x0297/0x0298）。逆向侧复核：除 `MM_BA_ADD_REQ/DEL_REQ` 外
  **没有任何聚合消息**，厂商驱动里有整套 host TX soft agg（`host tx soft agg is enable`、
  `tx_agg_check`、`tx_agg_max_len`）⇒ 聚合纯在传输层，协议层不加东西。
  取证脚本：`re/vendor_usb_agg_probe.py`、`re/vendor_desc_fields.py`；
  报告：`re/REPORT_AMPDU_PUSH.md`、`re/AMPDU_PUSH_STATUS.md`。
  **这条更正推翻了此前"mac80211 自己组 A-MPDU、驱动拿到聚合 skb"的记载**（那是错的）。
- **实验开关 `ampdu_en`（默认 0 = 行为与 0.3.2 完全一致）—— A-MPDU 聚合（厂商快路径第三步）**：
  打开后：① `ieee80211_hw_set(hw, AMPDU_AGGREGATION)` + `max_tx_aggregation_subframes=8`；
  ② TX 缓冲 2 KB → **16 KB**（否则多单元打包会被我们自己的长度检查丢掉 —— 注意：
  `ampdu_en` 是**加载期**参数，运行期改它**不会**重算 `tx_buf_size`，曾因此让 729 次传输全退化成 1 单元）；
  ③ 实现 `.ampdu_action`：`TX_START` 时发 `MM_BA_ADD_REQ`(0x28) 等 CFM 0x29、成功再回调
  `ieee80211_start_tx_ba_cb_irqsafe()`，停止时发 `MM_BA_DEL_REQ`(0x2A)；
  ④ **`zt_tx_agg_send()`：驱动自己把多个 MPDU 拼进同一次 bulk**（按厂商 1608 槽位规则），
  只聚合**已加密单播数据帧**，其余帧仍一帧一次 bulk。
  - 用户态已把两条消息验证过：**`MM_BA_ADD_CFM` 返回 status=0**（type=0、sta_idx=会话值、
    tid=0、A=64、B=0，不断言）；`MM_BA_DEL_CFM` 返回 5（语义未定，但同样不断言）。
  - **两个前提已查明**（2026-09-27，BTF + kprobe 定位）：
    ① **必须先声明 HT**（`ht_cap_enable=1`）：mac80211 的 `ieee80211_aggr_check()` 会检查
    对端链路能力字节，未声明 HT 时它直接返回，`start_tx_ba_session()` 永不触发；
    ② **`MM_BA_ADD_REQ` 的 `A`（bufsz）不能为 0**：mac80211 在 `TX_START` 传的
    `buf_size` 是 0，照发会让固件断言并 USB 掉线（"关联后约 84 ms 掉线"的真因），
    代码里已做 `A ≥ 64` 下限保护。
    满足这两条后链路可以真的建立起来：
    `ampdu_action TX_START → BA_ADD_CFM status=0 → AMPDU operational (tid=0 bufsz=8)`。
  - **仍存在的问题**：聚合生效后**一压流量就崩** —— 网关 `ping` 丢包 70%、
    本地 `tx_blast` 中位数 3.77 Mbit/s（无聚合时约 12）、期间 2 次 USB 掉线并反复重连。
    推断是"mac80211 交下来的是聚合后的大 skb（最多 8 子帧 ≈12 KB），而驱动仍按
    一帧一次 bulk + 28 字节描述符发送"，固件吃不下。**默认仍为 0**，待查清 A-MPDU 的
    封装要求后再评估。
  - **厂商侧对照（2026-10-01 逆向补充）**：厂商 hif.c 的 host TX soft agg 把单次聚合
    传输上限硬编码为 `tx_agg_max_len = 8192` 字节（`tx_buf_size` 超限即软断言），且带
    "USB hw page&table 检查需要时自动关闭 soft agg"的熔断路径（WARN 原文承认
    "to avoid hw problem"）。本驱动的聚合上限（8 单元 × 1608 = 12,864 字节）超出
    厂商机制允许的范围，排查聚合崩溃时应把 `agg_max_units=5`（8040 ≤ 8192）
    对齐厂商上限作为一个实验维度（`re/AMPDU_PUSH_STATUS.md` 第 14 轮）。
- **实验开关 `sta_add_en`（默认 0 = 行为与 0.3.2 完全一致）—— 固件站点会话已打通**：
  关联完成后发 `MM_STA_ADD_REQ`(0x0A)、记下 CFM 返回的 `sta_idx`，并让**加密单播数据帧**的
  描述符 `+0x09` 带上它（管理帧/广播/EAPOL 仍保持 `staid=0xff`）；断开时发
  `MM_STA_DEL_REQ`(0x0C) 把站点槽位还给固件（见下方 Fixed）。
  动机：厂商 Windows 驱动正是这么做，同链路上传 **95.3 Mbit/s**，而本驱动 3.7 Mbit/s
  （机制见 `re/REPORT_TX_SESSION.md`）。
  - **48 字节载荷已实测被固件接受**（`MM_STA_ADD_CFM` → `status=0, sta_idx=0/1/2`），
    内核 dmesg 可见 `STA_ADD_CFM: status=0 sta_idx=N`；链路、DHCP、上网均不受影响。
  - **关键更正**：字段归属以 `re/REPORT_STA_ADD_STRUCT.md` 为准 —— 速率配置在**头部**
    `+0x00~+0x13`，尾部 `+0x14~+0x2F` 是 **A-MPDU 上限 / flags / 站点键**（早期"尾部是 RC 块"
    的解读已作废）。实测 `+0x00 format=0` 被接受、`format=2` 会被固件在 `rc.c:676` 断言。
  - **吞吐尚未提升**：A/B（同一 AP、−49 dBm）上传 0.38 vs 0.33 Mbit/s、下载 3.99 vs 3.71，
    差异在噪声内 ⇒ **只让描述符带上 `sta_idx` 不足以打开快路径**；厂商的 95 Mbit/s
    很可能还依赖聚合（下一步：`ampdu_action` + `MM_BA_ADD_REQ`）。当天的上游也很差
    （有线对照仅约 5 Mbit/s），该仪器分辨不出快路径。
  - 副产品：固件断言后设备**可自恢复**（USB 重枚举 → 驱动重 probe → 重灌固件 ≈10 秒，
    不用拔插），实验循环成本因此下降。

### Fixed (v0.4 主线：会话槽位清理 + `zt_tx_agg_send()` 编排缺陷，2026-10-01，未上机)
- **断开时发 `MM_STA_DEL_REQ`(0x0C) 归还固件站点槽位**（此前只清本地状态、不发 IPC）：
  固件每次关联分配新 `sta_idx`（死亡取证实测 0,2,3,…,9 单调递增），不发 0x0C 表格只增不减
  ⇒ 耗尽后 `STA_ADD_CFM status=1` ⇒ `sta_valid=false` ⇒ BA 建不起来 ⇒ 无固件会话却仍打
  多单元传输 ⇒ `macif.c:1019` 断言。载荷布局按厂商 `rwnx_send_sta_del`（0x140006f20）
  指令级逆向：**1 字节 `sta_idx`**，同步等 CFM `0x0D`；CFM 失败不阻塞断开流程，结果打
  dmesg 供上机对拍。注意：厂商 Windows 驱动里该函数**零引用**（死代码），其清理策略未知，
  我们按协议语义发。
- **槽位补齐循环污染 `done[]` 写入游标 `k`**：第二遍 memmove 补 1608 槽位的内层循环
  复用了 `k` 作游标，把它覆盖成 `sub`。"批次前置单帧（ARP 广播最常见）+ ≥2 聚合帧"的
  混合序列必触发 ⇒ 已发出的聚合帧部分不 report ⇒ **skb 泄漏 + mac80211 AQL 债务只增不减
  ⇒ TX 停滞**。修复：内层循环改用独立变量。
- **`sub<2` 退化分支重复发送**：退化时 `stop=0; k=0`，但第一遍里 `total==0` 时已单帧发出的
  不合格帧（EAPOL/ARP）会被尾部循环**再发一遍** —— 固件收到厂商驱动不会产生的重复输入。
  修复：新增 `first_agg`（第一个聚合帧下标），退化从该处起重扫，`k` 不清零。
- 连带语义统一：合格帧先记 `agg_idx[]`、bulk 成功后才进 `done[]`。六场景推演
  （全聚合 / break / 前置单帧+聚合 / 退化 / break+退化 / bulk 失败）闭环，无泄漏、无重复、
  无 double-free。**因果定位（诚实）**：两处是确定的正确性缺陷，修复是净收益；是否为
  `macif.c:1019` 断言的死因**未证实**（泄漏改变 TX 模式 / 重复帧是新的固件输入，仅候选）。
  取证与判据：`re/AMPDU_PUSH_STATUS.md` 第 13 轮、`re/DRIVER_PROGRESS.md` v0.4 第 13 轮。
  > **后续（r20，2026-10-01 实测更正）**：上条"断开发 `MM_STA_DEL_REQ` 归还槽位"的行为
  > **默认已关闭**（`sta_del_en=0`）：实测固件对断开后的会话清理不可逆，发 STA_DEL 后重连的
  > 加密单播 TX 全聋；让固件会话跨重连复用（`sta_reuse=1`，跳过重发 STA_ADD）是实测可用的
  > 重连路径。参数默认与上方描述相反，以本注为准。

### Added (v0.4 主线：WPA2 密钥路径与会话载荷定稿 → 固件 TX 死锁定案，2026-09-28 ~ 10-02，r19~r22)

- **★ 固件 TX 死锁定案（r21，2026-10-01/02）—— 上传主线的当前总卡点**：
  正常数据面（WPA2 关联 + DHCP + `ping`）上，加密单播帧只有**会话头 4~10 个**被空口确认
  （用户态 `/dev/zt9612` 抓帧：PN 0x300~0x900，其中 0x700 常缺失，之后归零；
  beacon/管理帧照收，AP 因无 reply 可发而表现为"RX 单播归零"）。
  三条独立取证把病灶钉死在**固件 TX 任务死锁**（不是固件崩溃、不是主机丢帧）：
  ① netdev/station 的 `tx_packets` 正常增长、`failed=0`、`retries=0` ⇒ 帧正常进驱动、USB 无错误；
  ② chardev 消息普查（`re/_r21_census.py`）：`0x0601` 心跳 1 条/s 持续到达（内含三个 32 位
  计数器）⇒ 固件活着；
  ③ **TX status 消息零条** ⇒ 帧交下去就再无回音。
  三个候选病因已逐一排除：PS/PS-Poll（395/395 beacon TIM 全空 + `Power save: off`）、
  EP2-IN 通知信用（本设备 alt 0 根本没有 EP2-IN，`ntf_log=40` 零输出）、`0x010c` kick
  （厂商每 EP5 数据帧前发 6 字节 `00 00 00 00 00 q`，逐包复刻 + 预防性逐包 pacer +
  事后复苏，三种用法均无效）。
  附带工具更正：早前"头 8 帧后全灭"部分是**抓帧工具假象** —— `os.read(4096)` 一次返回多帧
  （`WLAN`+hlen 链式粘包）而旧解析只取第一帧；`re/_r21_rxdump6.py` 已按 hlen 循环切分 +
  积压队列回放（t=-1 标注）。
- **`key_en` / `key_rx_en`（r19~r20，WPA 聋态修复主线）**：驱动此前没有 `.set_key` ⇒ 从不发
  `MM_KEY_ADD_REQ`(0x24)，WPA 网络下固件没有 PTK/GTK，加密帧全聋（`arp_oracle` 只在开放
  网络有效）。44B 载荷布局用户态定稿（`tools/key_add_probe.py`，同日两次实测：组密钥与
  pairwise 均 CFM status=0）。`key_en=1`（固件密钥路径）保留用于逆向，但 r20 实测
  **固件拿到密钥也不做 TX 加密**；`key_en=2`（**默认**）走 mac80211 软件加解密，
  WPA2 `ping` 0% 丢包（ht=0/ht=1 两臂）。`key_rx_en` 控制 RX 解密声明方式（默认 0 =
  原样上交，用 `/dev/zt9612` dump 抓加密帧）。
- **`sta_add_fmt=5`（r21 起默认）**：`MM_STA_ADD_REQ` 的 RC 块改用 **2026-10-02 USBPcap
  厂商实抓的逐字节真载荷**（`re/_r21_decode.txt`），推翻早期按静态逆向构造的 format=2 载荷
  （r17/r19 两次上机实测：format=2 载荷当场打死固件 —— format 字节本身就是致命项）。
  `0` = legacy 模板基线；`1`~`4` 为历史实验臂（详见 `MODULE_PARM_DESC`）。
- **`sta_del_en`（默认 0）/ `sta_reuse`（默认 1，r20）**：固件对断开后的会话清理不可逆。
  r20 曾以 `sta_reuse=1` 验证两次重连 0% 丢包；r21 进一步观测到**环境劣化后重连必死**
  （复用死会话时 `STA_REUSE: no MM_STA_ADD sent`，运行时强制 `sta_reuse=0` 拿新 sta_idx
  也救不回来，甚至走不到 assoc=1）—— 重连聋的深层成因与 TX 死锁同源。
- **`vendor_seq_en`（r21，默认 0）**：关联后补发厂商 STA_ADD 与密钥之间的 8 条配置序列
  （0x001a×4、0x0054、0x001e、0x0020、0x005a，载荷逐字节来自 USBPcap 实抓
  `re/_r21_decode.txt` L943-974），此前从未实现。
- **`tx_staidx`（默认 1）/ `tx_staid4b`（默认 0，r22）**：r22 在厂商 EP5 上行抓包里解出
  **46 字节关联期描述符形态**（与稳态 28B 形态并存两套）：
  `+0x00=00 | +0x01=staid（02→03 随 STA_ADD_CFM 同步演化，铁证）| +0x02=05（恒，
  与 fmt=5 呼应）| +0x03=00 | +0x06=seq 递增 | +0x14=µs 时间戳 | +0x1A=BSSID |
  +0x20=our MAC | +0x24=DA`。`tx_staid4b=1` 把描述符头 4 字节从稳态 `0xffffffff` 换成
  `00 staid 05 00` —— r22 首臂实测**无差异**（头 N 帧窗口不变），两套形态的缝合
  需要完整 46B 形态（描述符长度参数化）才有意义。
- **`0x0211` 事件帧 = 厂商每帧一条的 TX 完成通知**（dest=4，第 2 字节随帧递增 0c→0f→10→11）；
  本驱动固件侧 0 条 ⇒ 与 TX 死锁自洽。接入 `ieee80211_tx_status` 是下一步候选
  （若固件在等主机回签才继续，这条就是解）。
  **（r23/r24 更正，2026-10-03：上述定性作废**——厂商满载数据面抓包 276,594 包里 0x0211
  **0 条**，且厂商驱动零解析；它与 M34 desc32 实验的格式错误上报统一为"通用异常/通告消息，
  `+0x02 type` 区分"。本驱动 0 条与厂商数据面一致，不再是死锁证据，"接入"候选撤下。
  死锁证据改用 `0x0601` 遥测：TX 相关计数器冻结而主循环计数仍在跑。见 r24 条目。）
- 工具沉淀：`re/_r21_rxdump6.py`（粘包修复版抓帧 + beacon TIM 解析）、`re/_r21_census.py`
  （chardev 消息普查）、`re/_r21_kick1.py`（用户态单发 0x010c）、`re/_r21_txfor.sh`（TX 取证）、
  `re/_r22_staid4b2.sh`（r22 实验臂全套流程）。

### Added (0.3.3 候选：实验开关与工具)
- **实验开关 `tx_desc_mask`（默认 0 = 行为与 0.3.2 完全一致）—— 上传瓶颈的追查与一个被修正的结论**：
  在厂商 Windows 驱动上传（95.3 Mbit/s）时抓了 USB（96,107 条数据帧），确认**厂商对数据帧
  用的描述符常量与我们不同**（我们的值全部抄自**管理帧**/扫描 probe request）。逐字段二分：
  单改 `+0x08`（0xff00 → 0x0000）就能把 `ZT_IOC_TXRAW` 微基准读数从 6.67 拉到 178.88 Mbit/s。
  - **但那个读数不是吞吐，已用功能判据证实**：A/B/A 实测 `+0x08` 一改，
    **关联正常但 DHCP 拿不到地址**（0x02→无 IP / 0x00→拿到 DHCP 地址 `172.20.x.y`，两次交替复现）。
    新增 ARP 判据（`tools/arp_oracle.py`：清 ARP 表 → 发指定描述符的 ARP 请求 → 看网关回不回）
    给出直接证据：**管理模板能收到网关 ARP 回复；`mask=0x02` 与厂商完整模板 `mask=0x3F`
    都收不到任何回复** ⇒ 那些配置下帧**根本没上过空口**，设备只是"接收/丢弃得更快"。
    ⇒ "换模板就能修好上传"**不成立**；`usb_bulk_msg` 完成速度**不能**单独当作 TX 吞吐证据。
  - 静态逆向进一步否掉了"照抄常量"：`0x1312`/`0x7540` 在驱动 `.text` 里**不是立即数**，
    在**任何 IPC 消息里也不存在**（`re/txdesc_re.py`、`re/find_handle.py`）⇒ 无依据可抄。
  - 结论：厂商那条 95 Mbit/s 路径依赖**我们尚未复现的其它状态**（TX 上下文/会话设置），
    不是几个描述符常量。下一步改为逆向厂商 TX 提交路径的完整上下文，或走厂商渠道要源码。
    详见 `re/EXPERIMENT_TX_RATE.md` §5~7。
- **EP2-IN 通知通道接管（实验，`ntf_log` 开关）**：为验证 `re/REPORT_TX_RE.md` 的 C1 假设而实现。
  实测该设备 interface 0 **只有 5 个端点**（EP4-IN + EP5/6/7/8-OUT），**根本没有 EP2-IN**
  ⇒ C1 的前提不成立，那条 6 字节通知不属于本设备（或不在本接口）。代码保留接管逻辑
  （找不到端点时自动跳过），供将来复核。
- 诊断工具：`tools/tx_rate_probe.py`（帧长/端点/并发/描述符字段矩阵，支持 `--desc data --mask N`）、
  `re/tx_frames.py`、`re/desc_cmp.py`；抓包 `re/captures/vendor_upload.pcap`（281 MB，本地）。

### Fixed (0.3.3 候选/维护组：内核 API 与调试通道收口)
- **进程上下文误用 `ieee80211_tx_dequeue()`（7.0 内核上首帧 TX 触发 `net/mac80211/tx.c:3832`
  WARNING）**：该函数要求调用者**已关 softirq**（反汇编 WARN 点可见条件是
  `testl $0xff00,%gs:__preempt_count`），而 `zt_tx_work` 是工作队列（进程上下文）。
  现改用头文件指定的 `ieee80211_tx_dequeue_ni()`（内部 `local_bh_disable/enable`），
  并按 API 要求用 `rcu_read_lock()` 只护住"出队"这一步 —— 随后的 USB 传输会睡眠，
  不能长期持有 RCU（CCMP 由 mac80211 软件完成，skb 到手时已加密）。
  实测 `7.0.0-34-generic`：首帧 TX 的 WARNING 由 1 条降为 **0**，且经历一次
  固件断言 → USB 重枚举 → 重灌固件的循环后仍为 0；功能无回退。
  （`7.0.0-31` 上无此断言，属"换内核才现形"的问题；定位过程见 `docs/04` D14。）
- **三个"原始帧 TX 通道"的帧长上限仍是 1024 字节**（`ZT_IOC_TXRAW`、
  debugfs `tx_raw`、`/dev/zt9612` 的 `write()`）：0.3.2 只放宽了 mac80211 主 TX 路径
  （到 2012），这些调试通道漏了，导致**帧长相关的实验会直接 `EINVAL` 且看不出原因**
  （本次做 TX 微基准时就撞上：1436 字节传输被拒）。现统一为 `ZT_TX_BUF_SIZE`（2048）。
- **公开文档与 0.3.1/0.3.2 的事实对齐**：`zt9612.conf` 补上随后新增的 3 个参数
  （`ht_cap_enable`、`tx_desc_mask`、`ntf_log`，当时共 12 个；**后续**：`[Unreleased]` 又加入
  `sta_add_en`/`ampdu_en`，现共 **14** 个）；`README`、`FAQ`、`CONTRIBUTING`
  移除已被 [0.3.2] 更正的"**设备收帧上限约 1030 字节**"表述（实为驱动 TX 侧上限，接收侧正常），
  并把 README 路线图里同样作废的"TX 字节率上限 6.2–7.2 Mbit/s"（那是**管理帧模板**下的读数）
  改为当前的**固件站点会话**线索；给"驱动侧无可改之处"补上**只在 2.4 GHz 口径成立**的限定；
  README 项目状态表补 `v0.3.1`/`v0.3.2` 两行。

### Added (v0.4 主线：固件遥测定案——死锁证据换轨，r24，2026-10-03)
- **`0x0601` 心跳字段语义定案（纯离线差分：厂商扫描/厂商满载数据面/我们正常/我们死锁
  四组样本逐字段对齐）**：`+0x01`=主循环计数（增速随负载；死锁窗口仍在跑 ⇒ 主循环没卡）、
  `+0x05`=空口活动累计、`+0x09`=TX 空口时间累计（增速与负载严格正相关：满载 +1345/s、
  扫描 +93/s、空闲 +2.8/s）、`+0x0D`=TX 队列空闲事件计数、`+0x41/+0x49/+0x5D/+0x69..`
  数据面遥测。
- **死锁证据换轨**：死锁窗口 TX 相关三计数器（+0x05/+0x09/+0x0D）**全部冻结**而主循环
  计数仍在跑 ⇒ 固件主循环活、TX 管线停摆。原"TX status 消息 0 条"取证退役（`0x0211`
  旧定性已被厂商满载数据面 0 条推翻——见上方 r23/r24 更正括注）。
- **死锁根因假说排序**（判据全部预登记，一次上机多判据验证，不再盲试）：
  H1 队列头阻塞（某帧进 PHY 后永不完成、后续帧堵死；判据：死锁后 `MM_SET_IDLE(1)→(0)`
  踢队列，若空闲事件计数 +1 且 TX 复活即确证）＞ H2 EDCA 参数缺失（厂商每轮发
  `MM_SET_EDCA`×4 AC，我们从未发过；判据：回放后死锁是否推迟/消失）＞ H3 加密路径卡死
  （判据：死锁后发未加密单播是否同样卡死）。
- **trace 诊断通道判死**：固件 219 KB 段内无 ≥107 项的消息分派表（最大 80 项）、
  厂商驱动 53 个组帧 id 里零 trace 消息、50 项表与抓包 id 的对齐假设被证伪
  ⇒ `MM_TRACE_SWITCH(0x6b)`/`SET_LOG_LEVEL(0x6f)`/`GET_LOG_STATIS(0x71)`/`GET_FW_STAT(0x79)`
  盲发即赌博，不投（`0x48a/0x48c` 实为 efuse 读/写，维持禁发红线）。
- 分析工具：`re/r24_0601_timeline.py`（四会话遥测时间线差分）、`re/r24_pcaps_scan.py`
  （库存 pcap 的 0x0211/0x0601 普查）；报告 `re/REPORT_R24_FW_TELEMETRY.md`。

### Added (v0.4 主线：上机连环臂判决——"TX 死锁"重定性为关联后定时炸弹，r25，2026-10-03/04)
- **r24 三假设上机判决（判据预登记，A/B/M/N/P/Q 六臂连环）——全部减分**：
  H1 队列头阻塞（死锁态 `MM_SET_IDLE(1)→(0)` 无 CFM 无扰动）、H2 EDCA 缺失
  （`vendor_seq_en` **首次上机回放成功 `8/8 steps ok`** 含 4 条 `MM_SET_EDCA`，
  但风暴 round 1 仍 100% 死，与 A 臂无差异）、H3 加密卡死（明文单播 USB 层 5/5
  被接受但 B 计数纹丝不动 = 零空口时间）。
- **死亡序列秒级钉死（P 臂）**：固件数据路径在**关联建立后 ~8.5s（有流量）～
  ≥20s（被动）死亡**，0x0601 tail 三态 `00 7A → 10 2C → 90 2C` 是**固件侧死亡
  标记**（5 个独立窗口一致），翻转早于 mac80211 掉关联 **~9 秒**——
  "信标丢失导致掉线"因果倒置，掉关联（`bss_info assoc=0`，无 deauth by AP）
  是死亡 9 秒后信标看门狗的**结果**。死亡时间与流量强度**负相关**。
- **"TX 死锁标准照"退役**：B 计数冻结/tick 高/A 残余/微事件签名实为**掉关联态
  照片**；风暴从不是死因，只是检测手段。r21 "加密单播帧只交付头 4~10 个"与
  "26 倍 TX 差距"（3.65 vs 95.3 Mbit/s）很可能是**死前残值**，不构成稳态对比。
- **legacy 同样死（Q 臂，HT 假说出局）**：`ht_cap_enable=0`（HT/MCS/聚合路径
  全旁路）下同样死于关联后 ~11s（关联仅保持 10.75s），tail 三态翻转与 HT
  **完全同构**；唯一差异是无心跳洪流（1Hz vs HT 死后 ~12k/s，洪流为 HT 伴生
  现象非死亡必要条件）。**重连必败**（前置序列/4 次握手 CFM 超时——数据路径
  已死，握手帧发不出去）。R 臂参数归因判据已预登记（基线 / 无 vendor_seq /
  HT 无 vendor_seq 三臂）。
- **查询通道到顶（M/M1b/N/NL 臂）**：死锁态 `DBG_MEM_READ(0x486)` 仍有 CFM
  但可读窗口仅 `0x000..0x1FF`（512B）且三态 MD5 一致 = 静态 efuse/config 镜像，
  无运行态可读；`DBG_GET_SYS_STAT(0x48f)`/`MM_TRACE_SWITCH(0x006b)` 健康/死锁
  均无 CFM（固件未实现或部分致聋）。固件侧不可查询，只能被动监听 0x0601。
  消息名表全解 184 项（`re/_r25_nametable.py`，x64 指针 `<Q`）。
- **新坑入册**：rmmod/insmod 快速循环 → AP 侧幽灵关联积累 → 新会话 5/5 连接
  失败（NL 臂），需物理清理（网卡重插/AP 断电）。
- **R 臂归因（同轮后半场）——引爆点锁定 `MM_STA_ADD` 登记**：
  R1（legacy + 纯基线参数）**5 分钟零掉关联存活**（ping 100 轮 mean 0.8%，
  assoc 保持 >350s）；R2（legacy + `sta_add_en=1 sta_add_fmt=5`，无 vendor_seq）
  **round 3 死**（连接后 ~8-10s），tail 三态与 Q/P/B 同构 ⇒
  **`MM_STA_ADD_REQ`(0x0A, fmt=5 厂商真载荷) 发出后固件数据路径 8~15s 定时
  死亡；vendor_seq/HT/聚合/加密全部无辜**。与第 17 轮自洽（fmt=2 当场死、
  fmt=5 定时死）。
  叙事波及：第 15 轮"Windows GO 必死循环"（关联/BA 成功后 8~25s 挂死）
  与 r21 死锁均发生在 sta_add 时代之后，"GO 必死"很可能是同一枚炸弹。
- **R11 健康链路终审（同日晚）——STA_ADD 毒性坐实**：R3 原定执行撞上 AP
  断电重启后的连环坑（芯片挂死 -71/-110、写死网关 MAC 失效、AP 5G 数据面
  失效），四次 5G 会话作废后查明两件事：
  ① **AP 重启后 5G 只拒 legacy 调制的数据帧**（assoc/4 次握手/加密 RX 全通、
  DHCP/ARP 零响应；2.4G 同卡同驱动全通；`ht_cap_enable=1` 完全绕过，
  beacon 仍通告 legacy 基础速率——AP 侧行为与其自播 beacon 矛盾，机制未解）；
  ② 借 `ht_cap_enable=1` 恢复健康 5G 数据面后重跑 fmt=5 炸弹臂（R11）：
  **PRE-LIVENESS 0% 起跑 → round 2 死（~5-8s），census tail `10 2C`×3→
  `90 2C`×57、B 冻结**，与 R2/Q/B/P 逐位同构且链路健康由 R10（同链路
  ping 10/10）与 PRE 0% 双重锚定 ⇒ **R 臂归因获得健康链路最终确认**，
  "AP 劣化伪象"假说出局。tail 标记分类学新增 `00 04` = 已关联但数据从未
  流动（坏链路/幽灵会话标志，与死亡序列区分）。工具：`re/_r3_rxdump.py`
  （chardev RX 帧分类 dump，EAPOL/单播定向/beacon）。方法坑入册：跨 BSSID
  切换不重载模块 = 固件会话陈旧，每臂必须全新模块；写死网关 MAC 的
  `ip neigh replace` 遇 AP 重启必翻车，改动态 ARP 解析（`_r25_armR3b.sh`）。
  **R12/R13 完成判决链闭环**：R12（ht=1 + fmt=0 健康链路）**出生即闸死**
  （PRE 100%、tail `00 04`、B≡0）；R13（ht=1 纯基线）**5 分钟全程存活**。
  同链路同驱动同时段、唯一变量为 STA_ADD 载荷 ⇒ **"消息本身有毒"否证，
  毒在载荷内容/固件对注册对端的处理**——fmt=0 立即闸死、fmt=5 延迟炸弹。
  下一步：fmt=5 载荷逐字段二分（它至少让数据流动）。
- 报告 `re/REPORT_R25_ARM_SWEEP.md`（七臂判决 + §9 后记 + tail 判别器 +
  定时炸弹时间线模型 + 名表全解）；工具 `tools/zt_arm_tool.py` 扩展
  memdump/sysstat/trace。

### Planned (v0.4 主线：上传快路径与厂商渠道)
- **会话修复已完成（2026-10-04，`sta_add_fmt=7`）**：宽切除载荷通过浸泡+洪泛
  三重验证（见上方 r26 条目）。**剩余唯一卡点 = 聚合吞吐的 mac80211 TXQ
  计量**（agg 路径 ~55 帧/s vs 单帧路径 1760 帧/s；airtime 94% 空闲；
  tx failed=0）。修复两条路线（见 r25 报告 §9 与 `re/REPORT_AIC_MIGRATION.md`）：
  ① 读 mac80211 `ieee80211_tx_dequeue` 的 hold/计量语义（kernel.org 反爬，
     改走 elixir.bootlin.com 或 sparse clone；重点查 AQL charge 与
     `agg_start` 后的 TXQ 放行条件）；
  ② **架构路线（参照 aic8800d80）**：挂自定义 netdev_ops 绕开 mac80211 TXQ
     ——aic 源码已入库 `re/reference/`（rwnx_txq.c 私有队列 + 自己的聚合
     = 现成模板）。
- **fmt=5 毒字段精确定位（可选，学术）**：R17 已证明毒物在 fmt=5 的
  +0x04..05/+0x18..0x1f/+0x24..25 三个掩码块（回填即死），逐字节二分可定
  具体字段，但对 fmt=7 路线非必需。
- **厂商渠道（最高优先级）**：索取官方 Linux 驱动包（`ZTOP_ACEV100_Android_
  wifi_bt_*.tar.gz`，OpenHarmony 适配者证实存在），联系人
  fangtekuan@ztopmicro.com / +86 15037065080（FCC 申报责任人）；
  或 Olimex 官方支持（卖同款 USB-WIFI6-5G-ANT）。
  现有 219 KB 镜像里没有速率控制代码（指针表指向段外），这是唯一还能打开
  "主机影响发射速率"通道的路径

### Planned (0.3.3 候选/维护组：收尾与观测项)
- ~~**设备收帧上限约 1030 字节的成因**~~ **已作废（见 [0.3.2]）**：该阈值是**驱动 TX 侧**的帧长
  上限（三个原始帧通道同样受影响，已统一到 2048），**接收侧完全正常** —— 用 `rx_debug` 采样
  14095 帧，其中 **98.9% 都 >1030 字节**（主峰 1558）。不再列为待办；`rx_debug` 只保留为
  RX 帧长观测工具。
- 干净开机自动加载复验（D10 已修复，但"插卡 + 开机自动加载"这条完整路径尚未重跑）
- RX 描述符里的**速率**字段：描述符 `+0x10`/`+0x25` 是弱候选，
  USB 层抓包没有 radiotap 真值，宁缺勿猜
- **可选（仅为显示一致性，不提升吞吐）**：2.4 GHz 补 OFDM 速率表、声明 HT/VHT
  —— 两者都**不会**改变实测吞吐（见 [0.3.0] 的结论）
- 清理驱动源码中历史遗留的乱码注释（必须与实机编译验证一起做）

## [0.3.2] - 2026-09-27

> **上传修复版**：此前**上传基本是坏的** —— TX 路径的帧长上限只有 988 字节，
> 满尺寸 TCP 段（802.11 帧 ≈1534 字节）被驱动直接丢弃。修复后 `ping -M do` payload 1472
> 由 100% 丢包变为 0% 丢包，近端上传由"送出一个窗口就卡死"变为 **3.72 Mbit/s**。
> 同时更正一条被误诊很久的"设备限制"。

### Fixed
- **TX 帧长上限 988 字节 → 2012 字节（上传方向此前基本不可用）**：`MAX_FRAME = 1024`
  同时被用作 TX 缓冲，于是 `zt_tx_frame()` 的上限是 `1024 - 28(描述符) - 8(WLAN 头) = 988`；
  超过的帧被驱动**直接丢弃**（`tx_dropped++`）且不报错。MTU 1500 的满尺寸段在 802.11 层
  约 1534 字节 ⇒ 全部发不出去。下载方向看不出来（主机只回 ACK），所以此前一直没被发现。
  - 修复：TX 单独给 2048 字节缓冲（`ZT_TX_BUF_SIZE`，上限 2012），RX 保持 2048。
  - 实测（同一张卡、同一 5 GHz 链路）：

    | 观测 | 修复前 | 修复后 |
    |---|---|---|
    | `ping -M do` payload 1000/1200/1400/**1472** | **100% 丢包** | **0% 丢包** |
    | 上传（Cloudflare 测速口，RTT 213 ms） | **0.05 Mbit/s**（卡死） | 0.86 Mbit/s |
    | 上传（近端 TCP sink，同网段） | — | **3.72 Mbit/s**（3 流 4.73） |
    | 下载（对照） | 10.48 Mbit/s | 13.30 Mbit/s |

  - **同时更正一条误诊**：此前把 约 1030 字节阈值记成"**设备**收帧上限、TCP 不受影响"。
    真相是驱动 TX 侧的限制，而且影响上传；接收侧完全正常 —— 下载时用 `rx_debug` 采样
    14095 帧，**98.9% 都 >1030 字节、主峰 1558 字节**（满尺寸数据帧）。
    当初"设备限制"的推理错在：ping 大包不通是**请求发不出去**，自然也没有回复帧可收。
  - 仍存在（未修，见 Planned）：上传 3.7–4.7 vs 下载 7–13 Mbit/s 的不对称，
    候选原因是 TX 的一帧一条同步 `usb_bulk_msg`（未验证）。

### Added
- **实验开关 `ht_cap_enable`（默认关闭）**：给 5GHz band 声明 HT 能力（HT20/SGI、MCS 0-15、
  不含 40MHz、不声明聚合）。用于量化"5GHz 残余差距是否来自未声明 HT/VHT"。
  **实测结论：收益不成立，且不应默认打开** ——
  - 同一张卡 + 同一 5GHz BSS + 同一 URL，A/B/A 共 5 臂：声明 HT 15.71 / 14.28 Mbit/s，
    不声明 12.91 / 14.29 / 12.27 Mbit/s ⇒ 约 **1.14×**，小于同臂内的漂移（16%）；
  - 机制：即使协商上 HT，mac80211 的速率也停在 **MCS 0**（驱动 `tx_status` 默认关闭 ⇒
    速率控制拿不到反馈、爬不上去）；
  - 副作用：声明后 mac80211 会尝试发起 TX Block-Ack 会话，而本驱动没有 `ampdu_action`，
    触发 `WARNING: net/mac80211/agg-tx.c:623`（`docs/09` 阶段 D 预警过的风险）。
    > **后续（[Unreleased]）**：`.ampdu_action` 已实装（`ampdu_en`，默认 0），该 WARNING
    > 不再出现；但聚合**压流量会掉线**，所以 `ht_cap_enable`/`ampdu_en` 两个开关**仍然默认关闭**
    > （见本文件 [Unreleased] 的 `ampdu_en` 条目与 `docs/04` D15/D16）。
  - 详细设计、判据与数据见 `re/EXPERIMENT_HT_VHT.md`。
- **顺带更正**："5GHz 残余差距由未声明 HT/VHT 造成"这一设想**被证伪**：修好 5GHz 后本驱动
  最好成绩 15.71 Mbit/s 已落进厂商 Windows 驱动的 13.37–18.21 区间内，两边的差别
  很可能只是环境与测量方差 ⇒ **本驱动吞吐已与厂商驱动同一水平**。

## [0.3.1] - 2026-09-27

> **5 GHz 修复版**：v0.3.0 发布当天，用"同一张卡、同一个 AP、同一个 URL"的差分实验
> （厂商 Windows 驱动 vs 本驱动）发现 **5 GHz 一直完全不可用**，并定位到一行 bug。
> 修复后同一张卡实测吞吐 2.4 GHz 4.16 Mbit/s → 5 GHz **9.00 Mbit/s**，
> 强信号 5 GHz BSS 上 **12.10 Mbit/s**。

### Fixed
- **5 GHz 扫描的 band 标志写死成 0（导致 5 GHz 完全不可用）**：`zt_scan_work()` 的逐信道
  `MM_SET_CHANNEL` 参数里 `band` 恒为 `0`，等于给 5 GHz 信道发"2.4 GHz band + 5 GHz 频率"
  这种自相矛盾的请求 —— 固件照样回 `MM_SET_CHANNEL_CFM`（所以驱动以为切成功了），
  但无线电不会真的切过去。
  - **症状**：5 GHz 扫描恒 `beacon=0`（2.4 GHz 是 `beacon=32–56`）；`iw scan` 里冒出的
    "5 GHz BSS"经不起检查 —— 例如 `freq: 5745` 的条目却带 `DS Parameter set: channel 11`、
    `ERP`、`Country: Channels [1-13]`（都是 2.4 GHz 专有元素），同一个 BSSID 还会同时
    以 2462 MHz 与 5745 MHz 出现。
  - **ground truth**：厂商抓包 81/81 条 `SET_CHANNEL` 满足 `band = (freq>2500) ? 1 : 0`
    （2484→0、5180→1，见 `re/REPORT_5GHZ.md` §1.1）。同一文件里的 `zt_set_channel()`
    一直是对的，只有扫描循环漏了。**这也是"逆向厂商驱动"最实际的用法：把它的抓包当标准答案
    逐字段对照，而不是照抄代码。**
  - **实机验证**（Ubuntu 24.04 / 内核 `7.0.0-31-generic`，同一张 ZT9612U，10 s×3 取中位数）：
    5 GHz 扫描从 `beacon=0` 恢复正常、真实 5 GHz BSS 由 1 个（自相矛盾项）变为 **29 个**；
    完成本项目历史上**第一次真正的 5 GHz 关联**（5 GHz BSS，ch153，−54 dBm）；
    吞吐 2.4 GHz 4.16 → 5 GHz **9.00 Mbit/s**，强信号 5 GHz BSS（−26 dBm）**12.10 Mbit/s**。
  - **对照**：同一张卡在厂商 Windows 驱动下 5 GHz 为 13.37–18.21 Mbit/s（本版仍略低，
    候选原因是本驱动未声明 HT/VHT；**尚未验证**）。
    > **后续**：该候选已被证伪 —— 声明 HT 只有约 1.14×、与漂移同量级，见本节下面的更正段
    > 与 `[Unreleased]` 的 `ht_cap_enable` 条目（`re/EXPERIMENT_HT_VHT.md`）。
- **同时更正两条旧结论**（README 已同步）：
  - M3.5「5 GHz 已完成并实机验证」当时的证据（"59 个 BSS 中 7 个在 5GHz"）是频点错误标注的产物，
    5 GHz 关联在此之前从未发生过；
  - v0.3「驱动侧无可改之处」**只在 2.4 GHz 范围内成立** —— 5 GHz 是一条值 2–3 倍的
    driver-side 杠杆，此前完全没被看见。

### 诊断工具（本次实验新增，本地未入库）
- `tools/win_diff_run.ps1`：Windows 侧测量器，自动识别被测网卡并用**网卡计数增量**核对出口
  （Windows 是弱主机模型，只绑源地址不保证走哪块网卡）。
- `tools/win_connect_bssid.ps1`：用 `WlanConnect` 的 desired-BSSID 列表把网卡锁到指定 BSS
  （`netsh` 做不到），用于逼厂商驱动上 2.4 GHz（结果：它拒绝，直接失败断开）。

## [0.3.0] - 2026-09-27

> **吞吐里程碑完成**：真修复是接收缓冲（`ZT_RX_BUF_SIZE` 由 1024 提到 2048），
> 吞吐从"HTTP 200 之后收不到数据"变为实测 **4.2–4.7 Mbit/s**；
> 速率由固件内的 ARRM 模块自主决定，主机侧三条通道（描述符 / 速率消息 / 速率掩码）
> 全部无效 ⇒ **驱动侧已无可改之处**。完整报告见 `re/REPORT_V03_THROUGHPUT.md`。
>
> **后续更正（见 [0.3.1]、[0.3.2]）**："驱动侧无可改之处"**只在 2.4 GHz 口径成立**：
> 发布当天就查出 5 GHz 扫描的 band 标志写死成 0（[0.3.1]，吞吐 2–3 倍），
> 随后又查出 TX 侧 988 字节的帧长上限（[0.3.2]，上传此前基本不可用）。
> 该结论应读作"**主机侧无法影响发射速率**"，而不是"吞吐没有驱动侧缺陷"。
> 上传方向的差距（本驱动 3.7–4.7 vs 厂商 Windows 驱动 95.3 Mbit/s）仍在追查。

### Changed
- 修正文档与代码不一致的历史遗留：`zt9612.conf` 补全全部 **9** 个模块参数
  （`do_init`/`do_boot`/`scan_probe`/`tx_ep`/`tx_prep`/`tx_variant`/`rx_debug`/
  `tx_status`/`tx_status_probe`）并更新
  `scan_probe` 说明（`2` 重放厂商 probe 已验证能收到 probe response，`1` 自建 probe 尚未单独复验）、
  `install-driver.sh` 的能力说明改为 v0.2.0 的真实状态（不再 grep `wlan0`，
  改为按实际接口名查找，因为接口名由 MAC 生成）、README 验收输出改用当前的 MAC 读取路径
  （**注**：此处"9 个"是当时的数量；0.3.2 与未发布改动又加入 `ht_cap_enable`/`tx_desc_mask`/
  `ntf_log`，当时为 **12** 个，`zt9612.conf` 已在 [Unreleased] 同步；**截至 2026-09-27 为 14 个**，
  [Unreleased] 又加入 `sta_add_en`/`ampdu_en`）
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
  1.0 / 11.0 Mbit/s，而**实测吞吐完全不跟着变**（各条件 3–4.7 Mbit/s）；
  且实测吞吐本身就是"1 Mbit/s 链路"的 3–4.7 倍，物理上不可能是按 1 Mbit/s 在发。
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
  因此 v0.3 的"吞吐优化"不能靠补速率表实现；**唯一还能提升实测吞吐的手段是改善空口条件**
  （信道/距离/干扰），属环境而非驱动。（此处原先还列了"声明 HT/VHT 能力"，
  后被论证为同样无效 —— 见下文「HT/VHT 也救不了吞吐」。）
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

**rank → wire id 的映射没有廉价解**：一度以为找到规律（`2×rank+8` 在 rank 12–21
上连续成立，与实测的 SET_SLOTTIME=0x20、SET_IDLE=0x22、SET_POWER=0x24 吻合），
但探针实测 `0x0054` 是 `MM_SET_P2P_OPPPS_REQ`（印证了固件表与厂商枚举**不同序**），
而 RRM 速率的候选位点 `0x0042–0x0045` **全部无效** ⇒ 公式作废，只能靠穷举探测，
而穷举要遍历大量**语义未知**的消息（部分会改信道/复位/断言），违反"不猜结构"的纪律。

**因此这条通道判定为结构性封闭**，两条理由：
1. **参数结构无从获得**：厂商驱动从未发送这两条消息（抓包里 0 次），所以没有可对照的
   样本；而包含 handler 的那部分固件代码**不在**我们手上的 219 KB 镜像里
   （指针表指向段外、`gp` 也越界）。即使穷举出 wire id，也仍然构造不出安全可用的参数。
2. **风险不对称**：猜参数的代价是固件断言 + 设备挂死（历史上盲改描述符就触发过，
   需要物理拔插），而收益只是"多一个可能影响速率的通道"。

**最终结论**：让主机重新影响速率的现实路径不存在（在本仓库现有资料范围内）。

**HT/VHT 也救不了吞吐（2026-09-27 更正）**：一度认为"声明 `band->ht_cap` 能给固件内
ARRM 更大选速空间，是唯一剩下的驱动侧手段"，随后论证为**预期无效**，三条理由：
① **固件自己发 probe request、自己关联**（`zt_scan_probe()`），关联请求里的 HT IE 由固件决定，
不是 mac80211 的 `ht_cap`；② **不存在"主机→固件 HT 能力"的消息**（`MM_SET_*` 里只有
`SET_BASIC_RATES`，而厂商从未发送）；③ **扫描结果里的 HT/VHT 能力本来就来自固件**
（`re/REPORT_5GHZ.md` 实测 5G 帧自带 HT Capabilities）。
因此在 Linux 侧声明 HT 只会改变 mac80211 内部行为并引入聚合风险，不会改善实测吞吐。

**v0.3 就此收尾，驱动侧无可改之处**：
实测吞吐 4.2–4.7 Mbit/s（链路仅报 1.0 Mbit/s）；唯一还能提升实测吞吐的手段是
**改善空口条件**（信道/距离/干扰），属环境而非驱动。
若将来能拿到**包含 RAM 段代码的完整固件**，"主机影响速率"这条通道才值得重新评估。
完整报告见 `re/REPORT_V03_THROUGHPUT.md`。

### 仍在未确定状态的（别当成结论）
- **速率控制算法本身**：`arrm_rc.c` 的代码**不在** `zt9612_fw.bin` 这 219 KB 里 ——
  镜像自己的指针表指向 `0x610D7xxx`/`0x610E8xxx`（1484 个代码段 u32 里 1370 个越界，
  连 `gp = 0x610CCC00` 都越界）。所以"表驱动 / 探测式 / 混合"无法从本文件判断。
- **固件短名表的 rank 到 wire id 的映射**：实证否证了"rank 即 id"与 `2×rank+8`
  两种规律，且固件表与厂商枚举不同序 ⇒ 无法静态推导（见上一节的封闭结论）。
  上游 `re/REPORT_FW3_TXGATE.md` §1.1 的
  "表项索引即 id" 与 `re/REPORT_FW2_DEOBF.md` §4.2 的
  "`0x19DAC` 是主机 TX 描述符解析点" 均已被本轮**推翻**，两份报告需带更正说明阅读。

### Known limitations（已知限制，不是待办）
- **`iw link` 的 tx bitrate 不是真实发射速率**（只是主机侧速率掩码/记账）：它报 1.0 Mbit/s，
  而实测吞吐 4.2–4.7 Mbit/s。**不要用它衡量性能**
- 速率由**固件内的 ARRM 模块**自主决定，主机侧无法影响（描述符无速率字段、厂商不发速率消息、
  速率掩码无效 —— 详见上文与 `re/REPORT_HOST_RATE.md`）
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
- **收帧上限 约 1024 字节（发送侧无此限制）** ⚠️ **本条结论已被 [0.3.2] 全面更正**：
  该阈值是**驱动 TX 侧**的帧长上限（上传因此基本不可用），接收侧完全正常。下面各条的
  "限制在设备侧""TCP 数据面不受影响"**均不成立**，保留仅作过程留痕。原文：禁止分片的 `ping`/UDP 归因实验
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
- 运行期 `ip link set mtu` 会让接口短暂失去关联（实测丢 1–2 个 ping，NetworkManager 会重配）；
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
  `wpa_cli status` 报 `wpa_state=COMPLETED`；随后 DHCP 拿到 `192.168.x.y/24`（租约 6 h），
  `ping` 网关 3/4、`ping 223.5.5.5` 3/3（48–69 ms）、`ping 8.8.8.8` 3/3（54–82 ms），
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
  -95..-20 兜底），`freq/band` 取 `+0x2A` 并在 wiphy 反查。实测 25 个不同取值、-94 至 -23 dBm、
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
  速率控制拿不到反馈）。**这一条已在 v0.3 查清并部分修复**：真正的缺陷是 RX 缓冲过小
  （已修，吞吐 4.2–4.7 Mbit/s），而"恒定 1 Mbit/s"本身是 `iw link` 的显示值、
  不是真实发射速率（速率由固件内 ARRM 自主决定）。见 `[Unreleased]` 与
  `re/REPORT_V03_THROUGHPUT.md`
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

[Unreleased]: https://github.com/Spkicn/zt9612-linux/compare/v0.3.2...HEAD
[0.3.2]: https://github.com/Spkicn/zt9612-linux/releases/tag/v0.3.2
[0.3.1]: https://github.com/Spkicn/zt9612-linux/releases/tag/v0.3.1
[0.3.0]: https://github.com/Spkicn/zt9612-linux/releases/tag/v0.3.0
[0.2.0]: https://github.com/Spkicn/zt9612-linux/releases/tag/v0.2.0
[0.1.0]: https://github.com/Spkicn/zt9612-linux/releases/tag/v0.1.0

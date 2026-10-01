// SPDX-License-Identifier: GPL-2.0-only
/*
 * zt9612.c - Linux driver for ZT9612U (ZTOP / 閸忓棝鈧艾浜?ACEV100) USB WiFi adapter
 *
 * M1 + M2:
 *   - 閸ヨ桨娆㈢憗鍛版祰閿涘牊褰欓幍?/ 488B 閸?/ 閺堫偄娼?XOR16 / 闁板秶鐤嗛崸?/ RUN閿? *   - 閸氬本顒為崚婵嗩潗閸栨牕绨崚妤嬬礄閸欐垳绔撮弶锛勭搼娑撯偓閺?CFM閿涘绱癛ESET -> VERSION -> 閸樺倸鏅?-> START(缁涘瀹?6.5s) -> 闁板秶鐤? *   - 5 缁夋帒绺剧捄?0x05c2閿涘牐娴囬懡宄版儓 ASCII 閺冨爼妫块幋绛圭礆閿涘奔绗夐崣鎴濇祼娴犳湹绱伴惇瀣，閻欐顦叉担? *   - /dev/zt9612閿涙氨鏁ら幋閿嬧偓浣稿讲閻╁瓨甯撮弨璺哄絺閸樼喎顫?"WLAN" 鐢? *
 * 閸楀繗顔呴弶銉ㄥ殰 USB 閹舵挸瀵?+ 闂堟瑦鈧線鈧棗鎮滈獮鍫曗偓鎰摟閼哄倿鐛欑拠渚婄礄鐟?zt9612-linux/re/REPORT*.md閵嗕笍RIVER_PROGRESS.md閿? */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/kfifo.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/time.h>
#include <linux/time64.h>
#include <linux/etherdevice.h>
#include <linux/unaligned.h>
#include <net/mac80211.h>

#define DRV_NAME	"zt9612"

#define ZT_VID		0x350B
#define ZT_PID		0x9612
#define EP_OUT_NUM	8
#define EP_IN_NUM	4
#define EP_NTF_NUM	2		/* EP2-IN：厂商 TX 期间每次都在读的 6 字节通知通道（见 ntf_log） */

#define ZT_BLOCK_SIZE	488
#define SETTINGS_ADDR	0x210CE700
#define MAX_FRAME	1024		/* /dev 调试通道与内部接收缓冲的帧上限 */
#define MAX_PAYLOAD	(MAX_FRAME - 8)
/*
 * TX 缓冲必须容得下满尺寸的 802.11 数据帧。
 *
 * 实测教训（2026-09-27，长期被误判为"设备限制"）：
 *   TX 路径沿用了 MAX_FRAME=1024，于是 `zt_tx_frame()` 的上限是
 *   1024 - 28(描述符) - 8(WLAN 头) = **988 字节**；超过的帧被驱动**直接丢弃**
 *   （`tx_dropped++`，见 zt_tx_one()），而且不报错。
 *   后果：主机发不出满尺寸段（MTU 1500 → 802.11 帧 ≈1534 字节）——
 *   下载方向看不出来（主机只回 ACK），但**上传几乎完全不通**：
 *   实测修复前 upload 0.05 Mbit/s vs download 10.48 Mbit/s（Cloudflare 测速口同口径）。
 *   同一条 ~1030 字节阈值也被误记成"设备收帧上限"：其实设备**完全能**收大帧——
 *   下载时 rxdbg 采样 14095 帧里 98.9% >1030 字节、主峰 1558 字节（满尺寸数据帧）。
 *   ping payload ≥1000 的 100% 丢包是**发不出去**（请求被丢），不是收不回来。
 *
 * 因此 TX 单独给 2048：最大帧 = 2048 - 28 - 8 = 2012 字节，足够放下 1500 MTU 的段。
 */
#define ZT_TX_BUF_SIZE	2048		/* TX 缓冲（z->tx / z->txd） */
#define ZT_TX_MAX_FRAME	(ZT_TX_BUF_SIZE - TX_DESC_LEN - 8)
/*
 * 聚合实验（ampdu_en）用的更大缓冲：**A-MPDU 由本驱动自己组帧** —— mac80211 一次
 * `ieee80211_tx_dequeue()` 只给一个 MPDU，把多个 MPDU 拼进**同一次** bulk 传输是驱动的事
 * （2026-09-30 从厂商上传抓包定案：EP5 的传输长度是 1608 / 3200 / 4808，
 *  即 1/2/3 个 `WLAN`+描述符+MPDU 单元首尾相接，每个单元 8 字节对齐）。
 */
#define ZT_AGG_BUF_SIZE		16384
#define ZT_AGG_SUBFRAMES	8
/* 一个 txq 每次唤醒最多做几轮"攒批 + 发送"（防止在 work 里长时间独占） */
#define ZT_AGG_MAX_BLOCKS	16
/*
 * 聚合时**非末尾单元**的固定槽位（厂商实测：下一个 `WLAN` 恒在 +1608）。
 * 1608 = 8(WLAN 头) + 28(描述符) + 1572(MPDU 上限) 且是 8 的倍数。
 * 末尾单元只补到 8 字节对齐 ⇒ 传输长度 = (n-1)×1608 + align8(8+hlen_last)。
 */
#define ZT_AGG_STRIDE		1608
/*
 * RX 缓冲必须容得下一个满尺寸的 802.11 数据帧，不能按"扫描帧都不大"来定。
 * 实测（2026-09-26）：MAX_FRAME=1024 时，ping payload ≥ 900 字节（MPDU≈968）
 * 的回复一个字节都上不了栈 —— 空口收到了，URB 缓冲装不下就直接丢，
 * 表现为 TCP/TLS 大包完全不通（MTU 声明 1500，实际可用约 917）。
 * 现在按最坏情况给：2304(MSDU 上限) + 24(MAC 头) + 8(LLC/SNAP) + 8(CCMP) + 8(WLAN 头)。
 */
#define ZT_RX_BUF_SIZE	2048		/* RX URB 缓冲：覆盖 ~1950 字节的帧 */
#define ZT_RX_MAX_FRAME	2048		/* 单帧硬上限（超过按脏帧丢弃） */
/*
 * 接口 MTU 上限。
 *
 * 历史：曾按"设备每帧只能处理约 1 KB"的观测（ping payload 905 通、920 丢）把它设成 900，
 * 想借此逼内核不用 1500 字节的段。实测（2026-09-26）证明这样做得不偿失：
 *   * 1500 字节 MTU 下 TCP 下载正常（RX 缓冲修好后中位数约 4.2 Mbit/s，
 *     比 900 下的 3.2 Mbit/s 还高）；
 *   * 而 MTU 上限 900 会让 **IPv6 完全不可用**（IPv6 要求 MTU ≥1280，
 *     也是 mac80211 的硬下限），接口再也拿不到 IPv6 地址。
 * 因此上限回到标准值：不做设备特有限制。
 * 那段 905 字节天花板已于 2026-09-27 定案：是**驱动自己 TX 侧的上限**（见 ZT_TX_BUF_SIZE 注释），
 * 与设备无关，也不影响 TCP 数据面。
 */
#define ZT_MAX_MTU	1500
#define RX_FIFO_SIZE	(64 * 1024)
#define HB_INTERVAL_MS	5000

/* M3.2 扫描 */
#define ZT_MAX_SCAN_CH	48		/* 2.4G 14 + 5G 25；厂商一轮扫 39 个信道，留余量 */
#define ZT_SCAN_DWELL_MS 120		/* 每信道停留时间 */
#define T_RX_DATA0	0x0000		/* RX 数据帧（描述符 48 字节） */
#define T_RX_DATA4	0x0004		/* RX 数据帧（描述符 52 字节） */

/* M3.4 TX 数据路径（从抓包还原，见 re/REPORT_M32_RX_PATH.md 与 analyze_tx.py）
 *   EP5-OUT + WLAN 帧 type=0x0000，28 字节描述符后接 802.11 帧
 *   描述符: +0x00 u32 0xffffffff | +0x04 u16 帧长 | +0x06 u16 0x0700
 *           +0x08 u16 0xff00 | +0x0a u16 0x0005 | +0x0e u16 序号
 *           +0x1a u16 0x003f
 */
#define EP_TX_NUM	5
#define TX_DESC_LEN	28
#define TX_TYPE_DATA	0x0000		/* TX 数据帧的 WLAN type（与 RX 侧 0x0000 同值） */
#define ZT_MAX_PEND_TXQ	16		/* wake_tx_queue 一次最多登记几个待处理队列 */

#define T_IPC		0x0100
#define T_FW_WRITE	0x0200
#define T_FW_START	0x0201
#define T_FW_WRITE_LAST	0x0204

#define SUB_FW_START	0x00
#define SUB_FW_READY	0x01
#define SUB_FW_BLOCK	0x02
#define SUB_FW_LAST	0x03
#define SUB_FW_ACK	0x04
#define SUB_RUN		0x05

#define ZT_MAGIC	0x545A		/* "ZT" */

static int do_init = 1;
module_param(do_init, int, 0644);
MODULE_PARM_DESC(do_init, "run the synchronous IPC init sequence after boot (default 1)");

static int do_boot = 1;
module_param(do_boot, int, 0644);
MODULE_PARM_DESC(do_boot, "download firmware (default 1); set 0 to reuse an already running firmware");

/*
 * M3.4 实验开关：扫描时是否主动发 probe request（TX 数据路径，EP5-OUT）。
 *   0 = 被动扫描（默认，已验证：只听 beacon）
 *   1 = 发送本驱动自建的广播 probe request
 *   2 = 逐字节重放抓包里厂商的 probe request（用于区分"帧内容问题"与"描述符/状态问题"）
 * 已知：修好 MAC/MM_ADD_IF（第 10 轮）之后，**模式 2 实测能收到 probe response**
 * （probe=13 / probe-resp=22）；模式 1 自那以后没有单独复验过，不要当成已验证。
 * 主动发送曾长期收不到响应，原因不在帧里而在 vif 地址（见 re/DRIVER_PROGRESS.md 第 10 轮）。
 */
static int scan_probe;
module_param(scan_probe, int, 0644);
MODULE_PARM_DESC(scan_probe, "0=passive scan (default), 1=send own probe request, 2=replay vendor probe");

/* M3.4 实验：TX 变体（用于定位"帧被接受但没发出去"的原因） */
static int tx_ep = EP_TX_NUM;		/* 发送端点号（5/6/7 试验） */
module_param(tx_ep, int, 0644);
MODULE_PARM_DESC(tx_ep, "TX bulk OUT endpoint number (default 5)");

static int tx_prep = 1;			/* 切信道前是否重放厂商的使能序列 */
module_param(tx_prep, int, 0644);
MODULE_PARM_DESC(tx_prep, "replay the vendor TX-enable sequence before SET_CHANNEL (default 1)");

static int tx_variant;			/* 0=正常 1=描述符+0x00 置 0 2=不带描述符 3=补齐到 512 */
module_param(tx_variant, int, 0644);
MODULE_PARM_DESC(tx_variant, "TX frame variant for experiments (default 0)");

/*
 * v0.3 阶段 B：TX status 上报（默认关闭，用实验开关控制）。
 *
 * 背景：驱动发完帧直接释放 skb，mac80211 的速率控制（minstrel_ht）收不到任何反馈，
 * 于是 `iw link` 恒报 1.0 Mbit/s。这个开关用来回答一个关键问题：
 * **速率控制拿到反馈后，速率与实测吞吐会不会动？**
 *
 *   0 = 不上报（默认，与 v0.2.0 行为一致）
 *   N = 上报；每 `tx_status_probe` 帧里把 1 帧标成"已被 ACK"，其余标成失败
 *       （N 的数值本身不参与判断，只看是否为 0）
 *
 * 为什么默认不上报、且要留"乐观探测"：**我们并不知道帧是否真的被 ACK** ——
 * USB 写入成功不等于空口成功，固件也没给我们确认通道。如果无条件上报
 * "已 ACK"，就是在骗速率控制（会让它以为高数据率可用）。
 * 因此：
 *   * `ack` 只在每 `tx_status_probe` 帧里"乐观探测"一次（默认 100，
 *     minstrel 的 ewma 约 100 帧半衰期，所以这个比例足以让它周期性往上试），
 *   * 其余帧一律如实报 false —— 宁可在这一轮显得慢，也不给速率控制灌假数据。
 *
 * 判据（见 docs/09-v0.3-吞吐开发计划.md 阶段 B）：
 *   * `iw link` 的 tx bitrate 开始变化 + 吞吐同步上升 ⇒ 主机速率进得去，走阶段 C；
 *   * `iw link` 变了但吞吐不动 ⇒ 固件自己选速，转逆向（docs/09 §4）；
 *   * 吞吐下降或出现重传/失败激增 ⇒ 立刻把开关调回 0。
 */
static int tx_status_on;		/* 0 = 关闭；非 0 = 开启上报 */
module_param_named(tx_status, tx_status_on, int, 0644);
MODULE_PARM_DESC(tx_status, "report TX status to mac80211: 0=off (default), non-zero=on");

static int tx_status_probe = 100;
module_param(tx_status_probe, int, 0644);
MODULE_PARM_DESC(tx_status_probe, "when reporting, mark 1 frame in N as acknowledged (default 100)");

/*
 * 调查用开关：把接下来 N 个收到的 USB 传输的长度打出来。
 *
 * 用法（可随时重置，不必重载模块）：
 *   sudo sh -c 'echo 40 > /sys/module/zt9612/parameters/rx_debug'
 *   ... 触发要观察的流量（例如 ping -s 1400 -M do <gw>）...
 *   sudo journalctl -k --since '-1 min' | grep rxdbg
 *
 * 两个坑（都踩过）：
 *   1) 别用 `echo N | sudo tee <file>` 写这个参数 —— tee 会把 stdin 吃光，
 *      N 根本进不去（用 `sudo sh -c 'echo N > ...'`）；
 *   2) 读日志用 journalctl -k：本机 `kernel.dmesg_restrict=1` 且 printk 级别低，
 *      `dmesg` 直接读是空的。
 *
 * 2026-09-26 用它得到的**现象**：ping payload 900 时能看到 usb_len=1038 的回复帧，
 * 而 payload 1400（100% 丢包）时整段窗口里 usb_len 最大只有 570（全是背景流量）。
 * 当时据此解读为"设备对超过约 1030 字节的帧直接丢弃" —— **该解读已于 2026-09-27 证伪**
 * （见文件头 ZT_TX_BUF_SIZE 注释）：payload 1400 的**请求帧本身**就被驱动 TX 侧 988 字节
 * 上限丢掉了，自然没有回复帧可看；设备接收侧完全正常（采样 14095 帧，98.9% >1030 字节）。
 *
 * 默认 0（关闭）。
 */
static int rx_debug_left;		/* >0 时继续打印，每条递减 */
module_param_named(rx_debug, rx_debug_left, int, 0644);
MODULE_PARM_DESC(rx_debug, "log the length of the next N received USB transfers (write N to start)");

/*
 * 实验开关（默认关闭）：给 5GHz band 声明 HT 能力。
 *
 * 背景：v0.3.1 修好 5GHz 之后，同一张卡实测 9~12 Mbit/s，而厂商 Windows 驱动
 * 在 5GHz 上是 13.4~18.2 Mbit/s，且厂商驱动跑的是 802.11ax / 573~1201 Mbps PHY。
 * 本驱动此前不声明 band->ht_cap，mac80211 只能跑 legacy OFDM（6~54 Mbps）。
 *
 * 实测结论（2026-09-27，同一张卡 + 同一个 5GHz BSS + 同一 URL，10s x 3 中位数，
 * A/B/A 共 5 臂 —— 详见 re/EXPERIMENT_HT_VHT.md）：
 *   - 声明 HT：15.71 / 14.28 Mbit/s；不声明：12.91 / 14.29 / 12.27 Mbit/s
 *     ⇒ 约 1.14x，**与同臂内的漂移（~15%）同量级，收益不成立**；
 *   - 机制：即使协商上 HT，`station dump` 的速率也停在 **MCS 0**（6.5 MBit/s）——
 *     驱动不上报 TX status（tx_status 默认关），mac80211 的速率控制拿不到反馈、爬不上去；
 *   - **副作用**：声明后 mac80211 会尝试发起 TX Block-Ack 会话。当时本驱动没有
 *     `ampdu_action`，于是触发 `WARNING: net/mac80211/agg-tx.c:623`（docs/09 阶段 D 预警过）。
 *     **已于 v0.4 处理**：实现了 `.ampdu_action`（见 ampdu_en），`ampdu_en=0` 时不声明
 *     `AMPDU_AGGREGATION`、mac80211 不会发起会话 ⇒ 本 WARNING 不再出现；
 *     `ampdu_en=1` 时会话能建立，但**压流量会掉线**，因此两个开关默认都关。
 *
 * ⇒ 默认保持关闭。**HT 的真正意义是"A-MPDU 聚合的前置条件"**（mac80211 的
 *   `ieee80211_aggr_check()` 在未声明 HT 时直接返回，实测见 docs/04 D16），
 *   而不是它自己能提速；要继续走聚合这条路，剩下的是**固件对 A-MPDU 的推送/封装语义**
 *   （驱动现在仍是一帧一次 bulk + 28 字节描述符，吃不下 mac80211 交下来的聚合 skb）。
 *   声明范围刻意保守：只 5GHz、只 HT20（不含 40MHz）、不声明聚合相关 hw 标志。
 */
static int ht_cap_enable;
module_param(ht_cap_enable, int, 0644);
MODULE_PARM_DESC(ht_cap_enable, "experimental: advertise HT on the 5GHz band (default 0; measured no throughput gain, but it is the prerequisite for A-MPDU aggregation - see docs/04 D16 and re/EXPERIMENT_HT_VHT.md)");

/*
 * 实验开关（默认关闭）：打印接下来 N 条 EP2-IN 通知的内容。
 *
 * 背景：厂商 Windows 驱动在**每一次** EP5-OUT 前后都在读 EP2-IN 上的 6 字节通知
 * （抓包：`ep=0x82 dlen=6`，内容如 `00fbffffff00` / `00fcff000000`，随 TX 推进变化，
 * 像"已完成 TX 序号 / 队列水位"），而本驱动此前**完全没有接管这条通道**
 * （见 re/REPORT_TX_RE.md 的 C1）。同一台机器实测：厂商驱动上传 95.3 Mbit/s、
 * 本驱动 3.65 Mbit/s —— 差 26 倍。待验证的假设是"固件要等主机把通知取走才放行下一帧"。
 *
 * 无论本开关是否为 0，这条通道都会被**持续读取**（计数见 debugfs `zt9612/ntf_count`）；
 * 本开关只决定是否把内容打进 dmesg。
 *
 * **实测结论（2026-09-27）：假设已被证伪** —— 本设备 interface 0 **只有 5 个端点**
 * （EP4-IN + EP5/6/7/8-OUT），**根本没有 EP2-IN**，所以那条 6 字节通知不属于本设备
 * （或不在本接口）。代码保留接管逻辑，v0.4 起找不到端点就自动跳过。
 */
static int ntf_log;
module_param(ntf_log, int, 0644);
MODULE_PARM_DESC(ntf_log, "experimental: log the next N EP2-IN TX notifications (0 = off; the channel is always read)");

/*
 * 实验开关（默认 0 = 与 0.3.x 行为完全一致）：数据帧描述符的**字段级**模板。
 *
 * 背景（re/EXPERIMENT_TX_RATE.md §5~6）：厂商上传时的 USB 抓包显示，厂商对**数据帧**用的
 * 描述符常量与我们不同 —— 我们的值全部抄自**管理帧**（扫描 probe request）。
 * 逐字段二分（裸 TX 微基准，同一链路、1436 字节传输）：
 *
 *   mask=0x00（全管理，基线） 6.67 Mbit/s
 *   mask=0x02 仅 +0x08=0x0000   → 178.88 Mbit/s   ← 关键字段（我们发 0xff00）
 *   其余单字段（0x01/0x04/0x08/0x10/0x20）→ 5.0~6.5 Mbit/s，**无效果**
 *   mask=0x3F（全部）           → 195~243 Mbit/s
 *
 * 位定义（仅作用于**加密单播数据帧**）：
 *   bit0 +0x06=0x0000   bit1 +0x08=0x0000   bit2 +0x0a=0x1312
 *   bit3 +0x0c=0x0040   bit4 +0x14=0x0200   bit5 +0x1a=0x7540
 *
 * 之所以做成位掩码而不是直接改常量：把全部字段都换成数据模板时 **DHCP 拿不到地址**
 * （关联与四次握手正常）。需要逐字段找出"提速必需"与"断网元凶"分别是哪个。
 */
static int tx_desc_mask;
module_param(tx_desc_mask, int, 0644);
MODULE_PARM_DESC(tx_desc_mask, "experimental: bitmask of vendor DATA-frame descriptor fields to apply to protected unicast data frames (0 = off; bit1 = +0x08, the one that matters)");

/*
 * 实验开关（默认 0 = 与 0.3.x 行为完全一致）：**固件站点会话**。
 *
 * 背景（re/REPORT_TX_SESSION.md）：厂商 Windows 驱动在关联完成后发
 * MM_STA_ADD_REQ(0x0A, 48 字节) 让固件分配 sta_idx，之后数据帧描述符 +0x09 带上它，
 * 同一链路上传 95.3 Mbit/s；本驱动从不发这条消息（所有帧 staid=0xff）⇒ 上传 3.7 Mbit/s。
 *
 * 现状（2026-09-27 实测，第 4~5 轮）：**已打通**。48 字节模板取自
 * `re/REPORT_STA_ADD_STRUCT.md`（头部 +0x00~+0x13 是速率配置、尾部 +0x14~+0x2F 是
 * A-MPDU 上限/flags/站点键），实机 CFM 返回 `status=0`、`sta_idx=0/1/2`，
 * 链路/DHCP/上网不受影响、0 条 WARNING。
 *
 * 但**吞吐没变**：A/B（同一 AP）上传 0.38 vs 0.33 Mbit/s、下载 3.99 vs 3.71，都在噪声内
 * ⇒ 只让描述符带上 `sta_idx` **不足以**打开厂商的 95 Mbit/s 快路径（见 CHANGELOG [Unreleased]）。
 * 因此默认仍为 0；聚合已经实装（`ampdu_en`，见下面的注释与 CHANGELOG `[Unreleased]`），
 * 剩下的卡点是 **A-MPDU 的推送/封装语义**。不要再在这里盲试载荷常量：
 * 改 `+0x00 format` 之类会直接被固件在 `rc.c:676` 断言（实测），BA/STA 的新消息一律
 * "先打日志、再发一条、一次一个变量"。
 */
static int sta_add_en;
module_param(sta_add_en, int, 0644);
MODULE_PARM_DESC(sta_add_en, "experimental: send MM_STA_ADD_REQ after association and put the sta_idx into data descriptors (0=off; firmware accepts the payload but throughput is unchanged - see re/REPORT_STA_ADD_STRUCT.md and CHANGELOG [Unreleased])");

/*
 * 实验开关（默认 0 = 与 0.3.x 行为完全一致）：**A-MPDU 聚合**。
 *
 * 背景：`re/REPORT_TX_SESSION.md` 指出厂商快路径 = ①固件站点会话 ②描述符带 sta_idx
 * ③聚合（`MM_BA_ADD_REQ` 由 mac80211 的 ampdu_action 触发）。前两步已在
 * `sta_add_en` 里打通，但只带 sta_idx **没有提升吞吐**（见 CHANGELOG）⇒ 本轮补第三步。
 *
 * 打开后会做三件事（都在注册/加载期生效，运行期改无效）：
 *   ① `ieee80211_hw_set(hw, AMPDU_AGGREGATION)` + `hw->max_tx_aggregation_subframes`
 *      —— 此后 **mac80211 自己组 A-MPDU**（见 mac80211.h 对该字段的说明），
 *      驱动从 `ieee80211_tx_dequeue()` 拿到的是**聚合后的 skb**；
 *   ② TX 缓冲从 2 KB 提到 `ZT_AGG_BUF_SIZE`（否则聚合帧会被我们自己的长度检查丢掉）；
 *   ③ 实现 `.ampdu_action`：TX_START 时发 `MM_BA_ADD_REQ`(0x28) 等 CFM 0x29，
 *      成功再回调 `ieee80211_start_tx_ba_cb_irqsafe()`；停止时发 `MM_BA_DEL_REQ`(0x2A)。
 *
 * 用户态已经把两条消息都验证过了（`MM_BA_ADD_CFM` 返回 status=0；`MM_BA_DEL_CFM` 返回 5
 * 但不断言）。**实测结果（2026-09-27 第 6~8 轮，实机）**：链路能真的建立起来 ——
 *   `ampdu_action TX_START: tid=0 ssn=0 buf_size=0` → `BA_ADD: type=0 sta=0 tid=0 A=64 B=0`
 *   → `BA_ADD_CFM: status=0` → `AMPDU operational (tid=0 bufsz=8)`；
 * 前提是**必须同时 `ht_cap_enable=1`**（否则 `ieee80211_aggr_check()` 直接返回，`docs/04` D16），
 * 且 `A`(bufsz) 不能为 0（mac80211 在 TX_START 传 0，照发会让固件断言并 USB 掉线，`docs/04` D15）。
 *
 * **但仍不可用（2026-09-30 前）**：一压流量就崩 —— 网关 ping 丢包 70%、`tools/tx_blast.py`
 * 中位数 3.77 Mbit/s（无聚合时约 12）、期间 2 次 USB 掉线重连。
 *
 * **2026-09-30 定案并修复**：真因不是"mac80211 给了聚合 skb"（它一次只给一个 MPDU），
 * 而是**驱动从不把多个 MPDU 拼进同一次 bulk 传输**。厂商上传抓包显示 EP5 的传输长度是
 * 1608 / 3200 / 4808（= 1/2/3 个自描述单元，非末尾单元固定 1608 字节槽位）⇒
 * A-MPDU 由主机在传输层组，固件只按单元逐个解析。实现见 `zt_tx_agg_send()`。
 */
static int ampdu_en;
module_param(ampdu_en, int, 0644);
MODULE_PARM_DESC(ampdu_en, "experimental: enable A-MPDU aggregation (AMPDU_AGGREGATION + ampdu_action + MM_BA_ADD_REQ, 16KB TX buffer, driver-side multi-MPDU packing per USB transfer); needs ht_cap_enable=1; 0=off (default)");
static uint agg_stride = ZT_AGG_STRIDE;
module_param(agg_stride, uint, 0644);
MODULE_PARM_DESC(agg_stride, "experimental: fixed per-unit slot for non-final units when packing several MPDUs into one bulk transfer (vendor-observed 1608; 0=compact packing)");
static int agg_dump;
module_param(agg_dump, int, 0644);
MODULE_PARM_DESC(agg_dump, "experimental: hexdump the first N aggregated transfers (0=off) to compare byte-for-byte with the vendor capture");
static int vendor_tmpl;
module_param(vendor_tmpl, int, 0644);
MODULE_PARM_DESC(vendor_tmpl, "experimental: use the vendor's exact DATA-frame descriptor template (a +0x02..+0x1b, fa-then-fe seq) and its i16/flen field, instead of the management-frame template; 0=off");
static int agg_block;
module_param(agg_block, int, 0644);
MODULE_PARM_DESC(agg_block, "experimental: keep the BA session (ampdu_en=1) but BLOCK multi-MPDU packing, so data frames still go one-per-bulk (isolates 'BA session' from 'aggregated transfer' as the crash cause); 0=off");
/*
 * 默认只允许 **1 次** 聚合传输：即 `ampdu_en=1` 时把第一个数据帧按聚合格式发出、
 * 之后立即退回单帧路径。原因：多单元打包目前**必崩**（2026-09-30，见
 * `re/AMPDU_PUSH_STATUS.md`），默认放开会让 `ampdu_en=1` 直接把设备打成挂死态。
 * 要继续做实验就显式调大（`agg_max_xfers=20`）—— 崩了也只会崩一次，不会 crash-loop。
 */
static int agg_max_xfers = 1;
module_param(agg_max_xfers, int, 0644);
MODULE_PARM_DESC(agg_max_xfers, "experimental: stop packing after N aggregated transfers and fall back to one-MPDU-per-bulk (0=unlimited, default 1 = never actually multi-pack); safety valve so a broken packing cannot crash-loop the device");
static int tx_dump;
module_param(tx_dump, int, 0644);
MODULE_PARM_DESC(tx_dump, "experimental: log the first N TX frames handed to the device (len/fc/prot/mcast/eligibility), to see what mac80211 is actually giving us");
/*
 * 诊断：打印两条 TX 路径的帧数与聚合计数（`txpath=` 行）。
 * 为什么需要：2026-10-01 实测 13k 帧的真实流量下 `tx(agg)` 一次都没出现 ——
 * 说明数据帧主要走**传统 .tx 路径**（zt_mac_tx → z->txq），而聚合分支在 TXQ 路径里。
 * 光看 tx_packets 分不出这件事，必须有这条计数。
 */
static int tx_diag;
module_param(tx_diag, int, 0644);
MODULE_PARM_DESC(tx_diag, "experimental: expose how many frames took the legacy .tx path vs the TXQ path (write 1 to start logging on every wake-up)");
/*
 * 只把 >= agg_min_len 的帧纳入聚合（默认 1500 = 满尺寸数据帧）。
 * 依据：厂商抓包里**每一个**聚合单元都是 1578 字节（MPDU 1550），最小的聚合传输是
 * 1888 = 1608 + 280；而我们第一枪聚合的是 394 + 138 字节两个小帧 —— 小帧进聚合是
 * 厂商样本里从未出现过的形态，先用"只聚合大帧"把它排除掉。
 */
static int agg_min_len = 1500;
module_param(agg_min_len, int, 0644);
MODULE_PARM_DESC(agg_min_len, "experimental: minimum MPDU length for aggregation (default 1500); smaller frames keep the single-frame path");

struct zt_dev {
	struct usb_device	*udev;
	struct usb_interface	*intf;
	u8			ep_out;
	u8			ep_in;
	u8			*tx;
	u8			*pl;
	u8			*rx;
	u8			*mac;

	struct urb		*rx_urb;
	u8			*rx_buf;
	struct kfifo		rx_fifo;
	wait_queue_head_t	rx_wait;
	bool			rx_running;
	bool			alive;		/* 拔出/卸载后置 0，停止一切 USB IO */
	bool			mac_started;	/* hw 是否已 start（drv_start/drv_stop），RX 注入的门控 */
	struct completion	rx_done;	/* 在途 rx_urb 回收信号（D9） */

	/*
	 * 实验（对应 re/REPORT_TX_RE.md 的 C1）：EP2-IN 的 6 字节通知通道。
	 * 厂商驱动在每一次 EP5-OUT 前后都在读它；本驱动此前完全没有接管。
	 * 假设：固件要等主机取走通知才放行下一帧 —— 若是这样，仅"读起来"就能提高 TX 吞吐
	 * （实测厂商 Windows 驱动上传 95.3 Mbit/s vs 本驱动 3.65 Mbit/s）。
	 */
	u8			ep_ntf;
	bool			ntf_is_int;
	u8			ntf_interval;
	struct urb		*ntf_urb;
	u8			*ntf_buf;
	bool			ntf_running;
	struct completion	ntf_done;
	unsigned long		ntf_count;
	unsigned long		ntf_bytes;
	unsigned long		ntf_log_left;

	struct delayed_work	hb_work;
	unsigned long		last_hb;
	u32			hb_seq;

	/* M3.2 扫描：hw_scan 在工作队列里逐个信道切换，期间把 RX 帧交给 mac80211 */
	struct work_struct	scan_work;
	u16			scan_freqs[ZT_MAX_SCAN_CH];
	int			scan_nfreqs;
	u16			scan_freq;
	bool			scan_active;
	bool			scan_aborted;
	bool			scan_running;

	/* M3.4 TX：EP5-OUT + 28 字节描述符 */
	u8			ep_tx;
	u8			*txd;
	u16			tx_seq;
	unsigned long		tx_frames;
	/* 实验：固件站点会话（见 sta_add_en / re/REPORT_STA_ADD_LAYOUT.md） */
	u8			sta_idx;	/* MM_STA_ADD_CFM 分配的站点索引 */
	bool			sta_valid;
	u16			sta_aid;
	u8			sta_bssid[6];
	unsigned long		sta_add_ok;
	unsigned long		sta_add_fail;
	/* 实验：A-MPDU 聚合（见 ampdu_en） */
	u16			tx_buf_size;	/* 运行期 TX 缓冲长度（聚合时更大） */
	u8			ba_tid;
	bool			ba_valid;
	unsigned long		ba_add_ok;
	unsigned long		ba_add_fail;
	unsigned long		ba_rx_start;
	unsigned long		ba_rx_stop;
	unsigned long		tx_agg_xfers;	/* 聚合：一次传输里装多个 MPDU 的次数 */
	unsigned long		tx_agg_mpdus;	/* 聚合：被聚合发送的 MPDU 总数 */
	unsigned long		tx_legacy_frames;	/* 走传统 .tx 路径的帧数（实测主路径） */
	unsigned long		tx_txq_frames;	/* 走 TXQ（wake_tx_queue）路径的帧数 */
	unsigned long		tx_agg_attempts;/* 聚合：尝试打包的次数（受 agg_max_xfers 限制） */
	unsigned long		tx_agg_multi;	/* 聚合：真正装了 >1 个单元的传输次数 */
	unsigned long		tx_probes;
	unsigned long		scan_probe_skip;	/* DFS/NO_IR 信道跳过的主动探测数 */
	unsigned long		scan_ch_5g;		/* 本次扫描实际切到的 5G 信道数 */
	unsigned long		rx_frames_5g;		/* 描述符频率 > 2500 的 RX 帧数 */
	unsigned long		rx_beacons;
	unsigned long		rx_probe_resp;
	/* 观测：扫描期间收到的帧类型分布 + 未识别的 IPC 消息 id */
	unsigned long		rx_type_cnt[5];	/* 0:0x0000 1:0x0004 2:0x0100 3:0x0300 4:其它 */
	u16			last_other_type;
	u16			unk_ids[4];
	unsigned long		unk_cnt[4];
	/* 观测：设备发来的所有 IPC 消息 id（对比厂商抓包，找缺失的通知） */
	u16			seen_ids[16];
	unsigned long		seen_cnt[16];
	/* 观测：RX 状态量的来源（描述符真值 vs 兜底），扫描结束时打印 */
	unsigned long		rx_freq_desc;	/* 用描述符 +0x2A 频率的帧数 */
	unsigned long		rx_freq_fb;	/* 描述符频率不可用 → 退回扫描信道 */
	unsigned long		rx_freq_ne_scan;/* 描述符频率 != 当前扫描信道 */
	unsigned long		rx_rssi_fb;	/* RSSI 越界 → 退回 -50 的帧数 */

	struct miscdevice	misc;
	bool			misc_ok;

	struct ieee80211_hw	*hw;
	unsigned long		tx_dropped;
	/* M3.3：mac80211 的发送队列 + 提交工作。.tx 可能在软中断上下文被调用，
	 * 而 usb_bulk_msg() 会睡眠，所以入队与提交必须分开。 */
	struct sk_buff_head	txq;
	struct work_struct	tx_work;
	unsigned long		tx_path_frames;
	/* v0.3 阶段 B：TX status 上报计数（观测用） */
	unsigned long		tx_status_reports;
	unsigned long		tx_status_ack;
	bool			tx_status_last_ack;
	/* wake_tx_queue 只登记"哪个队列有活"，提交在 tx_work 里做 */
	struct ieee80211_txq	*txq_pend[ZT_MAX_PEND_TXQ];
	int			txq_n;
	spinlock_t		txq_lock;
	unsigned long		txq_overflow;

	struct mutex		lock;
};

static void zt_note_msg(struct zt_dev *z, const u8 *frame, int len);
static int zt_scan_setup(struct zt_dev *z);

/* ------------------------------------------------------------------ 閸╄櫣顢?*/

static u16 zt_xor16(const u8 *p, size_t len)
{
	u16 cs = 0;
	size_t i;

	for (i = 0; i + 1 < len; i += 2)
		cs ^= (u16)p[i] | ((u16)p[i + 1] << 8);
	return cs;
}

static int zt_send(struct zt_dev *z, u16 type, const u8 *payload, u16 hlen)
{
	int ret, sent = 0;
	u16 total = 8 + hlen;

	if (!READ_ONCE(z->alive))
		return -ENODEV;
	if (total > MAX_FRAME)
		return -EINVAL;

	memcpy(z->tx, "WLAN", 4);
	z->tx[4] = hlen & 0xff;
	z->tx[5] = hlen >> 8;
	z->tx[6] = type & 0xff;
	z->tx[7] = type >> 8;
	if (hlen)
		memcpy(z->tx + 8, payload, hlen);

	ret = usb_bulk_msg(z->udev, usb_sndbulkpipe(z->udev, z->ep_out),
			   z->tx, total, &sent, 2000);
	if (ret)
		dev_err(&z->intf->dev, "bulk OUT failed (%d)\n", ret);
	return ret;
}

static int zt_recv(struct zt_dev *z, u16 *type, int *len, int timeout_ms)
{
	int ret, got = 0;

	if (!READ_ONCE(z->alive))
		return -ENODEV;
	ret = usb_bulk_msg(z->udev, usb_rcvbulkpipe(z->udev, z->ep_in),
			   z->rx, ZT_RX_BUF_SIZE, &got, timeout_ms);
	if (ret)
		return ret;
	if (got < 8 || memcmp(z->rx, "WLAN", 4))
		return -EPROTO;
	*len = got;
	*type = (u16)z->rx[6] | ((u16)z->rx[7] << 8);
	if (*type == T_IPC)
		zt_note_msg(z, z->rx, got);
	return 0;
}

/* ------------------------------------------------------------------ 韫囧啳鐑?*/

static void zt_heartbeat(struct zt_dev *z)
{
	struct timespec64 ts;
	struct tm tm;
	char stamp[24];
	u8 pl[8 + 4 + 24];
	u16 slen;

	ktime_get_real_ts64(&ts);
	time64_to_tm(ts.tv_sec, 0, &tm);
	scnprintf(stamp, sizeof(stamp), "%04d-%02d-%02d_%02d-%02d-%02d",
		  (int)(tm.tm_year + 1900), (int)(tm.tm_mon + 1), (int)tm.tm_mday,
		  (int)tm.tm_hour, (int)tm.tm_min, (int)tm.tm_sec);
	slen = strlen(stamp) + 1;

	put_unaligned_le16(0x05C2, pl + 0);
	put_unaligned_le16(0x05C2 >> 10, pl + 2);	/* 心跳的 dest 也是 1（抓包实测） */
	put_unaligned_le16(100, pl + 4);
	put_unaligned_le16(4 + slen, pl + 6);
	put_unaligned_le32(z->hb_seq++, pl + 8);	/* 厂商是递增序号 */
	memcpy(pl + 12, stamp, slen);

	if (!zt_send(z, T_IPC, pl, 8 + 4 + slen))
		z->last_hb = jiffies;
}

static void zt_hb_work(struct work_struct *w)
{
	struct zt_dev *z = container_of(to_delayed_work(w), struct zt_dev, hb_work);

	if (!READ_ONCE(z->alive))
		return;
	mutex_lock(&z->lock);
	if (READ_ONCE(z->alive) && z->rx_running)
		zt_heartbeat(z);
	mutex_unlock(&z->lock);
	if (READ_ONCE(z->alive))
		schedule_delayed_work(&z->hb_work, msecs_to_jiffies(HB_INTERVAL_MS));
}

/* ------------------------------------------------------------------ IPC 閸涙垝鎶?*/

static int zt_cmd(struct zt_dev *z, u16 id, const u8 *params, u16 plen,
		  int want_cfm, int timeout_ms, u8 *resp, u16 *resp_len)
{
	u8 *msg = z->pl;
	unsigned long end;
	int ret;

	put_unaligned_le16(id, msg + 0);
	/*
	 * dest_id 必须等于消息编号里的 task 字段（id >> 10）。
	 * 抓包实测：MM 段（0x00xx）dest=0，厂商段 0x050e 与心跳 0x05c2 都是 dest=1。
	 * 早期实现把 dest 写死成 0，导致 0x050e 那 4 条配置消息投递给了错误的固件任务。
	 */
	put_unaligned_le16(id >> 10, msg + 2);
	put_unaligned_le16(100, msg + 4);
	put_unaligned_le16(plen, msg + 6);
	if (plen && params)
		memcpy(msg + 8, params, plen);

	ret = zt_send(z, T_IPC, msg, 8 + plen);
	if (ret)
		return ret;
	if (want_cfm < 0)
		return 0;

	end = jiffies + msecs_to_jiffies(timeout_ms);
	while (time_before(jiffies, end)) {
		u16 type = 0;
		int len = 0;

		if (time_after(jiffies, z->last_hb + msecs_to_jiffies(HB_INTERVAL_MS)))
			zt_heartbeat(z);

		if (zt_recv(z, &type, &len, 100))
			continue;
		if (type != T_IPC || len < 8)
			continue;
		if (get_unaligned_le16(z->rx + 8) != (u16)want_cfm)
			continue;
		if (resp && resp_len) {
			u16 rl = (len >= 16) ? get_unaligned_le16(z->rx + 14) : 0;

			rl = min_t(u16, rl, (u16)(len - 16));
			if (rl)
				memcpy(resp, z->rx + 16, rl);
			*resp_len = rl;
		}
		return 0;
	}
	dev_warn(&z->intf->dev, "timeout waiting for cfm %#06x\n", want_cfm);
	return -ETIMEDOUT;
}

/*
 * M3.2：扫描期间 RX URB 一直在跑，不能再走 zt_recv() 的同步读（会和 URB 抢同一个
 * IN 端点）。这里改为从 URB 填充的 kfifo 里取帧，语义与 zt_recv 相同。
 */
static int zt_kfifo_recv(struct zt_dev *z, u16 *type, int *len, int timeout_ms)
{
	unsigned long end = jiffies + msecs_to_jiffies(timeout_ms);

	while (time_before(jiffies, end)) {
		u8 hdr[2];
		u16 flen;

		if (kfifo_len(&z->rx_fifo) < 2) {
			msleep(2);
			continue;
		}
		if (kfifo_out_peek(&z->rx_fifo, hdr, 2) != 2)
			continue;
		flen = (u16)hdr[0] | ((u16)hdr[1] << 8);
		if (flen < 8 || flen > ZT_RX_MAX_FRAME) {
			unsigned int d = kfifo_out(&z->rx_fifo, hdr, 2);	/* 脏数据，丢弃 */

			(void)d;
			continue;
		}
		if (kfifo_len(&z->rx_fifo) < 2 + flen) {
			msleep(2);
			continue;
		}
		if (kfifo_out(&z->rx_fifo, hdr, 2) != 2)
			continue;
		if (kfifo_out(&z->rx_fifo, z->rx, flen) != flen)
			continue;
		*len = flen;
		*type = get_unaligned_le16(z->rx + 6);
		return 0;
	}
	return -ETIMEDOUT;
}

static int zt_cmd_fifo(struct zt_dev *z, u16 id, const u8 *params, u16 plen,
		       int want_cfm, int timeout_ms)
{
	u8 *msg = z->pl;
	unsigned long end;
	int ret;

	put_unaligned_le16(id, msg + 0);
	put_unaligned_le16(id >> 10, msg + 2);	/* dest = task 字段，见 zt_cmd 注释 */
	put_unaligned_le16(100, msg + 4);
	put_unaligned_le16(plen, msg + 6);
	if (plen && params)
		memcpy(msg + 8, params, plen);

	ret = zt_send(z, T_IPC, msg, 8 + plen);
	if (ret)
		return ret;
	if (want_cfm < 0)
		return 0;

	end = jiffies + msecs_to_jiffies(timeout_ms);
	while (time_before(jiffies, end)) {
		u16 type = 0;
		int len = 0;

		if (time_after(jiffies, z->last_hb + msecs_to_jiffies(HB_INTERVAL_MS)))
			zt_heartbeat(z);

		if (zt_kfifo_recv(z, &type, &len, 50))
			continue;
		if (type != T_IPC || len < 8)
			continue;
		{
			u16 id = get_unaligned_le16(z->rx + 8);
			int k, slot = -1;

			if (id == (u16)want_cfm)
				return 0;
			/* 观测：记录未预期的 IPC 消息 id（TX 确认/错误可能在里面） */
			for (k = 0; k < 4; k++) {
				if (z->unk_ids[k] == id) {
					z->unk_cnt[k]++;
					slot = -2;
					break;
				}
				if (slot < 0 && z->unk_ids[k] == 0)
					slot = k;
			}
			if (slot >= 0) {
				z->unk_ids[slot] = id;
				z->unk_cnt[slot] = 1;
			}
		}
	}
	dev_warn(&z->intf->dev, "scan: timeout waiting for cfm %#06x\n", want_cfm);
	return -ETIMEDOUT;
}

/*
 * 与 zt_cmd_fifo() 相同（运行时 RX URB 在跑，只能从 kfifo 取帧），区别是**把 CFM 的
 * 载荷拷回 resp** —— STA_ADD 的 3 字节结果（sta_idx/pm_state/status）就在这里。
 * 帧布局：z->rx = WLAN 头(8) + lmac_msg{id@8, dest@10, src@12, param_len@14} + 载荷@16。
 */
static int zt_cmd_fifo_resp(struct zt_dev *z, u16 id, const u8 *params, u16 plen,
			    int want_cfm, int timeout_ms, u8 *resp, u16 *resp_len)
{
	u8 *msg = z->pl;
	unsigned long end;
	int ret;

	put_unaligned_le16(id, msg + 0);
	put_unaligned_le16(id >> 10, msg + 2);	/* dest = task 字段，见 zt_cmd 注释 */
	put_unaligned_le16(100, msg + 4);
	put_unaligned_le16(plen, msg + 6);
	if (plen && params)
		memcpy(msg + 8, params, plen);

	ret = zt_send(z, T_IPC, msg, 8 + plen);
	if (ret)
		return ret;
	if (want_cfm < 0)
		return 0;

	end = jiffies + msecs_to_jiffies(timeout_ms);
	while (time_before(jiffies, end)) {
		u16 type = 0;
		int len = 0;

		if (time_after(jiffies, z->last_hb + msecs_to_jiffies(HB_INTERVAL_MS)))
			zt_heartbeat(z);

		if (zt_kfifo_recv(z, &type, &len, 50))
			continue;
		if (type != T_IPC || len < 8)
			continue;
		if (get_unaligned_le16(z->rx + 8) != (u16)want_cfm)
			continue;
		if (resp && resp_len) {
			u16 rl = (len >= 16) ? get_unaligned_le16(z->rx + 14) : 0;

			rl = min_t(u16, rl, (u16)(len - 16));
			if (rl)
				memcpy(resp, z->rx + 16, rl);
			*resp_len = rl;
		}
		return 0;
	}
	dev_warn(&z->intf->dev, "sta: timeout waiting for cfm %#06x\n", want_cfm);
	return -ETIMEDOUT;
}

/*
 * MM_STA_ADD_REQ(0x0A) 的 48 字节载荷模板。
 *
 * 偏移与语义来自 `re/REPORT_STA_ADD_STRUCT.md`（指令级逆向：RC 字段在**头部**
 * +0x00~+0x13，尾部 +0x14~+0x2F 是 A-MPDU 上限/标志/站点键），
 * 数值来自 2026-09-27 的**用户态实测**（`tools/sta_add_probe.py --layout rc --format 0`）：
 * 固件回了 `MM_STA_ADD_CFM(0x0B)` = `{sta_idx, pm_state, status}` 且 status=0，
 * 链路与上网不受影响（复现两次，sta_idx=0 / 1）。
 *
 *   +0x00 format=0（**关键**：早期发 2 会被固件在 rc.c:676 断言）
 *   +0x01 mcs_max=7   +0x02 r_idx_min=0   +0x03 r_idx_max=7
 *   +0x04 rate_map[4]=ff 00 00 00         +0x08 rate_map_l(u16)=0x00ff
 *   +0x0a bw_max=0    +0x0b no_ss=1       +0x0c short_gi=0
 *   +0x14 flags=0     +0x18 he_max_ampdu=0x000FFFFF
 *   +0x1c vht_max_ampdu=0x1FFF            +0x24 ht_max_ampdu=0x1FFF
 *   +0x2a AID=0（逆向指向 params+0x1c=AID；实测填 0 即可被接受，故暂不填 aid）
 *   +0x2c min_ampdu=1 +0x2d 站点键=0
 *
 * 仍未证实的只有"哪些字段会被固件严格校验"：本模板是**实测可用**的那一份，
 * 改任何一个字节都要按"一次一个变量 + 看 0x0B / 0x0600"重新验证。
 */
#define ZT_STA_ADD_LEN	48
static const u8 zt_sta_add_tmpl[ZT_STA_ADD_LEN] = {
	0x00, 0x07, 0x00, 0x07, 0xff, 0x00, 0x00, 0x00,
	0xff, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xff, 0xff, 0x0f, 0x00, 0xff, 0x1f, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0x1f, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
};

/*
 * 关联后发 MM_STA_ADD_REQ，并记下 CFM 返回的 sta_idx 供描述符使用。
 * 约定：① 调用者处于进程上下文（bss_info_changed 是），本函数会睡眠等 CFM；
 *       ② 心跳在本函数内部维持（与 zt_cmd_fifo 同）；
 *       ③ 与并发扫描的 zt_cmd_fifo 争抢 kfifo 的风险存在（实验开关，默认关）。
 */
static void zt_sta_add(struct zt_dev *z, u16 aid, const u8 *bssid)
{
	u8 resp[8];
	u16 rlen = 0;

	z->sta_aid = aid;
	memcpy(z->sta_bssid, bssid, sizeof(z->sta_bssid));

	if (zt_cmd_fifo_resp(z, 0x000a, zt_sta_add_tmpl, ZT_STA_ADD_LEN,
			     0x000b, 1000, resp, &rlen)) {
		z->sta_add_fail++;
		dev_warn(&z->intf->dev,
			 "STA_ADD: no CFM (see re/REPORT_STA_ADD_LAYOUT.md)\n");
		return;
	}
	/*
	 * 厂商侧解包顺序（re/REPORT_TX_SESSION.md §1）：out[0]=cfm[2]=status、
	 * out[1]=cfm[0]=sta_idx、out[2]=cfm[1]=pm_state —— 仍待抓包复核。
	 */
	if (rlen >= 3 && resp[2] == 0) {
		z->sta_idx = resp[0];
		z->sta_valid = true;
		z->sta_add_ok++;
		dev_info(&z->intf->dev,
			 "STA_ADD_CFM: status=0 sta_idx=%u pm_state=%u (aid=%u)\n",
			 z->sta_idx, resp[1], aid);
		return;
	}
	z->sta_add_fail++;
	dev_warn(&z->intf->dev, "STA_ADD_CFM: unexpected result (len=%u status=%u)\n",
		 rlen, rlen >= 3 ? resp[2] : 0xff);
}

/* 断开时只清本地状态：MM_STA_DEL_REQ(0x0C) 的参数布局尚未证实，不猜着发。 */
static void zt_sta_del(struct zt_dev *z)
{
	if (!z->sta_valid)
		return;
	dev_info(&z->intf->dev,
		 "STA_DEL: clear local sta_idx=%u (no IPC sent; layout unconfirmed)\n",
		 z->sta_idx);
	z->sta_valid = false;
	z->sta_idx = 0;
}

/*
 * A-MPDU 聚合：向固件登记 / 注销 BA 会话（`MM_BA_ADD_REQ` 0x28 / `MM_BA_DEL_REQ` 0x2A）。
 * 参数布局【证据，re/REPORT_TX_SESSION.md §1】：
 *   0x28：8 字节 {u8 type; u8 sta_idx; u8 tid; u8 pad; u16 A; u16 B}，等 CFM 0x29
 *   0x2A：3 字节 {u8 type; u8 sta_idx; u8 tid}，等 CFM 0x2B
 * 用户态实测（2026-09-27）：type=0、sta_idx=会话值、tid=0、A=64、B=0 时
 * `MM_BA_ADD_CFM` 返回 status=0（不断言）；同参数发 0x2A 返回 5，语义仍待确认 ——
 * 这里只发不解释。
 */
static int zt_ba_add(struct zt_dev *z, u8 tid, u16 bufsz, u16 ssn)
{
	u8 params[8];
	u8 resp[4];
	u16 rlen = 0;

	if (!z->sta_valid)
		return -EINVAL;
	/* type：BA_AGMT_TX/RX 的取值未定；0 实测被接受 */
	params[0] = 0;
	params[1] = z->sta_idx;
	params[2] = tid;
	params[3] = 0;
	/*
	 * mac80211 在 TX_START 时给的 buf_size 可能是 0（那是"对端窗口"语义），
	 * 而用户态实测被固件接受的是 A=64 ⇒ 这里做下限保护。
	 * 2026-09-27 定案：A=0 就是"开聚合后关联约 84 ms 设备 USB 掉线"的真因
	 * （固件断言，docs/04 D15），不是聚合实现或 TX 路径的问题；两个值都打进 dmesg，
	 * 便于和"设备是否立刻掉线"对拍。
	 */
	if (bufsz < 64)
		bufsz = 64;
	put_unaligned_le16(bufsz, params + 4);	/* A：候选 bufsz */
	put_unaligned_le16(ssn, params + 6);	/* B：候选 ssn */
	dev_info(&z->intf->dev, "BA_ADD: type=0 sta=%u tid=%u A=%u B=%u\n",
		 z->sta_idx, tid, bufsz, ssn);

	if (zt_cmd_fifo_resp(z, 0x0028, params, sizeof(params), 0x0029, 1000, resp, &rlen)) {
		z->ba_add_fail++;
		dev_warn(&z->intf->dev, "BA_ADD: no CFM (tid=%u)\n", tid);
		return -EIO;
	}
	if (rlen >= 1 && resp[0] != 0) {
		z->ba_add_fail++;
		dev_warn(&z->intf->dev, "BA_ADD_CFM: status=%u (tid=%u)\n", resp[0], tid);
		return -EIO;
	}
	z->ba_valid = true;
	z->ba_tid = tid;
	z->ba_add_ok++;
	dev_info(&z->intf->dev,
		 "BA_ADD_CFM: status=0 sta_idx=%u tid=%u (bufsz=%u ssn=%u)\n",
		 z->sta_idx, tid, bufsz, ssn);
	return 0;
}

static void zt_ba_del(struct zt_dev *z, u8 tid)
{
	u8 params[3];
	u8 resp[4];
	u16 rlen = 0;

	if (!z->ba_valid)
		return;
	params[0] = 0;
	params[1] = z->sta_idx;
	params[2] = tid;
	if (zt_cmd_fifo_resp(z, 0x002a, params, sizeof(params), 0x002b, 1000, resp, &rlen))
		dev_warn(&z->intf->dev, "BA_DEL: no CFM (tid=%u)\n", tid);
	else
		dev_info(&z->intf->dev, "BA_DEL_CFM: status=%u (tid=%u)\n",
			 rlen >= 1 ? resp[0] : 0xff, tid);
	z->ba_valid = false;
}
/* `zt_mac_ampdu_action()` 定义在 `zt_from_hw()` 之后（ops 结构体前）。 */

static int zt_run_init(struct zt_dev *z)
{
	static const u8 zeros13[13];
	static const u8 five_e[4][12] = {
		{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x01, 0x07 },
		{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x01, 0x02 },
		{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x5e, 0x00, 0x02, 0x01, 0x02 },
		{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x2f, 0x00, 0x02, 0x01, 0x01 },
	};
	u8 addif[8] = { 0 };
	u8 chan[12];
	u8 resp[64];
	u16 rlen = 0;
	u8 one = 1, zero = 0, slottime = 0x14;
	int i, ret;

	dev_info(&z->intf->dev, "=== IPC init sequence ===\n");

	/* MM_RESET閿涙艾娴愭禒璺哄灠閸氼垰濮╅弮璺哄讲閼冲€熺箷濞屸€冲櫙婢跺洤銈介幒銉︽暪閸涙垝鎶ら敍宀勫櫢鐠?3 濞?*/
	for (i = 0; i < 3; i++) {
		if (!zt_cmd(z, 0x0000, NULL, 0, 0x0001, 3000, NULL, NULL))
			break;
		dev_warn(&z->intf->dev, "MM_RESET no cfm, retry %d/3\n", i + 1);
		msleep(200);
	}
	if (i == 3) {
		dev_err(&z->intf->dev, "firmware not answering MM_RESET\n");
		return -ETIMEDOUT;
	}
	ret = zt_cmd(z, 0x0004, NULL, 0, 0x0005, 3000, NULL, NULL);
	if (ret)
		return ret;

	zt_cmd(z, 0x0102, &zero, 1, 0x0103, 3000, NULL, NULL);
	zt_cmd(z, 0x010a, &zero, 1, 0x010b, 3000, NULL, NULL);
	zt_cmd(z, 0x0112, &zero, 1, -1, 300, NULL, NULL);

	rlen = 0;
	if (!zt_cmd(z, 0x0100, &zero, 1, 0x0101, 3000, resp, &rlen) && rlen >= 7) {
		/* 0x0101 的参数是 { u8 status; u8 mac[6]; }（抓包实测 plen=7）。
		 * 早期版本误把 resp[0..5] 当 MAC，导致 MAC 整体左移一字节、
		 * 末字节被 0 顶掉，vif 也就建在了错误的地址上。 */
		dev_info(&z->intf->dev, "fw 0x0101: status=%u mac=%pM\n",
			 resp[0], resp + 1);
		memcpy(z->mac, resp + 1, 6);
		dev_info(&z->intf->dev, "MAC = %pM\n", z->mac);
	} else {
		eth_random_addr(z->mac);
		dev_warn(&z->intf->dev, "no MAC from fw, using random %pM\n", z->mac);
	}

	dev_info(&z->intf->dev, "MM_START_REQ (rf init, waiting ~6.5s)\n");
	ret = zt_cmd(z, 0x0002, zeros13, sizeof(zeros13), 0x0003, 15000, NULL, NULL);
	if (ret) {
		dev_err(&z->intf->dev, "MM_START_CFM not received\n");
		return ret;
	}
	dev_info(&z->intf->dev, "MM_START_CFM received (firmware up)\n");

	for (i = 0; i < 4; i++)
		zt_cmd(z, 0x050E, five_e[i], sizeof(five_e[i]), -1, 0, NULL, NULL);
	msleep(50);

	zt_cmd(z, 0x0022, &one, 1, 0x0023, 3000, NULL, NULL);
	/* MM_ADD_IF_REQ 参数 = { u8 type; u8 mac[6]; u8 p2p; }，共 8 字节
	 * （抓包实测：type=0/STA + 本机 MAC + p2p=0）。
	 * 早期版本只发 7 字节且 MAC 错位，固件因此拒绝建 vif（TX 静默失效）。 */
	addif[0] = 0x00;			/* type: 0 = STA */
	memcpy(addif + 1, z->mac, 6);
	addif[7] = 0x00;			/* p2p */
	rlen = 0;
	if (zt_cmd(z, 0x0006, addif, sizeof(addif), 0x0007, 3000, resp, &rlen) ||
	    rlen < 2) {
		dev_warn(&z->intf->dev, "MM_ADD_IF 无 CFM\n");
	} else {
		/* mm_add_if_cfm = { u8 status; u8 inst_nbr; } */
		dev_info(&z->intf->dev, "MM_ADD_IF_CFM: status=%u inst_nbr=%u\n",
			 resp[0], resp[1]);
	}
	zt_cmd(z, 0x0020, &slottime, 1, 0x0021, 3000, NULL, NULL);

	memset(chan, 0, sizeof(chan));
	put_unaligned_le16(2412, chan + 2);
	put_unaligned_le16(2412, chan + 4);
	put_unaligned_le16(0x14, chan + 10);
	zt_cmd(z, 0x0010, chan, sizeof(chan), 0x0011, 3000, NULL, NULL);

	dev_info(&z->intf->dev, "=== init done, firmware running ===\n");
	return 0;
}

/* ------------------------------------------------------------------ 閸ヨ桨娆㈢憗鍛版祰 */

static int zt_wait_fw_ack(struct zt_dev *z, u8 want_sub, int timeout_ms)
{
	unsigned long end = jiffies + msecs_to_jiffies(timeout_ms);
	u16 type = 0;
	int l = 0;

	while (time_before(jiffies, end)) {
		if (zt_recv(z, &type, &l, 100))
			continue;
		if (type == T_FW_WRITE && l > 8 && z->rx[8] == want_sub)
			return 0;
	}
	return -ETIMEDOUT;
}

static int zt_write_blocks(struct zt_dev *z, u32 addr, const u8 *data, u32 len)
{
	u32 nblk = (len + ZT_BLOCK_SIZE - 1) / ZT_BLOCK_SIZE;
	u16 cs = zt_xor16(data, len);
	u32 i;
	int ret;

	for (i = 0; i < nblk; i++) {
		u32 off = i * ZT_BLOCK_SIZE;
		u32 blen = min_t(u32, ZT_BLOCK_SIZE, len - off);

		if (i == nblk - 1) {
			z->pl[0] = SUB_FW_LAST;
			put_unaligned_le32(addr, z->pl + 1);
			put_unaligned_le16(cs, z->pl + 5);
			put_unaligned_le32(off, z->pl + 7);
			put_unaligned_le16((u16)blen, z->pl + 11);
			memset(z->pl + 13, 0, 3);
			memcpy(z->pl + 16, data + off, blen);
			ret = zt_send(z, T_FW_WRITE_LAST, z->pl, 16 + blen);
			if (ret)
				return ret;
			ret = zt_wait_fw_ack(z, SUB_FW_ACK, 2000);
			if (ret) {
				dev_warn(&z->intf->dev, "no ack for %#010x\n", addr);
				return ret;
			}
		} else {
			z->pl[0] = SUB_FW_BLOCK;
			put_unaligned_le32(addr, z->pl + 1);
			put_unaligned_le32(off, z->pl + 5);
			put_unaligned_le16((u16)blen, z->pl + 9);
			z->pl[11] = 0;
			memcpy(z->pl + 12, data + off, blen);
			ret = zt_send(z, T_FW_WRITE, z->pl, 12 + blen);
			if (ret)
				return ret;
		}
	}
	dev_info(&z->intf->dev, "  wrote addr=%#010x len=%u (%u blocks) cs=%#06x\n",
		 addr, len, nblk, cs);
	return 0;
}

static int zt_boot(struct zt_dev *z)
{
	const struct firmware *fw, *st = NULL;
	u8 run[2 + 6 * 11];
	u32 run_len = 0;
	int ret, count, i;

	ret = request_firmware(&fw, "zt9612_fw.bin", &z->intf->dev);
	if (ret) {
		dev_err(&z->intf->dev, "request_firmware failed: %d\n", ret);
		return ret;
	}
	if (fw->size < 9 || get_unaligned_le16(fw->data) != ZT_MAGIC) {
		ret = -EINVAL;
		goto out;
	}
	count = fw->data[8];
	dev_info(&z->intf->dev, "firmware: pid=%#06x sections=%u size=%zu\n",
		 get_unaligned_le32(fw->data + 4), count, fw->size);
	if (count > 6 || fw->size < 9 + count * 19) {
		ret = -EINVAL;
		goto out;
	}

	z->pl[0] = SUB_FW_START;
	for (i = 0; i < 3; i++) {
		ret = zt_send(z, T_FW_START, z->pl, 1);
		if (ret)
			goto out;
		if (!zt_wait_fw_ack(z, SUB_FW_READY, 3000)) {
			dev_info(&z->intf->dev, "hello ack: yes (try %d)\n", i + 1);
			break;
		}
		dev_warn(&z->intf->dev, "hello ack timeout (try %d/3)\n", i + 1);
	}
	if (i == 3) {
		dev_err(&z->intf->dev, "device silent - wrong state? (needs fresh CD->wifi switch)\n");
		ret = -ETIMEDOUT;
		goto out;
	}

	for (i = 0; i < count; i++) {
		const u8 *e = fw->data + 9 + i * 19;
		u32 addr = get_unaligned_le32(e + 7);
		u32 len = get_unaligned_le32(e + 11);
		u32 foff = get_unaligned_le32(e + 15);

		if ((u64)foff + len > fw->size) {
			ret = -EINVAL;
			goto out;
		}
		ret = zt_write_blocks(z, addr, fw->data + foff, len);
		if (ret)
			goto out;
		put_unaligned_le32(addr, run + 2 + run_len * 11 + 0);
		put_unaligned_le32(len, run + 2 + run_len * 11 + 4);
		put_unaligned_le16(get_unaligned_le16(e + 1), run + 2 + run_len * 11 + 8);
		run[2 + run_len * 11 + 10] = e[0];
		run_len++;
	}

	ret = request_firmware(&st, "zt9612_settings.bin", &z->intf->dev);
	if (!ret) {
		ret = zt_write_blocks(z, SETTINGS_ADDR, st->data, st->size);
		release_firmware(st);
		if (ret)
			goto out;
	} else {
		dev_warn(&z->intf->dev, "no settings firmware, skipping\n");
		ret = 0;
	}

	run[0] = SUB_RUN;
	run[1] = (u8)run_len;
	dev_info(&z->intf->dev, "RUN (%u sections)\n", run_len);
	ret = zt_send(z, T_FW_WRITE, run, 2 + run_len * 11);
	if (ret)
		goto out;

	/* RUN 娑斿鎮楅崶杞版娴兼艾鍘涢崣鎴滅閺夆€虫儙閸斻劑鈧氨鐓￠敍鍧眣pe=0x0100, id=0x0200閿涘绱濈粵澶婄暊閸愬秴绱戞慨?IPC 閸掓繂顫愰崠鏍モ偓?	 * 閻劍鍩涢幀浣稿斧閸ㄥ鐤勫ù瀣剁窗娑撳秶鐡戦柅姘辩叀閻╁瓨甯撮崣?MM_RESET 娴兼碍鏁规稉宥呭煂 CFM閵?*/
	{
		unsigned long end = jiffies + msecs_to_jiffies(3000);
		u16 type = 0;
		int len = 0;

		while (time_before(jiffies, end)) {
			if (zt_recv(z, &type, &len, 100))
				continue;
			dev_info(&z->intf->dev, "boot notify: type=%#06x len=%d\n", type, len);
			break;
		}
		msleep(50);
	}
out:
	release_firmware(fw);
	return ret;
}

/* ------------------------------------------------------------------ 鏉╂劘顢戦弮鑸靛复閺€?*/

/* ------------------------------------------------------------------ TX 数据路径（M3.4） */

/*
 * 调试通道（M3.4）：/dev/zt9612 上的 ioctl ZT_IOC_TXRAW
 * 用户态把"完整的 WLAN 帧"（含 8 字节头、不含补零）交进来，驱动原样发到 tx_ep。
 * 为什么不用 debugfs：本机 Secure Boot 打开时内核处于 lockdown=integrity，
 * lockdown 会拒绝写 debugfs（实测 EPERM），而 ioctl 路径不受影响。
 */
struct zt_txraw_req {
	__u32 len;
	__u32 pad;
	__u64 data;		/* 用户态缓冲区指针 */
};

#define ZT_IOC_TXRAW	_IOW('Z', 1, struct zt_txraw_req)

static long zt_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct zt_dev *z = file->private_data;
	struct zt_txraw_req req;
	u8 *buf;
	int ret, sent = 0;

	if (cmd != ZT_IOC_TXRAW)
		return -ENOTTY;
	if (!READ_ONCE(z->alive))
		return -ENODEV;
	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	if (req.len < 9 || req.len > ZT_TX_BUF_SIZE)
		return -EINVAL;
	buf = memdup_user((void __user *)(unsigned long)req.data, req.len);
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	ret = usb_bulk_msg(z->udev, usb_sndbulkpipe(z->udev, (u8)tx_ep),
			   buf, (int)req.len, &sent, 1000);
	kfree(buf);
	if (ret) {
		dev_warn(&z->intf->dev, "ioctl tx: bulk OUT 失败 (%d)\n", ret);
		return ret;
	}
	z->tx_frames++;
	return 0;
}

/*
 * 调试通道：/sys/kernel/debug/zt9612/tx_raw
 * 用户态把"完整的 WLAN 帧"（含 8 字节头，不含任何补零）写进来，驱动原样发到 tx_ep。
 * 目的：M3.4 期间可以在不重新编译/加载模块的前提下批量试描述符与时序。
 */
static ssize_t zt_dbg_tx_write(struct file *f, const char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	struct zt_dev *z = f->private_data;
	u8 *buf;
	int ret, sent = 0;

	if (!z || !READ_ONCE(z->alive))
		return -ENODEV;
	if (count < 9 || count > ZT_TX_BUF_SIZE)
		return -EINVAL;
	buf = memdup_user(ubuf, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	ret = usb_bulk_msg(z->udev, usb_sndbulkpipe(z->udev, (u8)tx_ep),
			   buf, (int)count, &sent, 1000);
	kfree(buf);
	if (ret) {
		dev_warn(&z->intf->dev, "dbg tx: bulk OUT 失败 (%d)\n", ret);
		return ret;
	}
	z->tx_frames++;
	return count;
}

static const struct file_operations zt_dbg_tx_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = zt_dbg_tx_write,
	.llseek = noop_llseek,
};

static struct dentry *zt_dbg_root;

static void zt_dbg_init(struct zt_dev *z)
{
	if (!zt_dbg_root)
		zt_dbg_root = debugfs_create_dir("zt9612", NULL);
	if (IS_ERR_OR_NULL(zt_dbg_root))
		return;
	debugfs_create_file("tx_raw", 0200, zt_dbg_root, z, &zt_dbg_tx_fops);
	debugfs_create_u32("tx_frames", 0400, zt_dbg_root, (u32 *)&z->tx_frames);
	/* C1 观测：EP2-IN 通知通道的计数（低 32 位） */
	debugfs_create_u32("ntf_count", 0400, zt_dbg_root, (u32 *)&z->ntf_count);
	debugfs_create_u32("ntf_bytes", 0400, zt_dbg_root, (u32 *)&z->ntf_bytes);
}

/*
 * 前置声明：zt_tx_report_status() 在上传脚本区、zt_mac_wake_tx_queue() 在文件下半部分，
 * 而 TX 路径（本段）要用到它们。
 */
static void zt_tx_report_status(struct zt_dev *z, struct ieee80211_hw *hw,
				struct sk_buff *skb, bool legacy);
static void zt_mac_wake_tx_queue(struct ieee80211_hw *hw, struct ieee80211_txq *txq);

/*
 * 把一条 802.11 帧拼进 TX 传输缓冲：`WLAN` 头 + 28 字节描述符 + 802.11 帧。
 * 其中 WLAN 头的 hlen = 28 + 帧长（抓包实测：111 字节的 probe request → hlen=139）。
 * 第 9 轮教训：漏掉 WLAN 头直接发描述符会让固件断言、设备复位回 ROM 模式。
 *
 * `at` 是**本次传输内的写入偏移**：0 = 第一个单元，>0 = 追加（聚合）。
 * `stride` 是**非末尾单元之间的固定推进量**（厂商实测 1608 字节槽位；0 = 紧凑拼接），
 * `last` 为真表示这是本次传输的最后一个单元（只补到 8 字节对齐）。
 * 本函数**不做 USB 传输**，由调用者（zt_tx_frame / zt_tx_agg_send）决定何时发。
 *
 * 返回写入后的总长度（含补齐），失败返回负值。
 */
static int zt_tx_build(struct zt_dev *z, u8 *buf, size_t buf_size, size_t at,
		       const u8 *frame, u16 flen, size_t stride, bool last)
{
	size_t total = at;
	u8 *d;

	if (!buf || flen < 10)
		return -EINVAL;

	if (tx_variant == 2) {
		/* 变体 2：不带描述符，只发 WLAN 头 + 帧 */
		if (total + 8 + flen > buf_size)
			return -ENOSPC;
		memcpy(buf + total, "WLAN", 4);
		put_unaligned_le16(flen, buf + total + 4);
		put_unaligned_le16(TX_TYPE_DATA, buf + total + 6);
		memcpy(buf + total + 8, frame, flen);
		total += 8 + flen;
	} else {
		/*
		 * 描述符按 802.11 帧类型选模板（2026-09-27 定案，见 re/EXPERIMENT_TX_RATE.md）：
		 *
		 * 这里的常量此前全部抄自厂商的**管理帧**（扫描 probe request），却被用于**所有**帧，
		 * 包括数据帧。厂商上传时的 USB 抓包（re/captures/vendor_upload.pcap，96107 条数据帧）
		 * 显示厂商对数据帧用的是**另一套常量**。实测差异巨大：
		 *   同一段链路、同一帧长（1436 字节传输）：
		 *     管理帧模板 →  6.32 Mbit/s
		 *     数据帧模板 → 161.68 Mbit/s     （25.6 倍，与"厂商上传 95.3 vs 我们 3.65"吻合）
		 * 机制推测：固件按描述符把帧归类，管理帧走的是逐帧确认的慢速路径。
		 */
		bool is_data = (frame[0] & 0x0c) == 0x08;	/* 802.11 type == data */
		/*
		 * 只对**已加密**的数据帧用厂商的数据模板。
		 *
		 * 依据：厂商抓包里 96107 条数据帧**全部**带 Protected 位（FC[1] & 0x40），
		 * 而我们抓不到它发 EAPOL 的样子。实测教训：一开始对所有数据帧都用数据模板，
		 * 结果四次握手（EAPOL 是**未加密**的数据帧）失败、关联不上。
		 * 因此：加密数据帧走数据模板，其余（管理帧、EAPOL、未加密数据）沿用原管理模板。
		 */
		bool protected = (flen >= 2) && (frame[1] & 0x40);
		/*
		 * 广播/组播帧仍走管理模板：厂商抓包里 96107 条数据帧**全是单播**
		 * （addr1 恒为 AP 的 BSSID），我们没有它发广播帧的样本；而实测发现
		 * 加密广播（DHCP DISCOVER 就是）用数据模板会拿不到地址。
		 * addr1 在 frame[4]，其最低位是组播位。
		 */
		bool mcast = (flen >= 5) && (frame[4] & 0x01);

		if (total + 8 + TX_DESC_LEN + flen > buf_size)
			return -ENOSPC;
		d = buf + total + 8;
		memset(d, 0, TX_DESC_LEN);
		/*
		 * +0x00 = 0xffffffff（与厂商逐字节一致），+0x04 = 本帧长度。
		 */
		put_unaligned_le32(tx_variant == 1 ? 0 : 0xffffffff, d + 0);
		/*
		 * +0x04 = 本帧长度。**必须写**（2026-09-30 更正）：
		 *   - 厂商 probe request（re/ 里的 vendor_probe_139）在此处是 0x006f = 111 = 帧长；
		 *   - 厂商**数据帧**（vendor_upload.pcap，4808 字节三单元那条）此处是 0x0000，
		 *     但那是因为它用了另一套数据模板，该 2 字节在其模板里另有含义；
		 *   - 实测（2026-09-30）：去掉这次写入后**管理帧认证直接超时**（关联不上）⇒ 写回。
		 * 单帧/多帧都一样写，聚合时每个单元各自携带自己的长度。
		 */
		put_unaligned_le16(flen, d + 4);
		/* 先按厂商管理帧模板（我们一直以来的行为） */
		put_unaligned_le16(0x0700, d + 6);
		/*
		 * +0x08 = vif_idx、+0x09 = staid（re/REPORT_TX_SESSION.md §4：
		 * 厂商 TX 路径把固件分配的 sta_idx 写在这里，无站点时是 0xff）。
		 * 只有**有会话的加密单播数据帧**填 sta_idx，其余保持"无站点"，
		 * 避免把第四条实验通道（广播/DHCP、EAPOL）带进未验证的路径。
		 */
		if (z->sta_valid && is_data && protected && !mcast)
			put_unaligned_le16((u16)z->sta_idx, d + 8);
		else
			put_unaligned_le16(0xff00, d + 8);
		/*
		 * +0x0E = 帧自身的 802.11 序列号（12 位裸序号，不含分片位）。
		 *
		 * 两个雷都已排掉（2026-09-30）：
		 * ① 不能写 seq<<4：厂商证据（vendor_upload.pcap）里描述符 +0x0E
		 *   = 0x0296，而同一帧 MAC 头 sequence control = 0x2960 —— 描述符
		 *   装的是**裸序号**（seq_ctrl >> 4），不是 seq_ctrl 本身；
		 *   管理帧抓包（78 个 probe）同样 = 0..77 裸计数。
		 * ② 不能改写 MAC 头的序号：mac80211 交来的帧已被软件 CCMP 加密，
		 *   AAD 包含序号位 —— 此前 max(fseq, z->tx_seq) 一旦让驱动计数器
		 *   跑在帧序号前面（它连管理帧也数）就会改写帧头，
		 *   AP 侧 MIC 校验失败、静默丢帧。
		 * 驱动自行构造的帧（probe 重放）走 zt_tx_raw，不经过这里，
		 * 无需驱动侧兜底序号。
		 */
		{
			u16 fseq = (flen >= 24) ?
				(u16)(((frame[22] | (frame[23] << 8)) >> 4) & 0xfff) : 0;

			put_unaligned_le16(fseq, d + 14);
		}
		put_unaligned_le16(vendor_tmpl ? 0x0000 : 0x003f, d + 26);
		if (vendor_tmpl && is_data && protected && !mcast) {
			/*
			 * 厂商**数据帧**模板（2026-09-30 逐字节提取：样本 1888/3200/4152/4808
			 * 四种传输、共 10 个单元，除 +0x04 与本序号外**全部恒定**）：
			 *   +0x02 ffff | +0x04 flen | +0x06 0000 | +0x08 0000 | +0x0a 1312
			 *   +0x0c 0040 | +0x0e seq  | +0x10 0000 | +0x12 0000 | +0x14 0200
			 *   +0x16 0000 | +0x18 0000 | +0x1a 7540
			 * 其中 +0x04 = **MPDU 长**（1550 = hlen - 28，我们写的 flen 正是这个值）。
			 * 只用于**已加密单播数据帧**：管理帧/EAPOL/广播继续走管理模板
			 * （厂商抓包里没有这些帧的数据模板样本，实测数据模板会让 DHCP 拿不到地址）。
			 */
			put_unaligned_le16(0xffff, d + 2);
			put_unaligned_le16(0x0000, d + 6);
			put_unaligned_le16(0x0000, d + 8);
			put_unaligned_le16(0x1312, d + 10);
			put_unaligned_le16(0x0040, d + 12);
			put_unaligned_le16(0x0000, d + 16);
			put_unaligned_le16(0x0000, d + 18);
			put_unaligned_le16(0x0200, d + 20);
			put_unaligned_le16(0x0000, d + 22);
			put_unaligned_le16(0x0000, d + 24);
			put_unaligned_le16(0x7540, d + 26);
		}
		/*
		 * 实验：加密单播数据帧按位采用厂商**数据帧**的字段值（见 tx_desc_mask 注释）。
		 * 广播/组播仍用管理模板：厂商抓包里 96107 条数据帧全是单播，且实测加密广播
		 * （DHCP DISCOVER）用数据模板会拿不到地址。EAPOL 等未加密数据帧同理不动。
		 */
		if (tx_desc_mask && is_data && protected && !mcast) {
			if (tx_desc_mask & 0x01)
				put_unaligned_le16(0x0000, d + 6);
			if (tx_desc_mask & 0x02)
				put_unaligned_le16(0x0000, d + 8);
			if (tx_desc_mask & 0x04)
				put_unaligned_le16(0x1312, d + 10);
			if (tx_desc_mask & 0x08)
				put_unaligned_le16(0x0040, d + 12);
			if (tx_desc_mask & 0x10)
				put_unaligned_le16(0x0200, d + 20);
			if (tx_desc_mask & 0x20)
				put_unaligned_le16(0x7540, d + 26);
		}
		memcpy(d + TX_DESC_LEN, frame, flen);

		memcpy(buf + total, "WLAN", 4);
		put_unaligned_le16(TX_DESC_LEN + flen, buf + total + 4);
		put_unaligned_le16(TX_TYPE_DATA, buf + total + 6);
		total += 8 + TX_DESC_LEN + flen;
	}
	/*
	 * 单元之间的推进量（stride）—— **与厂商逐字节一致**（2026-09-30 定案）：
	 *   厂商不是紧凑拼接，而是给**每个非末尾单元固定 1608 字节槽位**
	 *   （下一个 `WLAN` 恒在 +1608），只有末尾单元才用 align8(8+hlen)。
	 *   传输长度公式：`(n-1) × 1608 + align8(8 + hlen_last)`。
	 *   证据：vendor_upload.pcap 里 1578/3200/4808 三种长度 × 全部单元 10/10 一致
	 *   （4808 = 2×1608 + 1592），见 re/REPORT_AMPDU_PUSH.md §5。
	 *   1608 = 8(WLAN头) + 28(描述符) + **1572** + 补齐 ⇒ 非末尾单元的 MPDU 上限 1572。
	 * `last` 为真（单帧或聚合的最后一帧）时只补到 8 的倍数。
	 */
	{
		size_t gran = (tx_variant == 3) ? 512 : 8;

		if (!last && stride) {
			if ((size_t)at + stride > buf_size)
				return -ENOSPC;
			while (total < (size_t)at + stride) {
				if (total >= buf_size)
					return -ENOSPC;
				buf[total++] = 0;
			}
		}
		while (total & (gran - 1)) {
			if (total >= buf_size)
				return -ENOSPC;
			buf[total++] = 0;
		}
	}
	return (int)total;
}

/*
 * 单帧发送路径（管理帧、EAPOL、广播，以及 ampdu_en=0 时的全部帧）：一次 bulk 一个单元。
 */
static int zt_tx_frame(struct zt_dev *z, const u8 *frame, u16 flen)
{
	u8 *buf = z->txd;
	int total, ret, sent = 0;

	if (!READ_ONCE(z->alive) || !z->ep_tx || !buf)
		return -ENODEV;

	total = zt_tx_build(z, buf, z->tx_buf_size, 0, frame, flen, 0, true);
	if (total < 0)
		return total;

	ret = usb_bulk_msg(z->udev, usb_sndbulkpipe(z->udev, (u8)tx_ep),
			   buf, total, &sent, 1000);
	if (ret) {
		dev_warn(&z->intf->dev, "tx: bulk OUT 失败 (%d)\n", ret);
		return ret;
	}
	z->tx_frames++;
	return 0;
}

/* 发送一条已经带好 WLAN 头的原始帧（用于逐字节重放厂商 probe） */
static int zt_tx_raw(struct zt_dev *z, const u8 *wlan_frame, u16 total)
{
	int ret, sent = 0;

	if (!READ_ONCE(z->alive) || !z->ep_tx)
		return -ENODEV;
	ret = usb_bulk_msg(z->udev, usb_sndbulkpipe(z->udev, z->ep_tx),
			   (u8 *)wlan_frame, total, &sent, 1000);
	if (ret) {
		dev_warn(&z->intf->dev, "tx(raw): bulk OUT 失败 (%d)\n", ret);
		return ret;
	}
	z->tx_frames++;
	return 0;
}

/*
 * 聚合发送（ampdu_en=1）：把 n 个已加密单播数据 MPDU 拼进**同一次** bulk 传输。
 *
 * 依据（2026-09-30，厂商上传抓包 re/captures/vendor_upload.pcap，EP5 共 96107 条传输）：
 *   传输长度只有 1608 / 3200 / 4808 三种众数，且 4808 = 3 × 1608；
 *   每个单元是 `"WLAN" + u16 hlen + u16 type + 28B 描述符 + MPDU`，hlen = 28 + MPDU 长，
 *   单元之间**没有** 802.11 MPDU delimiter、也没有额外的长度表。
 *   ⇒ 固件期望"一次传输 = 一串自描述单元"，**A-MPDU 由主机自己组**。
 * 我们此前每个 MPDU 单独一次 bulk（1578 → 1578 → …），固件永远收不到聚合，
 * 压流量时链路崩掉（丢包 70%、USB 掉线）。
 *
 * 只聚合**已加密的单播数据帧**：EAPOL（未加密数据帧）、管理帧、广播一律走单帧路径，
 * 与 zt_tx_build 的"帧身份"判断保持同一套，避免把四次握手/会话维护帧带进聚合。
 *
 * 返回 0 表示 skb 的所有权已转移（已上报 TX status 或已释放）。
 */
static int zt_tx_agg_send(struct zt_dev *z, struct sk_buff **skb, int n, bool *again)
{
	struct sk_buff *done[ZT_AGG_SUBFRAMES];
	size_t off[ZT_AGG_SUBFRAMES];
	int i, k = 0, sub = 0, ret, sent = 0, total = 0, stop = n;
	u8 *buf = z->tx;			/* 聚合缓冲：与 z->txd 分开，互不干扰 */
	bool dropping = false;

	*again = false;
	if (!READ_ONCE(z->alive) || !z->ep_tx || !buf)
		dropping = true;

	/* 第一遍：紧凑排列各单元，记下每个单元的起始偏移 */
	for (i = 0; i < n && !dropping; i++) {
		u8 *f = skb[i]->data;
		u16 flen = skb[i]->len;
		bool is_data = (flen >= 2) && ((f[0] & 0x0c) == 0x08);
		bool prot = (flen >= 2) && (f[1] & 0x40);
		bool mcast = (flen >= 5) && (f[4] & 0x01);
		/*
		 * 聚合资格（2026-09-30 收紧）：
		 *   ① 加密单播数据帧（Protected 位 且 非组播）；
		 *   ② 长度 ≤ 1572（= 1608 槽位 - 8 WLAN 头 - 28 描述符）；
		 * 只有同时满足的帧才参与聚合，其余走单帧路径（已验证稳定）。
		 * 实证动机：首次聚合传输（1792 字节）里混进了一个"标记为加密、
		 * 但长度只有 138/394 字节"的数据帧，混合传输后固件立刻断言掉线；
		 * 厂商抓包里 96107 条数据帧**全是单播**，没有这种混合样本。
		 */
		bool eligible = is_data && prot && !mcast && (flen <= 1572) &&
				(agg_min_len <= 0 || (int)flen >= agg_min_len);
		int unit;

		if (tx_dump > 0) {
			tx_dump--;
			dev_info(&z->intf->dev,
				 "txdump: len=%u fc=0x%04x data=%u prot=%u mcast=%u eligible=%u fseq=0x%04x\n",
				 flen, (u16)(f[0] | (f[1] << 8)), is_data, prot, mcast, eligible,
				 (u16)((f[22] | (f[23] << 8)) & 0xffff));
		}

		if (!eligible) {
			/*
			 * 不该进聚合路径：先把已攒的冲出去；stop 记住断点，
			 * 本帧及之后由函数尾部的单帧循环处理。
			 * （此前这里直接 break 返回：断点后的帧既没发出也没释放，
			 * 静默丢失 + skb 泄漏 —— ARP 广播在流量中间消失就是这条路径。）
			 */
			if (total > 0) {
				stop = i;
				*again = true;
				break;
			}
			if (zt_tx_frame(z, f, flen)) {
				z->tx_dropped++;
				ieee80211_free_txskb(z->hw, skb[i]);
				continue;
			}
			z->tx_path_frames++;
			done[k++] = skb[i];
			continue;
		}

		unit = zt_tx_build(z, buf, z->tx_buf_size, total, f, flen, 0, true);
		if (unit < 0) {
			if (total > 0) {
				stop = i;	/* 缓冲满：先发这一批，当前帧随尾部单帧循环发出 */
				*again = true;
				break;
			}
			z->tx_dropped++;
			ieee80211_free_txskb(z->hw, skb[i]);
			continue;
		}
		off[sub] = total;
		total = unit;
		done[k++] = skb[i];
		sub++;
	}

	if (dropping) {
		for (i = 0; i < n; i++) {
			z->tx_dropped++;
			ieee80211_free_txskb(z->hw, skb[i]);
		}
		return -ENODEV;
	}

	/*
	 * 计数在**尝试之前**自增：`agg_max_xfers` 限的是"尝试打包多少次"，
	 * 而不是"成功多少次" —— 崩掉的那次传输永远不会走到成功路径的计数
	 * （实测踩坑：用成功计数当上限时，`agg_max_xfers=1` 完全拦不住它）。
	 */
	if (z->tx_agg_attempts < 0xffff)
		z->tx_agg_attempts++;
	if (sub > 1)
		z->tx_agg_multi++;

	/*
	 * 第二遍：把**非末尾单元**补齐到固定槽位 ZT_AGG_STRIDE
	 * （厂商实测：下一个 `WLAN` 恒在 +1608；末尾单元只补到 8 字节对齐）。
	 * 用 memmove 逐个后移，避免第一遍就要预留槽位。
	 */
	for (i = 0; i + 1 < sub; i++) {
		size_t want = off[i] + agg_stride;
		size_t have = off[i + 1];
		size_t tail = (size_t)total - have;

		if (want < have)
			continue;			/* 单元比槽位还大：保持紧凑 */
		if (want + tail > z->tx_buf_size) {
			dev_warn_ratelimited(&z->intf->dev,
					     "tx(agg): 槽位补齐超出缓冲，按紧凑格式发送\n");
			break;
		}
		if (want > have) {
			memmove(buf + want, buf + have, tail);
			memset(buf + have, 0, want - have);
			total = (int)(want + tail);
			for (k = i + 1; k < sub; k++)
				off[k] += want - have;
		}
	}

	/*
	 * **关键修正（2026-09-30）**：传输长度不能是端点包长(512)的整数倍。
	 * 批量端点上"刚好整数个最大包"时，设备无法判定传输结束、会一直等后续数据
	 * ⇒ 固件卡住、USB 掉线（本设备 bulk-IN 是 512，EP5-OUT 同）。
	 * 实证：我们的首次聚合传输 = **1792 = 3.5 × 512** 后立刻掉线；
	 * 而厂商全部传输长度（1578/1888/3200/4152/4808）**没有一个是 512 的倍数**。
	 * 处理：补到下一个 8 字节边界后，若仍是 512 的倍数，再多补 8 字节。
	 */
	if (total > 0 && (total & 511) == 0) {
		if ((size_t)total + 8 > z->tx_buf_size) {
			/*
			 * 缓冲塞不下这 8 字节：**不能直接返回**（那样这一批 skb 既没发出也没释放，
			 * 会泄漏），退化为"按原长发出"并告警。
			 */
			dev_warn_ratelimited(&z->intf->dev,
					     "tx(agg): 传输长度 %d 是 512 的倍数但缓冲已满，按原长发送\n",
					     total);
		} else {
			dev_info(&z->intf->dev,
				 "tx(agg): 传输长度 %d 是 512 的倍数，补 8 字节避免设备等待后续数据\n",
				 total);
			memset(buf + total, 0, 8);
			total += 8;
		}
	}

	if (total > 0 && sub < 2) {
		/*
		 * 只攒到 1 个单元：不发聚合缓冲（单单元传输与"BA 在但不打包"
		 * 没有区别，多单元形态未定案前不冒险），全部改走尾部单帧循环。
		 */
		total = 0;
		sub = 0;
		stop = 0;
		k = 0;
	}

	if (total > 0) {
		if (agg_dump > 0) {
			char line[3 * 64 + 4];
			int p = 0, b, u;
			u16 fc0 = buf[8 + TX_DESC_LEN] | (buf[9 + TX_DESC_LEN] << 8);
			u16 sc = buf[30 + TX_DESC_LEN] | (buf[31 + TX_DESC_LEN] << 8);

			agg_dump--;
			for (b = 0; b < 56 && b < total; b++)
				p += scnprintf(line + p, sizeof(line) - p, "%02x ", buf[b]);
			dev_info(&z->intf->dev,
				 "aggdump: units=%d total=%d fc=0x%04x (type=%u sub=%u tods=%u prot=%u qos=%u) sc=0x%04x | %s\n",
				 sub, total, fc0, (fc0 >> 2) & 3, (fc0 >> 4) & 1,
				 (fc0 >> 8) & 1, (fc0 >> 14) & 1, (fc0 >> 15) & 1, sc, line);
			/* 逐单元打印**描述符 28 字节**与帧自身的 sequence control（对齐用） */
			for (u = 0; u < sub && u < 4; u++) {
				size_t o = off[u];
				u16 fsc = (u16)(buf[o + 8 + TX_DESC_LEN + 22] |
						(buf[o + 8 + TX_DESC_LEN + 23] << 8));

				p = 0;
				for (b = 0; b < TX_DESC_LEN; b++)
					p += scnprintf(line + p, sizeof(line) - p, "%02x",
						       buf[o + 8 + b]);
				dev_info(&z->intf->dev,
					 "aggdump: unit%d off=%zu desc=%s fseq=0x%04x\n",
					 u, o, line, fsc);
			}
		}
		mutex_lock(&z->lock);
		ret = usb_bulk_msg(z->udev, usb_sndbulkpipe(z->udev, (u8)tx_ep),
				   buf, total, &sent, 1000);
		mutex_unlock(&z->lock);
		if (ret) {
			dev_warn(&z->intf->dev, "tx(agg): bulk OUT 失败 (%d)\n", ret);
			for (i = 0; i < n; i++) {
				z->tx_dropped++;
				ieee80211_free_txskb(z->hw, skb[i]);
			}
			return ret;
		}
		z->tx_frames += sub;
		z->tx_agg_xfers++;
		z->tx_agg_mpdus += sub;
		dev_info(&z->intf->dev,
			 "tx(agg): %d 单元 / 一次传输 %d 字节（累计 %u 次 / %u MPDU）\n",
			 sub, total, (u32)z->tx_agg_xfers, (u32)z->tx_agg_mpdus);
	}

	/*
	 * 批次之外的帧（stop..n-1，或 sub<2 退化时的 0..n-1）：逐帧走单帧路径。
	 * 此前这些帧在两个提前 break 分支里被静默丢弃（没发出也没释放）。
	 */
	for (i = stop; i < n; i++) {
		if (zt_tx_frame(z, skb[i]->data, skb[i]->len)) {
			z->tx_dropped++;
			ieee80211_free_txskb(z->hw, skb[i]);
			continue;
		}
		z->tx_path_frames++;
		done[k++] = skb[i];
	}

	for (i = 0; i < k; i++) {
		z->tx_path_frames++;
		zt_tx_report_status(z, z->hw, done[i], false);
	}
	return 0;
}

/*
 * 抓包里厂商 2.4G probe request 的原样重放（WLAN 头 + 28 字节描述符 + 111 字节帧）。
 * 只改两处：描述符 +0x0e 的序号、DS 参数（数组下标 80）改成当前信道。
 */
static const u8 vendor_probe_139[147] = {
	0x57, 0x4c, 0x41, 0x4e, 0x8b, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x6f, 0x00, 0x00, 0x07, 0x00, 0xff, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x00,
	0x40, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xb4, 0x01,
	0x1a, 0x00, 0x12, 0x64, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00,
	0x00, 0x00, 0x01, 0x08, 0x02, 0x04, 0x0b, 0x16, 0x0c, 0x12, 0x18, 0x24,
	0x32, 0x04, 0x30, 0x48, 0x60, 0x6c, 0x03, 0x01, 0x01, 0x2d, 0x1a, 0xff,
	0x09, 0x1b, 0xff, 0xff, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x2c, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0xbf, 0x0c, 0x91, 0x71, 0x90, 0x03, 0xfa, 0xff, 0x68, 0x01, 0xfa,
	0xff, 0x68, 0x01, 0xff, 0x16, 0x23, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x02, 0xe0, 0x2f, 0x64, 0x0d, 0xc0, 0x6f, 0x04, 0x82, 0x30, 0x00, 0xfa,
	0xff, 0xfa, 0xff,
};

/*
 * 厂商 5G probe request 的原样重放：WLAN 头 8 + 28 字节描述符 + 102 字节帧 = 138，补零到 144。
 * 与 2.4G 模板的差异（re/REPORT_5GHZ.md §2.2/§2.3）：Supported Rates 换成纯 OFDM、删掉
 * Extended Supported Rates(50) 与 DS 参数(3)，厂商 IE(191)/(255) 载荷不同。
 * 注意：5G 帧里没有任何"当前信道"字节，数组下标 80 落在 HT Capabilities 载荷内，
 * 绝不能像 2.4G 那样写信道号（那会破坏 HT Capabilities 的 MCS 集，见报告 §2.4）。
 * 与抓包逐字节核对：python tools/verify_probe_5g.py
 */
static const u8 vendor_probe_5g[144] = {
	0x57, 0x4c, 0x41, 0x4e, 0x82, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x66, 0x00, 0x00, 0x07, 0x00, 0xff, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x00,
	0x40, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xb4, 0x01,
	0x1a, 0x00, 0x12, 0x64, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00,
	0x00, 0x00, 0x01, 0x08, 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c,
	0x2d, 0x1a, 0xff, 0x09, 0x1b, 0xff, 0xff, 0x00, 0x00, 0x01, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x2c, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xbf, 0x0c, 0xb1, 0x71, 0x90, 0x03, 0xfa, 0xff,
	0x0c, 0x03, 0xfa, 0xff, 0x0c, 0x03, 0xff, 0x16, 0x23, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x00, 0x04, 0xe0, 0x2f, 0x64, 0x0d, 0xc0, 0x6f, 0x04, 0x80,
	0x30, 0x00, 0xfa, 0xff, 0xfa, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* 频率 -> 信道号（用 wiphy 表里的 hw_value，2484 因此得到 14 而不是 (f-2407)/5=15） */
static u8 zt_chan_hw_value(struct zt_dev *z, u16 freq)
{
	struct ieee80211_channel *c;

	if (!READ_ONCE(z->hw))
		return 0;
	c = ieee80211_get_channel(z->hw->wiphy, freq);
	return c ? (u8)c->hw_value : 0;
}

static int zt_scan_probe_vendor(struct zt_dev *z, u16 freq)
{
	bool is_5g = freq > 2500;
	u16 seq = z->tx_seq & 0x0fff;		/* 描述符 +0x0e 与 802.11 序号同值（78/78） */
	u8 *b = z->txd;
	const u8 *tpl;
	u16 total;

	if (!b)
		return -ENODEV;
	if (is_5g) {
		tpl = vendor_probe_5g;
		total = sizeof(vendor_probe_5g);
	} else {
		tpl = vendor_probe_139;
		total = sizeof(vendor_probe_139);
	}
	memcpy(b, tpl, total);
	put_unaligned_le16(z->tx_seq++, b + 8 + 14);	/* 描述符 +0x0e：序号 */
	put_unaligned_le16(seq << 4, b + 8 + 28 + 22);	/* 802.11 seq_ctrl：与描述符同值 */
	if (!is_5g)
		b[80] = zt_chan_hw_value(z, freq);	/* DS 参数：仅 2.4G 帧有 */
	while (total & 7)				/* 与抓包一致的 8 字节对齐 */
		b[total++] = 0;
	if (zt_tx_raw(z, b, total))
		return -EIO;
	z->tx_probes++;
	return 0;
}

/* 广播 probe request（SSID 通配 + 速率 + HT 能力；2.4G 另有扩展速率与 DS 参数） */
static int zt_scan_probe(struct zt_dev *z, u16 freq)
{
	/* 2.4G：CCK/B + OFDM 混合速率；5G：纯 OFDM（厂商 5G probe 的 rates IE，
	 * re/REPORT_5GHZ.md §2.2）。5G 帧里没有 DS 参数 IE，也没有扩展速率 IE。 */
	static const u8 rates_2ghz[8] = { 0x02, 0x04, 0x0b, 0x16, 0x0c, 0x12, 0x18, 0x24 };
	static const u8 xrates_2ghz[4] = { 0x30, 0x48, 0x60, 0x6c };
	static const u8 rates_5ghz[8] = { 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c };
	static const u8 ht[28] = {
		0x2d, 0x1a, 0xff, 0x09, 0x1b, 0xff, 0xff, 0x00, 0x00, 0x01,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x01, 0x01, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	bool is_5g = freq > 2500;
	u16 seq = z->tx_seq & 0x0fff;
	u8 f[24 + 2 + 10 + 6 + 3 + sizeof(ht)];
	u16 n = 24;

	put_unaligned_le16(0x0040, f + 0);	/* probe request */
	put_unaligned_le16(0x0000, f + 2);
	memset(f + 4, 0xff, 6);			/* DA = 广播 */
	memcpy(f + 10, z->mac, 6);		/* SA = 本机 MAC */
	memset(f + 16, 0xff, 6);		/* BSSID = 广播 */
	put_unaligned_le16(seq << 4, f + 22);	/* 序号（描述符 +0x0e 由 zt_tx_frame 用同值） */

	f[n++] = 0x00; f[n++] = 0x00;		/* SSID：通配 */
	f[n++] = 0x01; f[n++] = 8;
	memcpy(f + n, is_5g ? rates_5ghz : rates_2ghz, 8); n += 8;
	if (!is_5g) {
		f[n++] = 0x32; f[n++] = sizeof(xrates_2ghz);
		memcpy(f + n, xrates_2ghz, sizeof(xrates_2ghz)); n += sizeof(xrates_2ghz);
		f[n++] = 0x03; f[n++] = 0x01;	/* DS 参数 = 当前信道（仅 2.4G） */
		f[n++] = zt_chan_hw_value(z, freq);
	}
	memcpy(f + n, ht, sizeof(ht)); n += sizeof(ht);

	if (zt_tx_frame(z, f, n))
		return -EIO;
	z->tx_probes++;
	return 0;
}

/* 记录一条设备→主机 IPC 消息 id（观测用：与厂商抓包对比，找缺失的通知） */
static void zt_note_msg(struct zt_dev *z, const u8 *frame, int len)
{
	u16 id;
	int i, slot = -1;

	if (len < 10 || memcmp(frame, "WLAN", 4))
		return;
	if (get_unaligned_le16(frame + 6) != T_IPC)
		return;
	id = get_unaligned_le16(frame + 8);
	for (i = 0; i < 16; i++) {
		if (z->seen_ids[i] == id) {
			z->seen_cnt[i]++;
			return;
		}
		if (slot < 0 && z->seen_ids[i] == 0)
			slot = i;
	}
	if (slot >= 0) {
		z->seen_ids[slot] = id;
		z->seen_cnt[slot] = 1;
	}
}

/*
 * M3.2：扫描期间把 RX 数据帧里的 802.11 帧交给 mac80211。
 * RX 帧格式（第 9 轮实测）：WLAN 头 + 每帧描述符 + 802.11 帧；
 *   type=0x0000 → 描述符 48 字节；type=0x0004 → 描述符 52 字节。
 * 在 URB 完成上下文里调用，用 GFP_ATOMIC 并走 ieee80211_rx_irqsafe()。
 */
static void zt_rx_inject(struct zt_dev *z, const u8 *buf, int len)
{
	struct ieee80211_rx_status *st;
	struct ieee80211_channel *chan;
	struct sk_buff *skb;
	const u8 *d, *payload;
	u16 hlen, type, freq;
	s8 rssi;
	int off, flen;

	if (len < 8)
		return;
	hlen = get_unaligned_le16(buf + 4);
	type = get_unaligned_le16(buf + 6);

	/*
	 * 描述符归一化：type=0x0004 的 52 字节描述符 = 4 字节 00 前缀 +
	 * 与 type=0x0000 的 48 字节逐字节同布局的结构（整体偏移 +4），
	 * 所以丢掉前 4 字节后按同一套偏移读取（re/REPORT_RX_DESC.md §4）。
	 * 一律按 type 判定，不用"前 4 字节是否为 0"来猜。
	 */
	switch (type) {
	case T_RX_DATA0:
		off = 48;
		d = buf + 8;
		break;
	case T_RX_DATA4:
		off = 52;
		d = buf + 12;		/* payload + 4 */
		break;
	default:
		return;			/* IPC / 事件帧不往 mac80211 送 */
	}
	if (hlen <= off || 8 + hlen > len)
		return;
	flen = hlen - off;
	if (flen < 24 || flen > 2304)
		return;
	payload = buf + 8 + off;

	skb = __dev_alloc_skb(flen + 32, GFP_ATOMIC);
	if (!skb)
		return;
	skb_put_data(skb, payload, flen);

	/* 统计：beacon(0x80) 与 probe response(0x50)，用于验证 TX 是否真的发出去了 */
	if (flen >= 24) {
		u16 fc = get_unaligned_le16(payload);

		if ((fc & 0x00fc) == 0x0080)
			z->rx_beacons++;
		else if ((fc & 0x00fc) == 0x0050)
			z->rx_probe_resp++;
	}

	st = IEEE80211_SKB_RXCB(skb);
	memset(st, 0, sizeof(*st));

	/*
	 * d 指向归一化（48B 坐标系）描述符，字段语义见 re/REPORT_RX_DESC.md：
	 *   +0x0E  int8 RSSI（dBm，抓包实测 -87~-42，同组内极差 <=2 dB）
	 *   +0x28  band（0 = 2.4 GHz，1 = 5 GHz）
	 *   +0x2A  u16 中心频率 MHz（真实接收信道：切信道后有最长 ~22 ms 延迟）
	 */
	rssi = (s8)d[0x0E];
	if (rssi < -95 || rssi > -20) {	/* 合理性兜底：越界时退回旧行为 */
		rssi = -50;
		z->rx_rssi_fb++;
	}
	st->signal = rssi;
	st->chains = BIT(0);
	st->chain_signal[0] = rssi;

	freq = get_unaligned_le16(d + 0x2A);
	if (freq > 2500)
		z->rx_frames_5g++;
	chan = freq ? ieee80211_get_channel(z->hw->wiphy, freq) : NULL;
	if (chan) {
		z->rx_freq_desc++;
		if (freq != READ_ONCE(z->scan_freq))
			z->rx_freq_ne_scan++;
	} else {
		/* 未注册的频段（例如 5G 尚未加进 wiphy）不能用，退回本次扫描信道，
		 * 否则 mac80211 查不到 channel 会把这一帧丢掉。 */
		z->rx_freq_fb++;
		freq = READ_ONCE(z->scan_freq);
		if (!freq)
			freq = 2412;
		chan = ieee80211_get_channel(z->hw->wiphy, freq);
	}
	st->freq = freq;
	st->band = chan ? chan->band : NL80211_BAND_2GHZ;
	ieee80211_rx_irqsafe(z->hw, skb);
}

static void zt_rx_complete(struct urb *urb)
{
	struct zt_dev *z = urb->context;
	int len = urb->actual_length;

	/*
	 * 临时诊断（默认关闭，诊断大包为何不上栈）：
	 * 记录 USB 层实际收到的长度与帧头里的 hlen。计数由模块参数 rx_debug 控制，
	 * 可随时 `sudo sh -c 'echo N > .../parameters/rx_debug'` 重新开始。
	 */
	if (rx_debug_left > 0 && urb->status == 0 && len >= 8 &&
	    !memcmp(z->rx_buf, "WLAN", 4)) {
		u16 dh = get_unaligned_le16(z->rx_buf + 4);
		u16 dt = get_unaligned_le16(z->rx_buf + 6);

		rx_debug_left--;
		dev_info(&z->intf->dev,
			 "rxdbg: usb_len=%d hlen=%u type=%#06x left=%d\n",
			 len, dh, dt, rx_debug_left);
	}

	/* D9：卸载中（alive=0）不再碰任何缓冲，只回报回收完成 */
	if (!READ_ONCE(z->alive)) {
		complete(&z->rx_done);
		return;
	}
	if (urb->status == 0 && len >= 8 && !memcmp(z->rx_buf, "WLAN", 4)) {
		u16 flen = (u16)len;
		u16 ftype = get_unaligned_le16(z->rx_buf + 6);

		/* 观测：统计收到的帧类型（扫描期间才有意义） */
		if (ftype == 0x0000)
			z->rx_type_cnt[0]++;
		else if (ftype == 0x0004)
			z->rx_type_cnt[1]++;
		else if (ftype == 0x0100)
			z->rx_type_cnt[2]++;
		else if (ftype == 0x0300)
			z->rx_type_cnt[3]++;
		else {
			z->rx_type_cnt[4]++;
			z->last_other_type = ftype;
		}
		if (ftype == 0x0100)
			zt_note_msg(z, z->rx_buf, len);

		if (kfifo_avail(&z->rx_fifo) >= flen + 2) {
			kfifo_in(&z->rx_fifo, (u8 *)&flen, 2);
			kfifo_in(&z->rx_fifo, z->rx_buf, flen);
			wake_up_interruptible(&z->rx_wait);
		}
		/*
		 * RX 注入不能只在扫描期间做：认证/关联/数据帧都要交给 mac80211。
		 * 早期版本用 scan_active 门控，导致扫描之外收到的一切管理帧都被丢掉，
		 * 表现为 iw connect 永远超时。
		 * 但必须用"hw 已 start（netdev up）"门控（mac_started）：接口 down 时
		 * mac80211 的 local->started=0，此时注帧会在 rx.c:5475 打 WARNING。
		 */
		if (READ_ONCE(z->hw) && READ_ONCE(z->mac_started))
			zt_rx_inject(z, z->rx_buf, len);
	}
	if (READ_ONCE(z->rx_running)) {
		usb_submit_urb(urb, GFP_ATOMIC);
	} else {
		/*
		 * D9：停止 RX 时也要唤醒等待者。原实现只在 alive==0 时 complete()，
		 * 于是设备正常但 RX 处于空闲（URB 在途等数据）时，zt_rx_stop() 会白等满
		 * 2 秒并走到"泄漏实例"分支。
		 */
		complete(&z->rx_done);
	}
}

static int zt_rx_start(struct zt_dev *z)
{
	z->rx_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!z->rx_urb)
		return -ENOMEM;
	z->rx_buf = kmalloc(ZT_RX_BUF_SIZE, GFP_KERNEL);
	if (!z->rx_buf)
		return -ENOMEM;

	usb_fill_bulk_urb(z->rx_urb, z->udev, usb_rcvbulkpipe(z->udev, z->ep_in),
			  z->rx_buf, ZT_RX_BUF_SIZE, zt_rx_complete, z);
	z->rx_running = true;
	if (usb_submit_urb(z->rx_urb, GFP_KERNEL)) {
		z->rx_running = false;	/* 未提交成功 → 不会有 completion */
		return -EIO;
	}
	return 0;
}

/*
 * D9：停止 RX 时绝不做无界等待。设备挂死时 usb_kill_urb() 会永久阻塞在
 * 在途 URB 上，把卸载/拔出流程（甚至 xhci 内核线程）一起卡住 —— 现场表现
 * 为整机失联、只能硬重启。这里改为：poison（拒绝重投 + 异步 unlink）
 * 之后最多等 2 s 回收；超时就故意不释放，宁可泄漏也不 use-after-free。
 * 返回 0 表示可安全释放，-ETIMEDOUT 表示实例已泄漏（调用者不要再 free）。
 */
static int zt_rx_stop(struct zt_dev *z)
{
	int ret = 0;

	if (!z->rx_urb)
		goto out;
	if (READ_ONCE(z->rx_running)) {
		WRITE_ONCE(z->rx_running, false);
		usb_poison_urb(z->rx_urb);
		if (!wait_for_completion_timeout(&z->rx_done, msecs_to_jiffies(2000))) {
			dev_warn(&z->intf->dev,
				 "rx urb 2s 未回收（设备可能已挂死），泄漏该实例以避免 UAF\n");
			ret = -ETIMEDOUT;
			goto out;
		}
	}
	usb_free_urb(z->rx_urb);
	z->rx_urb = NULL;
out:
	if (!ret) {
		kfree(z->rx_buf);
		z->rx_buf = NULL;
	}
	return ret;
}

/*
 * EP2-IN 的 6 字节通知通道（re/REPORT_TX_RE.md 的 C1）。
 *
 * 厂商驱动在每一次 EP5-OUT 前后都在读这条通道，本驱动此前完全没有接管它；
 * 同一台机器上厂商上传 95.3 Mbit/s、本驱动 3.65 Mbit/s。假设是"固件要等主机把
 * 完成通知取走才放行下一帧"。这里先把它**持续读起来**并计数，用 ntf_log 看内容，
 * 再看 TX 吞吐是否变化 —— 先验证，不预设结论。
 */
static void zt_ntf_complete(struct urb *urb)
{
	struct zt_dev *z = urb->context;

	if (urb->status == 0 && urb->actual_length) {
		unsigned int n = min_t(unsigned int, urb->actual_length, 8);

		z->ntf_count++;
		z->ntf_bytes += urb->actual_length;
		if (z->ntf_log_left > 0) {
			z->ntf_log_left--;
			dev_info(&z->intf->dev, "ntf: len=%u %*phN left=%lu\n",
				 urb->actual_length, (int)n, z->ntf_buf, z->ntf_log_left);
		}
	}
	if (READ_ONCE(z->ntf_running) && READ_ONCE(z->alive))
		usb_submit_urb(urb, GFP_ATOMIC);
	else
		complete(&z->ntf_done);
}

static int zt_ntf_start(struct zt_dev *z)
{
	if (!z->ep_ntf)
		return 0;
	z->ntf_buf = kmalloc(64, GFP_KERNEL);
	z->ntf_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!z->ntf_buf || !z->ntf_urb)
		return -ENOMEM;
	if (z->ntf_is_int)
		usb_fill_int_urb(z->ntf_urb, z->udev,
				 usb_rcvintpipe(z->udev, z->ep_ntf),
				 z->ntf_buf, 64, zt_ntf_complete, z,
				 z->ntf_interval ? z->ntf_interval : 1);
	else
		usb_fill_bulk_urb(z->ntf_urb, z->udev,
				  usb_rcvbulkpipe(z->udev, z->ep_ntf),
				  z->ntf_buf, 64, zt_ntf_complete, z);
	z->ntf_log_left = ntf_log > 0 ? (unsigned long)ntf_log : 0;
	WRITE_ONCE(z->ntf_running, true);
	if (usb_submit_urb(z->ntf_urb, GFP_KERNEL)) {
		WRITE_ONCE(z->ntf_running, false);
		return -EIO;
	}
	dev_info(&z->intf->dev, "ntf: EP%u-IN 通知通道已接管（%s, interval=%u）\n",
		 EP_NTF_NUM, z->ntf_is_int ? "interrupt" : "bulk", z->ntf_interval);
	return 0;
}

/* 与 zt_rx_stop 同一套纪律：poison + 有界等待，超时宁可泄漏也不 UAF */
static int zt_ntf_stop(struct zt_dev *z)
{
	if (!z->ntf_urb)
		return 0;
	if (READ_ONCE(z->ntf_running)) {
		WRITE_ONCE(z->ntf_running, false);
		usb_poison_urb(z->ntf_urb);
		if (!wait_for_completion_timeout(&z->ntf_done, msecs_to_jiffies(2000))) {
			dev_warn(&z->intf->dev, "ntf urb 2s 未回收，泄漏该实例\n");
			return -ETIMEDOUT;
		}
	}
	usb_free_urb(z->ntf_urb);
	z->ntf_urb = NULL;
	kfree(z->ntf_buf);
	z->ntf_buf = NULL;
	return 0;
}

/* ------------------------------------------------------------------ /dev/zt9612 */
static int zt_open(struct inode *inode, struct file *file)
{
	struct zt_dev *z = container_of(file->private_data, struct zt_dev, misc);

	file->private_data = z;
	return 0;
}

static ssize_t zt_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct zt_dev *z = file->private_data;
	u8 hdr[2];
	u8 junk[2];
	u16 flen;
	unsigned int copied = 0;
	unsigned long end;

	if (!READ_ONCE(z->alive))
		return -ENODEV;

	/*
	 * 只等到"完整的一帧"再返回。早期实现只看 kfifo 里有没有 2 字节长度头，
	 * 于是半帧（URB 分片）会让 read() 反复错位、每次都白等 3 秒。
	 */
	end = jiffies + msecs_to_jiffies(3000);
	for (;;) {
		if (kfifo_out_peek(&z->rx_fifo, hdr, 2) == 2) {
			flen = get_unaligned_le16(hdr);
			if (flen > ZT_RX_MAX_FRAME) {	/* 脏数据：丢掉长度头继续 */
				if (kfifo_out(&z->rx_fifo, junk, 2) != 2)
					return -EIO;
				continue;
			}
			if (kfifo_len(&z->rx_fifo) >= (unsigned int)flen + 2)
				break;			/* 完整帧到齐 */
		}
		if (!READ_ONCE(z->alive))
			return -ENODEV;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (time_after(jiffies, end))
			return -ETIMEDOUT;
		if (wait_event_interruptible_timeout(z->rx_wait,
						     kfifo_len(&z->rx_fifo) >= 2 ||
						     !READ_ONCE(z->alive),
						     msecs_to_jiffies(200)) < 0)
			return -ERESTARTSYS;
	}

	if (count < flen)
		return -EINVAL;			/* 头不消费，调用方可以放大缓冲重试 */
	if (kfifo_out(&z->rx_fifo, hdr, 2) != 2)
		return -EIO;
	if (kfifo_to_user(&z->rx_fifo, buf, flen, &copied))
		return -EFAULT;
	return copied;
}

static __poll_t zt_poll(struct file *file, poll_table *wait)
{
	struct zt_dev *z = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &z->rx_wait, wait);
	if (!READ_ONCE(z->alive))
		return EPOLLHUP | EPOLLERR;
	if (kfifo_len(&z->rx_fifo) >= 2)
		mask |= EPOLLIN | EPOLLRDNORM;
	return mask;
}

static ssize_t zt_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct zt_dev *z = file->private_data;
	u8 *tmp;
	int ret;

	if (count < 8 || count > ZT_TX_BUF_SIZE)
		return -EINVAL;
	if (!READ_ONCE(z->alive))
		return -ENODEV;
	tmp = kmalloc(count, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;
	if (copy_from_user(tmp, buf, count)) {
		kfree(tmp);
		return -EFAULT;
	}
	if (memcmp(tmp, "WLAN", 4)) {
		kfree(tmp);
		return -EINVAL;
	}
	mutex_lock(&z->lock);
	ret = usb_bulk_msg(z->udev, usb_sndbulkpipe(z->udev, z->ep_out),
			   tmp, count, NULL, 2000);
	mutex_unlock(&z->lock);
	kfree(tmp);
	return ret ? ret : count;
}

static int zt_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations zt_fops = {
	.owner = THIS_MODULE,
	.open = zt_open,
	.read = zt_read,
	.write = zt_write,
	.unlocked_ioctl = zt_ioctl,
	.poll = zt_poll,
	.release = zt_release,
	.llseek = noop_llseek,
};


/* ================================================================== mac80211 (M3.1)
 *
 * 閻╊喗鐖ｉ敍姘暈閸?wiphy / mac80211閿涘矁顔€ wlan0 閸戣櫣骞囬敍鍦?.1閿涘鈧? * 閺佺増宓侀棃顫礄TX/RX閿涘娈忛張顏呭复闁熬绱皌x 閻╁瓨甯存稉銏犲瘶閿涘本澹傞幓蹇撶毣閺堫亜鐤勯悳甯礄M3.2 閸愬秴浠涢敍澶堚偓? */
static struct ieee80211_channel zt_ch_2ghz[] = {
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2412, .hw_value = 1,  .max_power = 20 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2417, .hw_value = 2,  .max_power = 20 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2422, .hw_value = 3,  .max_power = 20 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2427, .hw_value = 4,  .max_power = 20 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2432, .hw_value = 5,  .max_power = 20 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2437, .hw_value = 6,  .max_power = 20 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2442, .hw_value = 7,  .max_power = 20 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2447, .hw_value = 8,  .max_power = 20 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2452, .hw_value = 9,  .max_power = 20 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2457, .hw_value = 10, .max_power = 20 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2462, .hw_value = 11, .max_power = 20 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2467, .hw_value = 12, .max_power = 20 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2472, .hw_value = 13, .max_power = 20 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2484, .hw_value = 14, .max_power = 20 },
};

static struct ieee80211_rate zt_rates_2ghz[] = {
	{ .bitrate = 10,  .hw_value = 0 },
	{ .bitrate = 20,  .hw_value = 1 },
	{ .bitrate = 55,  .hw_value = 2 },
	{ .bitrate = 110, .hw_value = 3 },
};

static struct ieee80211_supported_band zt_band_2ghz = {
	.band = NL80211_BAND_2GHZ,
	.channels = zt_ch_2ghz,
	.n_channels = ARRAY_SIZE(zt_ch_2ghz),
	.bitrates = zt_rates_2ghz,
	.n_bitrates = ARRAY_SIZE(zt_rates_2ghz),
};

/*
 * M3.6 5GHz：厂商一轮扫描覆盖的标准 20MHz 栅格 25 个信道（re/REPORT_5GHZ.md §1.3/§4.1）。
 * 速率表用纯 OFDM 6..54Mbps —— 依据是厂商 5G probe request 的 Supported Rates IE
 * = 0c 12 18 24 30 48 60 6c（同一份报告 §2.2）；2.4G 那份是 CCK/B + OFDM 混合。
 * DFS/NO_IR 标志不在这里写死：由 cfg80211 按当前监管域标注，扫描只做被动接收即可；
 * 主动探测（scan_probe=1/2）会跳过被标 NO_IR/RADAR 的信道。
 */
static struct ieee80211_channel zt_ch_5ghz[] = {
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5180, .hw_value = 36,  .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5200, .hw_value = 40,  .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5220, .hw_value = 44,  .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5240, .hw_value = 48,  .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5260, .hw_value = 52,  .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5280, .hw_value = 56,  .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5300, .hw_value = 60,  .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5320, .hw_value = 64,  .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5500, .hw_value = 100, .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5520, .hw_value = 104, .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5540, .hw_value = 108, .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5560, .hw_value = 112, .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5580, .hw_value = 116, .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5600, .hw_value = 120, .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5620, .hw_value = 124, .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5640, .hw_value = 128, .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5660, .hw_value = 132, .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5680, .hw_value = 136, .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5700, .hw_value = 140, .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5720, .hw_value = 144, .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5745, .hw_value = 149, .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5765, .hw_value = 153, .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5785, .hw_value = 157, .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5805, .hw_value = 161, .max_power = 20 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5825, .hw_value = 165, .max_power = 20 },
};

static struct ieee80211_rate zt_rates_5ghz[] = {
	{ .bitrate = 60,  .hw_value = 0 },
	{ .bitrate = 90,  .hw_value = 1 },
	{ .bitrate = 120, .hw_value = 2 },
	{ .bitrate = 180, .hw_value = 3 },
	{ .bitrate = 240, .hw_value = 4 },
	{ .bitrate = 360, .hw_value = 5 },
	{ .bitrate = 480, .hw_value = 6 },
	{ .bitrate = 540, .hw_value = 7 },
};

static struct ieee80211_supported_band zt_band_5ghz = {
	.band = NL80211_BAND_5GHZ,
	.channels = zt_ch_5ghz,
	.n_channels = ARRAY_SIZE(zt_ch_5ghz),
	.bitrates = zt_rates_5ghz,
	.n_bitrates = ARRAY_SIZE(zt_rates_5ghz),
};

/*
 * 实验用 HT 能力（见 ht_cap_enable 的说明）。
 * 保守声明：HT20（不含 SUP_WIDTH_20_40）+ SGI_20 + MCS 0~15（2 空间流，本卡 2T2R）、
 * 不声明聚合相关的 hw 标志，因此 mac80211 只用 HT 速率、不做 A-MPDU。
 * 射频的真实能力未从固件确证，故先按"能协商上"的最小集合试；若实测无收益即回退。
 */
static const struct ieee80211_sta_ht_cap zt_ht_cap_5ghz = {
	.ht_supported = true,
	.cap = IEEE80211_HT_CAP_SGI_20,
	.ampdu_factor = IEEE80211_HT_MAX_AMPDU_8K,
	.ampdu_density = IEEE80211_HT_MPDU_DENSITY_NONE,
	.mcs = {
		.rx_mask = { 0xff, 0xff, 0, 0 },
		.rx_highest = cpu_to_le16(144),	/* HT20/2SS/SGI ≈ 144.4 Mbps */
		.tx_params = IEEE80211_HT_MCS_TX_DEFINED,
	},
};

static struct zt_dev *zt_from_hw(struct ieee80211_hw *hw)
{
	return *(struct zt_dev **)hw->priv;
}

static int zt_mac_start(struct ieee80211_hw *hw)
{
	struct zt_dev *z = zt_from_hw(hw);

	/*
	 * M3.5：hw 已 start 才允许把 RX 帧交给 mac80211。
	 * mac80211 的 ieee80211_rx_list() 里有
	 *     if (!local->in_reconfig && !local->started) WARN(...)
	 * （反汇编 + BTF 定位：rx.c:5475，字段 local->in_reconfig / local->started）
	 * 而 local->started 正是在 drv_start/drv_stop 前后置位/清零的。
	 * 我们原来的 RX 注入常开，接口 down（local->started=0）期间设备仍在送 beacon，
	 * 于是每帧打一条 WARNING（实测 down 期间 6.6 条/秒，NM 活动时爆发、taint 内核）。
	 */
	WRITE_ONCE(z->mac_started, true);
	dev_info(&z->intf->dev, "mac80211: start\n");
	return 0;
}

static void zt_mac_stop(struct ieee80211_hw *hw, bool suspend)
{
	struct zt_dev *z = zt_from_hw(hw);

	WRITE_ONCE(z->mac_started, false);
	dev_info(&z->intf->dev, "mac80211: stop\n");
}

static int zt_mac_add_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct zt_dev *z = zt_from_hw(hw);

	dev_info(&z->intf->dev, "mac80211: add_interface type=%d addr=%pM\n",
		 vif->type, vif->addr);
	return 0;
}

static void zt_mac_remove_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct zt_dev *z = zt_from_hw(hw);

	dev_info(&z->intf->dev, "mac80211: remove_interface\n");
}

/* 切换信道：厂商格式的 12 字节参数（5 GHz 时首字段为 1）。 */
static int zt_set_channel(struct zt_dev *z, u16 freq)
{
	u8 chan[12];

	memset(chan, 0, sizeof(chan));
	put_unaligned_le16(freq > 2500 ? 1 : 0, chan + 0);
	put_unaligned_le16(freq, chan + 2);
	put_unaligned_le16(freq, chan + 4);
	put_unaligned_le16(0x14, chan + 10);
	return zt_cmd_fifo(z, 0x0010, chan, sizeof(chan), 0x0011, 1000);
}

/*
 * M3.3：把 mac80211 交下来的 skb 提交到 EP5-OUT。
 * 本内核（7.0）要求驱动必须实现 wake_tx_queue（TXQ 路径），而 wake_tx_queue 与 .tx
 * 都可能在软中断上下文里执行、不能直接 usb_bulk_msg()，所以两者都只做登记，
 * 真正的 USB 提交统一在 tx_work（进程上下文，可以睡眠）里做。
 */
/*
 * v0.3 阶段 B：把发送结果告诉 mac80211。
 *
 * 本内核（7.0）里 `ieee80211_tx_status_irqsafe()` 会把 skb 交给 mac80211 处理并释放，
 * 因此**调用它之后不能再自己 free skb**（否则 double free）。
 * 关掉开关时必须自己释放，两条路都要保证 skb 只被释放一次。
 *
 * ack 的取值见 tx_status 参数的说明：只有每 N 帧一次的"乐观探测"标 true，
 * 其余如实标 false。
 */
static void zt_tx_report_status(struct zt_dev *z, struct ieee80211_hw *hw,
				struct sk_buff *skb, bool legacy)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	bool ack;

	if (!tx_status_on) {
		if (legacy)
			dev_kfree_skb_any(skb);
		else
			ieee80211_free_txskb(hw, skb);
		return;
	}

	if (tx_status_probe > 0 && (z->tx_status_reports % tx_status_probe) == 0) {
		ack = true;
		info->flags |= IEEE80211_TX_STAT_ACK;
		z->tx_status_ack++;
	} else {
		ack = false;
		info->flags &= ~IEEE80211_TX_STAT_ACK;
	}
	/* 速率保持 mac80211 当初选的那个（info->control.rates[0]），不编造 */
	z->tx_status_reports++;
	z->tx_status_last_ack = ack;
	ieee80211_tx_status_irqsafe(hw, skb);
}

static void zt_tx_one(struct zt_dev *z, struct ieee80211_hw *hw, struct sk_buff *skb,
		      bool legacy)
{
	int ret;

	mutex_lock(&z->lock);
	ret = zt_tx_frame(z, skb->data, skb->len);
	mutex_unlock(&z->lock);
	if (ret) {
		z->tx_dropped++;
		if (legacy)
			dev_kfree_skb_any(skb);
		else
			ieee80211_free_txskb(hw, skb);
		return;
	}
	z->tx_path_frames++;
	zt_tx_report_status(z, hw, skb, legacy);
}

static void zt_tx_work(struct work_struct *w)
{
	struct zt_dev *z = container_of(w, struct zt_dev, tx_work);
	struct ieee80211_txq *pend[ZT_MAX_PEND_TXQ];
	unsigned long flags;
	struct sk_buff *skb;
	int n, i;

	for (;;) {
		/*
		 * 1) 传统 .tx 路径登记进来的 skb
		 *
		 * ⚠️ 2026-10-01 实测：**本内核把数据帧走这条路**（不是注释原先写的
		 * "TXQ 才是主路径"）—— 13k 帧的真实流量下 TXQ 分支一次都没被走到。
		 *
		 * 也试过在这里攒批（复用 zt_tx_agg_send）：结果**明显回归** ——
		 * `tx_blast` 1.26 Mbit/s、网关 ping 丢包 **90%**（改前同链路 10.3 Mbit/s / 0%），
		 * 于是整体回退，只保留计数。原因推测：这批帧走的是"传统路径"，
		 * 与 BA 会话/聚合所需的 txq 语义不同（当时也没有 BA 会话）。
		 */
		while ((skb = skb_dequeue(&z->txq))) {
			z->tx_legacy_frames++;
			zt_tx_one(z, z->hw, skb, true);
		}

		/* 2) TXQ 路径：取出本轮被唤醒的队列，逐个 dequeue 到空 */
		spin_lock_irqsave(&z->txq_lock, flags);
		n = z->txq_n;
		memcpy(pend, z->txq_pend, n * sizeof(pend[0]));
		z->txq_n = 0;
		spin_unlock_irqrestore(&z->txq_lock, flags);
		if (tx_diag)
			dev_info(&z->intf->dev,
				 "txpath: legacy=%lu txq=%lu agg_xfers=%lu agg_multi=%lu txq_pend=%d\n",
				 z->tx_legacy_frames, z->tx_txq_frames,
				 z->tx_agg_xfers, z->tx_agg_multi, n);
		if (!n)
			return;
		for (i = 0; i < n; i++) {
			/*
			 * 进程上下文必须用 _ni 版本：7.0 的
			 * ieee80211_tx_dequeue() 带 in_softirq 断言
			 * （踩坑记录见 docs/04 的 D14）。
			 * RCU 只护"出队"这一步 —— 之后 USB 传输会睡眠。
			 *
			 * ampdu_en=1 时改用"攒一批再一次 bulk"：mac80211 一次只给一个
			 * MPDU，把多个 MPDU 拼进同一次传输是驱动的事（zt_tx_agg_send）。
			 */
			if (ampdu_en && !agg_block &&
			    (agg_max_xfers <= 0 || (int)z->tx_agg_attempts < agg_max_xfers)) {
				struct sk_buff *batch[ZT_AGG_SUBFRAMES];
				int cnt = 0, blk = 0;
				bool again = false;

				while (blk++ < ZT_AGG_MAX_BLOCKS) {
					while (cnt < ZT_AGG_SUBFRAMES) {
						rcu_read_lock();
						skb = ieee80211_tx_dequeue_ni(z->hw, pend[i]);
						rcu_read_unlock();
						if (!skb)
							break;
						batch[cnt++] = skb;
					}
					if (!cnt)
						break;
					zt_tx_agg_send(z, batch, cnt, &again);
					cnt = 0;
					if (!again)
						break;	/* 该队列已空 */
				}
				if (again)
					zt_mac_wake_tx_queue(z->hw, pend[i]);	/* 还有余量：重新排队 */
			} else
			while (1) {
				rcu_read_lock();
				skb = ieee80211_tx_dequeue_ni(z->hw, pend[i]);
				rcu_read_unlock();
				if (!skb)
					break;
				z->tx_txq_frames++;
				zt_tx_one(z, z->hw, skb, false);
			}
		}
	}
}

static void zt_mac_wake_tx_queue(struct ieee80211_hw *hw, struct ieee80211_txq *txq)
{
	struct zt_dev *z = zt_from_hw(hw);
	unsigned long flags;
	int i;

	spin_lock_irqsave(&z->txq_lock, flags);
	for (i = 0; i < z->txq_n; i++) {
		if (z->txq_pend[i] == txq)
			goto out;
	}
	if (z->txq_n < ZT_MAX_PEND_TXQ)
		z->txq_pend[z->txq_n++] = txq;
	else
		z->txq_overflow++;
out:
	spin_unlock_irqrestore(&z->txq_lock, flags);
	schedule_work(&z->tx_work);
}

/* M3.3：mac80211 切信道时同步给固件，认证/关联帧才有正确的射频配置 */
static int zt_mac_config(struct ieee80211_hw *hw, int radio_idx, u32 changed)
{
	struct zt_dev *z = zt_from_hw(hw);
	u16 freq;

	if (!(changed & IEEE80211_CONF_CHANGE_CHANNEL) || !hw->conf.chandef.chan)
		return 0;
	freq = hw->conf.chandef.chan->center_freq;
	if (!READ_ONCE(z->alive))
		return 0;
	mutex_lock(&z->lock);
	/* 厂商在发第一帧之前会补一遍使能序列（SET_IDLE / 0x0104 / SET_FILTER），
	 * 顺序是"使能序列 → SET_CHANNEL → TX"。扫描路径有自己的前置序列，
	 * 关联/数据路径（mac80211 通过 config 切信道）必须在这里补上。 */
	if (tx_prep && zt_scan_setup(z))
		dev_warn(&z->intf->dev, "config: 前置序列失败\n");
	if (zt_set_channel(z, freq))
		dev_warn(&z->intf->dev, "config: 切到 %u MHz 无 CFM\n", freq);
	else
		dev_info(&z->intf->dev, "config: 信道 -> %u MHz\n", freq);
	mutex_unlock(&z->lock);
	return 0;
}

/* M3.1閿涙瓖X 閺嗗倷绗夐幒銉р€栨禒璁圭礉閻╁瓨甯存稉銏犲瘶閿涘牆褰х紒鐔活吀閿涘绱滿3.4 閸愬秷藟 TX 閹诲繗鍫粭?*/
static void zt_mac_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control,
		      struct sk_buff *skb)
{
	struct zt_dev *z = zt_from_hw(hw);

	if (!READ_ONCE(z->alive)) {
		z->tx_dropped++;
		ieee80211_free_txskb(hw, skb);
		return;
	}
	skb_queue_tail(&z->txq, skb);
	schedule_work(&z->tx_work);
}

static void zt_mac_configure_filter(struct ieee80211_hw *hw, unsigned int changed_flags,
				    unsigned int *total_flags, u64 multicast)
{
	/* M3.1: driver does no hardware filtering - let mac80211 filter in software */
	*total_flags = 0;
}

/* M3.3：关联状态观测（认证/关联是否走通，先看 bss_info 回调） */
static void zt_mac_bss_info_changed(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				    struct ieee80211_bss_conf *info, u64 changed)
{
	struct zt_dev *z = zt_from_hw(hw);

	dev_info(&z->intf->dev, "bss_info: changed=%#llx assoc=%d aid=%u bssid=%pM\n",
		 changed, vif->cfg.assoc, (unsigned int)vif->cfg.aid, info->bssid);

	/*
	 * 实验（sta_add_en，默认 0）：关联完成后把站点注册给固件，
	 * 让数据帧描述符能带上固件分配的 sta_idx（见 re/REPORT_TX_SESSION.md）。
	 * 注意：**断开的清理不受开关影响** —— 否则运行期把 sta_add_en 改成 0 再重连，
	 * 会留下一个"仍然有效"的本地会话（sta_valid=true），描述符继续带旧 sta_idx。
	 */
	if (!(changed & BSS_CHANGED_ASSOC))
		return;
	if (vif->cfg.assoc) {
		if (sta_add_en)
			zt_sta_add(z, vif->cfg.aid, info->bssid);
	} else {
		zt_sta_del(z);
	}
}
/* ------------------------------------------------------------------ M3.2 扫描 */

/*
 * 扫描前置序列，逐步照抄厂商抓包（第 9 轮实测的时序）：
 *   SET_IDLE(0) → 0x0104(1) → SET_FILTER(98860215) → SET_FILTER(88860215)
 *   → 0x0104(0) → SET_IDLE(1) → SET_IDLE(0) → 0x0104(1) → SET_FILTER(98860215)
 * 每条都要等到对应 CFM 再发下一条。
 */
static int zt_scan_setup(struct zt_dev *z)
{
	static const u8 f1[4] = { 0x98, 0x86, 0x02, 0x15 };
	static const u8 f2[4] = { 0x88, 0x86, 0x02, 0x15 };
	u8 v;

	v = 0;
	if (zt_cmd_fifo(z, 0x0022, &v, 1, 0x0023, 1000))
		return -EIO;
	v = 1;
	if (zt_cmd_fifo(z, 0x0104, &v, 1, 0x0105, 1000))
		return -EIO;
	if (zt_cmd_fifo(z, 0x000e, f1, 4, 0x000f, 1000))
		return -EIO;
	if (zt_cmd_fifo(z, 0x000e, f2, 4, 0x000f, 1000))
		return -EIO;
	v = 0;
	if (zt_cmd_fifo(z, 0x0104, &v, 1, 0x0105, 1000))
		return -EIO;
	v = 1;
	if (zt_cmd_fifo(z, 0x0022, &v, 1, 0x0023, 1000))
		return -EIO;
	v = 0;
	if (zt_cmd_fifo(z, 0x0022, &v, 1, 0x0023, 1000))
		return -EIO;
	v = 1;
	if (zt_cmd_fifo(z, 0x0104, &v, 1, 0x0105, 1000))
		return -EIO;
	if (zt_cmd_fifo(z, 0x000e, f1, 4, 0x000f, 1000))
		return -EIO;
	return 0;
}

static void zt_scan_work(struct work_struct *w)
{
	struct zt_dev *z = container_of(w, struct zt_dev, scan_work);
	bool aborted = false, done = false;
	u8 chan[12];
	int i;

	mutex_lock(&z->lock);
	if (!READ_ONCE(z->alive)) {
		aborted = true;
		goto out;
	}
	/* RX 注入已改为常开，这些计数会跨扫描累计；不清零就无法用它们判断
	 * "本次扫描有没有收到响应"（曾因此误判为没有发射）。 */
	z->tx_frames = 0;
	z->tx_probes = 0;
	z->scan_probe_skip = 0;
	z->scan_ch_5g = 0;
	z->rx_frames_5g = 0;
	z->rx_beacons = 0;
	z->rx_probe_resp = 0;
	z->rx_freq_desc = 0;
	z->rx_freq_fb = 0;
	z->rx_freq_ne_scan = 0;
	z->rx_rssi_fb = 0;
	memset(z->rx_type_cnt, 0, sizeof(z->rx_type_cnt));
	dev_info(&z->intf->dev, "scan: 开始，%d 个信道\n", z->scan_nfreqs);
	if (zt_scan_setup(z)) {
		dev_warn(&z->intf->dev, "scan: 前置序列失败\n");
		aborted = true;
		goto out;
	}
	WRITE_ONCE(z->scan_active, true);
	for (i = 0; i < z->scan_nfreqs; i++) {
		u16 f = z->scan_freqs[i];

		if (!READ_ONCE(z->alive) || z->scan_aborted) {
			aborted = true;
			break;
		}
		memset(chan, 0, sizeof(chan));
		/*
		 * band 标志必须跟着频段走：厂商抓包里 81/81 条 SET_CHANNEL 都满足
		 * band = (freq > 2500) ? 1 : 0（2484→0、5180→1，见 re/REPORT_5GHZ.md §1.1）。
		 * 这里原先写死 0，等于给 5GHz 信道发"2.4GHz band + 5GHz 频率"这种自相矛盾的
		 * 请求：固件照样回 CFM，却不会真的切过去，于是 5GHz 扫描永远 beacon=0、
		 * 5GHz 关联无从发生（实测 2026-09-27；同一张卡在厂商 Windows 驱动下
		 * 5GHz 可跑 13~18 Mbit/s）。zt_set_channel() 一直是对的，只有扫描循环漏了。
		 */
		put_unaligned_le16(f > 2500 ? 1 : 0, chan + 0);
		put_unaligned_le16(f, chan + 2);
		put_unaligned_le16(f, chan + 4);
		put_unaligned_le16(20, chan + 10);
		WRITE_ONCE(z->scan_freq, f);
		if (f > 2500)
			z->scan_ch_5g++;
		if (zt_cmd_fifo(z, 0x0010, chan, 12, 0x0011, 1000))
			dev_warn(&z->intf->dev, "scan: 切换信道 %u 无 CFM\n", f);
		msleep(20);			/* 让信道先稳定（厂商 SET_CHANNEL→TX 中位 16ms） */
		if (scan_probe) {
			struct ieee80211_channel *ch = z->hw ?
				ieee80211_get_channel(z->hw->wiphy, f) : NULL;

			/* DFS/NO_IR 信道不做主动探测（合规；这些标志由 cfg80211 按
			 * 当前监管域打在信道表上）。默认是被动扫描，不受影响。 */
			if (ch && (ch->flags & (IEEE80211_CHAN_NO_IR |
						IEEE80211_CHAN_RADAR)))
				z->scan_probe_skip++;
			else if (scan_probe == 1)
				zt_scan_probe(z, f);
			else
				zt_scan_probe_vendor(z, f);
		}
		msleep(ZT_SCAN_DWELL_MS);
	}
	done = !aborted;
	if (done) {
		u8 v = 1;

		zt_cmd_fifo(z, 0x0022, &v, 1, 0x0023, 1000);	/* 回到 idle */
	}
out:
	WRITE_ONCE(z->scan_active, false);
	WRITE_ONCE(z->scan_freq, 0);
	WRITE_ONCE(z->scan_running, false);
	dev_info(&z->intf->dev,
		 "scan: 结束（%s）tx=%lu probe=%lu(跳过 %lu) beacon=%lu probe-resp=%lu 5G信道=%lu\n",
		 done ? "完成" : "中止", z->tx_frames, z->tx_probes,
		 z->scan_probe_skip, z->rx_beacons, z->rx_probe_resp,
		 z->scan_ch_5g);
	/* v0.3 阶段 B 观测：上报开关状态与计数（关掉时三项都是 0） */
	dev_info(&z->intf->dev,
		 "scan: TX status 上报 tx_status=%d probe=1/%d 已报=%lu 标ACK=%lu 末次ack=%d\n",
		 tx_status_on, tx_status_probe, z->tx_status_reports,
		 z->tx_status_ack, z->tx_status_last_ack);
	dev_info(&z->intf->dev,
		 "scan: RX type 0x0000=%lu 0x0004=%lu 0x0100=%lu 0x0300=%lu 其它=%lu(last=%#06x)\n",
		 z->rx_type_cnt[0], z->rx_type_cnt[1], z->rx_type_cnt[2],
		 z->rx_type_cnt[3], z->rx_type_cnt[4], z->last_other_type);
	dev_info(&z->intf->dev,
		 "scan: RX 状态来源 描述符频率=%lu 退回信道=%lu 描述符频率!=扫描信道=%lu RSSI兜底=%lu 5G帧=%lu\n",
		 z->rx_freq_desc, z->rx_freq_fb, z->rx_freq_ne_scan, z->rx_rssi_fb,
		 z->rx_frames_5g);
	dev_info(&z->intf->dev,
		 "scan: 未预期 IPC: %#06x x%lu | %#06x x%lu | %#06x x%lu | %#06x x%lu\n",
		 z->unk_ids[0], z->unk_cnt[0], z->unk_ids[1], z->unk_cnt[1],
		 z->unk_ids[2], z->unk_cnt[2], z->unk_ids[3], z->unk_cnt[3]);
	dev_info(&z->intf->dev,
		 "scan: 设备消息表(1): %#06x x%lu %#06x x%lu %#06x x%lu %#06x x%lu %#06x x%lu %#06x x%lu %#06x x%lu %#06x x%lu\n",
		 z->seen_ids[0], z->seen_cnt[0], z->seen_ids[1], z->seen_cnt[1],
		 z->seen_ids[2], z->seen_cnt[2], z->seen_ids[3], z->seen_cnt[3],
		 z->seen_ids[4], z->seen_cnt[4], z->seen_ids[5], z->seen_cnt[5],
		 z->seen_ids[6], z->seen_cnt[6], z->seen_ids[7], z->seen_cnt[7]);
	dev_info(&z->intf->dev,
		 "scan: 设备消息表(2): %#06x x%lu %#06x x%lu %#06x x%lu %#06x x%lu %#06x x%lu %#06x x%lu %#06x x%lu %#06x x%lu\n",
		 z->seen_ids[8], z->seen_cnt[8], z->seen_ids[9], z->seen_cnt[9],
		 z->seen_ids[10], z->seen_cnt[10], z->seen_ids[11], z->seen_cnt[11],
		 z->seen_ids[12], z->seen_cnt[12], z->seen_ids[13], z->seen_cnt[13],
		 z->seen_ids[14], z->seen_cnt[14], z->seen_ids[15], z->seen_cnt[15]);
	mutex_unlock(&z->lock);

	if (READ_ONCE(z->alive) && z->hw) {
		/* 本内核（7.0）的签名是 cfg80211_scan_info，不是 bool */
		struct cfg80211_scan_info info = { .aborted = aborted };

		ieee80211_scan_completed(z->hw, &info);
	}
}

static int zt_mac_hw_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			  struct ieee80211_scan_request *hw_req)
{
	struct zt_dev *z = zt_from_hw(hw);
	struct cfg80211_scan_request *req = &hw_req->req;
	int i;

	if (READ_ONCE(z->scan_running))
		return -EBUSY;
	if (req->n_channels == 0 || req->n_channels > ZT_MAX_SCAN_CH)
		return -EINVAL;

	mutex_lock(&z->lock);
	for (i = 0; i < req->n_channels; i++)
		z->scan_freqs[i] = req->channels[i]->center_freq;
	z->scan_nfreqs = req->n_channels;
	z->scan_aborted = false;
	WRITE_ONCE(z->scan_running, true);
	mutex_unlock(&z->lock);

	schedule_work(&z->scan_work);
	return 0;
}

/*
 * mac80211 的 ampdu_action。契约（mac80211.h @ampdu_action）：
 * TX_START 时必须先准备好会话，再调 `ieee80211_start_tx_ba_cb_irqsafe()`；
 * TX_STOP_CONT 之后调 `ieee80211_stop_tx_ba_cb_irqsafe()`；FLUSH 不需要回调。
 * RX 方向由固件自己管，这里只回成功（回错误会让 mac80211 认为设备不支持聚合）。
 */
static int zt_mac_ampdu_action(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			       struct ieee80211_ampdu_params *params)
{
	struct zt_dev *z = zt_from_hw(hw);
	struct ieee80211_sta *sta = params->sta;

	switch (params->action) {
	case IEEE80211_AMPDU_TX_START:
		/* 记录 mac80211 给的参数：这是"设备为何立刻掉线"的第一手线索 */
		dev_info(&z->intf->dev,
			 "ampdu_action TX_START: tid=%u ssn=%u buf_size=%u amsdu=%d\n",
			 params->tid, params->ssn, params->buf_size, params->amsdu);
		if (zt_ba_add(z, (u8)params->tid, params->buf_size, params->ssn))
			return -EIO;
		ieee80211_start_tx_ba_cb_irqsafe(vif, sta->addr, params->tid);
		return 0;
	case IEEE80211_AMPDU_TX_STOP_CONT:
		zt_ba_del(z, (u8)params->tid);
		ieee80211_stop_tx_ba_cb_irqsafe(vif, sta->addr, params->tid);
		return 0;
	case IEEE80211_AMPDU_TX_STOP_FLUSH:
	case IEEE80211_AMPDU_TX_STOP_FLUSH_CONT:
		zt_ba_del(z, (u8)params->tid);
		return 0;
	case IEEE80211_AMPDU_TX_OPERATIONAL:
		dev_info(&z->intf->dev, "AMPDU operational (tid=%u bufsz=%u)\n",
			 params->tid, params->buf_size);
		return 0;
	case IEEE80211_AMPDU_RX_START:
		z->ba_rx_start++;
		return 0;
	case IEEE80211_AMPDU_RX_STOP:
		z->ba_rx_stop++;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct ieee80211_ops zt_mac_ops = {
	.start = zt_mac_start,
	.stop = zt_mac_stop,
	.add_interface = zt_mac_add_interface,
	.remove_interface = zt_mac_remove_interface,
	.config = zt_mac_config,
	.tx = zt_mac_tx,
	.configure_filter = zt_mac_configure_filter,
	/* 本内核把 TXQ 路径定为必选：alloc_hw 会检查 wake_tx_queue 是否存在 */
	.wake_tx_queue = zt_mac_wake_tx_queue,
	.bss_info_changed = zt_mac_bss_info_changed,
	/* M3.2：被动扫描（只听 beacon），probe request 的 TX 留到 M3.4 */
	.hw_scan = zt_mac_hw_scan,
	/* 实验：A-MPDU 聚合（ampdu_en=1 时才会被 mac80211 调用） */
	.ampdu_action = zt_mac_ampdu_action,
	/* 鍗曚俊閬?STA 鍦烘櫙锛氱敤 mac80211 鎻愪緵鐨?chanctx 妯℃嫙瀹炵幇 */
	.add_chanctx = ieee80211_emulate_add_chanctx,
	.remove_chanctx = ieee80211_emulate_remove_chanctx,
	.change_chanctx = ieee80211_emulate_change_chanctx,
};

static void zt_mac_register(struct zt_dev *z)
{
	struct ieee80211_hw *hw;
	int ret;

	hw = ieee80211_alloc_hw(sizeof(struct zt_dev *), &zt_mac_ops);
	if (!hw) {
		dev_err(&z->intf->dev, "mac80211: alloc_hw failed\n");
		return;
	}
	*(struct zt_dev **)hw->priv = z;
	z->hw = hw;

	/*
	 * 声明 RX RSSI 的单位是 dBm。mac80211 的 ieee80211_bss_info_update() 只在
	 * 该标志存在时才把 rx_status->signal 换算成 mBm 交给 cfg80211，否则
	 * data.signal 恒为 0，cfg80211 就不发 NL80211_BSS_SIGNAL_MBM，
	 * 结果是 iw scan 里完全没有 "signal:" 行（实测过）。
	 */
	ieee80211_hw_set(hw, SIGNAL_DBM);
	hw->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	hw->wiphy->bands[NL80211_BAND_2GHZ] = &zt_band_2ghz;
	hw->wiphy->bands[NL80211_BAND_5GHZ] = &zt_band_5ghz;
	if (ht_cap_enable) {
		zt_band_5ghz.ht_cap = zt_ht_cap_5ghz;
		dev_info(&z->intf->dev,
			 "experimental: 5GHz HT cap 已声明（HT20/SGI，MCS0-15，未开聚合）\n");
	}
	if (ampdu_en) {
		/*
		 * A-MPDU 聚合：**组帧在驱动里做**，不是 mac80211 做。
		 * mac80211 只负责按对端能力/BA 会话决定"这一帧允许聚合"，
		 * 一次 `ieee80211_tx_dequeue()` 仍只给一个 MPDU；把它们拼进同一次
		 * bulk 传输（zt_tx_agg）才是驱动的活。厂商抓包实测：EP5 上
		 * 1608 / 3200 / 4808 字节的传输 = 1/2/3 个单元首尾相接。
		 */
		ieee80211_hw_set(hw, AMPDU_AGGREGATION);
		hw->max_tx_aggregation_subframes = ZT_AGG_SUBFRAMES;
		dev_info(&z->intf->dev,
			 "experimental: A-MPDU 聚合已启用（驱动组帧，TX 缓冲 %u 字节，最多 %u 子帧/传输）\n",
			 z->tx_buf_size, ZT_AGG_SUBFRAMES);
	}
	hw->wiphy->max_scan_ssids = 1;
	hw->queues = 4;
	/*
	 * 接口 MTU 上限。
	 *
	 * 2026-09-26 实测教训（两次反转，记录在此避免重犯）：
	 *   1) 一开以为是"设备只吃 ~1 KB 的帧"导致 TCP 大包停滞，于是把上限设成 900。
	 *      后来查明真正的根因是 RX URB 缓冲只有 1024 字节（已改为 2048），
	 *      修好后 MTU=1500 下下载完全正常，中位数约 4.2 Mbit/s，
	 *      反而比 MTU=900 的约 3.2 Mbit/s 更高。
	 *   2) 上限压到 900 还有一个严重后果：**IPv6 不可用**——IPv6 要求 MTU ≥1280，
	 *      这也是 mac80211 的硬下限，接口拿不到 IPv6 地址。
	 * 结论：不要给这块设备设非标准的 MTU 上限，保持 1500。
	 * ping 在 payload >905 字节时失败是**另一个现象，已于 0.3.2 定案**：那是驱动
	 * TX 侧的 988 字节帧长上限（请求帧被驱动丢掉），见文件头 ZT_TX_BUF_SIZE 注释；
	 * 与 MTU 设置无关，也不影响 TCP 数据面。
	 */
	hw->max_mtu = ZT_MAX_MTU;
	SET_IEEE80211_PERM_ADDR(hw, z->mac);
	/*
	 * D10 修复：必须设置 wiphy 的父设备。否则 wiphy_dev(wiphy) 为 NULL，
	 * userspace 通过 ethtool 取驱动信息时 cfg80211_get_drvinfo() 会空指针崩溃
	 * （实测 NetworkManager 在 wlan0 出现后立即触发，进程带关中断退出，导致整机
	 *  用户态卡死、"能 ping 不能 SSH"）。
	 */
	SET_IEEE80211_DEV(hw, &z->intf->dev);

	ret = ieee80211_register_hw(hw);
	if (ret) {
		dev_err(&z->intf->dev, "mac80211: register_hw failed: %d\n", ret);
		ieee80211_free_hw(hw);
		z->hw = NULL;
		return;
	}
	dev_info(&z->intf->dev, "mac80211 registered (M3.1) - wlan0 should appear\n");
}

static void zt_mac_unregister(struct zt_dev *z)
{
	if (!z->hw)
		return;
	WRITE_ONCE(z->mac_started, false);	/* 注销后不再注帧，避免 race */
	ieee80211_unregister_hw(z->hw);
	ieee80211_free_hw(z->hw);
	z->hw = NULL;
}


/* ------------------------------------------------------------------ probe */

static int zt_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_host_interface *alt = intf->cur_altsetting;
	struct zt_dev *z;
	int i, ret;

	dev_info(&intf->dev, "probing %04x:%04x (iface %d)\n",
		 le16_to_cpu(udev->descriptor.idVendor),
		 le16_to_cpu(udev->descriptor.idProduct), alt->desc.bInterfaceNumber);

	z = kzalloc(sizeof(*z), GFP_KERNEL);
	if (!z)
		return -ENOMEM;
	z->udev = udev;
	z->intf = intf;
	z->mac = kmalloc(6, GFP_KERNEL);
	if (!z->mac) {
		kfree(z);
		return -ENOMEM;
	}
	mutex_init(&z->lock);
	init_waitqueue_head(&z->rx_wait);
	init_completion(&z->rx_done);
	init_completion(&z->ntf_done);
	z->alive = true;		/* 从这里开始才允许 USB IO */
	INIT_DELAYED_WORK(&z->hb_work, zt_hb_work);
	INIT_WORK(&z->scan_work, zt_scan_work);
	INIT_WORK(&z->tx_work, zt_tx_work);
	skb_queue_head_init(&z->txq);
	spin_lock_init(&z->txq_lock);

	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *ep = &alt->endpoint[i].desc;

		if (usb_endpoint_num(ep) == EP_OUT_NUM && usb_endpoint_dir_out(ep) &&
		    usb_endpoint_is_bulk_out(ep))
			z->ep_out = ep->bEndpointAddress;
		if (usb_endpoint_num(ep) == EP_IN_NUM && usb_endpoint_dir_in(ep) &&
		    usb_endpoint_is_bulk_in(ep))
			z->ep_in = ep->bEndpointAddress;
		/* M3.4：EP5-OUT 是数据发送端点（抓包实测） */
		if (usb_endpoint_num(ep) == EP_TX_NUM && usb_endpoint_dir_out(ep) &&
		    usb_endpoint_is_bulk_out(ep))
			z->ep_tx = ep->bEndpointAddress;
		/* C1：EP2-IN 的通知通道（厂商 TX 期间一直在读） */
		if (usb_endpoint_num(ep) == EP_NTF_NUM && usb_endpoint_dir_in(ep)) {
			z->ep_ntf = ep->bEndpointAddress;
			z->ntf_is_int = usb_endpoint_xfer_int(ep);
			z->ntf_interval = ep->bInterval;
		}
		dev_info(&intf->dev, "  ep %#04x %s\n", ep->bEndpointAddress,
			 usb_endpoint_dir_in(ep) ? "IN" : "OUT");
	}
	if (!z->ep_out || !z->ep_in) {
		ret = -ENODEV;
		goto err;
	}
	if (!z->ep_tx)
		dev_warn(&intf->dev, "未找到 EP%u-OUT，TX 数据路径不可用\n", EP_TX_NUM);

	z->tx_buf_size = ampdu_en ? ZT_AGG_BUF_SIZE : ZT_TX_BUF_SIZE;
	z->tx = kmalloc(z->tx_buf_size, GFP_KERNEL);
	z->pl = kmalloc(MAX_PAYLOAD, GFP_KERNEL);
	z->rx = kmalloc(ZT_RX_BUF_SIZE, GFP_KERNEL);
	z->txd = kmalloc(z->tx_buf_size, GFP_KERNEL);
	if (!z->tx || !z->pl || !z->rx || !z->txd) {
		ret = -ENOMEM;
		goto err;
	}
	ret = kfifo_alloc(&z->rx_fifo, RX_FIFO_SIZE, GFP_KERNEL);
	if (ret)
		goto err;

	usb_set_intfdata(intf, z);

	if (do_boot) {
		ret = zt_boot(z);
		if (ret) {
			dev_err(&intf->dev, "firmware boot failed: %d\n", ret);
			goto err_kfifo;
		}
		dev_info(&intf->dev, "firmware loaded\n");
	} else {
		dev_info(&intf->dev, "do_boot=0: reusing already running firmware\n");
	}

	if (do_init) {
		ret = zt_run_init(z);
		if (ret) {
			dev_err(&intf->dev, "init sequence failed: %d\n", ret);
			goto err_kfifo;
		}
	}

	z->last_hb = jiffies;
	ret = zt_rx_start(z);
	if (ret) {
		dev_err(&intf->dev, "rx urb start failed: %d\n", ret);
		goto err_kfifo;
	}
	if (zt_ntf_start(z))
		dev_warn(&intf->dev, "EP%u-IN 通知通道启动失败（继续，但 TX 吞吐可能受影响）\n",
			 EP_NTF_NUM);
	schedule_delayed_work(&z->hb_work, msecs_to_jiffies(HB_INTERVAL_MS));

	z->misc.minor = MISC_DYNAMIC_MINOR;
	z->misc.name = "zt9612";
	z->misc.fops = &zt_fops;
	z->misc.mode = 0660;
	ret = misc_register(&z->misc);
	if (ret)
		dev_warn(&intf->dev, "misc_register failed: %d\n", ret);
	else
		z->misc_ok = true;

	dev_info(&intf->dev, "M1+M2 done: firmware running, /dev/zt9612 %s\n",
		 z->misc_ok ? "ready" : "unavailable");

	zt_dbg_init(z);
	zt_mac_register(z);
	return 0;

err_kfifo:
	usb_set_intfdata(intf, NULL);
	WRITE_ONCE(z->alive, false);
	z->scan_aborted = true;
	cancel_work_sync(&z->scan_work);
	cancel_delayed_work_sync(&z->hb_work);
	zt_ntf_stop(z);
	if (zt_rx_stop(z))		/* 泄漏实例，避免 use-after-free */
		return ret;
	kfifo_free(&z->rx_fifo);
err:
	kfree(z->tx);
	kfree(z->txd);
	kfree(z->pl);
	kfree(z->rx);
	kfree(z->mac);
	kfree(z);
	return ret;
}

static void zt_disconnect(struct usb_interface *intf)
{
	struct zt_dev *z = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);
	if (!z)
		return;

	/* 顺序很重要（D9）：
	 * 1) 先置 alive=0 —— 此后 zt_send/zt_recv/heartbeat 一律不再发起 USB IO；
	 * 2) 停后台工作；
	 * 3) 撤掉用户态与 mac80211 入口；
	 * 4) 回收 RX URB（有界等待，见 zt_rx_stop）。
	 */
	WRITE_ONCE(z->alive, false);
	z->scan_aborted = true;
	cancel_work_sync(&z->scan_work);	/* 扫描可能正在切信道，先等它退出 */
	cancel_work_sync(&z->tx_work);		/* 在途 TX 提交也要收尾 */
	skb_queue_purge(&z->txq);
	spin_lock_irq(&z->txq_lock);
	z->txq_n = 0;
	spin_unlock_irq(&z->txq_lock);
	cancel_delayed_work_sync(&z->hb_work);
	wake_up_interruptible_all(&z->rx_wait);	/* 唤醒阻塞中的 read() */

	if (z->misc_ok) {
		misc_deregister(&z->misc);
		z->misc_ok = false;
	}
	if (zt_dbg_root) {
		debugfs_remove_recursive(zt_dbg_root);
		zt_dbg_root = NULL;
	}
	zt_mac_unregister(z);

	zt_ntf_stop(z);		/* 通知通道：同样有界等待 */
	if (zt_rx_stop(z)) {
		dev_warn(&intf->dev, "disconnected (teardown incomplete, instance leaked)\n");
		return;
	}
	kfifo_free(&z->rx_fifo);
	kfree(z->tx);
	kfree(z->txd);
	kfree(z->pl);
	kfree(z->rx);
	kfree(z->mac);
	kfree(z);
	dev_info(&intf->dev, "disconnected\n");
}

static const struct usb_device_id zt_id_table[] = {
	{ USB_DEVICE(ZT_VID, ZT_PID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, zt_id_table);

static struct usb_driver zt_driver = {
	.name		= DRV_NAME,
	.id_table	= zt_id_table,
	.probe		= zt_probe,
	.disconnect	= zt_disconnect,
};

module_usb_driver(zt_driver);

MODULE_AUTHOR("Spkicn <Spkicn@users.noreply.github.com>");
MODULE_DESCRIPTION("ZT9612U (ZTOP/ACEV100) USB WiFi driver - mac80211 station: 2.4/5 GHz scan, connect, WPA2");
MODULE_VERSION("0.3.2");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE("zt9612_fw.bin");
MODULE_FIRMWARE("zt9612_settings.bin");

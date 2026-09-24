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

#define ZT_BLOCK_SIZE	488
#define SETTINGS_ADDR	0x210CE700
#define MAX_FRAME	1024
#define MAX_PAYLOAD	(MAX_FRAME - 8)
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
 * 主动发送尚未验证成功（收不到 probe response），因此默认 0。
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
			   z->rx, MAX_FRAME, &got, timeout_ms);
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
		if (flen < 8 || flen > MAX_FRAME) {
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
	 * （抓包实测：00 b4 01 1a 00 12 64 00，即 type=0/STA + 本机 MAC + p2p=0）。
	 * 早期版本只发 7 字节且 MAC 错位，固件可能因此拒绝建 vif。 */
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
	if (req.len < 9 || req.len > MAX_FRAME)
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
	if (count < 9 || count > MAX_FRAME)
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
}

/*
 * 把一条 802.11 帧交给设备发送：EP5-OUT。
 * 线上格式与其它帧一致：WLAN 头 + 28 字节描述符 + 802.11 帧，
 * 其中 WLAN 头的 hlen = 28 + 帧长（抓包实测：111 字节的 probe request → hlen=139）。
 * 第 9 轮教训：漏掉 WLAN 头直接发描述符会让固件断言、设备复位回 ROM 模式。
 */
static int zt_tx_frame(struct zt_dev *z, const u8 *frame, u16 flen)
{
	u8 *buf = z->txd;
	u8 *d;
	u16 total;
	int ret, sent = 0;

	if (!READ_ONCE(z->alive) || !z->ep_tx || !buf)
		return -ENODEV;
	if (flen < 10 || flen > MAX_FRAME - TX_DESC_LEN - 8)
		return -EINVAL;

	if (tx_variant == 2) {
		/* 变体 2：不带描述符，只发 WLAN 头 + 帧 */
		memcpy(buf, "WLAN", 4);
		put_unaligned_le16(flen, buf + 4);
		put_unaligned_le16(TX_TYPE_DATA, buf + 6);
		memcpy(buf + 8, frame, flen);
		total = 8 + flen;
	} else {
		d = buf + 8;
		memset(d, 0, TX_DESC_LEN);
		put_unaligned_le32(tx_variant == 1 ? 0 : 0xffffffff, d + 0);
		put_unaligned_le16(flen, d + 4);
		put_unaligned_le16(0x0700, d + 6);
		put_unaligned_le16(0xff00, d + 8);
		put_unaligned_le16(0x0005, d + 10);
		put_unaligned_le16(z->tx_seq++, d + 14);
		put_unaligned_le16(0x003f, d + 26);
		memcpy(d + TX_DESC_LEN, frame, flen);

		memcpy(buf, "WLAN", 4);
		put_unaligned_le16(TX_DESC_LEN + flen, buf + 4);
		put_unaligned_le16(TX_TYPE_DATA, buf + 6);
		total = 8 + TX_DESC_LEN + flen;
	}
	/*
	 * 抓包实测：EP5 的传输长度是 8 的倍数（147→152、138→144），不足处补零。
	 * 变体 3 则补齐到 512（批量端点包长的整数倍）。
	 */
	if (tx_variant == 3) {
		while (total & 0x1ff)
			buf[total++] = 0;
	} else {
		while (total & 7)
			buf[total++] = 0;
	}

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
	z->rx_buf = kmalloc(MAX_FRAME, GFP_KERNEL);
	if (!z->rx_buf)
		return -ENOMEM;

	usb_fill_bulk_urb(z->rx_urb, z->udev, usb_rcvbulkpipe(z->udev, z->ep_in),
			  z->rx_buf, MAX_FRAME, zt_rx_complete, z);
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
			if (flen > MAX_FRAME) {		/* 脏数据：丢掉长度头继续 */
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

	if (count < 8 || count > MAX_FRAME)
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
	dev_kfree_skb_any(skb);
}

static void zt_tx_work(struct work_struct *w)
{
	struct zt_dev *z = container_of(w, struct zt_dev, tx_work);
	struct ieee80211_txq *pend[ZT_MAX_PEND_TXQ];
	unsigned long flags;
	struct sk_buff *skb;
	int n, i;

	for (;;) {
		/* 1) 传统 .tx 路径登记进来的 skb */
		while ((skb = skb_dequeue(&z->txq)))
			zt_tx_one(z, z->hw, skb, true);

		/* 2) TXQ 路径：取出本轮被唤醒的队列，逐个 dequeue 到空 */
		spin_lock_irqsave(&z->txq_lock, flags);
		n = z->txq_n;
		memcpy(pend, z->txq_pend, n * sizeof(pend[0]));
		z->txq_n = 0;
		spin_unlock_irqrestore(&z->txq_lock, flags);
		if (!n)
			return;
		for (i = 0; i < n; i++) {
			while ((skb = ieee80211_tx_dequeue(z->hw, pend[i])))
				zt_tx_one(z, z->hw, skb, false);
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
		put_unaligned_le16(0, chan + 0);
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
	hw->wiphy->max_scan_ssids = 1;
	hw->queues = 4;
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
		dev_info(&intf->dev, "  ep %#04x %s\n", ep->bEndpointAddress,
			 usb_endpoint_dir_in(ep) ? "IN" : "OUT");
	}
	if (!z->ep_out || !z->ep_in) {
		ret = -ENODEV;
		goto err;
	}
	if (!z->ep_tx)
		dev_warn(&intf->dev, "未找到 EP%u-OUT，TX 数据路径不可用\n", EP_TX_NUM);

	z->tx = kmalloc(MAX_FRAME, GFP_KERNEL);
	z->pl = kmalloc(MAX_PAYLOAD, GFP_KERNEL);
	z->rx = kmalloc(MAX_FRAME, GFP_KERNEL);
	z->txd = kmalloc(MAX_FRAME, GFP_KERNEL);
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
MODULE_VERSION("0.2.0");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE("zt9612_fw.bin");
MODULE_FIRMWARE("zt9612_settings.bin");

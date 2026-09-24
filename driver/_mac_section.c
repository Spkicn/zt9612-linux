/* SPDX-License-Identifier: GPL-2.0-only
 *
 * zt9612.c 的内联 mac80211 段（M3.1）——只读摘录，不参与编译。
 *
 * 本文件不是构建输入（Makefile 只编译 zt9612.o）。它的唯一作用是方便单独阅读
 * mac80211 部分，改它不会影响编译结果。
 *
 * 保持同步的办法：改动 zt9612.c 时，从该文件里"mac80211 (M3.1)"那段段首注释开始，
 * 到函数 zt_mac_unregister() 的结束大括号为止，整段原样复制到本文件末尾。
 *
 * 历史教训：这份摘录曾经停留在"首版缺 configure_filter/wake_tx_queue/chanctx"的
 * 旧版本，与 zt9612.c 不一致（见 docs/04 与 docs/07）。
 */

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

static struct zt_dev *zt_from_hw(struct ieee80211_hw *hw)
{
	return *(struct zt_dev **)hw->priv;
}

static int zt_mac_start(struct ieee80211_hw *hw)
{
	struct zt_dev *z = zt_from_hw(hw);

	dev_info(&z->intf->dev, "mac80211: start\n");
	return 0;
}

static void zt_mac_stop(struct ieee80211_hw *hw, bool suspend)
{
	struct zt_dev *z = zt_from_hw(hw);

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

static int zt_mac_config(struct ieee80211_hw *hw, int radio_idx, u32 changed)
{
	return 0;
}

/* M3.1閿涙瓖X 閺嗗倷绗夐幒銉р€栨禒璁圭礉閻╁瓨甯存稉銏犲瘶閿涘牆褰х紒鐔活吀閿涘绱滿3.4 閸愬秷藟 TX 閹诲繗鍫粭?*/
static void zt_mac_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control,
		      struct sk_buff *skb)
{
	struct zt_dev *z = zt_from_hw(hw);

	z->tx_dropped++;
	ieee80211_free_txskb(hw, skb);
}

static void zt_mac_configure_filter(struct ieee80211_hw *hw, unsigned int changed_flags,
				    unsigned int *total_flags, u64 multicast)
{
	/* M3.1: driver does no hardware filtering - let mac80211 filter in software */
	*total_flags = 0;
}

/* M3.1: modern TX API is mandatory in this kernel; dequeue and drop for now (data path = M3.4) */
static void zt_mac_wake_tx_queue(struct ieee80211_hw *hw, struct ieee80211_txq *txq)
{
	struct zt_dev *z = zt_from_hw(hw);
	struct sk_buff *skb;

	while ((skb = ieee80211_tx_dequeue(hw, txq))) {
		z->tx_dropped++;
		ieee80211_free_txskb(hw, skb);
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
	.wake_tx_queue = zt_mac_wake_tx_queue,
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

	hw->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	hw->wiphy->bands[NL80211_BAND_2GHZ] = &zt_band_2ghz;
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
	ieee80211_unregister_hw(z->hw);
	ieee80211_free_hw(z->hw);
	z->hw = NULL;
}

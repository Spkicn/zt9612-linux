// SPDX-License-Identifier: GPL-2.0
/*
 * zt9612_fw.c - M1: firmware loader for ZT9612U (ZTOP / 兆通微 ACEV100) USB WiFi adapter
 *
 * 协议来自 USB 抓包 + 静态逆向，逐字节验证（见 zt9612-linux/re/REPORT*.md）：
 *   握手 0x0201 -> 488B 块 (0x0200) xN -> 末块 0x0204(整段 XOR16) -> 配置块 -> RUN(0x0200 sub 0x05)
 * 帧格式: "WLAN" + u16 hlen + u16 type + payload[hlen]
 * 端点: OUT 0x08 (EP8), IN 0x84 (EP4)
 *
 * 本模块只做固件装载 + 观察 IPC 帧，不接管网络栈（M2/M3 再做）。
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include <linux/unaligned.h>

#define DRV_NAME	"zt9612_fw"

#define ZT_VID		0x350B
#define ZT_PID		0x9612
#define EP_OUT_NUM	8
#define EP_IN_NUM	4

#define ZT_BLOCK_SIZE	488
#define SETTINGS_ADDR	0x210CE700
#define MAX_FRAME	1024
#define MAX_PAYLOAD	(MAX_FRAME - 8)

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

static int ipc_poll_ms = 2000;
module_param(ipc_poll_ms, int, 0644);
MODULE_PARM_DESC(ipc_poll_ms, "poll EP4-IN for IPC frames after boot (ms)");

struct zt_dev {
	struct usb_device	*udev;
	struct usb_interface	*intf;
	u8			ep_out;
	u8			ep_in;
	u8			*tx;	/* whole frame */
	u8			*pl;	/* payload build buffer */
	u8			*rx;
};

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

/* 读一帧；成功返回 0 并填充 type/len（payload 在 z->rx+8） */
static int zt_recv(struct zt_dev *z, u16 *type, int *len, int timeout_ms)
{
	int ret, got = 0;

	ret = usb_bulk_msg(z->udev, usb_rcvbulkpipe(z->udev, z->ep_in),
			   z->rx, MAX_FRAME, &got, timeout_ms);
	if (ret)
		return ret;
	if (got < 8 || memcmp(z->rx, "WLAN", 4))
		return -EPROTO;
	*len = got;
	*type = (u16)z->rx[6] | ((u16)z->rx[7] << 8);
	return 0;
}

/* 等待指定 type（want_sub < 0 表示不检查 payload[0]） */
static int zt_wait(struct zt_dev *z, u16 want_type, int want_sub, int timeout_ms)
{
	unsigned long end = jiffies + msecs_to_jiffies(timeout_ms);
	u16 type;
	int len, ret;

	while (time_before(jiffies, end)) {
		ret = zt_recv(z, &type, &len, 200);
		if (ret)
			continue;
		if (type == want_type &&
		    (want_sub < 0 || (len > 8 && z->rx[8] == (u8)want_sub)))
			return 0;
		dev_dbg(&z->intf->dev, "  (skip frame type=%#06x len=%d)\n", type, len);
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
		bool last = (i == nblk - 1);

		if (last) {
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
			/* 等写完成确认 */
			ret = zt_wait(z, T_FW_WRITE, SUB_FW_ACK, 2000);
			if (ret) {
				dev_warn(&z->intf->dev, "no ack for addr %#010x\n", addr);
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
	dev_info(&z->intf->dev, "  section addr=%#010x len=%u (%u blocks) checksum=%#06x ok\n",
		 addr, len, nblk, cs);
	return 0;
}

static int zt_boot(struct zt_dev *z)
{
	const struct firmware *fw, *st = NULL;
	int ret, count, i;
	u8 run[2 + 6 * 11];
	u32 run_len = 0;

	ret = request_firmware(&fw, "zt9612_fw.bin", &z->intf->dev);
	if (ret) {
		dev_err(&z->intf->dev, "request_firmware(zt9612_fw.bin) failed: %d\n", ret);
		return ret;
	}
	if (fw->size < 9 || get_unaligned_le16(fw->data) != ZT_MAGIC) {
		dev_err(&z->intf->dev, "bad firmware magic\n");
		ret = -EINVAL;
		goto out_fw;
	}
	dev_info(&z->intf->dev, "firmware: pid=%#06x sections=%u size=%zu\n",
		 get_unaligned_le32(fw->data + 4), fw->data[8], fw->size);
	count = fw->data[8];
	if (count > 6 || fw->size < 9 + count * 19) {
		ret = -EINVAL;
		goto out_fw;
	}

	/* 1) 握手 */
	z->pl[0] = SUB_FW_START;
	ret = zt_send(z, T_FW_START, z->pl, 1);
	if (ret)
		goto out_fw;
	ret = zt_wait(z, T_FW_WRITE, SUB_FW_READY, 2000);
	dev_info(&z->intf->dev, "device %s\n", ret ? "did NOT answer hello" : "ready (hello ack)");
	if (ret)
		ret = 0;	/* 继续尝试 */

	/* 2) 各段固件 */
	for (i = 0; i < count; i++) {
		const u8 *e = fw->data + 9 + i * 19;
		u32 addr = get_unaligned_le32(e + 7);
		u32 len = get_unaligned_le32(e + 11);
		u32 foff = get_unaligned_le32(e + 15);

		if ((u64)foff + len > fw->size) {
			dev_err(&z->intf->dev, "section %d out of range\n", i);
			ret = -EINVAL;
			goto out_fw;
		}
		ret = zt_write_blocks(z, addr, fw->data + foff, len);
		if (ret)
			goto out_fw;
		/* run 表项: {u32 addr; u32 len; u16 type; u8 idx} = 11 字节 */
		put_unaligned_le32(addr, run + 2 + run_len * 11 + 0);
		put_unaligned_le32(len, run + 2 + run_len * 11 + 4);
		put_unaligned_le16(get_unaligned_le16(e + 1), run + 2 + run_len * 11 + 8);
		run[2 + run_len * 11 + 10] = e[0];
		run_len++;
	}

	/* 3) 配置块 */
	ret = request_firmware(&st, "zt9612_settings.bin", &z->intf->dev);
	if (!ret) {
		ret = zt_write_blocks(z, SETTINGS_ADDR, st->data, st->size);
		release_firmware(st);
		if (ret)
			goto out_fw;
	} else {
		dev_warn(&z->intf->dev, "no zt9612_settings.bin, skip settings\n");
		ret = 0;
	}

	/* 4) RUN */
	run[0] = SUB_RUN;
	run[1] = (u8)run_len;
	dev_info(&z->intf->dev, "sending RUN (%u sections)\n", run_len);
	ret = zt_send(z, T_FW_WRITE, run, 2 + run_len * 11);

out_fw:
	release_firmware(fw);
	return ret;
}

static void zt_poll_ipc(struct zt_dev *z, int ms)
{
	unsigned long end = jiffies + msecs_to_jiffies(ms);
	u16 type;
	int len, n = 0, ipc = 0;

	while (time_before(jiffies, end)) {
		if (zt_recv(z, &type, &len, 200))
			continue;
		n++;
		if (type == T_IPC || type == 0x0300)
			ipc++;
		if (n <= 12)
			dev_info(&z->intf->dev, "  <- type=%#06x len=%d %*ph\n",
				 type, len - 8, min(16, len - 8), z->rx + 8);
	}
	dev_info(&z->intf->dev, "received %d frames (%d IPC) -> %s\n",
		 n, ipc, ipc ? "FIRMWARE RUNNING" : "no IPC seen");
}

static int zt_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_host_interface *alt = intf->cur_altsetting;
	struct zt_dev *z;
	int i, ret;

	dev_info(&intf->dev, "probing %04x:%04x (iface %d, %d endpoints)\n",
		 le16_to_cpu(udev->descriptor.idVendor),
		 le16_to_cpu(udev->descriptor.idProduct),
		 alt->desc.bInterfaceNumber, alt->desc.bNumEndpoints);

	z = kzalloc(sizeof(*z), GFP_KERNEL);
	if (!z)
		return -ENOMEM;
	z->udev = udev;
	z->intf = intf;
	z->ep_out = 0;
	z->ep_in = 0;

	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *ep = &alt->endpoint[i].desc;

		if (!usb_endpoint_is_bulk_out(ep) && !usb_endpoint_is_bulk_in(ep))
			continue;
		if (usb_endpoint_num(ep) == EP_OUT_NUM && usb_endpoint_dir_out(ep))
			z->ep_out = ep->bEndpointAddress;
		if (usb_endpoint_num(ep) == EP_IN_NUM && usb_endpoint_dir_in(ep))
			z->ep_in = ep->bEndpointAddress;
		dev_info(&intf->dev, "  ep %#04x bulk %s maxpkt=%d\n", ep->bEndpointAddress,
			 usb_endpoint_dir_in(ep) ? "IN" : "OUT", usb_endpoint_maxp(ep));
	}
	if (!z->ep_out || !z->ep_in) {
		dev_err(&intf->dev, "expected bulk endpoints not found\n");
		ret = -ENODEV;
		goto err_free;
	}

	z->tx = kmalloc(MAX_FRAME, GFP_KERNEL);
	z->pl = kmalloc(MAX_PAYLOAD, GFP_KERNEL);
	z->rx = kmalloc(MAX_FRAME, GFP_KERNEL);
	if (!z->tx || !z->pl || !z->rx) {
		ret = -ENOMEM;
		goto err_free;
	}

	usb_set_intfdata(intf, z);
	ret = usb_set_interface(udev, alt->desc.bInterfaceNumber, 0);
	if (ret)
		dev_warn(&intf->dev, "usb_set_interface: %d\n", ret);

	dev_info(&intf->dev, "loading firmware ...\n");
	ret = zt_boot(z);
	if (ret) {
		dev_err(&intf->dev, "firmware boot failed: %d\n", ret);
		goto err_unset;
	}
	dev_info(&intf->dev, "boot sequence sent\n");

	if (ipc_poll_ms > 0)
		zt_poll_ipc(z, ipc_poll_ms);

	dev_info(&intf->dev, "M1 done (firmware loaded, IPC observed)\n");
	return 0;

err_unset:
	usb_set_intfdata(intf, NULL);
err_free:
	kfree(z->tx);
	kfree(z->pl);
	kfree(z->rx);
	kfree(z);
	return ret;
}

static void zt_disconnect(struct usb_interface *intf)
{
	struct zt_dev *z = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);
	if (!z)
		return;
	kfree(z->tx);
	kfree(z->pl);
	kfree(z->rx);
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

MODULE_AUTHOR("dhs-agent");
MODULE_DESCRIPTION("ZT9612U (ZTOP/ACEV100) firmware loader - M1");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE("zt9612_fw.bin");
MODULE_FIRMWARE("zt9612_settings.bin");

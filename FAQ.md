# FAQ — ZT9612U (ZTOP / ACEV100) Linux 驱动

> Question / Answer 形式，按常见程度排序。文档里的路径都以仓库根目录为基准。

-----

**Question:** 这个驱动现在能用吗？插上就能上网吗？

**Answer:** 还不能。当前进度：

| 里程碑 | 状态 |
|---|---|
| M1 内核态固件装载 | ✅ 已实现并在实机验证 |
| M2 IPC 初始化 + `/dev/zt9612` 通道 | ✅ 已实现并在实机验证（IPC 往返成功） |
| M3.1 mac80211 注册 / `wlan0` | 🟡 代码已写完，**尚未在实机验证** |
| M3.2 扫描 / M3.3 关联 / M3.4 数据面 | ⬜ 未开始 |

所以现在加载模块后，你能得到的是：芯片被点亮、固件在跑、可以通过 `/dev/zt9612`
收发原始 IPC 帧。**还不会有可用的 `wlan0`。**

-----

**Question:** 支持哪些网卡？

**Answer:** 只有 VID:PID = `350b:9612` 的 ZT9612U / ACEV100 模组（网卡模式）。
完整 ID 说明见 `supported-device-IDs`。同厂 `350b:9101`、`350b:9611` 不适用。

-----

**Question:** 插上以后只看到一个光驱（`350b:f179`），没有网卡，怎么回事？

**Answer:** 这是设计如此 —— 设备默认枚举成"驱动光盘"，需要发一次标准 SCSI 弹出才切到
网卡模式：

```bash
sudo eject /dev/sr0
# 或
sudo usb_modeswitch -K -W -v 350b -p f179
# 自动切换（推荐）：
sudo ./scripts/setup-mode-switch.sh
```

注意切模式后有**秒级窗口**：芯片 ROM 只在很短时间里接受固件下载，所以要"切完立刻加载驱动"。
自动切换（udev 规则）就是为此存在的。

-----

**Question:** 仓库里为什么没有 `zt9612_fw.bin`？

**Answer:** 固件版权属于芯片厂商，本仓库不再分发。请从你自己的网卡配套驱动盘里取
（Windows 下运行 `auto_load.exe` 之后，在安装目录里能找到 `zt9612_fw.bin` 与
`zt9612_settings.bin`），或向模组厂商索取 Linux/Android 版本驱动包。

取到之后用 `sudo ./scripts/install-firmware.sh <目录>` 安装，脚本会校验 SHA-256，
确保你拿到的确实是这一版固件（见 `firmware/README.md`）。

-----

**Question:** Secure Boot 开着，`insmod` 报 `Key was rejected by service` 怎么办？

**Answer:** 内核在 Secure Boot 下处于 `lockdown=integrity`，拒绝未签名模块。两条路：

1. 关掉 Secure Boot（BIOS 里最简单）；
2. 自签并注册 MOK（`install-driver.sh` 会在检测到 Secure Boot 时引导你生成密钥）：

```bash
cd driver
openssl req -new -x509 -newkey rsa:2048 -keyout MOK.priv -outform DER -out MOK.der \
    -nodes -days 36500 -subj "/CN=zt9612 module signing/"
openssl x509 -inform DER -in MOK.der -out MOK.pem
sudo mokutil --import MOK.der      # 设一次性密码
sudo reboot                        # 开机蓝屏选 Enroll MOK → Continue → 输密码
make sign                          # 重新签名模块
```

`MOK.priv` / `MOK.pem` / `MOK.der` 已被 `.gitignore` 排除，**永远不要提交**。

-----

**Question:** 加载后 `dmesg` 里出现 `-110`、`can't set config #1`、`Entity not found`？

**Answer:** 芯片已经挂死，软件复位无效 —— **只能物理拔插**（拔下等 10 秒再插），
然后重新切模式、重新加载。常见触发原因：

- 固件正在运行却又灌了一次固件（同一上电周期不能重复装载）；
- 切模式后拖太久才开始装载（超出 ROM 窗口）；
- 固件崩溃（例如不等 CFM 连发 IPC）。

-----

**Question:** 设备本来好好的，突然又变回 `350b:f179` 光驱了？

**Answer:** 固件看门狗复位了。固件要求主机**每 5 秒**发一次心跳（`0x05c2`），
本驱动用 `delayed_work` 实现；如果你是用自己的脚本做实验，请确认心跳没停。
另外脚本异常退出、长时间不通信都会触发。

-----

**Question:** 为什么 `install-driver.sh` 默认不让我开机自动加载？

**Answer:** 因为这条路径有**未排查清楚的风险**：本项目观察到把模块装进
`/lib/modules/$(uname -r)/extra/` 后，重启时内核通过 USB modalias 自动加载模块，
机器会变成"能 ping、能连 22 端口，但 SSH 永远读不到 banner"，只能硬重启。
根因尚未定位（可能与 xhci 枚举抖动、probe 在 udev worker 中耗时约 7 秒有关）。

所以脚本默认写入 `blacklist zt9612`（只挡自动加载，手动 `modprobe` 仍然可用）。
确认过你的机器没问题后，可以重跑 `sudo ./install-driver.sh --enable-autoload`。

-----

**Question:** 可以 `rmmod` 换个模块再 `insmod` 吗？

**Answer:** **不要反复这样做。** 本项目有两次把整机搞成"能 ping 不能 SSH"的经历，其中一次
就是连续 `rmmod` + `insmod`。卸载路径已经加固（`usb_poison_urb()` + 2 秒上限，
超时宁可泄漏实例也不无界阻塞），但硬件/控制器层面是否仍会被拖住没有证据排除。

换模块请**重启机器**；`uninstall-driver.sh` 默认也只删文件、不 `rmmod`。

-----

**Question:** 支持 DKMS 吗？

**Answer:** 支持：

```bash
sudo ./install-driver.sh --dkms
# 或者手动
sudo dkms install .
```

`dkms.conf` 在仓库根目录，版本 `0.1.0`，安装到 `/lib/modules/<kernel>/updates/dkms/`。
内核升级后 DKMS 会自动重建（Secure Boot 机器需要先把 MOK 配好，否则新模块没签名）。

-----

**Question:** 内核版本要求？

**Answer:** 开发与验证环境是 Ubuntu 24.04 + 内核 `7.0.0-31-generic`。
`dkms.conf` 里限制了 `>= 5.15`，但这只是保守下限 —— 本驱动用到 7.0 时代的 mac80211
ops 签名（`config(hw, radio_idx, changed)`、`tx(hw, control, skb)`），
**较老内核很可能编译不过**。遇到编译错误欢迎提 issue（附完整报错与 `uname -a`）。

-----

**Question:** 为什么不用厂商的 Linux 驱动？

**Answer:** 厂商（兆通微 / ZTOP）确实有 Linux/Android 驱动包
（`ZTOP_ACEV100_Android_wifi_bt_*.tar.gz`），但不公开分发，需要向厂商索取。
拿到的话可以省很多事 —— 本项目是在索取未果的情况下，通过逆向 Windows 驱动 + USB 抓包
自己实现协议栈的。

如果你能从厂商拿到源码包，欢迎在 issue 里说一声，那是最省力的路线。

-----

**Question:** 为什么不用 AIC8800 的开源驱动？

**Answer:** 两者确实同源（CEVA RivieraWaves rwnx 血统，消息头 `struct lmac_msg` 一致），
但：VID/PID 不同、固件容器格式不同（AIC 是裸分节表，兆通微是 `"ZT"` + PID 的 19 字节段表）、
厂商私有消息段（`0x01xx`/`0x05xx`）也不一样。所以只能借鉴它的分层与消息框架，
不能直接套用。

-----

**Question:** 会提交到内核主线吗？

**Answer:** 远期可能，但当前不现实：主线要求 driver 完整（扫描/关联/数据面）、
有维护者承诺、并且固件必须能通过 `linux-firmware` 合法再分发。目前先把 M3 做完。

-----

**Question:** 支持蓝牙 / AP 模式 / 监听模式吗？

**Answer:** 不支持。同一芯片有蓝牙功能（USB 复合接口），但本项目只做 WLAN STA；
AP / monitor / 注入都不在范围内。

-----

**Question:** 怎么参与？

**Answer:** 见 `CONTRIBUTING.md`。最需要的帮助是：

1. **实机测试**：不同的主板/内核上跑一遍，把 dmesg 贴出来；
2. **M3 的未知项**：RX 数据路径格式、TX 描述符布局（见 README「路线图」）；
3. **厂商渠道**：能拿到官方 Linux 驱动的同学；
4. 文档与脚本的修正。

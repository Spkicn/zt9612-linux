# FAQ

以下问答按常见程度排序。文中路径均相对仓库根目录。

-----

**问：这个驱动现在能用吗，插上就能上网吗？**

答：**能上网**。驱动已实机验证固件装载、mac80211 接口、双频扫描、关联、数据面、
WPA2-PSK 加密、DHCP 与外网 `ping`。唯一明显短板是吞吐（实测速率恒定 1 Mbit/s）。
当前进度如下：

| 里程碑 | 状态 |
|---|---|
| M1 内核态固件装载 | 已实现并在实机验证 |
| M2 IPC 初始化与 `/dev/zt9612` | 已实现并在实机验证（IPC 往返成功） |
| M3.1 mac80211 注册（无线接口） | 已实现并在实机验证（接口按真实 MAC 命名） |
| M3.2 扫描 / M3.3 关联 / M3.4 数据面 | 已实现并在实机验证（双频扫描、关联、DHCP 与公网 ping） |
| M3.5 5 GHz | 已实现并在实机验证（`iw phy info` 列出 14 + 25 个信道） |
| WPA2-PSK 加密 | 已实现并在实机验证（四次握手 `PTK=CCMP GTK=CCMP`） |
| v0.3 吞吐 | 未开始（下一步：TX status 上报 + 补 2.4G OFDM 速率表） |

所以现在能得到的是一块**可用的 managed 模式无线网卡**：双频（2.4G + 5G）扫描、
WPA2-PSK 关联与加密、DHCP 拿地址、能访问外网；也可以通过 `/dev/zt9612` 收发原始帧。
版本为 `0.2.0`。当前已知短板是吞吐（实测速率恒定 1 Mbit/s），原因见 README「已知问题」。

-----

**问：支持哪些网卡？**

答：仅支持 VID:PID 为 `350b:9612` 的 ZT9612U / ACEV100 模组（网卡模式）。完整说明见
`supported-device-IDs`。同厂的 `350b:9101`、`350b:9611` 不适用。

-----

**问：插上以后只看到一个光驱（`350b:f179`），没有网卡，是怎么回事？**

答：这是设备的设计：默认枚举为驱动光盘，需要发送一次标准 SCSI 弹出才会切到网卡模式。

```bash
sudo eject /dev/sr0
sudo usb_modeswitch -K -W -v 350b -p f179
```

也可以做一次性配置，让插入时自动切换：

```bash
sudo ./scripts/setup-mode-switch.sh
```

切换后有一段很短的窗口期，芯片 ROM 只在这段时间内接受固件下载，因此切完应尽快加载驱动。
自动切换（udev 规则）就是为此存在的。

-----

**问：仓库里为什么没有 `zt9612_fw.bin`？**

答：固件版权属于芯片厂商，本仓库不再分发。请从自己网卡的配套驱动盘中取得：在 Windows 下
运行 `auto_load.exe` 之后，安装目录中通常可以找到 `zt9612_fw.bin` 与
`zt9612_settings.bin`。也可以向模组厂商索取 Linux 或 Android 版本的驱动包。

取得后执行 `sudo ./scripts/install-firmware.sh <目录>`，脚本会校验 SHA-256，确保拿到的是
已知版本。详见 `firmware/README.md`。

-----

**问：Secure Boot 开着，`insmod` 报 `Key was rejected by service`，怎么办？**

答：Secure Boot 下内核处于 `lockdown=integrity`，会拒绝未签名模块。有两种处理方式。

一是关闭 Secure Boot（在 BIOS 中操作，最省事）。

二是自签并注册 MOK。`install-driver.sh` 在检测到 Secure Boot 且没有密钥时会引导完成，
也可以手动执行：

```bash
cd driver
openssl req -new -x509 -newkey rsa:2048 -keyout MOK.priv -outform DER -out MOK.der \
    -nodes -days 36500 -subj "/CN=zt9612 module signing/"
openssl x509 -inform DER -in MOK.der -out MOK.pem
sudo mokutil --import MOK.der      # 设置一次性密码
sudo reboot                        # 开机蓝屏界面选 Enroll MOK，输入该密码
make sign                          # 重新签名模块
```

`MOK.priv`、`MOK.pem`、`MOK.der` 已被 `.gitignore` 排除，不要提交。

-----

**问：加载后 `dmesg` 里出现 `-110`、`can't set config #1`、`Entity not found`，是什么情况？**

答：芯片已经挂死，软件复位无效，只能物理拔插：拔下网卡，等待 10 秒，再插上，
然后重新切换模式并加载驱动。常见触发原因有：

- 固件正在运行时又灌了一次固件，同一上电周期内不能重复装载
- 切模式后拖延太久才开始装载，超出 ROM 窗口
- 固件崩溃，例如不等 CFM 就连发 IPC

-----

**问：设备本来正常，突然又变回 `350b:f179` 光驱了？**

答：固件看门狗复位。固件要求主机每 5 秒发送一次心跳（`0x05c2`），本驱动用 `delayed_work`
实现。如果使用自己编写的用户态脚本做实验，请确认心跳没有中断。脚本异常退出或长时间不通信
也会触发复位。

-----

**问：为什么 `install-driver.sh` 默认不允许开机自动加载？**

答：因为这条路径曾经把机器搞死过，历史经过如下。

模块安装到 `/lib/modules/$(uname -r)/extra/` 后，重启时内核会通过 USB modalias 自动加载它。
接口出现之后 NetworkManager 立刻查询驱动信息，而驱动注册 wiphy 时漏了
`SET_IEEE80211_DEV()`，导致 `cfg80211_get_drvinfo()` 解引用空指针并在关中断状态下崩溃，
用户态随之卡死：机器能 ping、22 端口能连，但 SSH 永远读不到 banner，只能硬重启。

这个缺陷已经修复（在 `ieee80211_register_hw()` 之前设置 `wiphy->dev.parent`），修复后
`ethtool -i` 与 NetworkManager 都正常。不过「干净开机自动加载」这条完整路径还没有重新验证
一遍，所以脚本仍默认写 `blacklist zt9612`（只阻止自动加载，手动 `modprobe` 仍然可用）。
确认机器安全后，可以重新运行 `sudo ./install-driver.sh --enable-autoload`。

-----

**问：可以 `rmmod` 之后换个模块再 `insmod` 吗？**

答：不要反复这样做。本项目有两次把整机变成「能 ping 不能 SSH」的经历，其中一次就是连续
`rmmod` 与 `insmod`。卸载路径已经加固（`usb_poison_urb()` 加 2 秒上限，超时宁可泄漏实例
也不无界阻塞），但硬件与控制器层面是否仍会被拖住，尚无证据排除。

换模块请重启机器。`uninstall-driver.sh` 默认也只删除文件，不执行 `rmmod`。

-----

**问：支持 DKMS 吗？**

答：支持。

```bash
sudo ./install-driver.sh --dkms
sudo dkms install .                  # 等价的手动方式
```

`dkms.conf` 位于仓库根目录，版本为 `0.2.0`，安装到
`/lib/modules/<kernel>/updates/dkms/`。内核升级后 DKMS 会自动重建。Secure Boot 机器需要
先配置好 MOK，否则新模块没有签名。

-----

**问：对内核版本有什么要求？**

答：开发与验证环境是 Ubuntu 24.04，内核 `7.0.0-31-generic`。CI 实测 `6.17.0-1022-azure`
可以无告警编译，而 `6.8` 因缺少 `linux/unaligned.h` 失败，该头文件自 6.12 引入，因此
`dkms.conf` 中的编译下限设为 6.12。

驱动使用了较新的 mac80211 ops 签名，例如 `config(hw, radio_idx, changed)`。在较老内核上
很可能编译不过。遇到编译错误欢迎提 issue，请附完整报错与 `uname -a`。

-----

**问：为什么不直接用厂商的 Linux 驱动？**

答：厂商（兆通微 / ZTOP）确实有 Linux 与 Android 驱动包
（`ZTOP_ACEV100_Android_wifi_bt_*.tar.gz`），但不公开分发，需要向厂商索取。本项目是在
索取未果的情况下，依据设备对外可观测的行为自行实现协议栈的。

如果你能从厂商拿到源码包，欢迎在 issue 中说明，那是最省力的路线。

-----

**问：为什么不直接套用 AIC8800 的开源驱动？**

答：两者确实同源（CEVA RivieraWaves rwnx 血统，消息头 `struct lmac_msg` 一致），但存在
几处硬差异：VID/PID 不同；固件容器格式不同（AIC 是裸分节表，兆通微是 `"ZT"` 加 PID 的
19 字节段表）；厂商私有消息段（`0x01xx`、`0x05xx`）也不一样。因此只能借鉴其分层结构与
消息框架，无法直接套用。

-----

**问：会提交到内核主线吗？**

答：远期可能，当前不现实。主线要求驱动功能完整（扫描、关联、数据面）、有维护者承诺，
并且固件必须能通过 `linux-firmware` 合法再分发。M3（M3.1~M3.5）已做完，
目前的重点是 v0.3 吞吐（见 README 路线图）。

-----

**问：支持蓝牙、AP 模式或监听模式吗？**

答：不支持。同一芯片带有蓝牙功能（USB 复合接口），但本项目只做 WLAN STA，
AP、monitor 与注入都不在范围内。

-----

**问：怎么参与？**

答：见 `CONTRIBUTING.md`。目前最需要的帮助是：

1. 实机测试，把不同主板与内核上的 dmesg 贴出来
2. **吞吐（v0.3）**：让速率控制生效——需要 TX status 上报，以及弄清固件是否自行选速
3. **RX 描述符里的速率字段**：USB 层抓包没有 radiotap 真值，目前宁缺勿猜
4. 厂商渠道：能够取得官方 Linux 驱动的同学
5. 文档与脚本的修正

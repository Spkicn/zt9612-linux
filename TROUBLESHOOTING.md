# 已知问题与故障处置

> 本文件是 [README.md](README.md) 的故障分册：完整已知问题清单、设备挂死后的恢复步骤，
> 以及"遇到问题去哪里问"。日常安装见 [INSTALL.md](INSTALL.md)。

## 已知问题

- **上传吞吐上限约 15 Mbit/s，主机侧无法再提**：端到端实测见「项目状态」；UDP（无拥塞
  控制）容量上限 **15.0 Mbit/s**，TCP 已达其 94% ⇒ 瓶颈不是 TCP 拥塞退避。同一张卡的
  下载为 18.5–20.4 Mbit/s ⇒ **上限在固件 TX 侧，不是链路容量**。主机侧七个杠杆已逐一
  证否（描述符速率字 / 速率消息 / `MM_STA_ADD_REQ` 的 RC 块 / A-MPDU 打包 / 换更强 AP /
  声明 HT40 / A-MSDU），速率由固件内部 ARRM 决定，而该模块不在 219 KB 镜像内 ——
  再往上需要厂商固件文档、第二个固件段，或一台支持 A-MSDU 的 AP。
- **聚合默认关闭；UDP 无流控满压会打死设备，需同时开 `agg_pred_us>0`**：`ampdu_en=0` 是
  默认值。打开后（须同时 `ht_cap_enable=1`，且 `agg_min_len=1549` 与 `agg_stop` 一起用）
  在真实 TCP 负载下可长跑（连续 10,930 次聚合传输零断言、USB 设备号不变，真正的变量是
  **有无背压**），但**不带来吞吐增益**（12.5 vs 单帧 14.15 Mbit/s）；UDP 无流控持续满压
  仍会断言并 USB 重枚举 ⇒ 必须开拥塞预测（r44 A/B：不开 = USB 重枚举死亡；开 = 存活零
  bulk 失败）。
- **设备挂死后的唯一可靠复位 = 冷启动（S5 冷关机 + RTC 唤醒），不是热重启**：出现 `-110`、
  `can't set config #1`、`Entity not found`，或 `dmesg` 反复 `hello ack timeout` /
  `firmware boot failed: -110`，就表示芯片已挂死。热 `systemctl reboot` 不断 USB VBUS，
  芯片 ROM 的秒级固件接收窗口不会重开，之后任何 `insmod` 都会失败。已逐一实测无效：
  端口 `disable` 往返（仅逻辑重枚举）、设备级/驱动级 unbind-bind、`USBDEVFS_RESET`、
  `authorized` 往返、`xhci_hcd` unbind→bind、`usb_modeswitch -R`、D3cold。

  ```bash
  sudo rtcwake -m no -s 180      # 设唤醒闹钟
  sudo systemctl poweroff        # 等 ping 100% 丢包 = 真的断电
  # RTC 自动唤醒后，udev 规则会紧跟枚举自动加载驱动（本机 WoL 在 S5 下无效）
  ```

  改代码后必须把新 `.ko` 装进 `/lib/modules/$(uname -r)/extra/`，并 **移走 DKMS 的
  `updates/dkms/zt9612.ko.zst`**（`updates/` 优先级更高，否则加载的是旧代码）。详见
  [FAQ.md](FAQ.md)。

- **反复 `rmmod` / `insmod` 会泄漏实例**：卸载时设备不应答在途 bulk-IN，驱动按设计
  "宁可泄漏也不 UAF"。实测 12 次卸载里 11 次如此。**换模块优先重启机器。**
- **默认不自动加载**：`install-driver.sh` 默认写 `blacklist zt9612`。原因是历史上
  "开机自动加载"曾导致机器失联（根因是漏设 wiphy 父设备，已在 v0.2.0 修复），
  但**"插卡 + 开机自动加载"这条完整路径还没有重新复验过**。确认可用后加
  `--enable-autoload` 即可。
- **做 TX 实验必须带功能判据**：`usb_bulk_msg` 完成得快**不等于**帧发出去了。用
  `ping -M do 1472`、DHCP，或"在开放网络上发 ARP 看网关是否应答"这类 A 级判据。
  注意**未加密的裸帧判据在 WPA/WPA2 链路上无效**：那些帧会被 AP 直接丢弃，而同一链路上的
  普通 `ping` 与 ARP 一切正常 —— 本项目在这里踩过坑（`ZT_IOC_TXRAW` 恒判"没发出"）。
- **运行期改 MTU 会让接口短暂失去关联**（丢 1–2 个 `ping`，NetworkManager 会重配）；
  请勿在运行期反复改。
- **设备可能自行回到光盘模式**：固件看门狗复位（主机超过 5 秒没有发心跳）。内核驱动用
  `delayed_work` 每 5 秒发一次；自写用户态脚本需要自己维持心跳。
- **WPA2-Enterprise（802.1X）未验证**：实验环境没有 802.1X AP。
- 驱动源码中部分中文注释在早期编辑中损坏成乱码，不影响编译，待清理。
- 0.x 阶段的接口（模块参数、`/dev` 协议）可能变化。

已修复的历史问题（过程与数据见 [CHANGELOG.md](CHANGELOG.md)）：RX 缓冲过小导致下载停滞、
5 GHz 扫描 band 标志错误导致 5 GHz 完全不可用、TX 帧长上限导致上传不可用、
`max_mtu` 曾被设成 900 的弯路、开机自动加载的空指针崩溃。

## 遇到问题去哪里问

**遇到问题去哪里问**：先查 [FAQ.md](FAQ.md)；仍未解决就到
[Issues](https://github.com/Spkicn/zt9612-linux/issues) 反馈，请附 `lsusb -d 350b:`、`uname -a`
与 `dmesg` 全段（模板见 [.github/ISSUE_TEMPLATE/bug_report.md](.github/ISSUE_TEMPLATE/bug_report.md)）。
无线网卡驱动的问题几乎都能从这三样里看出来；缺信息时只能来回问，效率很低。

不想一条条敲就把这些交给脚本收集：

```bash
sudo ./scripts/collect-debug-info.sh        # 生成 zt9612-debug-<时间戳>.txt，直接附到 issue
```

它只读、不改配置，收集系统/设备/模块/固件校验/参数/接口/dmesg 共十来项。**贴之前自己过一遍**：
`dmesg`、`ip`、`iw` 的输出里可能有真实 MAC、SSID 与主机名。

## 自己先判一遍：排障包判读表

拿到上面那份报告后，**按下面这张表先自己定位一次**（维护者也是按同一套规则看的）：

| 你看到什么 | 说明什么 | 下一步 |
|---|---|---|
| `固件校验` 段出现 `No such file` | 固件没装或路径不对 —— 这是 M1 装载失败最常见的原因 | 用 `scripts/install-firmware.sh` 安装两个固件（自带 SHA-256 校验） |
| `USB 设备` 段是 `(没有 350b: 设备)` | 设备可能退回了**光盘模式**（`350b:f179`）或没插好 | `lsusb -d 350b:` 复核；退回光盘模式时先断电复位 |
| dmesg 里有 `hello ack timeout` | 设备没进入固件下载态（握手超时） | **S5 冷启动 + RTC 唤醒**（见下） |
| dmesg 里有 `-110` / `Entity not found` | USB 传输失败，设备已挂死 | 同上；**不要**反复 unbind-bind 或 `USBDEVFS_RESET`（实测无效） |
| dmesg 里有 `assert`（`macif.c` / `scm_admin`） | **固件断言**：进了已知致命区（短单元或描述符形态不符） | 先把实验开关全关（`ampdu_en=0`、`sta_add_en=0`、`key_en=2`、`agg_pred_us=0`）复现；仍断言请附**完整** dmesg |
| dmesg 里有 `WARNING:` / `BUG:` / `Oops` / `Call Trace` | 内核侧异常（可能是驱动，也可能在 USB/内核层） | 附该行**前后各 20 行**与复现步骤；这是我们最优先处理的一类 |
| dmesg 里读到 `=== init done, firmware running ===` 且 `接口状态` 有 `wlan0` | 驱动侧到这一步是**正常**的 | 问题更可能在关联/密钥/NetworkManager 侧，先查 [FAQ.md](FAQ.md) |
| `dmesg` 段是 `(读不到 dmesg…)` | 权限不足，证据没采到 | 用 root 重跑一次收集脚本 |

**挂死的唯一可靠复位**是 **S5 冷启动 + RTC 唤醒**（`rtcwake -m no -s 180` + `systemctl poweroff`），
热重启无效、也不必物理拔插；细节见 [README.md](README.md)「已知问题」与内部文档 `docs/06`。

## 稳定性怎么测（建议一起报这三个数）

本项目的公开状态里目前只有**吞吐**，但日常使用更关心**丢包与延迟**。这两样**不需要厂商资料**就能测，
而且能区分"慢"和"不稳"——请按同一口径测、把三个数一起报上来：

```bash
# 1) 到网关（20 个包就够看出异常；稳态观察用 -c 300）
ping -I wlan0 -c 100 "$(ip route show default | awk '{print $3; exit}')"
# 2) 到公网（同上；两者差别大 ⇒ 问题在无线段而不是出口）
ping -I wlan0 -c 100 223.5.5.5
# 3) 报告里请连同：内核版本（uname -r）、频段与信道（iw dev wlan0 link）、距离与环境
```

看三件事：**丢包率**（应接近 0）、**平均延迟**、**mdev/jitter**（波动大说明链路不稳，
即使吞吐数字好看也不适合视频/通话）。把这三点塞进 issue，我们就能判断是驱动、固件还是环境问题。

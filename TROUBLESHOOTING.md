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

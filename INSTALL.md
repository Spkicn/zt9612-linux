# 安装与验收

> 本文件是 [README.md](README.md) 的安装分册：准备固件 → 切换设备模式 → 编译安装 →
> Secure Boot / MOK 签名 → 验收。驱动能做什么、性能如何、有哪些已知问题见 README。

## 安装

### 1. 准备固件

固件版权属于厂商，本仓库不分发。请从自己网卡的配套驱动盘中取得
（Windows 下运行 `auto_load.exe` 后，在安装目录中找 `zt9612_fw.bin` 与
`zt9612_settings.bin`），或向厂商索取。取得后执行：

```bash
sudo ./scripts/install-firmware.sh /path/to/firmware-dir
```

脚本会校验 SHA-256 后才安装。文件大小、哈希与来源说明见
[firmware/README.md](firmware/README.md)。

### 2. 切换设备模式

设备出厂默认枚举为光驱，不是网卡。可以做一次性配置让插入时自动切换：

```bash
sudo ./scripts/setup-mode-switch.sh
```

也可以手动切换一次：

```bash
sudo eject /dev/sr0
sudo usb_modeswitch -K -W -v 350b -p f179
```

切换后 `lsusb` 应显示 `350b:9612`。芯片 ROM 只在切换后的数秒内接受固件下载，
因此切完应尽快加载驱动，这也是配置 udev 自动切换的原因。

### 3. 安装驱动

```bash
sudo ./install-driver.sh                     # 编译、按需签名、安装，并手动加载一次
sudo ./install-driver.sh --dkms              # 通过 DKMS 安装，内核升级后自动重建
sudo ./install-driver.sh --enable-autoload   # 允许插卡或开机自动加载，见「已知问题」
```

默认行为是安装模块到 `/lib/modules/$(uname -r)/extra/`，并写入
`/etc/modprobe.d/zt9612-blacklist.conf`，内容是**一行 `blacklist zt9612`** ——
它**只挡住插卡/开机的自动加载，不挡手动 `modprobe`**（`--enable-autoload` 会删掉这个文件）：

```bash
sudo modprobe zt9612
```

> [!IMPORTANT]
> **手工加过守卫的机器**上是另一种情况：守卫文件里除了 `blacklist` 还可能有
> `install zt9612 /bin/true`。那种环境下 `sudo modprobe zt9612` 会**静默返回 0 且什么都不做**
> （实测 2026-09-27，内核 7.0.0-34），必须用：
>
> ```bash
> sudo modprobe --ignore-install zt9612
> ```
>
> 手工加过守卫的机器上，`--enable-autoload` 会**整个删除**该 blacklist 文件（连同守卫）。

也可以直接用 DKMS：

```bash
sudo dkms install .                          # add、build、install 一步完成
dkms status
sudo dkms remove zt9612/0.3.2 --all
```

手动编译的等价流程：

```bash
cd driver
make                       # 生成 zt9612.ko
make sign                  # Secure Boot 机器：用 MOK 密钥签名
sudo make install          # 安装到 /lib/modules/$(uname -r)/extra/ 并 depmod
sudo modprobe zt9612
```

可用目标见 `make help`；可覆盖变量为 `KVER`、`KSRC`（或 `KERNEL_SRC`）、`JOBS`。

### 4. Secure Boot 签名

Secure Boot 打开时内核处于 `lockdown=integrity`，会拒绝未签名模块。有两种办法：
在 BIOS 中关闭 Secure Boot，或生成 MOK 密钥并注册（一次性）：

```bash
cd driver
openssl req -new -x509 -newkey rsa:2048 -keyout MOK.priv -outform DER -out MOK.der \
    -nodes -days 36500 -subj "/CN=zt9612 module signing/"
openssl x509 -inform DER -in MOK.der -out MOK.pem
sudo mokutil --import MOK.der     # 设置一次性密码
sudo reboot                       # 开机蓝屏界面选 Enroll MOK，输入该密码
make sign
```

`install-driver.sh` 检测到 Secure Boot 且没有密钥时，会引导完成上述步骤。
`MOK.*` 已被 `.gitignore` 排除，不要提交。

**DKMS 已知坑**：dkms 3.0.11 即使配好了 `mok_signing_key` / `mok_certificate`，
也**不会给压缩后的 `.ko.zst` 签名**，`modprobe` 会以 `Key was rejected by service` 失败。
就地补签即可：

```bash
KVER=$(uname -r); KO=/lib/modules/$KVER/updates/dkms/zt9612.ko.zst
sudo zstd -d -f $KO -o /tmp/zt9612.ko
sudo $KVER/build/scripts/sign-file sha256 MOK.priv MOK.pem /tmp/zt9612.ko   # 密钥路径按实际调整
sudo zstd -f -q /tmp/zt9612.ko -o $KO
sudo depmod -a && sudo modprobe zt9612
modinfo -k $KVER zt9612 | grep signer      # 应显示已注册的签名者
```

补签后 `dkms status` 会提示 `WARNING! Diff between built and installed module!`，属预期现象；
下一次 `dkms install` 会再次覆盖成未签名版本，需要重复这一步。

## 验收

```bash
sudo dmesg -C
sudo modprobe zt9612            # 约 7 秒，其中 MM_START 需等待约 6.5 秒
sudo dmesg | tail -30
```

正常输出如下（实测时间线）：

```
zt9612 1-9:1.0: probing 350b:9612 (iface 0)
zt9612 1-9:1.0:   ep 0x84 IN
zt9612 1-9:1.0:   ep 0x05 OUT
zt9612 1-9:1.0: hello ack: yes (try 1)
zt9612 1-9:1.0:   wrote addr=0x61070000 len=219048 (449 blocks) cs=0x2d14
zt9612 1-9:1.0:   wrote addr=0x210ce700 len=212 (1 blocks) cs=0x0001
zt9612 1-9:1.0: RUN (1 sections)
zt9612 1-9:1.0: boot notify: type=0x0100 len=16
zt9612 1-9:1.0: firmware loaded
zt9612 1-9:1.0: === IPC init sequence ===
zt9612 1-9:1.0: fw 0x0101: status=0 mac=XX:XX:XX:XX:XX:XX
zt9612 1-9:1.0: MM_START_REQ (rf init, waiting 约 6.5s)
zt9612 1-9:1.0: MM_START_CFM received (firmware up)
zt9612 1-9:1.0: MM_ADD_IF_CFM: status=0 inst_nbr=0
zt9612 1-9:1.0: M1+M2 done: firmware running, /dev/zt9612 ready
zt9612 1-9:1.0: mac80211 registered (M3.1) - wlan0 should appear
zt9612 1-9:1.0 wlxXXXXXXXXXXXX: renamed from wlan0
```

> 上面的 MAC 与接口名已用 `XX` 隐去（驱动实际打印的是内核 `%pM` 格式的地址）。
> 接口名是 systemd 按 MAC 生成的可预测名（`wlx` + 12 位十六进制），
> 所以**不要**假设它叫 `wlan0`。

再做 IPC 往返自测：

```bash
sudo python3 scripts/zt9612-devtest.py
# 期望：往返成功 2/2（MM_VERSION_REQ 收到 CFM；厂商 0x0100 返回 MAC）
sudo python3 scripts/zt9612-devtest.py --seconds 20    # 顺带检查心跳稳定性
```

接口与无线验收：

```bash
IFACE=$(ls /sys/class/net | grep -E '^(wlx|wlan)' | head -1)
iw dev ; iw phy "$(iw dev "$IFACE" info | awk '/wiphy/{print "phy"$2}')" info | head -40
ethtool -i "$IFACE" | head -2                 # 期望 driver: zt9612
sudo iw dev "$IFACE" scan | grep -c '^BSS'    # 期望 40 个以上（双频）
```

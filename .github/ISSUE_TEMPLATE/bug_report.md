---
name: Bug report / 问题报告
about: 加载失败、dmesg 报错、设备异常等
title: ''
labels: bug
assignees: ''
---

<!--
中文/英文都可以。请尽量把下面的信息填全 —— 无线网卡驱动的问题几乎都能从
dmesg 与 lsusb 里看出来；信息缺失时只能来回问，效率很低。
-->

## 1. 硬件与系统

- 网卡型号 / 外壳丝印：
- `lsusb | grep -i 350b` 的输出：
- `uname -a`：
- 发行版与版本：
- Secure Boot 状态（`mokutil --sb-state`）：

## 2. 安装方式

- [ ] `install-driver.sh`
- [ ] `install-driver.sh --dkms`
- [ ] 手动 `make` + `make install`
- [ ] 其他（说明）

固件是否已安装？请贴上校验结果：

```bash
sha256sum /lib/firmware/zt9612_fw.bin /lib/firmware/zt9612_settings.bin
# 期望 8562003c...  (219076 B)  /  58c2d73a...  (212 B)
```

## 3. 驱动状态

```bash
modinfo zt9612 | head -20
lsmod | grep zt9612
ls -l /dev/zt9612
cat /proc/sys/kernel/tainted      # 外部模块加载后非 0 是正常的
```

## 4. dmesg（关键部分）

```bash
sudo dmesg -C
sudo modprobe zt9612      # 或 sudo insmod ...
sudo dmesg | tail -60
```

```
（把 dmesg 贴在这里）
```

## 5. 现象描述

- 期望发生什么：
- 实际发生什么：
- 是否可复现（每次 / 偶尔 / 只出现一次）：
- 设备最终处于哪个状态：`f179`（光驱）/ `9612`（网卡）/ 完全消失

## 6. 已经尝试过

- [ ] 物理拔插网卡（拔下等 10 秒）
- [ ] 重启机器
- [ ] 重新切模式（`sudo eject /dev/sr0`）
- [ ] 换了另一个 USB 口 / 换了根线
- [ ] 关闭 Secure Boot 或完成 MOK 注册

## 7. 其他

- 这个网卡在 Windows 上是否正常工作：
- 之前是否有某个版本/某次改动后开始出问题：
- 补充说明：

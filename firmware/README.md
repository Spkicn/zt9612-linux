# 固件目录 — 本目录**不包含**任何固件二进制

`zt9612_fw.bin` 及其配置块 `zt9612_settings.bin` 的版权属于芯片厂商
（ZTOP / 山东兆通微电子）。本仓库没有获得再分发许可，因此**不提供下载**，
只在这里说明需要哪些文件、怎么校验、装到哪里 —— 做法参照
[linux-firmware 的 WHENCE 约定](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git)。

驱动的加载路径（`driver/zt9612.c` 使用 `request_firmware()`）：

```c
request_firmware(&fw, "zt9612_fw.bin", ...)          /* MODULE_FIRMWARE 已声明 */
request_firmware(&st, "zt9612_settings.bin", ...)
```

⇒ 两个文件都要放在 `/lib/firmware/` 下（不要放进子目录）。

## 需要的文件

| 文件名 | 大小 | SHA-256 |
|---|---|---|
| `zt9612_fw.bin` | 219,076 字节 | `8562003c3558c7f608d5ff926d368d6de021c1ae12ac833978ea7c4e5a9ebe2d` |
| `zt9612_settings.bin` | 212 字节 | `58c2d73a4d2a1c1eb10a606a6e0bf6a201fb8f288fce1cbe0ce5364f7271fb46` |

`zt9612_fw.bin` 的容器头是 `"ZT"` + `u16 0x9612`，如果你手上的文件大小差很多
（例如 100 多 KB），那是别的型号（`zt9611_fw.bin` 之类）的固件，不能用。

## 从哪里取得

1. **网卡自带的驱动光盘（最常见）**
   设备出厂默认就是这张光盘（`350b:f179`，卷标 "Wi-Fi6 Adapter"）。
   在 Windows 上运行里面的 `auto_load.exe` 安装驱动后，在安装目录
   （通常 `C:\Program Files (x86)\...` 或驱动解压目录）里找 `zt9612_fw.bin`
   和 `zt9612_settings.bin`；也可以直接从光盘里的 CAB 中解出来。
2. **向模组厂商索取**
   厂商有 Linux/Android 驱动包 `ZTOP_ACEV100_Android_wifi_bt_*.tar.gz`
   （内含 `fw/zt9612_fw.bin`）。模组型号 **ZT9612U V1.0**，平台 **ACEV100**。
3. **问卖家 / 原厂 FAE** —— 淘宝/京东/亚马逊卖家通常能转给原厂技术支持。

## 安装

```bash
sudo ./scripts/install-firmware.sh /path/to/firmware-dir
# 或指定文件
sudo ./scripts/install-firmware.sh ~/zt9612_fw.bin ~/zt9612_settings.bin
```

脚本会逐个校验 SHA-256，只有匹配已知版本才会安装到 `/lib/firmware/`。

## 关于配置块

`zt9612_settings.bin`（212 字节）是固件下载协议里的"配置块"，
由驱动写到设备地址 `0x210CE700`，整段 XOR16 校验值为 `0x0001`。
它和驱动盘里的 `rwnx_settings.ini` 是同一份配置的两种形式。

## 如果将来拿到再分发许可

把固件放进本目录，并在此处补充（WHENCE 风格）：

- 文件名 / 版本 / 来源产品
- 许可方与许可条款（单独的 `LICENSE.<vendor>` 文件）
- 是否允许再分发、是否包含专利许可

在拿到明确许可之前，请不要把 `.bin` 提交到本仓库 —— `.gitignore` 已经排除了 `*.bin`。

# 贡献指南（CONTRIBUTING）

本仓库是一个**实验性的、尚未完成**的 Linux 无线网卡驱动。欢迎任何形式的帮助，
但请先读完本页 —— 尤其是「硬件相关铁律」和「不要提交的内容」两节。

## 1. 你可以怎么帮（按价值排序）

| 优先级 | 事项 | 说明 |
|---|---|---|
| ⭐⭐⭐ | **实机测试并把 dmesg 贴出来** | 不同主板 / 不同内核上的加载结果最有价值；即使失败也有价值 |
| ⭐⭐⭐ | **M3.2 扫描**：RX 数据路径格式 | 扫描到的 AP 以什么帧格式从 EP4-IN 上来 —— 当前最大的未知 |
| ⭐⭐ | **M3.4**：TX 描述符布局 | 发送数据帧时主机侧要填什么 |
| ⭐⭐ | **厂商渠道** | 如果你能拿到官方 Linux 驱动包（`ZTOP_ACEV100_*_Linux*.tar.gz`），请开 issue 告知 |
| ⭐ | 文档、脚本、CI 的修正 | 拼写、失效链接、兼容性提示 |
| ⭐ | 代码清理 | 例如 `driver/zt9612.c` 里历史遗留的乱码注释（**必须与实机编译一起做**） |

不确定从哪开始？先看 issue 里带 `help wanted` 标签的条目，或者在 issue 里描述你的硬件环境。

## 2. 开发环境

- 目标环境：Ubuntu 24.04 / 内核 `7.0.0-31-generic`（开发与验证所用，`x86_64`）
- 依赖：`build-essential`、`linux-headers-$(uname -r)`、`usb-modeswitch`、`sg3-utils`、`eject`
- 编译与运行见 `README.md`；本驱动**必须**在真机 + 真卡上验证，CI 只能保证编译通过
- 需要一张 `350b:9612` 网卡（见 `supported-device-IDs`）

## 3. 代码规范

### 3.1 风格

- **内核代码风格**：`Documentation/process/coding-style.rst` —— 制表符缩进（宽度 8）、
  括号与空行遵循 K&R、函数名小写加下划线。行宽：内核文档建议 **单行 ≤ 80 列**，
  而 checkpatch 的默认上限是 **100**；本仓库沿用 100（`.checkpatch.conf`），
  新代码尽量不超过 80~100，且**不要为了折行而拆断用户可见字符串**
- 提交前请跑一遍：

  ```bash
  cd driver && make checkpatch
  ```

- CI 也会跑 checkpatch（`.github/workflows/checkpatch.yml`），已有代码的存量告警不阻塞，
  **但新增代码不应引入新告警**
- `.editorconfig` 已配置好缩进/换行/编码，请让编辑器读取它

### 3.2 每个文件都要有 SPDX 头

```c
// SPDX-License-Identifier: GPL-2.0-only
```

本仓库统一使用 **GPL-2.0-only**（因为直接调用 `ieee80211_*` 等 GPL-only 符号）。
新文件请照抄，不要引入其它许可证。

### 3.3 内核侧约定（本驱动特有）

- **日志**：一律用 `dev_info/dev_warn/dev_err(&z->intf->dev, ...)`，关键阶段打点，
  方便和 `dmesg` 时间线对照；不要用 `printk` 裸打
- **时间**：用 `jiffies` / `msecs_to_jiffies()`，不要忙等
- **USB IO 前检查 `z->alive`**：卸载/拔出后一律不再发起 USB 传输
  （这是已知的挂机问题的加固措施，见 `README.md`「已知问题」）
- **收尾路径禁止无界等待**：`usb_kill_urb()` 在设备挂死时会永久阻塞。
  本驱动使用 `usb_poison_urb()` + 2 秒上限，超时**故意泄漏实例**而不是 use-after-free。
  新写的收尾代码请沿用这个模式
- **不要调用 `usb_set_interface()`**：本设备只有单 alt setting，实测调用后行为异常
- **IPC 必须一问一答**：发一条 → 等对应 CFM → 再发下一条；连发会让固件断言崩溃
- **心跳不能停**：初始化开始后每 5 秒一次 `0x05c2`，否则固件看门狗复位

### 3.4 文件组织

当前是**单文件** `driver/zt9612.c`（固件装载 + IPC + mac80211）。暂时不拆：
它还没有在实机上完成端到端验证，拆分收益低于风险。等 M3.1 验证通过后，
可以按 HIF 层 / 消息层 / mac80211 层拆分，届时请单独开 issue 讨论。

`driver/_mac_section.c` 是 mac80211 段的**只读摘录，不参与编译**；
改 `zt9612.c` 的该段后请同步更新它。

## 4. 提交规范

### 4.1 DCO（必须）

每个提交都要有 `Signed-off-by` 行，表示你同意下面的
[Developer Certificate of Origin 1.1](https://developercertificate.org/)：

```
Developer's Certificate of Origin 1.1

By making a contribution to this project, I certify that:

(a) The contribution was created in whole or in part by me and I have the right
    to submit it under the open source license indicated in the file; or

(b) The contribution is based upon previous work that, to the best of my
    knowledge, is covered under an appropriate open source license and I have
    the right under that license to submit that work with modifications,
    whether created in whole or in part by me, under the same open source
    license (unless I am permitted to submit under a different license), as
    indicated in the file; or

(c) The contribution was provided directly to me by some other person who
    certified (a), (b) or (c) and I have not modified it.

(d) I understand and agree that this project and the contribution are public
    and that a record of the contribution (including all personal information
    I submit with it, including my sign-off) is maintained indefinitely and may
    be redistributed consistent with this project or the open source license(s)
    involved.
```

用法（`git commit -s` 会自动加上）：

```bash
git commit -s -m "driver: fix heartbeat payload length"
```

### 4.2 提交信息

```
<区域>: <一句话说明做了什么>

为什么这么做（背景/现象/根因）。

验证方式与结果（命令 + 关键输出），没有实机验证要写清楚。
Signed-off-by: 你的名字 <you@example.com>
```

- 区域用：`driver`、`scripts`、`docs`、`ci`、`build`
- 标题用**祈使句**、不超过 **75 字符**，描述"改了什么 + 为什么需要"；不要只写文件名
- 正文按 **75 列**折行；一个提交只做一件事，保持可 bisect

### 4.3 分支与 PR

- 从 `main` 拉分支，名字如 `fix/xxx`、`feat/m3.2-scan`
- PR 描述里写：**改了什么 / 为什么 / 怎么验证的**；CI 必须绿
- 涉及驱动行为变更的 PR，**必须附 dmesg 证据**；没有硬件的改动请明确标注「未实机验证」

## 5. 硬件相关铁律（血泪总结）

这些不是风格问题，是会把机器搞挂的问题：

1. **禁止在运行中的机器上反复 `rmmod` + `insmod`** —— 已两次把整机搞成
   "能 ping、能连 22 端口，但 sshd 发不出 banner"，只能硬重启。换模块请重启机器。
2. **开发期不要让模块开机自动加载** —— 模块放到 `/lib/modules/$(uname -r)/extra/`
   后，USB modalias 会在开机时自动加载它；若新模块有问题，机器直接失联。
   做法：`blacklist zt9612`（只挡自动加载，手动 `modprobe` 仍可用）。
3. **每次实验前 `sudo dmesg -C`，结束后 `sudo dmesg > ~/dmesg_$(date +%H%M%S).txt`** ——
   整机失联时 `/var/log` 里的东西不保证留得下来。
4. **改动顺序：用户态先验证 → 再进内核** —— 协议类改动请先写 Python/用户态原型跑通，
   再搬进驱动，避免反复把芯片搞挂。
5. **重灌固件前必须断电复位**（切一次模式或物理拔插）；同一上电周期内重复装载会失败。
6. **芯片挂死后（`-110` / `Entity not found`）只能物理拔插**，软件复位无效。

## 6. 不要提交的内容

`.gitignore` 已经挡掉大部分，但请自查：

| 不要提交 | 原因 |
|---|---|
| `*.bin`、`*.sys`、`*.inf`、`*.cab`、`*.exe` | 厂商固件与 Windows 驱动，版权不属于本项目 |
| USB 抓包（`*.pcap`）、寄存器/固件 dump | 可能含厂商可执行代码与设备唯一信息 |
| `MOK.priv`、`MOK.pem`、`MOK.der`、任何 `*.pem`/`*.priv` | 模块签名私钥 |
| SSH 私钥、`pass.txt`、`known_hosts` | 凭据 |
| 内部交接文档、测试机 IP / 用户名 / 密码 | 隐私与安全 |
| 构建产物（`*.ko`、`*.o`、`*.mod*`、`Module.symvers`、`modules.order`） | 无意义且易冲突 |

提交前自查：

```bash
git status --ignored --short | head -40     # 确认没有意外跟踪敏感文件
git diff --cached --stat                    # 确认改动范围
```

## 7. AI 辅助贡献政策

用 AI 工具写代码是允许的，但请遵守内核上游的既定约定
（`Documentation/process/coding-assistants.rst`）：

- **AI 不能代签 DCO**：`Signed-off-by` 只能由人类添加 —— DCO 是法律声明，
  AI 无法承担。**AI 生成的提交里不得出现 AI 署名的 `Signed-off-by`**
- **责任在人**：你对提交内容的正确性、许可合规性负全部责任；
  不要提交你没读懂、没验证过的代码
- **如实标注**：使用了 AI 工具时，在提交信息末尾加一行 trailer：

  ```
  Assisted-by: LLM <工具名>
  ```

  多个工具可以写多个 `Assisted-by:` 行
- **验证要求不降低**：AI 辅助的改动同样需要通过 checkpatch，并且同样需要实机证据

## 8. 许可与来源合规

向本仓库提交贡献即表示你同意以 **GPL-2.0-only** 授权你的贡献。
不要提交来源不明或与 GPL-2.0-only 不兼容的代码。

另外，**请勿提交来自下列渠道的内容**（即使是"参考"）：

- 厂商 Windows 驱动或固件的反编译/反汇编产物、字符串表、导出表
- 任何受 NDA / 保密协议约束的资料（datasheet、SDK、内部驱动源码）
- 从非公开渠道（泄漏、破解包）获得的代码

本驱动是**独立实现**：只依据设备对外可观测的行为（USB 描述符、总线数据流）与公开的
rwnx 消息框架知识编写。保持这个边界，对项目的长期安全最重要。

# 贡献指南

本仓库是一个尚未完成的实验性 Linux 无线网卡驱动。欢迎以各种方式参与，但请先读完本页，
尤其是「硬件相关纪律」与「不要提交的内容」两节。

## 1. 可以参与的工作

| 优先级 | 事项 | 说明 |
|---|---|---|
| 高 | 实机测试并回贴 dmesg | 不同主板、不同内核上的加载结果最有价值，失败的结果同样有价值 |
| 高 | **厂商渠道** | 官方 Linux 驱动包，或**含 RAM 段代码的完整固件包** —— 现有 219 KB 镜像里没有速率控制代码，这是唯一还能打开"主机影响速率"通道的路径 |
| 中 | 设备收帧上限约 1030 字节的成因 | 超限帧被设备静默丢弃；TCP 不受影响，但大包 ping/UDP 受限 |
| 中 | RX 描述符里的速率字段 | USB 层抓包没有 radiotap 真值，目前宁缺勿猜 |
| 低 | 文档与脚本的修正 | 拼写、失效链接、兼容性提示 |
| 低 | 代码清理 | 例如 `driver/zt9612.c` 中损坏的中文注释，必须与实机编译一起做 |

> 已定案、不必再投入的方向：**吞吐**。v0.3 已查明速率由固件内的 ARRM 模块自主决定
> （描述符无速率字段、厂商驱动从不发速率消息、速率掩码无效），驱动侧无可改之处；
> 补速率表与声明 HT/VHT 都不会改变实测吞吐。详见 `re/REPORT_V03_THROUGHPUT.md`。

不确定从哪里开始，可以先看带 `help wanted` 标签的 issue，或在 issue 中描述你的硬件环境。

## 2. 开发环境

- 目标环境：Ubuntu 24.04，内核 `7.0.0-31-generic`，x86_64（这是开发与验证所用的组合）
- 依赖：`build-essential`、`linux-headers-$(uname -r)`、`usb-modeswitch`、`sg3-utils`、`eject`
- 编译与运行方式见 `README.md`。本驱动必须在真机与真卡上验证，CI 只能保证编译通过
- 需要一张 `350b:9612` 网卡，判定方法见 `supported-device-IDs`

## 3. 代码规范

### 3.1 风格

- 遵循内核代码风格（`Documentation/process/coding-style.rst`）：制表符缩进，宽度 8；
  括号与空行遵循 K&R；函数名小写加下划线
- 行宽方面，内核文档建议单行不超过 80 列，checkpatch 的默认上限是 100 列。本仓库沿用 100
  （见 `.checkpatch.conf`），新代码尽量控制在 80 到 100 之间，并且不要为了折行而拆断
  用户可见的字符串
- 提交前运行：

  ```bash
  cd driver && make checkpatch
  ```

- CI 也会运行 checkpatch（`.github/workflows/checkpatch.yml`）。存量告警不阻塞合并，
  但新增代码不应引入新的告警
- `.editorconfig` 已配置好缩进、换行与编码，请让编辑器读取它

### 3.2 SPDX 头

每个文件都需要许可证标识：

```c
// SPDX-License-Identifier: GPL-2.0-only
```

本仓库统一使用 GPL-2.0-only，因为驱动直接调用了 `ieee80211_*` 等 GPL-only 符号。
新文件请照抄，不要引入其它许可证。头文件用 `/* */` 形式，脚本放在 shebang 之后。

### 3.3 内核侧约定

- 日志统一使用 `dev_info`、`dev_warn`、`dev_err(&z->intf->dev, ...)`，关键阶段打点，
  便于与 `dmesg` 时间线对照，不要直接用 `printk`
- 时间判断使用 `jiffies` 与 `msecs_to_jiffies()`，不要忙等
- 发起 USB 传输前检查 `z->alive`。卸载或拔出后不得再发起 USB 传输，这是对已知挂机问题的
  加固措施，详见 `README.md` 的「已知问题」
- 收尾路径禁止无界等待。`usb_kill_urb()` 在设备挂死时会永久阻塞，本驱动改用
  `usb_poison_urb()` 加 2 秒上限，超时后故意泄漏实例，而不是造成 use-after-free。
  新增收尾代码请沿用这一模式
- 不要调用 `usb_set_interface()`。本设备只有单个 alt setting，实测调用后行为异常
- IPC 必须一问一答：发送一条，等到对应 CFM，再发下一条。连续发送会让固件断言崩溃
- 心跳不能中断：初始化开始后每 5 秒发送一次 `0x05c2`，否则固件看门狗复位

### 3.4 文件组织

当前是单文件实现（`driver/zt9612.c`，包含固件装载、IPC 与 mac80211）。暂不拆分，原因是
单文件边界清晰、回归成本低；若将来要拆，可按 HIF 层、消息层、mac80211 层的划分进行，
请单独开 issue 讨论。

> 历史上还存在一份 `driver/_mac_section.c`（mac80211 区段的只读摘录）。它不参与编译，
> 又长期与源码漂移，已在 v0.2.0 删除。

## 4. 提交规范

### 4.1 DCO

每个提交都需要 `Signed-off-by` 行，表示同意下面的
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

`git commit -s` 会自动添加该行。

### 4.2 提交信息

```
<区域>: <一句话说明做了什么>

为什么这么做（背景、现象、根因）。

验证方式与结果（命令与关键输出）。没有实机验证的改动要写明。
Signed-off-by: 你的名字 <you@example.com>
```

- 区域使用 `driver`、`scripts`、`docs`、`ci`、`build`
- 标题使用祈使句，不超过 75 字符，需要同时说明改了什么以及为什么需要
- 正文按 75 列折行；一个提交只做一件事，保持可 bisect

### 4.3 分支与 PR

- 从 `main` 拉分支，命名如 `fix/xxx`、`feat/m3.2-scan`
- PR 描述中写清改了什么、为什么改、如何验证，CI 必须通过
- 涉及驱动行为变更的 PR 必须附 dmesg 证据；没有硬件条件的改动请明确标注「未实机验证」

## 5. 硬件相关纪律

以下几条不是风格问题，而是会把机器搞挂的问题：

1. 不要在运行中的机器上反复 `rmmod` 与 `insmod`。这已经两次把整机变成「能 ping、能连 22
   端口，但 sshd 发不出 banner」的状态，只能硬重启。需要换模块时请重启机器。
2. 开发期不要让模块开机自动加载。模块放入 `/lib/modules/$(uname -r)/extra/` 后，USB
   modalias 会在开机时自动加载它；如果新模块有问题，机器会直接失联。做法是保留
   `blacklist zt9612`，它只阻止自动加载，手动 `modprobe` 仍然可用。
3. 每次实验前执行 `sudo dmesg -C`，结束后执行
   `sudo dmesg > ~/dmesg_$(date +%H%M%S).txt`。整机失联时 `/var/log` 中的内容不保证能留下。
4. 改动顺序是先用户态验证，再进内核。协议类改动请先用 Python 或用户态原型跑通，再搬进
   驱动，避免反复把芯片搞挂。
5. 重新灌固件前必须断电复位（切换一次模式或物理拔插）。同一上电周期内重复装载会失败。
6. 芯片挂死后（`-110`、`Entity not found`）只能物理拔插，软件复位无效。

## 6. 不要提交的内容

`.gitignore` 已经挡住大部分，但仍请自查：

| 不要提交 | 原因 |
|---|---|
| `*.bin`、`*.sys`、`*.inf`、`*.cab`、`*.exe` | 厂商固件与 Windows 驱动，版权不属于本项目 |
| USB 抓包（`*.pcap`）、寄存器或固件 dump | 可能包含厂商可执行代码与设备唯一信息 |
| `MOK.priv`、`MOK.pem`、`MOK.der`、任何 `*.pem` 与 `*.priv` | 模块签名私钥 |
| SSH 私钥、`pass.txt`、`known_hosts` | 凭据 |
| 内部交接文档、测试机 IP、用户名、密码 | 隐私与安全 |
| 构建产物（`*.ko`、`*.o`、`*.mod*`、`Module.symvers`、`modules.order`） | 无意义且容易冲突 |

提交前可以这样自查：

```bash
git status --ignored --short | head -40     # 确认没有意外跟踪敏感文件
git diff --cached --stat                    # 确认改动范围
```

## 7. AI 辅助贡献

使用 AI 工具写代码是允许的，但需要遵守内核上游的既定约定
（`Documentation/process/coding-assistants.rst`）：

- AI 不能代签 DCO。`Signed-off-by` 只能由人类添加，因为 DCO 是法律声明，
  AI 无法承担相应责任。AI 生成的提交中不得出现 AI 署名的 `Signed-off-by`
- 责任在人。你需要对提交内容的正确性与许可合规性负全部责任，不要提交自己没有读懂、
  没有验证过的代码
- 如实标注。使用 AI 工具时，在提交信息末尾添加 trailer：

  ```
  Assisted-by: LLM <工具名>
  ```

  使用多个工具时可以写多行 `Assisted-by:`
- 验证要求不降低。AI 辅助的改动同样需要通过 checkpatch，同样需要实机证据

## 8. 许可与来源合规

向本仓库提交贡献，即表示你同意以 GPL-2.0-only 授权你的贡献。不要提交来源不明、
或与 GPL-2.0-only 不兼容的代码。

另外，请不要提交来自下列渠道的内容，即使是作为参考：

- 厂商 Windows 驱动或固件的反编译、反汇编产物，字符串表，导出表
- 任何受 NDA 或保密协议约束的资料，例如 datasheet、SDK、内部驱动源码
- 从非公开渠道（泄漏、破解包）获得的代码

本驱动是独立实现，只依据设备对外可观测的行为（USB 描述符、总线数据流）与公开的 rwnx
消息框架知识编写。保持这条边界对项目的长期安全最重要。

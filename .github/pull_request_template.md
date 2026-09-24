## 这个 PR 做了什么

<!-- 一句话概述；如果修的是 issue，写 "Closes #123" -->

## 为什么

<!-- 现象 / 根因 / 背景 -->

## 怎么验证的

- 内核版本 / 发行版：
- 网卡 ID（`lsusb | grep 350b`）：
- 命令与关键输出（dmesg 片段）：

```
（贴在这里）
```

- [ ] **已在实机 + 真卡上验证**
- [ ] 未实机验证（说明原因，例如没有硬件 / 只改了文档）

## 自检清单

- [ ] 每个新增文件都有 `SPDX-License-Identifier: GPL-2.0-only` 头
- [ ] `make checkpatch` 无新增 ERROR / WARNING
- [ ] 脚本改动用 `shellcheck` 自检过
- [ ] 提交带 `Signed-off-by:`（`git commit -s`）
- [ ] 使用 AI 工具时加了 `Assisted-by:` trailer（AI 不得代签 DCO）
- [ ] 没有提交固件、抓包、密钥、凭据、内部文档（见 CONTRIBUTING.md §6）
- [ ] 驱动行为变更附了 dmesg 证据
- [ ] 文档/CHANGELOG 已同步（如果影响用户可见行为）

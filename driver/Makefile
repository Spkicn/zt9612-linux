# SPDX-License-Identifier: GPL-2.0-only
#
# ZT9612U (ZTOP / Zhaotong Micro ACEV100) USB Wi-Fi driver - out-of-tree build
#
# 这个 Makefile 同时是 Kbuild 文件，也是用户入口。
# 内核目录定位遵循约定：KERNEL_SRC / KSRC / KDIR 任选其一覆盖，
# 默认 /lib/modules/$(uname -r)/build，**不含任何绝对开发机路径**。
#
# 常用目标：
#   make             编译 zt9612.ko
#   make sign        编译并用 MOK 密钥签名（Secure Boot 机器必须）
#   make sign-install 签名 + 安装
#   make install     安装到 /lib/modules/$(uname -r)/extra/ 并 depmod
#   make uninstall   移除已安装模块
#   make checkpatch  用内核 checkpatch.pl 检查源码风格
#   make clean       清理构建产物
#   make help        列出全部目标

obj-m += zt9612.o

KVER ?= $(if $(KERNELRELEASE),$(KERNELRELEASE),$(shell uname -r))
KSRC ?= $(if $(KERNEL_SRC),$(KERNEL_SRC),$(if $(KDIR),$(KDIR),/lib/modules/$(KVER)/build))
JOBS ?= $(shell nproc --ignore=1 2>/dev/null || echo 2)

MODNAME    := zt9612
MODDESTDIR := /lib/modules/$(KVER)/extra

# 签名密钥只应存在于本机，切勿提交到仓库（见 .gitignore）
MOK_PRIV ?= MOK.priv
MOK_PEM  ?= MOK.pem

all: modules

modules:
	$(MAKE) -j$(JOBS) -C $(KSRC) M=$(CURDIR) modules

sign: modules
	@if [ ! -f $(MOK_PRIV) ] || [ ! -f $(MOK_PEM) ]; then \
		echo "错误：找不到签名密钥 $(MOK_PRIV) / $(MOK_PEM)"; \
		echo "生成密钥与 MOK 注册流程见 README.md「Secure Boot」一节"; \
		exit 1; \
	fi
	$(KSRC)/scripts/sign-file sha256 $(MOK_PRIV) $(MOK_PEM) $(MODNAME).ko
	@modinfo $(MODNAME).ko | grep -E "^(name|signer|sig_id)" || true

sign-install: sign install

install: modules
	install -d $(MODDESTDIR)
	install -p -m 644 $(MODNAME).ko $(MODDESTDIR)/
	/sbin/depmod -a $(KVER)
	@echo "已安装到 $(MODDESTDIR)/$(MODNAME).ko"

uninstall:
	rm -f $(MODDESTDIR)/$(MODNAME).ko
	/sbin/depmod -a $(KVER)
	@echo "已移除 $(MODDESTDIR)/$(MODNAME).ko （已加载的模块需重启后才消失）"

checkpatch:
	@test -f $(KSRC)/scripts/checkpatch.pl || { \
		echo "错误：找不到 $(KSRC)/scripts/checkpatch.pl（需要 linux-headers-$(KVER)）"; exit 1; }
	$(KSRC)/scripts/checkpatch.pl --no-tree --strict --file $(MODNAME).c

clean:
	$(MAKE) -C $(KSRC) M=$(CURDIR) clean
	rm -f *.o *.ko *.mod *.mod.c *.cmd .*.cmd modules.order Module.symvers

help:
	@echo "可用目标："
	@grep -E '^[a-zA-Z_-]+:' $(lastword $(MAKEFILE_LIST)) | cut -d: -f1 | sort -u | sed 's/^/  /'
	@echo
	@echo "可覆盖变量：KVER=$(KVER)  KSRC=$(KSRC)  JOBS=$(JOBS)"

.PHONY: all modules sign sign-install install uninstall checkpatch clean help

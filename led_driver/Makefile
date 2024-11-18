# 内核目录，确保路径正确
KERNELDIR := /home/wenshuyu/stm32/stm32mp1-openstlinux-6.1-yocto-mickledore-mpu-v24.06.26/sources/arm-ostl-linux-gnueabi/linux-stm32mp-6.1.82-stm32mp-r2-r0/linux-6.1.82
CURRENT_PATH := $(shell pwd)

obj-m := led_driver.o

# 使用 Bear 生成编译数据库
build: kernel_modules

kernel_modules:
	@if [ -x "$(shell which bear)" ]; then \
		bear -- $(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) modules; \
	else \
		echo "Bear 未安装，请安装 Bear 后重试"; \
		exit 1; \
	fi

clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) clean

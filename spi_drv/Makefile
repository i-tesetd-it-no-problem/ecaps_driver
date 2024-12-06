KERNELDIR := /home/wenshuyu/stm32/stm32mp1-openstlinux-6.1-yocto-mickledore-mpu-v24.06.26/sources/arm-ostl-linux-gnueabi/linux-stm32mp-6.1.82-stm32mp-r2-r0/linux-6.1.82
CURRENT_PATH := $(shell pwd)

obj-m := spi_drv.o

build: kernel_modules

kernel_modules:
	$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) modules;

clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) clean

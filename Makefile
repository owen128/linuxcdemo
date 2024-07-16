obj-m += demo.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all: build

build:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean

install:
	sudo insmod demo.ko

uninstall:
	sudo rmmod demo

show:
	sudo dmesg | tail -n20

reload: uninstall install

test: install
	@echo "模塊已加載，查看設備文件："
	@ls -l /dev/sysmonitor
	@echo "\n查看proc入口內容："
	@cat /proc/sysmonitor
	@echo "\n寫入設備："
	@echo "test" | sudo tee /dev/sysmonitor
	@echo "\n查看內核日誌最後幾行："
	@dmesg | tail -n20

.PHONY: all build clean install uninstall reload test
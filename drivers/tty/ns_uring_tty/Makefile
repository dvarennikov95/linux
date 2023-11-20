obj-$(CONFIG_NS_URING_TTY) += ns_uring_tty.o

ns_uring_tty-y := ns_uring_tty.o tty_chardev.o \
	tty_dev_mmap.o ns_uring.o

ccflags-y += -DCHAN_DYNAMICALY_ALLOCATED
ccflags-y += -DNS_URING_CHAN_INIT_IS_DOWNSTREAM=0

# obj-m += ns_uring_tty.o

# tty-objs := src/ns_uring_tty.o src/tty_chardev.o \
# 	src/tty_dev_mmap.o ns_uring_lib/ns_uring.o
# ccflags-y += -I$(src)/include $(CFLAGS)
# ccflags-y += -DCHAN_DYNAMICALY_ALLOCATED

# KERNEL_VERSION ?= $(shell uname -r)
# KDIR := /lib/modules/$(KERNEL_VERSION)/build/
# EXTRA_CFLAGS := -I$(src)/include  -I$(src)/ns_uring_lib/include

# all:
# 	make -C $(KDIR) M=$(shell pwd) modules
# clean:
# 	make -C $(KDIR) M=$(shell pwd) clean
# install:
# 	make -C $(KDIR) M=$(shell pwd) modules_install
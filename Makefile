# Build the smbdirect module
#
obj-m = smbd.o
smbd-objs += smbdirect.o smbd_rdma.o

KVERSION = $(shell uname -r)

all:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) modules
clean:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) clean

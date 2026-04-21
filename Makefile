obj-m += idkfs_blk.o
obj-m += idkfs_vfs.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	$(CC) -std=c11 -O2 mkfs_idkfs.c -o mkfs.idkfs
	$(CC) -std=c11 -O2 tools/idkfs_fsck.c -o idkfs_fsck

linux-check:
	@test -d "$(KDIR)" || (echo "Linux kernel headers not found at $(KDIR). Build on Linux with matching headers."; exit 1)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(RM) -f mkfs.idkfs idkfs_fsck

modinfo:
	modinfo ./idkfs_blk.ko || true
	modinfo ./idkfs_vfs.ko || true

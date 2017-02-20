KDIR=/lib/modules/$(shell uname -r)/build
ccflags-y	+= -DCONFIG_CIFSSRV_NETLINK_INTERFACE=y
ccflags-y       += -DCONFIG_CIFS_SMB2_SERVER=y 
ccflags-y       += -DCONFIG_CIFS_SERVER=m 

obj-m := cifssrv.o
cifssrvmodule-objs := module  
cifssrv-y := asn1.o auth.o connect.o dcerpc.o 	encrypt.o \
	export.o fh.o misc.o netlink.o netmisc.o nterr.o \
	oplock.o smb1ops.o smb1pdu.o smb2ops.o smb2pdu.o srv.o \
	unicode.o vfs.o winreg.o	

all:
	make -C $(KDIR) SUBDIRS=$(PWD) modules
clean:
	make -C $(KDIR) M=$(PWD) clean

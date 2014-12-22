
HPATH_26 = /lib/modules/`uname -r`/build

obj-m += nbs_driver.o
nbs-y := nbs.o

module: 
	make -C $(HPATH_26) M=`pwd` modules

all: module server

server: server.c

clean:
	rm -f *.o
	rm -f *.ko
	rm -f *.mod.c
	rm -f Module.symvers
	rm -f modules.order
	rm -f server


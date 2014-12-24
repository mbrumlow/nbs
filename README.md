# Network Block Store 

NBS is a network service backed block device driver experiment. The main goal 
is to to provide EBS type functionality to hosted VMs 

## Overview 

This project consist of a Linux kernel driver and a back end service. Both are
written in C.

The Linux driver exports a block device that can be used as a normal block 
device without requiring a user space process to handle the network operations 
of sending block data two and from back end service. 

The service is reasonable to responding to block request and storing the block 
data on the system that the service is running on. 


## Building 

Building is simple right now. The following will build both the kernel module 
and the service. 

> make all 

## Configuration 

Right now all values are hard coded. See the goals section for more information 
on this. 

## Running

Values are currently hard coded at 1 gigabyte for the disk size. The module 
expects to connect to the server on 127.0.0.1 port 1337. The backing file is 
stored in '/tmp/blockdev.img', so make sure you have 1 gigabyte available there 
or change the code in server.c 

You should start the server first, although starting the module first will not 
harm your system, it will just generate IO errors forever until you unload 
and load it with the server started first. 

## Things to be done 

1) Make configurable without needing to change source code. 

2) As said above, handle ERRORS, IO errors on the backing device do not 
currently make their way back up to the module. Although this is #4 in list I 
will likely work to fix that first. 

3) Have the server connect to the kernel and setup the block device. 
This is mostly because any good use case you will not want to have to login to 
this server directly to attach a block device.

4) Make storage redundant. 

5) To minimize the work on the kernel module its self I think the connected
server should handle fanout for redundancy. This would keep the module simple 
allowing the bulk of the redundancy logic to live in user space. Also provides 
ways to change the redundancy without needing to load a new module. 


## Goals 

The end goal is to have a solid kernel module with a well defined protocol and 
a robust service that can handle different redundancy levels and fault 
situations with grace. 

## Notes

I compiled and tested on 3.11.0-12-generic. Due to the kernel changing 
frequently it may need some tweaks to run on older or newer kernels. 






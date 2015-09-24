#!/bin/bash

sudo fio --randrepeat=1 \
	--ioengine=libaio \
	--buffered=1 \
	--name=fio\
	--filename=/media/blueDBM/fio \
	--bs=4k \
	--iodepth=128 \
	--size=10000M \
	--readwrite=write \
	--rwmixwrite=100 \
	--overwrite=1 \
	--numjobs=1 \
	--direct=0
	#--end_fsync=1 \
	#--fsync_on_close=1 \

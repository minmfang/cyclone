#!/bin/sh
echo 128000 > /proc/sys/vm/max_map_count
cat inputs | ./dpdk-16.04/tools/setup.sh
ifdown p1p1
./dpdk-16.04/tools/dpdk_nic_bind.py --bind igb_uio 0000:41:00.0

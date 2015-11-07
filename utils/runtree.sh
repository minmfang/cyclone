#!/bin/bash
rm -f /tmp/raftdata*
rm -f /tmp/dispdata*
echo "starting servers"
cd test1
./rbtree_map_server &> output &
echo $!
#sleep 3
cd ../test2
./rbtree_map_server &> output &
echo $!
#sleep 1
cd ../test3
./rbtree_map_server &> output &
echo $!
sleep 3
echo "starting clients"
cd ../test1
./rbtree_map_client 0 &> output_client &
echo $!
#sleep 3
cd ../test2
./rbtree_map_client 1 &> output_client &
echo $!
#sleep 1
cd ../test3
./rbtree_map_client 2 &> output_client &
echo $!

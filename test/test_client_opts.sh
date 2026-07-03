#!/bin/bash

. common.sh

node_bg

client -C 500 -n 10 -p 1000
client -C 500 -n 10 -r 1000

t1=$(date +%s%3N)
client -C 500 -n 10 -p 100000
t2=$(date +%s%3N)
echo elapsed_ms=$[$t2-$t1]
[ $[ $t2 - $t1 ] -ge 850 -a $[ $t2 - $t1 ] -lt 1500 ]


t1=$(date +%s%3N)
client -C 500 -n 10 -r 10
t2=$(date +%s%3N)
echo elapsed_ms=$[$t2-$t1]
[ $[ $t2 - $t1 ] -ge 850 -a $[ $t2 - $t1 ] -lt 1500 ]

client --ps=1024
client --rs=1024

client --nd 0 -C 1000
client --nd 1 -C 1000

strace_client --nd=0 2>&1 | grep sockopt | grep 'TCP_NODELAY, \[0\]'
strace_client --nd=1 2>&1 | grep sockopt | grep 'TCP_NODELAY, \[1\]'

client --ws -C 0
client --wait-spin -C 0

#client --non-block -C 0

#!/bin/bash

. common.sh

node_bg

client -C 500 -n 10 -p 1000
client -C 500 -n 10 -r 1000

# check period 100k ns, using -p
t1=$(date +%s%N)
client -C 500 -n 10 -p 100000
t2=$(date +%s%N)
elapsed_ns=$(( t2 - t1 ))
echo "elapsed_ns=$elapsed_ns"

[ $elapsed_ns -gt 1000000000 -a $elapsed_ns -lt 1350000000 ]

# check frequency 10/second (i.e. period 100k ns), using -r
t1=$(date +%s%N)
client -C 500 -n 10 -r 10
t2=$(date +%s%N)
elapsed_ns=$(( t2 - t1 ))
echo "elapsed_ns=$elapsed_ns"
[ $elapsed_ns -gt 1000000000 -a $elapsed_ns -lt 1350000000 ]

client --ps=1024
client --rs=1024

client --nd 0 -C 1000
client --nd 1 -C 1000

strace_client --nd=0 2>&1 | grep sockopt | grep 'TCP_NODELAY, \[0\]'
strace_client --nd=1 2>&1 | grep sockopt | grep 'TCP_NODELAY, \[1\]'

client --ws -C 0
client --wait-spin -C 0

#client --non-block -C 0

#!/bin/bash

. common.sh


echo "TEST SKIP 1"

TMP1=$(mktemp /tmp/dw-node-test_skip-1-XXX.txt)

node_bg &> $TMP1

client -C 50 --skip 1 -C 100

grep -q "COMPUTE.50us" $TMP1
! grep -q "COMPUTE.100us" $TMP1
kill_all SIGINT

rm $TMP1


#
echo "TEST SKIP 2"

TMP2=$(mktemp /tmp/dw-node-test_skip-2-XXX.txt)
node_bg &> $TMP2

client -C 50 --skip 2 -C 100 -C 150 -C 200

grep -q "COMPUTE.50us" $TMP2
! grep -q "COMPUTE.100us" $TMP2
! grep -q "COMPUTE.150us" $TMP2
grep -q "COMPUTE.200us" $TMP2
kill_all SIGINT

rm $TMP2


#
echo "TEST SKIP EVERY"

TMP3=$(mktemp /tmp/dw-node-skip-every-test_skip-3-XXX.txt)

node_bg &> $TMP3

client --skip 1,every=2 -C 100 -C 200 -n 2

grep "COMPUTE....us" $TMP3 | head -1 | grep -q " COMPUTE(200us)->REPLY"
grep "COMPUTE....us" $TMP3 | tail -1 | grep -q " COMPUTE(100us)->COMPUTE(200us)->REPLY"

kill_all SIGINT

rm $TMP3


#
echo "TEST SKIP PROB"
TMP4=$(mktemp /tmp/dw-node-skip-prob-test_skip-4-XXX.txt)

node_bg &> $TMP4

client -p 1000 -C 10 --skip 1,prob=0.5 -C 20 -n 100 --seed 17

[ $(grep -c "COMPUTE.10us" $TMP4) -eq 100 ]
[ $(grep -c "COMPUTE.20us" $TMP4) -eq 45 ]

client -p 1000 -C 15 --skip 1,prob=0.3 -C 25 -n 100 --seed 17

[ $(grep -c "COMPUTE.15us" $TMP4) -eq 100 ]
[ $(grep -c "COMPUTE.25us" $TMP4) -eq 68 ]
kill_all SIGINT

rm $TMP4


#
echo "TEST SKIP PROB FWD"

TMP5=$(mktemp /tmp/dw-node1-fwd-skip-test_skip-5-XXX.txt)
TMP6=$(mktemp /tmp/dw-node2-fwd-skip-test_skip-6-XXX.txt)

node_bg -b :7891 &> $TMP5
node_bg -b :7892 &> $TMP6

client -p 1000 -C 10 --skip 1,prob=0.5 -F :7892 -C 20 -n 100 --seed 17

[ $(grep -c "COMPUTE.10us" $TMP5) -eq 100 ]
[ $(grep -c "COMPUTE.20us" $TMP6) -eq 45 ]
kill_all SIGINT

rm $TMP5
rm $TMP6


#
echo "TEST SKIP PROB FWD FWD"

TMP7=$(mktemp /tmp/dw-node1-fwd-fwd-skip-test_skip-7-XXX.txt)
TMP8=$(mktemp /tmp/dw-node2-fwd-fwd-skip-test_skip-8-XXX.txt)
TMP9=$(mktemp /tmp/dw-node3-fwd-fwd-skip-test_skip-9-XXX.txt)

node_bg -b :7891 &> $TMP7
node_bg -b :7892 &> $TMP8
node_bg -b :7893 &> $TMP9

client -p 1000 -C 10 -F :7892 -C 20 --skip 1,prob=0.5 -F :7893 -C 30 -n 100 --seed 17

[ $(grep -c "COMPUTE.10us" $TMP7) -eq 100 ]
[ $(grep -c "COMPUTE.20us" $TMP8) -eq 100 ]
[ $(grep -c "COMPUTE.30us" $TMP9) -eq 45 ]

kill_all SIGINT

echo $?


rm $TMP7
rm $TMP8
rm $TMP9


#!/bin/bash

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$DIR/common.sh"

node --help
client --help

node -h
client -h

node --usage
client --usage

node_bg
client
client -n 10
client -C 10000

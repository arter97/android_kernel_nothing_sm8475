#!/bin/bash

./build_kernel.sh $* || exit 1

sed -i -e 's/ CMDLINE_EOF /             /g' boot.img

echo "Debug boot.img generated"

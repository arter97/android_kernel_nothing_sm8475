#!/bin/bash

./build_kernel.sh $* || exit 1

sed -i -e 's/ boot_using_nvme /                 /g' boot.img

echo "UFS boot.img generated"

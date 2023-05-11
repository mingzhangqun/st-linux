#!/bin/sh

. ~/st/sdk/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi
echo "" > .scmversion

make ARCH=arm O="$PWD/../build" multi_v7_defconfig fragment*.config
for f in `ls -1 ../fragment*.config`; do scripts/kconfig/merge_config.sh -m -r -O $PWD/../build $PWD/../build/.config $f; done
yes '' | make ARCH=arm oldconfig O="$PWD/../build"
make -j ARCH=arm uImage vmlinux dtbs modules LOADADDR=0xC2000040 O="$PWD/../build"
rm -rf $PWD/../build/_install
make ARCH=arm INSTALL_MOD_PATH="$PWD/../build/_install" modules_install O="$PWD/../build"
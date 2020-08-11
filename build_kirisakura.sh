#!/bin/bash

echo
echo "Clean Build Directory"
echo 

#make clean && make mrproper

echo
echo "Issue Build Commands"
echo

mkdir -p out
export ARCH=arm64
export SUBARCH=arm64
export CLANG_PATH=/builds/kernels/linux-x86/clang-r383902c/bin/
export PATH=${CLANG_PATH}:/builds/aosp10/prebuilts/misc/linux-x86/libufdt/:${PATH}
export DTC_EXT=/builds/aosp10/prebuilts/misc/linux-x86/dtc/dtc
export CLANG_TRIPLE=aarch64-linux-gnu-
export CROSS_COMPILE=/builds/aosp10/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin/aarch64-linux-android-
export CROSS_COMPILE_ARM32=/builds/aosp10/prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9/bin/arm-linux-androidkernel-
export LD_LIBRARY_PATH=/builds/kernels/linux-x86/clang-r383902c/lib64/:$LD_LIBRARY_PATH

echo
echo "Set DEFCONFIG"
echo 
make CC=clang AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip O=out kirisakura_defconfig

echo
echo "Build The Good Stuff"
echo 

make CC=clang AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip O=out -j4

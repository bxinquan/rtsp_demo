#!/bin/bash

PLAT=$1
if [ "$PLAT" == "" ]; then
    echo "$0 x86|android_arm|dm6446|dm365|hi3515|hi3518|hi3516|hi3535|hi3516a|hi3518ev20x"
    exit
fi

CROSS_COMPILE=
CFLAGS=

case $PLAT in
    x86)
        CROSS_COMPILE=
        CFLAGS=
        ;;
    android_arm)
        CROSS_COMPILE="arm-linux-androideabi-"
        CFLAGS="--sysroot=/opt/android-ndk-r10/platforms/android-19/arch-arm -march=armv7-a"
        ;;
    dm6446)
        CROSS_COMPILE="arm_v5t_le-"
        CFLAGS=
        ;;
    dm365)
        CROSS_COMPILE="arm-none-linux-gnueabi-"
        CFLAGS=
        ;;
    hi3515)
        CROSS_COMPILE="arm-hisi-linux-"
        CFLAGS=
        ;;
    hi3518)
        CROSS_COMPILE="arm-hisiv100nptl-linux-"
        CFLAGS=
        ;;
    hi3516)
        CROSS_COMPILE="arm-hisiv100-linux-"
        CFLAGS=
        ;;
    hi3535)
        CROSS_COMPILE="arm-hisiv100nptl-linux-"
        CFLAGS="-march=armv7-a -mcpu=cortex-a9"
        ;;
    hi3516a)
        CROSS_COMPILE="arm-hisiv300-linux-"
        CFLAGS="-mcpu=cortex-a7 -mfloat-abi=softfp -mfpu=neon-vfpv4 -mno-unaligned-access -fno-aggressive-loop-optimizations"
        ;;
    hi3518ev20x)
        CROSS_COMPILE="arm-hisiv300-linux-"
        CFLAGS="-march=armv5te -mcpu=arm926ej-s  -mno-unaligned-access -fno-aggressive-loop-optimizations"
        ;;
esac

PREFIX=$(pwd)/install/${PLAT}
CC=${CROSS_COMPILE}gcc
export CC CFLAGS

make -f Makefile.shared clean
make -f Makefile.shared

mkdir -p ${PREFIX}/include
mkdir -p ${PREFIX}/lib

cp rtsp_demo.h ${PREFIX}/include/
cp librtsp_demo.so ${PREFIX}/lib/

rm librtsp_demo.so


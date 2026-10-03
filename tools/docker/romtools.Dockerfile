# The decompilations' own ROM toolchain, for the one job only they can do:
# link the matching ARM9 binary whose map (main.nef.xMAP), trainer-AI object
# and generated resource headers the native core is built against.
#
# amd64 because metroskrew's mwcc is relinked i686 code; i386 libc is what it
# loads. Built and run through OrbStack on macOS (see tools/rom_build.sh).
FROM --platform=linux/amd64 debian:trixie-slim

RUN dpkg --add-architecture i386 \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      bison flex g++ gcc gcc-arm-none-eabi binutils-arm-none-eabi git make \
      ninja-build pkg-config python3 python3-pip wget xz-utils libpng-dev \
      ca-certificates libc6:i386 \
 && pip3 install --break-system-packages 'meson>=1.12,<2' \
 && rm -rf /var/lib/apt/lists/* /root/.cache

ENV LANG=C.UTF-8

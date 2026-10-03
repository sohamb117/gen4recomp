#!/bin/sh
# The armhf cross toolchain and qemu-user, installed without root.
#
# `sudo apt install -y gcc-arm-linux-gnueabihf qemu-user` is still the right
# first thing to try; if it works, pc/Makefile.arm finds the compiler on PATH
# and this script is not needed. It exists for a machine where sudo wants a
# password nobody is there to type, and the alternatives are worse:
#
#   * brew has arm-linux-gnueabihf-binutils and no compiler, so it cannot
#     build anything;
#   * Arm's own arm-none-linux-gnueabihf tarball is half a gigabyte and is a
#     different glibc from the one the device runs;
#   * the arm-none-eabi-gcc already on PATH is a bare-metal compiler. It has
#     no Linux libc at all and fails as a link error that reads like ours.
#
# So: fetch Ubuntu's own packages, which `apt-get download` needs no privilege
# for, and unpack them under a prefix. Same compiler, same glibc, same version
# as the apt install would have given.
#
# Three things have to be repaired after unpacking, because a .deb expects to
# land on /. All three were found by running the compiler, not by reading:
#
#   1. binutils' host-side tools link against private libbfd-*-armhf.so under
#      /usr/lib/x86_64-linux-gnu. patchelf writes the prefix's copy into their
#      RPATH. Every one of those libs is -armhf-suffixed, so nothing here can
#      shadow a system library.
#   2. GCC finds its own cc1 by walking up from the driver, so it relocates by
#      itself and needs nothing.
#   3. libc.so is an ld script holding absolute paths. Passing --sysroot on
#      every compile would fix it; stripping the directory off instead leaves
#      plain names that ld resolves from the search path it already has.
#
# Idempotent: re-running rebuilds the prefix from scratch.
set -eu

PREFIX=${ARM_PREFIX:-$HOME/.local/opt/cross-armhf}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

command -v patchelf >/dev/null 2>&1 || {
    echo "install_arm_toolchain: need patchelf (brew install patchelf)" >&2
    exit 1
}

echo "==> resolving the package closure"
# libc6-dev-armhf-cross and linux-libc-dev-armhf-cross are named explicitly:
# They carry the target headers and apt-cache does not reach them from the
# compiler metapackage alone.
apt-cache depends --recurse --no-recommends --no-suggests --no-conflicts \
    --no-breaks --no-replaces --no-enhances \
    gcc-arm-linux-gnueabihf libc6-dev-armhf-cross linux-libc-dev-armhf-cross \
    qemu-user-static 2>/dev/null \
  | grep '^[a-zA-Z0-9]' | sed 's/:.*//' | sort -u > "$WORK/all.txt"

# Skip anything the host already has: those are its own amd64 libraries and
# unpacking a second copy under the prefix would be dead weight at best.
: > "$WORK/want.txt"
while read -r p; do
    dpkg-query -W -f='${Status}' "$p" 2>/dev/null \
      | grep -q 'install ok installed' || echo "$p" >> "$WORK/want.txt"
done < "$WORK/all.txt"
echo "    $(wc -l < "$WORK/want.txt") packages to fetch"

echo "==> downloading"
mkdir -p "$WORK/debs"
( cd "$WORK/debs" && xargs -a "$WORK/want.txt" apt-get download >/dev/null )

echo "==> unpacking into $PREFIX"
rm -rf "$PREFIX"
mkdir -p "$PREFIX"
for d in "$WORK"/debs/*.deb; do dpkg-deb -x "$d" "$PREFIX"; done

echo "==> repair 1/2: RPATH on the host-side binutils"
libdir=$PREFIX/usr/lib/x86_64-linux-gnu
for f in "$PREFIX"/usr/bin/*; do
    [ -f "$f" ] || continue
    head -c4 "$f" | grep -q ELF || continue
    readelf -d "$f" 2>/dev/null | grep -q 'armhf\.so' || continue
    patchelf --set-rpath "$libdir" "$f"
done
for f in "$libdir"/*.so*; do patchelf --set-rpath "$libdir" "$f" 2>/dev/null || true; done

echo "==> repair 2/2: absolute paths out of the ld scripts"
( cd "$PREFIX/usr/arm-linux-gnueabihf/lib"
  for f in $(grep -l 'GNU ld script' ./*.so ./*.a 2>/dev/null); do
      sed -i 's#/usr/arm-linux-gnueabihf/lib/##g' "$f"
  done )

echo "==> checking it works"
cat > "$WORK/t.c" <<'EOF'
#include <stdio.h>
#include <time.h>
int main(void) {
    printf("ptr=%zu time_t=%zu alignof(long long)=%zu\n",
           sizeof(void *), sizeof(time_t), _Alignof(long long));
    return 0;
}
EOF
"$PREFIX/usr/bin/arm-linux-gnueabihf-gcc" -O1 "$WORK/t.c" -o "$WORK/t.bin"
"$PREFIX/usr/bin/qemu-arm-static" -L "$PREFIX/usr/arm-linux-gnueabihf" "$WORK/t.bin"

echo
echo "toolchain ready: $PREFIX/usr/bin/arm-linux-gnueabihf-gcc"
echo "pc/Makefile.arm looks here by itself; nothing needs to go on PATH."

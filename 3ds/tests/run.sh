#!/usr/bin/env bash
#
# 3ds/tests/run.sh: the 3DS port's gate.
#
#   3ds/tests/run.sh          build, then every cheap check
#   TEST_EMU=1 3ds/tests/run.sh   also boot the 3dsx in the emulator
#
# One script, no suite counter. This is not `pc/tests/run_tests.py`: that
# suite is the PC port and it is only this port's business when a run edits a
# shared host file (pc/src, pc/hw, pc/include).
#
# A check that cannot run yet prints SKIP and is counted separately. It must
# never print ok; a skip that reads as a pass is how a broken thing survives
# a green run.

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
cd "$root" || exit 2

MK="make -f 3ds/Makefile"
TARGET=build/3ds/pokeplatinum.3dsx
ROM=${ROM:-build/rom/pokeplatinum.us.nds}

pass=0
fail=0
skip=0

# The rebuild check below drops a header into 3ds/include for one build. An
# interrupted run must not leave it behind: it would be committed as a shadow.
SENTINEL=3ds/include/.rebuild_sentinel.h
trap 'rm -f "$SENTINEL"' EXIT INT TERM
rm -f "$SENTINEL"

ok()   { printf '  %-52s ok\n'   "$1"; pass=$((pass + 1)); }
bad()  { printf '  %-52s FAILED\n' "$1"; fail=$((fail + 1)); shift; [ $# -gt 0 ] && printf '      %s\n' "$@"; }
skipd(){ printf '  %-52s SKIP (%s)\n' "$1" "$2"; skip=$((skip + 1)); }

if [ -z "${DEVKITARM:-}" ]; then
    echo "3ds/tests: DEVKITARM is not set. source \$HOME/devkitpro/3ds-env.sh" >&2
    exit 2
fi

echo "3DS port checks"

# ---------------------------------------------------------------- build
out=$($MK 2>&1)
if [ $? -eq 0 ]; then
    ok "make -f 3ds/Makefile"
else
    bad "make -f 3ds/Makefile" "$(echo "$out" | tail -3)"
fi

# ---------------------------------------------------------------- 3dsx
if [ -f "$TARGET" ]; then
    magic=$(file -b "$TARGET")
    case $magic in
        *"Nintendo 3DS Homebrew Application"*) ok "file(1): $magic" ;;
        *) bad "file(1) on the 3dsx" "got: $magic" ;;
    esac
else
    bad "the 3dsx exists" "no $TARGET"
fi

# ---------------------------------------------------------------- cartridge
# The ROM rides inside the 3dsx as its RomFS, so this asks the packed file
# rather than the staging directory: parse the 3DSX extended header for the
# filesystem offset, walk the RomFS level-3 header to where file data starts,
# and hash the image out of the 3dsx itself. Equal md5s mean CARDi_ReadRom
# will be reading the same bytes the PC port reads.
if [ -f "$TARGET" ] && [ -f "$ROM" ]; then
    if out=$(python3 - "$TARGET" "$ROM" <<'PY' 2>&1
import hashlib, struct, sys

def digest(f, n):
    m = hashlib.md5()
    while n:
        b = f.read(min(1 << 20, n))
        if not b:
            sys.exit("the 3dsx ends %d bytes early" % n)
        m.update(b)
        n -= len(b)
    return m.hexdigest()

dsx, rom = sys.argv[1], sys.argv[2]
with open(dsx, "rb") as f:
    magic, hdrsz = struct.unpack("<4sH", f.read(6))
    if magic != b"3DSX":
        sys.exit("not a 3dsx: %r" % magic)
    if hdrsz < 44:
        sys.exit("no extended header: the 3dsx carries no RomFS")
    f.seek(40)
    fs, = struct.unpack("<I", f.read(4))
    if not fs:
        sys.exit("the extended header has no filesystem offset")
    f.seek(fs)
    data, = struct.unpack("<I", f.read(40)[36:40])
    f.seek(fs + data)
    with open(rom, "rb") as r:
        r.seek(0, 2)
        size = r.tell()
    got = digest(f, size)
    want = hashlib.md5(open(rom, "rb").read()).hexdigest()
    if got != want:
        sys.exit("md5 %s in the 3dsx, %s on disk" % (got, want))
    print("the 3dsx carries the cartridge: %d bytes, md5 %s" % (size, want))
PY
    ); then
        ok "$out"
    else
        bad "the 3dsx carries the cartridge" "$out"
    fi
elif [ ! -f "$ROM" ]; then
    skipd "the 3dsx carries the cartridge" "no $ROM"
else
    bad "the 3dsx carries the cartridge" "no $TARGET"
fi

# ---------------------------------------------------------------- overlays
# 9.8. The overlay statics' addresses, written into the link after it because
# there is no .symtab here to read them from at run time. A wrong entry fails
# nothing (not the link, not a boot) and then restores an overlay's
# statics from the wrong address, which surfaces days later as a Poketch app
# that remembers its last visit. So every entry is compared with nm.
if [ ! -f build/3ds/overlay_statics.c ]; then
    skipd "the overlay statics' addresses agree with the link" "no overlay_statics.c"
elif out=$(NM=${PREFIX:-arm-none-eabi-}nm 3ds/tests/ov_addrs.py \
             build/3ds/pokeplatinum.elf build/3ds/overlay_statics.c 2>&1); then
    ok "the overlay statics' addresses agree with the link: $(echo "$out" | sed 's/^ov_addrs: //')"
else
    bad "the overlay statics' addresses agree with the link" "$out"
fi

# ---------------------------------------------------------------- cia
# 9.5. The installable title. makerom and bannertool are not devkitPro tools
# and a checkout will usually not have them, so this skips rather than fails,
# but when they ARE there the CIA is built on every gate run, because the
# thing it guards against is an RSF whose fields makerom silently ignored.
if ! command -v makerom > /dev/null || ! command -v bannertool > /dev/null; then
    skipd "the CIA is the title it says it is" "no makerom/bannertool on PATH"
elif ! out=$($MK cia 2>&1); then
    bad "the CIA is the title it says it is" "$(echo "$out" | tail -3)"
elif out=$(3ds/tests/cia_check.py build/3ds/pokeplatinum.cia 2>&1); then
    ok "the CIA is the title it says it is:$(echo "$out" | sed 's/^ *cia *://')"
else
    bad "the CIA is the title it says it is" "$out"
fi

# ---------------------------------------------------------------- toolchain
status=$($MK status 2>&1)
case $status in
    *devkitARM*) ok "the compiler is devkitARM" ;;
    *) bad "the compiler is devkitARM" "$(echo "$status" | grep -i compiler)" ;;
esac

# ---------------------------------------------------------------- include order
# The port's shadows have to be found before the SDK's headers, and -I order is
# the only thing that decides it. There are two chains, 3ds/src sees libctru,
# game code sees the DS SDK, and never both in one TU, so both are checked.
if [ -d 3ds/include ]; then
    port=$($MK -B V=1 2>&1 | grep -o -- '-I[^ ]*' | head -1)
    game=$($MK shadowflags 2>/dev/null | grep -o -- '-I[^ ]*' | head -1)
    case $port:$game in
        -I*3ds/include:-I*3ds/include)
            ok "3ds/include is first on both compile lines" ;;
        *)
            bad "3ds/include is first on both compile lines" \
                "port chain: ${port:-none}" "game chain: ${game:-none}" ;;
    esac
else
    skipd "3ds/include is first on both compile lines" "no 3ds/include yet"
fi

# ---------------------------------------------------------------- shadows
# Which file the compiler actually opened, from its own -H trace. The -I list
# says what was intended; this says what happened.
if out=$(3ds/tests/shadow_chain.sh 2>&1); then
    ok "$(echo "$out" | tail -1)"
else
    bad "every shadowed name resolves to a shadow" "$(echo "$out" | head -4)"
fi

# A NEW header under 3ds/include shadows one that earlier compiles resolved
# elsewhere, and no .d file can mention a file that did not exist. Without the
# shadow stamp the stale objects survive, served from ccache, even. So: add
# one, and the game TU must rebuild.
probe=build/3ds/shadow_probe.o
if [ -f "$probe" ]; then
    before=$(stat -c %y "$probe")
    printf '/* transient; 3ds/tests/run.sh */\n' > "$SENTINEL"
    $MK > /dev/null 2>&1
    after=$(stat -c %y "$probe")
    rm -f "$SENTINEL"
    $MK > /dev/null 2>&1
    if [ "$before" != "$after" ]; then
        ok "a new shadow rebuilds the game translation unit"
    else
        bad "a new shadow rebuilds the game translation unit" \
            "$probe did not move when the sentinel header appeared"
    fi
else
    bad "a new shadow rebuilds the game translation unit" "no $probe"
fi

# ---------------------------------------------------------------- game TU
# pret's src/boot.c, compiled by the 3DS compiler on the game chain with the
# PCH. Everything above this line is code written for this port; this is the
# first object made out of code that was not.
#
# The undefined symbol is the point, not a bonus. boot.c reads the cartridge
# header out of the shared work area, which on the DS is a number in the
# 0x027FFxxx range; if the memory shadow had been missed the file would still
# compile, still link one day, and read whatever is 8 MB into a 4 MB slab. An
# undefined armrec_shared_base is the compiler saying it used the port's base.
obj=build/3ds/gametu_boot.o
if [ -f "$obj" ]; then
    machine=$("$DEVKITARM/bin/arm-none-eabi-readelf" -h "$obj" |
              sed -n 's/^ *Machine: *//p')
    if [ "$machine" != "ARM" ]; then
        bad "a pret game translation unit compiles for ARM11" "machine: $machine"
    elif "$DEVKITARM/bin/arm-none-eabi-nm" "$obj" | grep -q '^ *U armrec_shared_base$'; then
        ok "a pret game translation unit compiles for ARM11"
    else
        bad "a pret game translation unit compiles for ARM11" \
            "no undefined armrec_shared_base in $obj; the memory shadow was missed"
    fi
else
    bad "a pret game translation unit compiles for ARM11" "no $obj"
fi

# What that object was built from. -H does not trace a header reached through
# -include, and the PCH pulls in most of the SDK, so -M is the question that
# gets a real answer: the whole dependency list, command-line includes and all.
# Two things are asked of it; that the shadows are in it, and that libctru is
# not. A game TU that opens <3ds.h> gets an enum four bytes wide on one side of
# the link and one byte wide on the other (settled: the two chains never meet).
deps=$("$DEVKITARM/bin/arm-none-eabi-gcc" $($MK gameflags 2>/dev/null) \
       -M -MF /dev/stdout src/boot.c 2>/dev/null | tr ' \\' '\n\n' | grep '^/')
shadows=$(echo "$deps" | grep -c '/3ds/include/')
ctru=$(echo "$deps" | grep -c 'libctru/')
if [ "$ctru" != "0" ]; then
    bad "the game TU sees the shadows and never libctru" "$ctru libctru header(s)"
elif [ "$shadows" -lt 1 ]; then
    bad "the game TU sees the shadows and never libctru" "no 3ds/include header opened"
else
    ok "the game TU opened $shadows shadow(s) of $(echo "$deps" | grep -c .), no libctru"
fi

# What the host layer asks of a C library, against what this one has. Derived
# from the PC build's objects, so a host file that starts calling something new
# arrives here rather than at the first link. Needs build/pc/obj, which is the
# PC pipeline's and not this port's, so it skips instead of failing without it.
if [ -d build/pc/obj/pc ]; then
    if out=$(3ds/tests/newlib_inventory.py 2>&1); then
        ok "$(echo "$out" | tail -1)"
    else
        bad "every host-layer symbol is in newlib or has a rule" \
            "$(echo "$out" | head -3)"
    fi
else
    skipd "every host-layer symbol is in newlib or has a rule" "no build/pc/obj"
fi

# What the game link cannot resolve, and who owns each name. Not run here,
# compiling 1,392 translation units takes minutes and this suite is the fast
# gate, so it reads the log the last `make -f 3ds/Makefile link` wrote and
# skips when there is none. What it catches is the list GROWING: a new
# undefined symbol with no rule, or a multiple definition, which is never a
# wall and always a file that should not be in the link.
if [ -f build/3ds/link.log ]; then
    if out=$(3ds/tests/link_walls.py 2>&1); then
        ok "$(echo "$out" | tail -1)"
    else
        bad "every unresolved symbol in the game link is owned" \
            "$(echo "$out" | head -3)"
    fi
else
    skipd "every unresolved symbol in the game link is owned" \
          "run make -f 3ds/Makefile link"
fi

# Which pc/src files this console takes, and which it answers itself. The
# build globs that directory, so an unclassified file is linked here by
# default; which is how a POSIX dependency arrives without anybody deciding
# to take it. The table is the decision and this is what holds it against
# the Makefile, the object tree and the archive.
if out=$(python3 3ds/tests/pc_src_class.py 2>&1); then
    ok "$(echo "$out" | tail -1)"
else
    bad "every pc/src file is classified" "$(echo "$out" | head -4)"
fi

# The one place in this port where a wrong answer jumps to a DS address:
# FS_StartOverlay's walk over the overlay's static initialisers. The port
# replaces it with a walk over host pointers registered by a constructor, and
# every part of that, whose definition wins, whether the constructor runs
# under --gc-sections, whether the needles still find their translation units:
# is a chain fact that can be true on the PC chain and false on this one.
if out=$(python3 3ds/tests/sinit_walk.py 2>&1); then
    ok "$(echo "$out" | tail -1)"
else
    bad "the overlay sinit walk is the port's, over host pointers" \
        "$(echo "$out" | head -4)"
fi

# One virtual CPU. The SDK's critical sections are OS_DisableInterrupts, which
# is a variable here, so a second host thread running guest code would race
# everything a DS was entitled to treat as atomic. Nothing asks for one today;
# the check is for the file that does later.
if out=$(python3 3ds/tests/one_cpu.py 2>&1); then
    ok "$(echo "$out" | tail -1)"
else
    bad "no object asks the host for a thread" "$(echo "$out" | head -4)"
fi

# ---------------------------------------------------------------- div by zero
# ARM has no divide instruction, so `/` and `%` are calls into libgcc, and
# libgcc answers a zero divisor with +/-INT_MAX where the cartridge answers
# with the numerator. The wrappers are three instructions each and the link
# reaches them through -Wl,--wrap; the ANSWER is checked on the console,
# because the host that runs this script would die of the same expressions.
# What is checked here is that the four names are wrapped and defined, a
# wrapper that is not on the link line is not a wrapper, and the failure is
# silent arithmetic.
wrapped=$($MK linkflags 2>/dev/null | grep -o -- '--wrap=[a-z_]*' | sed 's/--wrap=//' | sort)
defined=$("$DEVKITARM/bin/arm-none-eabi-nm" build/3ds/3ds_div0_wrap.o 2>/dev/null |
          sed -n 's/^[0-9a-f]* T __wrap_//p' | sort)
want="__aeabi_idiv
__aeabi_idivmod
__aeabi_uidiv
__aeabi_uidivmod"
if [ "$wrapped" = "$want" ] && [ "$defined" = "$want" ]; then
    ok "the four division helpers are wrapped and defined"
else
    bad "the four division helpers are wrapped and defined" \
        "link line: $(echo "$wrapped" | tr '\n' ' ')" \
        "object:    $(echo "$defined" | tr '\n' ' ')"
fi

# ---------------------------------------------------------------- io registers
# HW_REG_BASE moved from a constant to the slab's I/O row, which is what makes
# every reg_* in the game and the SDK land in guest memory. The object file
# says whether that happened: a game translation unit that touches registers
# must carry an undefined reference to the port's pointer. If the shadow were
# missed, the same code would compile to an absolute address and link clean.
# The values behind those addresses are checked on the console; see the
# emulator line below, and 3ds/src/3ds_ioreg.c for why the host cannot.
obj=build/3ds/3ds_ioreg.o
if [ -f "$obj" ]; then
    if "$DEVKITARM/bin/arm-none-eabi-nm" "$obj" | grep -q '^ *U armrec_io_base$'; then
        ok "the game object reaches the registers through the slab"
    else
        bad "the game object reaches the registers through the slab" \
            "no undefined armrec_io_base in $obj"
    fi
else
    bad "the game object reaches the registers through the slab" "no $obj"
fi

# The game and the host libraries are one binary here, and where both define a
# name the game's object wins, weak or not. One of those names is time(),
# which libctru reads while mounting the RomFS.
if out=$(python3 3ds/tests/libc_shadow.py 2>&1); then
    ok "$(echo "$out" | tail -1)"
else
    bad "no game symbol shadows a host library one unaccounted" \
        "$(echo "$out" | head -4)"
fi

# The sound driver's registers are the other kind: literals, one per macro,
# with no base to move. The shadow rewrites all of them and this derives the
# list from the driver's own headers rather than trusting the shadow's.
if out=$(OBJDUMP="$DEVKITARM/bin/arm-none-eabi-objdump" \
         python3 3ds/tests/snd_reg_pin.py 2>&1); then
    ok "$(echo "$out" | tail -1)"
else
    bad "the sound driver's registers reach the slab" "$(echo "$out" | head -4)"
fi

# The shared-work constants the memory shadow moves are derived from the SDK
# headers, not typed. Sixty-one names kept by hand is a list that will one day
# be missing one, silently, because a constant left with the DS's value still
# compiles and still points somewhere.
if out=$(python3 3ds/tests/mmap_pin.py 2>&1); then
    ok "$(echo "$out" | tail -1)"
else
    bad "the memory shadow matches the SDK" "$(echo "$out" | head -3)"
fi

# nitro/types.h is a whole copy of pc/include's rather than an #include_next
# shadow, a typedef cannot be undefined, and u64 needs aligned(4) on it. A
# copy of a hundred-line header forks silently, so it is generated and pinned.
if out=$(python3 3ds/tests/types_pin.py 2>&1); then
    ok "$(echo "$out" | tail -1)"
else
    bad "nitro/types.h is still pc/include's, with the aligned typedefs" \
        "$(echo "$out" | head -3)"
fi

# What the shadows do not reach: DS addresses still written as numbers. Every
# one has to be a 3DS override, a pc/patches hunk, accepted as dead, or not an
# address at all, and named. A hit matching no rule fails, so the next
# literal somebody adds cannot arrive silently.
if out=$(python3 3ds/tests/literal_scan.py 2>&1); then
    ok "$(echo "$out" | tail -1)"
else
    bad "every DS address literal is accounted for" "$(echo "$out" | head -4)"
fi

# ---------------------------------------------------------------- host blit
# 3ds/src/3ds_view.c is pure C, so the host can link it and check the picture
# without a 3DS: geometry, letterbox, font, and four PNGs under build/3ds.
if gcc -O2 -Wall -Wextra -I3ds/src -o build/3ds/view_dump \
       3ds/tests/view_dump.c 3ds/src/3ds_view.c 2> /tmp/3ds_view_dump.cc.log; then
    if out=$(./build/3ds/view_dump 2>&1); then
        ok "view_dump: $(echo "$out" | grep -c ' ok$') geometry checks"
    else
        bad "view_dump" "$(echo "$out" | grep -i fail | head -3)"
    fi
else
    bad "view_dump compiles on the host" "$(tail -3 /tmp/3ds_view_dump.cc.log)"
fi

# ---------------------------------------------------------------- guest map
# 3ds/include/3ds_guest_map.h against tools/armrec/armrec_rt.h. The static
# asserts are the real check and they fire at compile time; the program adds
# ordering, overlap and slab-offset arithmetic that literals cannot state.
if gcc -O2 -Wall -Wextra -I3ds/include -Itools/armrec \
       -o build/3ds/guest_map_check 3ds/tests/guest_map_check.c \
       2> /tmp/3ds_guest_map.cc.log; then
    if out=$(./build/3ds/guest_map_check 2>&1); then
        ok "guest_map_check: $(echo "$out" | grep -c ' ok$') checks"
    else
        bad "guest_map_check" "$(echo "$out" | grep FAILED | head -3)"
    fi
else
    bad "guest_map_check compiles on the host" "$(tail -3 /tmp/3ds_guest_map.cc.log)"
fi

# ---------------------------------------------------------------- translation
# 3ds_guest.c is pure C over a bound slab pointer, so the host runs the same
# guest_selftest() the 3dsx runs, every region, every hole, both directions.
# GUEST_SELFTEST_VERBOSE turns a failure count into a line number.
if gcc -O2 -Wall -Wextra -std=gnu99 -D_POSIX_C_SOURCE=200112L \
       -DGUEST_SELFTEST_VERBOSE -I3ds/include -I3ds/src \
       -o build/3ds/guest_xlat 3ds/tests/guest_xlat.c 3ds/src/3ds_guest.c \
       2> /tmp/3ds_guest_xlat.cc.log; then
    if out=$(./build/3ds/guest_xlat 2>&1); then
        ok "guest_xlat: $(echo "$out" | sed -n 's/.*: \([0-9]*\) checks.*/\1/p') checks"
    else
        bad "guest_xlat" "$(echo "$out" | head -3)"
    fi
else
    bad "guest_xlat compiles on the host" "$(tail -3 /tmp/3ds_guest_xlat.cc.log)"
fi

# ---------------------------------------------------------------- port window
# The window allocator is 3ds_window.c over 3ds_guest.c, both pure C, so the
# host runs the same window_selftest() the 3dsx runs: bump arithmetic,
# 32-byte alignment, zeroing, the SoundSystem-sized block, exhaustion, and the
# round trip through the translator.
if gcc -O2 -Wall -Wextra -std=gnu99 -D_POSIX_C_SOURCE=200112L \
       -DWINDOW_SELFTEST_VERBOSE -I3ds/include -I3ds/src \
       -o build/3ds/window_alloc 3ds/tests/window_alloc.c 3ds/src/3ds_window.c \
       3ds/src/3ds_guest.c 2> /tmp/3ds_window_alloc.cc.log; then
    if out=$(./build/3ds/window_alloc 2>&1); then
        ok "window_alloc: $(echo "$out" | sed -n 's/.*: \([0-9]*\) checks.*/\1/p') checks"
    else
        bad "window_alloc" "$(echo "$out" | head -3)"
    fi
else
    bad "window_alloc compiles on the host" "$(tail -3 /tmp/3ds_window_alloc.cc.log)"
fi

# ---------------------------------------------------------------- sound
# SOUNDxSAD carries 27 bits and the SPU is a DMA engine, so the port window's
# addresses have to survive it: 3ds_snd_addr.c is the classifier, and this run
# also measures how much of a 3DS process's memory the mask aliases onto real
# guest regions; which is the reason the classifier has a `stray` class.
if gcc -O2 -Wall -Wextra -std=gnu99 -D_POSIX_C_SOURCE=200112L \
       -DSND_ADDR_SELFTEST_VERBOSE -I3ds/include -I3ds/src \
       -o build/3ds/snd_addr 3ds/tests/snd_addr.c 3ds/src/3ds_snd_addr.c \
       3ds/src/3ds_window.c 3ds/src/3ds_guest.c \
       2> /tmp/3ds_snd_addr.cc.log; then
    if out=$(./build/3ds/snd_addr 2>&1); then
        ok "snd_addr: $(echo "$out" | sed -n 's/.*: \([0-9]*\) checks.*/\1/p') checks"
    else
        bad "snd_addr" "$(echo "$out" | head -3)"
    fi
else
    bad "snd_addr compiles on the host" "$(tail -3 /tmp/3ds_snd_addr.cc.log)"
fi

# The other half of the same rule: 3ds_snd_addr.c proves it over addresses the
# check makes up, and 3ds_snd_watch.c judges the ones the game actually keys a
# channel with. Here the SPU's log is planted so every class can be produced on
# purpose, including a host pointer put through SOUNDxSAD's mask, which a
# boot is not going to produce to order, and the report the console leaves on
# its SD card is written and read back.
if gcc -O2 -Wall -Wextra -std=gnu99 -D_POSIX_C_SOURCE=200112L \
       -DSND_WATCH_SELFTEST_VERBOSE -I3ds/include -I3ds/src \
       -o build/3ds/snd_watch 3ds/tests/snd_watch.c 3ds/src/3ds_snd_watch.c \
       3ds/src/3ds_guest.c 3ds/src/3ds_window.c 3ds/src/3ds_snd_addr.c \
       3ds/src/3ds_sdcard.c 2> /tmp/3ds_snd_watch.cc.log; then
    if out=$(./build/3ds/snd_watch 2>&1); then
        ok "snd_watch: $(echo "$out" | sed -n 's/.*: \([0-9]*\) checks.*/\1/p') checks"
    else
        bad "snd_watch" "$(echo "$out" | head -3)"
    fi
else
    bad "snd_watch compiles on the host" "$(tail -3 /tmp/3ds_snd_watch.cc.log)"
fi

# ---------------------------------------------------------------- io pages
# 0x04000000 is one slab row and two pages of registers. 3ds_io.c saves and
# restores them the way armrec_rt.c does; the host runs the same self-test.
if gcc -O2 -Wall -Wextra -std=gnu99 -D_POSIX_C_SOURCE=200112L \
       -DIO_SELFTEST_VERBOSE -I3ds/include -I3ds/src \
       -o build/3ds/io_pages 3ds/tests/io_pages.c 3ds/src/3ds_io.c \
       3ds/src/3ds_guest.c 2> /tmp/3ds_io_pages.cc.log; then
    if out=$(./build/3ds/io_pages 2>&1); then
        ok "io_pages: $(echo "$out" | sed -n 's/.*: \([0-9]*\) checks.*/\1/p') checks"
    else
        bad "io_pages" "$(echo "$out" | head -3)"
    fi
else
    bad "io_pages compiles on the host" "$(tail -3 /tmp/3ds_io_pages.cc.log)"
fi

# The mirrored registers are armrec's list and this port does not get its own.
# 3ds_io.c holds a copy because armrec_rt.c cannot be compiled here yet; if the
# PC side ever measures a third register across, this is what fails.
mirror_pc=$(sed -n '/io_mirror\[\] = {/,/^};/p' tools/armrec/armrec_rt.c |
            grep -o '0x[0-9A-Fa-f]*u' | sort -u)
mirror_3ds=$(sed -n '/io_mirror\[\] = {/,/^};/p' 3ds/src/3ds_io.c |
             grep -o '0x[0-9A-Fa-f]*u' | sort -u)
if [ "$mirror_pc" = "$mirror_3ds" ]; then
    ok "the mirrored I/O registers match armrec's list"
else
    bad "the mirrored I/O registers match armrec's list" \
        "armrec: $(echo "$mirror_pc" | tr '\n' ' ')" \
        "3ds:    $(echo "$mirror_3ds" | tr '\n' ' ')"
fi

# ---------------------------------------------------------------- divider
# 0x04000280 is the one part of the I/O row that is not storage: without a
# model every FX_Div, FX_Inv and FX_Sqrt in the game reads whatever was last
# left in a result register. Pure C over the translator, so the host runs the
# same cp_selftest() the 3dsx runs, plus a square-root sweep against its own
# arithmetic that the console has no room for.
if gcc -O2 -Wall -Wextra -std=gnu99 -D_POSIX_C_SOURCE=200112L \
       -DCP_SELFTEST_VERBOSE -I3ds/include -I3ds/src -Itools/armrec \
       -o build/3ds/cp_div 3ds/tests/cp_div.c 3ds/src/3ds_cp.c \
       3ds/src/3ds_guest.c 2> /tmp/3ds_cp_div.cc.log; then
    if out=$(./build/3ds/cp_div 2>&1); then
        ok "cp_div: $(echo "$out" | sed -n 's/.*: \([0-9]*\) checks.*/\1/p') checks"
    else
        bad "cp_div" "$(echo "$out" | head -3)"
    fi
else
    bad "cp_div compiles on the host" "$(tail -3 /tmp/3ds_cp_div.cc.log)"
fi

# ---------------------------------------------------------------- vram
# Nine banks in one 0xA4000 store, reachable through whichever of the five
# windows VRAMCNT puts them in. No mmap and no 16 MB of anything, so the model
# is pure C and the host runs all of it.
if gcc -O2 -Wall -Wextra -std=gnu99 -D_POSIX_C_SOURCE=200112L \
       -DVRAM_SELFTEST_VERBOSE -I3ds/include -I3ds/src -Itools/armrec \
       -o build/3ds/vram_banks 3ds/tests/vram_banks.c 3ds/src/3ds_vram.c \
       3ds/src/3ds_guest.c 2> /tmp/3ds_vram_banks.cc.log; then
    if out=$(./build/3ds/vram_banks 2>&1); then
        ok "vram_banks: $(echo "$out" | sed -n 's/.*: \([0-9]*\) checks.*/\1/p') checks"
    else
        bad "vram_banks" "$(echo "$out" | head -3)"
    fi
else
    bad "vram_banks compiles on the host" "$(tail -3 /tmp/3ds_vram_banks.cc.log)"
fi

# The placement tables and vram_place_bank() are armrec's, copied. They came
# out of a console sweep, not a document, so a copy that drifts is a model that
# looks right and puts every bank in the wrong place.
if out=$(python3 3ds/tests/vram_pin.py 2>&1); then
    ok "$(echo "$out" | tail -1)"
else
    bad "the VRAM placement model still matches armrec" "$(echo "$out" | head -4)"
fi

# ------------------------------------------------------------------ MI walk
# The bulk memory primitives take a void* and write through it, which is right
# on a port whose guest is identity-mapped and wrong here: a VRAM address is a
# DS address and the memory behind it is nine banks placed per 16 KB. The
# wrappers split a range into contiguous host runs. Same self-test the 3dsx
# runs, over a malloc'd slab, with the real functions stubbed; what is under
# test is where each run goes.
if gcc -O2 -Wall -Wextra -std=gnu99 -D_POSIX_C_SOURCE=200112L \
       -DMI_HOST_SELFTEST_VERBOSE -I3ds/include -I3ds/src \
       -o build/3ds/mi_host 3ds/tests/mi_host.c 3ds/src/3ds_mi_host.c \
       3ds/src/3ds_guest.c 3ds/src/3ds_vram.c \
       2> /tmp/3ds_mi_host.cc.log; then
    if out=$(./build/3ds/mi_host 2>&1); then
        ok "mi_host: $(echo "$out" | sed -n 's/.*: \([0-9]*\) checks.*/\1/p') checks"
    else
        bad "mi_host" "$(echo "$out" | head -3)"
    fi
else
    bad "mi_host compiles on the host" "$(tail -3 /tmp/3ds_mi_host.cc.log)"
fi

# --wrap is silent when it misses: a name in the link's list with no wrapper
# behind it links as though the flag were absent.
if out=$(python3 3ds/tests/mi_wrap.py 2>&1); then
    ok "$(echo "$out" | tail -1)"
else
    bad "every wrapped memory primitive has a wrapper" "$(echo "$out" | head -4)"
fi

# Every region armrec maps has a row here. The asserts pin the rows we know
# about; this catches a region *added* to armrec_rt.c's regions[] by PC-port
# work, which would otherwise leave a hole in the slab that translates to NULL.
missing=
for m in $(sed -n '/regions\[\] = {/,/^};/p' tools/armrec/armrec_rt.c |
           grep -o 'ARM_[A-Z0-9_]*_BASE' | sort -u); do
    grep -q "\b$m\b" 3ds/tests/guest_map_check.c || missing="$missing $m"
done
if [ -z "$missing" ]; then
    ok "every armrec region has a guest-map row"
else
    bad "every armrec region has a guest-map row" "not in the map:$missing"
fi

# ---------------------------------------------------------------- armrec mem
# armrec_mem_init() and the region table, over the slab instead of over mmap.
# The only thing this port's file needs from the console is 3ds_mem.h, so the
# host supplies those four functions and runs the rest: the same
# armrec_mem_selftest() the 3dsx runs, plus a slab that refuses to allocate and
# a free-then-init round trip.
if gcc -O2 -Wall -Wextra -std=gnu99 -D_POSIX_C_SOURCE=200112L \
       -DARMREC_MEM_SELFTEST_VERBOSE -I3ds/include -I3ds/src -Itools/armrec \
       -o build/3ds/armrec_mem 3ds/tests/armrec_mem.c 3ds/tests/mem_host.c \
       3ds/src/armrec_mem_3ds.c \
       3ds/src/3ds_guest.c 3ds/src/3ds_vram.c 3ds/src/3ds_io.c \
       2> /tmp/3ds_armrec_mem.cc.log; then
    if out=$(./build/3ds/armrec_mem 2>&1); then
        ok "armrec_mem: $(echo "$out" | sed -n 's/.*: \([0-9]*\) checks.*/\1/p') checks"
    else
        bad "armrec_mem" "$(echo "$out" | grep -i fail | head -3)"
    fi
else
    bad "armrec_mem compiles on the host" "$(tail -3 /tmp/3ds_armrec_mem.cc.log)"
fi

# The rows and their order are armrec_rt.c's, not the slab's: a walker that
# digests region 3 has to find the same region on both ports. This compares the
# names in armrec's table and its VRAM window list against the ones the 3DS
# file reports, in order.
win_name=$(sed -n 's/^#define ARMREC_PORT_WINDOW_NAME "\(.*\)"$/\1/p' \
           tools/armrec/armrec_rt.h)
pc_rows=$( { sed -n '/^static const struct region regions\[\] = {/,/^};/p' \
               tools/armrec/armrec_rt.c |
             sed -n 's/^ *{ *[^,]*, *[^,]*, *\([^,]*\), *[01] *},$/\1/p' |
             sed "s/^ARMREC_PORT_WINDOW_NAME\$/\"$win_name\"/"
             sed -n '/wname\[VW_COUNT\] = {/,/};/p' tools/armrec/armrec_rt.c |
             grep -o '"[^"]*"'; } | tr -d '"')
if [ -x build/3ds/armrec_mem ]; then
    ds_rows=$(./build/3ds/armrec_mem 2>/dev/null |
              sed -n 's/^  region *[0-9]*: [^ ]* + [^ ]*  //p')
else
    ds_rows=
fi
if [ -n "$ds_rows" ] && [ "$pc_rows" = "$ds_rows" ]; then
    ok "the region table matches armrec's, name for name and in order"
else
    bad "the region table matches armrec's, name for name and in order" \
        "armrec: $(echo "$pc_rows" | tr '\n' '|')" \
        "3ds:    $(echo "$ds_rows" | tr '\n' '|')"
fi

# ---------------------------------------------------------------- digest
# The state digest, walking guest memory through the translator instead of
# through a cast. VRAM is why it is not one call per region: a window's blocks
# come from whichever banks VRAMCNT put there, and a block with no bank hashes
# as the zeros both ports read.
if gcc -O2 -Wall -Wextra -std=gnu99 -D_POSIX_C_SOURCE=200112L \
       -DSTATE_SELFTEST_VERBOSE -I3ds/include -I3ds/src -Itools/armrec \
       -o build/3ds/state_digest 3ds/tests/state_digest.c 3ds/tests/mem_host.c \
       3ds/src/3ds_state.c 3ds/src/armrec_mem_3ds.c 3ds/src/3ds_guest.c \
       3ds/src/3ds_vram.c 3ds/src/3ds_io.c 2> /tmp/3ds_state_digest.cc.log; then
    if out=$(./build/3ds/state_digest 2>&1); then
        ok "state_digest: $(echo "$out" | sed -n 's/.*: \([0-9]*\) checks.*/\1/p') checks"
    else
        bad "state_digest" "$(echo "$out" | grep -i fail | head -3)"
    fi
else
    bad "state_digest compiles on the host" "$(tail -3 /tmp/3ds_state_digest.cc.log)"
fi

# And the claim that makes that digest worth anything: the PC port, linked from
# its own files and identity-mapping guest memory the way it always has, digests
# a zeroed map to the same number. Needs 32-bit libraries, armrec_rt.c refuses
# to compile otherwise, so it skips rather than failing where they are absent.
pin=$(sed -n 's/^#define STATE_ZERO_DIGEST 0x\([0-9A-Fa-f]*\)ULL$/\1/p' \
      3ds/src/3ds_state.h)
if gcc -O1 -w -std=gnu99 -m32 -fno-pie -no-pie \
       -Itools/armrec -Ipc/src -Ipc/include \
       -o build/3ds/pc_zero_digest 3ds/tests/pc_zero_digest.c \
       pc/src/pc_state.c tools/armrec/armrec_rt.c \
       2> /tmp/3ds_pc_zero_digest.cc.log; then
    out=$(./build/3ds/pc_zero_digest 2>&1)
    pc_digest=$(echo "$out" | sed -n 's/^pc_zero_digest: \([0-9A-F]*\) .*/\1/p')
    if [ "$pc_digest" = "$pin" ]; then
        ok "the PC port digests a zeroed map to $pin too"
    else
        bad "the PC port digests a zeroed map to $pin too" \
            "PC says: ${pc_digest:-nothing}" "$(echo "$out" | tail -1)"
    fi
else
    skipd "the PC port digests a zeroed map the same" "no 32-bit libraries"
fi

# ---------------------------------------------------------------- input
# The two words a reset console starts with, published through the translator
# by the same pc_input_init() name the PC port's crt calls.
if gcc -O2 -Wall -Wextra -std=gnu99 -D_POSIX_C_SOURCE=200112L \
       -DINPUT_SELFTEST_VERBOSE -I3ds/include -I3ds/src -Itools/armrec \
       -o build/3ds/input_reset 3ds/tests/input_reset.c 3ds/tests/mem_host.c \
       3ds/src/3ds_input.c 3ds/src/3ds_replay.c 3ds/src/armrec_mem_3ds.c \
       3ds/src/3ds_guest.c \
       3ds/src/3ds_vram.c 3ds/src/3ds_io.c 2> /tmp/3ds_input_reset.cc.log; then
    if out=$(./build/3ds/input_reset 2>&1); then
        ok "input_reset: $(echo "$out" | sed -n 's/.*: \([0-9]*\) checks.*/\1/p') checks"
    else
        bad "input_reset" "$(echo "$out" | grep -i fail | head -3)"
    fi
else
    bad "input_reset compiles on the host" "$(tail -3 /tmp/3ds_input_reset.cc.log)"
fi

# The two addresses and the two masks are pc/src/pc_input.c's. Both ports have
# to idle identically or a later comparison between them means nothing, and
# neither file can include the other's header.
pc_input=$(grep -o '0x0400013[0-9A-Fa-f]*u\|0x027FFFA8u\|0x03FFu\|0x2C00u' \
           pc/src/pc_input.c | sort -u)
ds_input=$(grep -o '0x0400013[0-9A-Fa-f]*u\|0x027FFFA8u\|0x03FFu\|0x2C00u' \
           3ds/src/3ds_input.h | sort -u)
if [ -n "$ds_input" ] && [ "$pc_input" = "$ds_input" ]; then
    ok "the keypad words match pc/src/pc_input.c"
else
    bad "the keypad words match pc/src/pc_input.c" \
        "pc:  $(echo "$pc_input" | tr '\n' ' ')" \
        "3ds: $(echo "$ds_input" | tr '\n' ' ')"
fi

# ---------------------------------------------------------------- replay
# Scripted input, so a scene deeper than the title screen can be reached the
# same way twice. The parser and the frame walk run here, and so do the PC
# port's own scripts; that last part is the claim: these are the same files,
# not a dialect, so a session recorded on the desktop replays on the console.
if gcc -O2 -Wall -Wextra -std=gnu99 -D_POSIX_C_SOURCE=200112L \
       -DREPLAY_SELFTEST_VERBOSE -I3ds/include -I3ds/src \
       -o build/3ds/replay 3ds/tests/replay.c 3ds/src/3ds_replay.c \
       2> /tmp/3ds_replay.cc.log; then
    if out=$(./build/3ds/replay 2>&1); then
        ok "replay: $(echo "$out" | sed -n 's/.*: \([0-9]*\) checks.*/\1/p') checks"
    else
        bad "replay" "$(echo "$out" | grep -iv '^3ds-replay' | head -3)"
    fi
else
    bad "replay compiles on the host" "$(tail -3 /tmp/3ds_replay.cc.log)"
fi

# The button names are pc/src/pc_input.c's table, copied because neither file
# can include the other's header. A name that moves in one and not the other
# turns a recorded script into a different session, silently.
pc_names=$(sed -n '/sButtons\[\]/,/^};/p' pc/src/pc_input.c \
           | grep -o '"[A-Z]*", *0x[0-9A-Fa-f]*' | tr -d ' ' | sort)
ds_names=$(sed -n '/sButtons\[\] *= *{/,/^};/p' 3ds/src/3ds_replay.c \
           | grep -o '"[A-Z]*", *0x[0-9A-Fa-f]*' | tr -d ' ' | sort)
if [ -n "$ds_names" ] && [ "$pc_names" = "$ds_names" ]; then
    ok "the button names match pc/src/pc_input.c: $(echo "$ds_names" | wc -l)"
else
    bad "the button names match pc/src/pc_input.c" \
        "pc:  $(echo "$pc_names" | tr '\n' ' ')" \
        "3ds: $(echo "$ds_names" | tr '\n' ' ')"
fi

# ---------------------------------------------------------------- frame time
# What a frame cost and what was in it. The arithmetic and the windowing run
# over a made-up clock; the report is written and read back, because the file
# on the SD card is the only thing this console says about its frame rate.
# 3ds_apt.c and 3ds_watchdog.c are here for the same measurement: what stops
# the frames, and which stop is legitimate. A Home press puts minutes of host
# time inside one frame and this is where the number that takes it back out is
# computed; a port that will never present again is the other end of it.
if gcc -O2 -Wall -Wextra -std=gnu99 -D_POSIX_C_SOURCE=200112L \
       -DPERF_SELFTEST_VERBOSE -DAPT_SELFTEST_VERBOSE -I3ds/include -I3ds/src \
       -o build/3ds/perf 3ds/tests/perf.c 3ds/src/3ds_perf.c \
       3ds/src/3ds_apt.c 3ds/src/3ds_watchdog.c 3ds/src/3ds_sdcard.c \
       2> /tmp/3ds_perf.cc.log; then
    if out=$(./build/3ds/perf 2>&1); then
        ok "perf: $(echo "$out" | sed -n 's/.*: \([0-9]*\) checks.*/\1/p') checks"
    else
        bad "perf" "$(echo "$out" | head -3)"
    fi
else
    bad "perf compiles on the host" "$(tail -3 /tmp/3ds_perf.cc.log)"
fi

# ---------------------------------------------------------------- home, sleep
# The system can take the console away at any frame boundary, and every way
# out of this binary has to end in the same sequence: the frame-time report,
# the DSP, the cartridge, the screens. What is checked is that there is only
# one of them. A second gfxExit() somewhere else is how the guest path and the
# self-test loop drifted apart the first time, and the failure it produces,
# a console left holding the GPU after the process is gone, is one nothing
# in this suite would see.
#
# Two exceptions, both deliberate and both the same shape. 3ds_fault.c runs
# after a data abort and 3ds_watchdog.c runs on the GSP event thread with the
# main thread wedged; neither is a process that can be asked to tear itself
# down in order, and a teardown attempted from one is how a report turns into
# a second hang. Both draw, then leave through svcExitProcess(), which runs no
# atexit handler and asks no service for anything. The check holds them to
# that; either one calling exit() instead would run the atexit teardown on
# top of whatever went wrong.
apt_why=""
for name in gfxExit romfsExit; do
    where=$(grep -l "$name(" 3ds/src/*.c | grep -v '3ds_apt\.c$' |
            grep -v '3ds_rom\.c$' | grep -v '3ds_fault\.c$' |
            grep -v '3ds_watchdog\.c$')
    [ -n "$where" ] && apt_why="$apt_why $name in $(echo "$where" | tr '\n' ' ')"
done
for f in 3ds_fault 3ds_watchdog; do
    grep -q 'svcExitProcess()' "3ds/src/$f.c" ||
        apt_why="$apt_why $f no longer leaves through svcExitProcess"
    grep -q '\bexit(' "3ds/src/$f.c" &&
        apt_why="$apt_why $f runs the atexit teardown"
done
for hook in ONSUSPEND ONRESTORE ONSLEEP ONWAKEUP ONEXIT; do
    grep -q "APTHOOK_$hook" 3ds/src/3ds_apt.c || apt_why="$apt_why no APTHOOK_$hook"
done
grep -q 'atexit(apt_shutdown)' 3ds/src/3ds_apt.c ||
    apt_why="$apt_why apt_shutdown is not registered with atexit"
grep -q 'apt_install()' 3ds/src/3ds_main.c ||
    apt_why="$apt_why the hook is never installed"
grep -q 'audio_suspend' 3ds/src/3ds_apt.c ||
    apt_why="$apt_why the DSP plays on into the Home Menu"
if [ -z "$apt_why" ]; then
    ok "Home, sleep and close end in one teardown"
else
    bad "Home, sleep and close end in one teardown" "$apt_why"
fi

# ---------------------------------------------------------------- host init
# The order the host models come up in. 3ds/src/3ds_init.c is the sequencer;
# the two binaries here are its two links, with the pc/src steps present
# (the game) and with them absent (the self-test .3dsx, where four of the five
# weak references are NULL). stderr is dropped rather than folded in: the run
# deliberately fails four steps and 3ds_init.c says so on stderr each time.
# Everything the test itself has to report goes to stdout.
hi_src="3ds/tests/host_init.c 3ds/tests/mem_host.c 3ds/src/3ds_init.c
        3ds/src/3ds_input.c 3ds/src/3ds_replay.c 3ds/src/armrec_mem_3ds.c
         3ds/src/3ds_guest.c 3ds/src/3ds_vram.c 3ds/src/3ds_io.c"
hi_ok=1
for hi_variant in "build/3ds/host_init:" \
                  "build/3ds/host_init_bare:-DHOST_INIT_NO_STUBS"; do
    hi_bin=${hi_variant%%:*}
    hi_def=${hi_variant#*:}
    # shellcheck disable=SC2086
    if gcc -O2 -Wall -Wextra -std=gnu99 -D_POSIX_C_SOURCE=200112L $hi_def \
           -I3ds/include -I3ds/src -Itools/armrec \
           -o "$hi_bin" $hi_src 2> /tmp/3ds_host_init.cc.log; then
        if hi_out=$("./$hi_bin" 2> /dev/null); then
            hi_n=$(echo "$hi_out" | sed -n 's/.*: \([0-9]*\) checks.*/\1/p')
        else
            bad "host_init ($hi_bin)" "$(echo "$hi_out" | grep -i fail | head -3)"
            hi_ok=0
        fi
    else
        bad "host_init compiles on the host ($hi_bin)" \
            "$(tail -3 /tmp/3ds_host_init.cc.log)"
        hi_ok=0
    fi
    hi_total=$((${hi_total:-0} + ${hi_n:-0}))
done
[ "$hi_ok" = 1 ] && ok "host_init: $hi_total checks, both links"

# And that the sequencer's order IS pc_main.c's. Neither file can include the
# other (one is the DS SDK's include chain and one is libctru's) so the
# agreement is checked in the source: the calls pc_main.c makes, in the order
# it makes them, against 3ds_init.c's step table. pc_view_init is dropped from
# the comparison because this console answers it with two LCDs instead.
pc_seq=$(grep -o 'pc_[a-z0-9]*_init' pc/src/pc_main.c | awk '!seen[$0]++' \
         | grep -E 'pc_(rom|input|rtc|wvr|snd)_init' | tr '\n' ' ')
ds_seq=$(sed -n '/kSteps\[\] = {/,/^};/p' 3ds/src/3ds_init.c \
         | grep -o 'pc_[a-z0-9]*_init' | tr '\n' ' ')
if [ -n "$ds_seq" ] && [ "$pc_seq" = "$ds_seq" ]; then
    ok "the host init order matches pc/src/pc_main.c"
else
    bad "the host init order matches pc/src/pc_main.c" \
        "pc:  $pc_seq" "3ds: $ds_seq"
fi

# The one fact that lets a responder be registered before NitroMain: the game
# calls PXI_Init() itself and PXI_InitFifo() clears the ARM9 receive
# callbacks. If it ever cleared the responder table too, tags 5, 15 and 7
# would go back to dropped words the moment the game booted, silently, and
# three phases from the file that caused it.
pxi_body=$(awk '/^void PXI_InitFifo\(void\)$/,/^}$/' pc/src/pc_pxi.c)
pxi_writers=$(grep -cE '^ *sResponder\[[a-z]*\] *=' pc/src/pc_pxi.c)
if [ -n "$pxi_body" ] \
   && ! echo "$pxi_body" | grep -q 'sResponder' \
   && echo "$pxi_body" | grep -q 'sRecvCallback' \
   && [ "$pxi_writers" = 1 ]; then
    ok "PXI_InitFifo does not clear the responder table"
else
    bad "PXI_InitFifo does not clear the responder table" \
        "sResponder is written on $pxi_writers line(s) outside the setter"
fi

# ---------------------------------------------------------------- 2D renderer
# 8.1: pc/hw/pc_gpu2d.c no longer casts a guest address to a host pointer, and
# the claim is not that the macro compiles but that the renderer still draws
# the same picture with the address space taken out of it. The background path links the GPU
# renderer beside it for the same reason: the two compose the same frame, and
# whether they agree is a question a build machine can answer pixel for pixel. The oracle is here
# rather than on the console: pc_gpu2d.c includes no DS SDK header, so it
# builds for the host against the same 3ds/src model files, while the .3dsx
# links no pc/hw and the game link is waiting on 8.4.
#
# Two binaries, one row. The texture converter adds the 3D texture converter to this check
# rather than beside it: both ask the same question of the same oracle, does
# a model in 3ds/src agree with the renderer in pc/hw, over real guest memory,
# and the tile cache already took this row from 90 checks to 320 without adding a row
# of its own. The count in the line is the sum, and a failure in either fails
# the row and names which one.
oracle_checks=0
oracle_fail=""

count_of() { echo "$1" | sed -n 's/.*: \([0-9]*\) checks.*/\1/p'; }

if gcc -O2 -Wall -Wextra -std=gnu99 -D_POSIX_C_SOURCE=200112L -D__3DS__ \
       -I3ds/include -I3ds/src -Itools/armrec -Ipc/include -Ipc/hw -Ipc/src \
       -o build/3ds/gpu2d_render 3ds/tests/gpu2d_render.c 3ds/tests/mem_host.c \
       pc/hw/pc_gpu2d.c 3ds/src/3ds_hostmap.c 3ds/src/armrec_mem_3ds.c \
       3ds/src/armrec_rt_3ds.c 3ds/src/3ds_guest.c 3ds/src/3ds_vram.c \
       3ds/src/3ds_io.c 3ds/src/3ds_cp.c 3ds/src/3ds_tile.c \
       3ds/src/3ds_gpu2d.c 3ds/src/3ds_layer3d.c 3ds/src/3ds_effect.c \
       2> /tmp/3ds_gpu2d.cc.log; then
    if out=$(./build/3ds/gpu2d_render 2>&1); then
        oracle_checks=$((oracle_checks + $(count_of "$out")))
        surfaces=$(echo "$out" | sed -n 's/^  surface/      surface/p')
    else
        oracle_fail="gpu2d_render: $(echo "$out" | grep -i fail | head -3)"
    fi
else
    oracle_fail="gpu2d_render does not compile: $(tail -3 /tmp/3ds_gpu2d.cc.log)"
fi

# The texture converter. It INCLUDES pc/hw/pc_gpu3d_soft.c rather than linking it, because
# the texture unit and the shading it is checked against are static in that
# file; see the test's own header for why exporting them would have been the
# worse trade.
if gcc -O2 -Wall -Wextra -std=gnu99 -D_POSIX_C_SOURCE=200112L -D__3DS__ \
       -I3ds/include -I3ds/src -Itools/armrec -Ipc/include -Ipc/hw -Ipc/src \
       -o build/3ds/tex3d_convert 3ds/tests/tex3d_convert.c \
       3ds/tests/mem_host.c 3ds/src/3ds_hostmap.c 3ds/src/armrec_mem_3ds.c \
       3ds/src/armrec_rt_3ds.c 3ds/src/3ds_guest.c 3ds/src/3ds_vram.c \
       3ds/src/3ds_io.c 3ds/src/3ds_cp.c 3ds/src/3ds_tile.c \
       3ds/src/3ds_tex3d.c \
       2> /tmp/3ds_tex3d.cc.log; then
    if out=$(./build/3ds/tex3d_convert 2>&1); then
        oracle_checks=$((oracle_checks + $(count_of "$out")))
        texels=$(echo "$out" | sed -n 's/.*, \([0-9]*\) texels compared/      \1 texels against the software texture unit/p')
    else
        oracle_fail="${oracle_fail}${oracle_fail:+; }tex3d_convert: $(echo "$out" | grep -i fail | head -3)"
    fi
else
    oracle_fail="${oracle_fail}${oracle_fail:+; }tex3d_convert does not compile: $(tail -3 /tmp/3ds_tex3d.cc.log)"
fi

# The 3D producer. The vertex transform and the depth measurement, against the same
# software renderer, included whole again, because interp_interpolate_z() is
# static and it is the oracle for what a DS depth ramp does.
if gcc -O2 -Wall -Wextra -std=gnu99 -D_POSIX_C_SOURCE=200112L -D__3DS__ \
       -I3ds/include -I3ds/src -Itools/armrec -Ipc/include -Ipc/hw -Ipc/src \
       -o build/3ds/pica3d_vtx 3ds/tests/pica3d_vtx.c 3ds/src/3ds_pica3d.c \
       3ds/tests/mem_host.c 3ds/src/3ds_hostmap.c 3ds/src/armrec_mem_3ds.c \
       3ds/src/armrec_rt_3ds.c 3ds/src/3ds_guest.c 3ds/src/3ds_vram.c \
       3ds/src/3ds_io.c 3ds/src/3ds_cp.c \
       2> /tmp/3ds_pica3d.cc.log; then
    if out=$(./build/3ds/pica3d_vtx 2>&1); then
        oracle_checks=$((oracle_checks + $(count_of "$out")))
        depth=$(echo "$out" | sed -n 's/^      tail/      depth tail/p')
    else
        oracle_fail="${oracle_fail}${oracle_fail:+; }pica3d_vtx: $(echo "$out" | grep -i fail | head -3)"
    fi
else
    oracle_fail="${oracle_fail}${oracle_fail:+; }pica3d_vtx does not compile: $(tail -3 /tmp/3ds_pica3d.cc.log)"
fi

if [ -z "$oracle_fail" ]; then
    ok "the host oracles agree with pc/hw: $oracle_checks checks"
    [ -n "$surfaces" ] && echo "$surfaces"
    [ -n "$texels" ] && echo "$texels"
    [ -n "$depth" ] && echo "$depth"
else
    bad "the host oracles agree with pc/hw" "$oracle_fail"
fi

# And that the file really has no cast left. The macro is the whole of the host map's
# change to a file both ports compile, so a new `(uintptr_t)` in it is either a
# new guest read that skipped the hook or someone undoing the hook.
casts=$(grep -c '(uintptr_t)' pc/hw/pc_gpu2d.c)
if [ "$casts" = 1 ]; then
    ok "pc_gpu2d.c reaches guest memory only through G2D_HOST"
else
    bad "pc_gpu2d.c reaches guest memory only through G2D_HOST" \
        "$casts occurrence(s) of (uintptr_t); 1 is the PC identity macro"
fi

# ---------------------------------------------------------------- 3D renderer
# The same rule for the geometry engine and the rasterizer, and it cost more
# here than it did there: POWCNT1 gates every geometry write, an unmapped read
# answers zero on the emulator rather than faulting, and the engine therefore
# refused every command the game sent while looking like a game that had not
# drawn yet. The three extra casts in pc_gpu3d.c are pc_gpu3d_store_through()'s,
# which compare a HOST pointer against the staging buffer and are not guest
# reads.
casts=$(grep -c '(uintptr_t)' pc/hw/pc_gpu3d.c)
softcasts=$(grep -c '(uintptr_t)' pc/hw/pc_gpu3d_soft.c)
if [ "$casts" = 4 ] && [ "$softcasts" = 1 ]; then
    ok "the 3D files reach guest memory only through G3D_HOST"
else
    bad "the 3D files reach guest memory only through G3D_HOST" \
        "pc_gpu3d.c has $casts (1 identity macro + 3 host-pointer compares)," \
        "pc_gpu3d_soft.c has $softcasts (1 identity macro)"
fi

# And the buffers the rasterizer renders into. At the desktop maximum, 342
# columns wide, doubled for HD; the three are 2.12 MB each, which is 6.35 MB
# of a 64 MB console for two enhancements it has no screen for. Sized at the
# DS's own resolution here; this asserts the sizes in the linked image rather
# than the #define, because the #define is not what the loader allocates.
if [ -f build/3ds/pokeplatinum.elf ]; then
    bufs=$(nm -S build/3ds/pokeplatinum.elf 2>/dev/null \
           | awk '$4=="ColorBuffer"||$4=="DepthBuffer"||$4=="AttrBuffer" \
                  {n++; t+=strtonum("0x" $2)} END {print n, t+0}')
    set -- $bufs
    if [ "${1:-0}" != 3 ]; then
        bad "the render buffers are the DS's size, not the desktop's" \
            "found ${1:-0} of the three in the image"
    elif [ "$2" -le 1310720 ]; then
        ok "the render buffers are the DS's size, not the desktop's: $2 bytes for three"
    else
        bad "the render buffers are the DS's size, not the desktop's" \
            "$2 bytes for the three; the DS-sized trio is about 1.2 MB"
    fi
else
    skipd "the render buffers are the DS's size, not the desktop's" "no ELF"
fi

# ---------------------------------------------------------------- fault
# What the fault screen says. The half that matters after a crash is the
# sentence naming the address, and it is pure C over the translator, so the
# host compiles 3ds_fault.c without __3DS__ and gets that function alone. The
# drawing half is checked on the emulator by the L+R+Y probe.
if gcc -O2 -Wall -Wextra -std=gnu99 -D_POSIX_C_SOURCE=200112L \
       -I3ds/include -I3ds/src -Itools/armrec \
       -o build/3ds/fault_report 3ds/tests/fault_report.c 3ds/tests/mem_host.c \
       3ds/src/3ds_fault.c 3ds/src/armrec_mem_3ds.c 3ds/src/3ds_guest.c \
       3ds/src/3ds_vram.c 3ds/src/3ds_io.c 2> /tmp/3ds_fault_report.cc.log; then
    if out=$(./build/3ds/fault_report 2>&1); then
        ok "fault_report: $(echo "$out" | sed -n 's/.*: \([0-9]*\) checks.*/\1/p') checks"
    else
        bad "fault_report" "$(echo "$out" | grep -i fail | head -3)"
    fi
else
    bad "fault_report compiles on the host" "$(tail -3 /tmp/3ds_fault_report.cc.log)"
fi

# ---------------------------------------------------------------- emulator
if [ "${TEST_EMU:-0}" = "1" ]; then
    # Fast-forwarded by default. Azahar's frame limiter is a percentage of real
    # CONSOLE speed, not a host limit, so leaving it at 100 paces this section
    # against a 268 MHz ARM11 for no reason: raised, the emulator runs at about
    # 568% here and the game presents 19 FPS instead of 6. Nothing in this
    # section is a wall-clock claim, the frame times the port reports are
    # ARM11 system ticks, which advance with emulated time, and the sound
    # check gets three times the frames in the same seconds, which is the
    # check getting stronger. `SPEED= 3ds/tests/run.sh` runs at console speed.
    export SPEED=${SPEED-1000}
    # Booting is half of it. The other half is what the console then says;
    # every model's self-test runs there and its verdict is the corner block's
    # colour, which shot_verdict.py reads back. Without that second half this
    # check reported ok over a console drawing SELF FAIL.
    #
    # One boot, two claims. shot_capture.sh grabs the diagnostic screen, hands
    # the console to the game with A and grabs a second frame, so the game
    # check costs no extra emulator run, and it needs the diagnostic shot
    # anyway, to find where the two panels landed in the window.
    shot=build/3ds/shots/run-emu.png
    gameshot=build/3ds/shots/run-game.png
    rc=0
    out=$(SHOT_DIAG="$shot" 3ds/tests/shot_capture.sh "$TARGET" "$gameshot" 2>&1) || rc=$?

    if [ ! -s "$shot" ]; then
        bad "the emulator booted it and the console passes" "$out"
    elif verdict=$(3ds/tests/shot_verdict.py "$shot" 2>&1); then
        ok "the emulator booted it and the console passes: $verdict"
    else
        bad "the emulator booted it and the console passes" "$verdict"
    fi

    # Black is the only failure. A white screen is the DS's forced blank and a
    # logo is a logo; both are real frames, and the line says which.
    if [ "$rc" = "0" ]; then
        ok "the game draws a frame and it is not black: $out"
    else
        bad "the game draws a frame and it is not black" "$out"
    fi

    # The GPU present, and it is the phase's whole claim: the picture the PICA draws is
    # the picture the CPU blit drew, byte for byte. The console is the one
    # asked, not a screenshot, PRESENT_VERIFY composes every frame with
    # 3ds_view.c's blit and memcmps it against what the transfer engine left
    # in the framebuffer, so what lands on the card is a count and not an
    # impression. A screenshot cannot make this claim: the first version of
    # this path was sixty-four rows out and photographed as correct, because
    # the rows it lost were letterbox-coloured.
    #
    # It boots into the game rather than stopping at the diagnostic screen: a
    # flat test pattern is four colours and the title is thousands.
    #
    # And it runs with the backgrounds on the GPU, which is the second half of
    # the same claim: a frame the PICA composed out of tiles is compared with
    # the frame pc_gpu2d.c composed in software, whole and byte for byte. The
    # two paths run together only here, shipping, one of them replaces the
    # other, so `tile-frames` has to be non-zero or this check passed by
    # never taking the path it is about.
    #
    # The replay is staged here rather than assumed. The scenes this renderer
    # can draw are menus, and the buttons above only get as far as the title
    # screen; the run reaches a menu because the script on the card walks it
    # there. Leaving that to whatever the card happened to hold made this check
    # pass or fail on invisible state; it reported "identical, but no screen
    # was composed out of tiles" the first time somebody moved the file.
    verifyrep=$HOME/.local/share/azahar-emu/sdmc/3ds/pokeplatinum/present-report.txt
    verifycfg=$(dirname "$verifyrep")/present.txt
    bgcfg=$(dirname "$verifyrep")/bg.txt
    replaycfg=$(dirname "$verifyrep")/input.txt
    rm -f "$verifyrep"
    mkdir -p "$(dirname "$verifycfg")"
    echo verify > "$verifycfg"
    echo gpu > "$bgcfg"
    cp pc/replays/new-game.txt "$replaycfg"
    if ! out=$(INPUT="wait:3 hold:a:1 wait:${VERIFY_WAIT:-45} hold:m:1 wait:2"                3ds/tests/azahar_shot.sh "$TARGET" "" "${WARMUP:-12}" 2>&1); then
        bad "the GPU draws the same picture the CPU blit did" "$out"
    elif [ "$(sed -n 's/^verdict //p' "$verifyrep" 2>/dev/null)" != "PASS" ]; then
        bad "the GPU draws the same picture the CPU blit did"             "$(cat "$verifyrep" 2>/dev/null || echo "no report at $verifyrep")"
    elif [ "$(sed -n 's/^tile-frames //p' "$verifyrep")" = "0" ]; then
        bad "the GPU draws the same picture the CPU blit did" \
            "identical, but no screen was composed out of tiles, raise VERIFY_WAIT"
    else
        ok "the GPU draws the same picture the CPU blit did: $(sed -n 's/^exact-identical //p' "$verifyrep") of $(sed -n 's/^exact-frames //p' "$verifyrep") frames byte-identical, $(sed -n 's/^tile-identical //p' "$verifyrep") of them composed on the GPU"
        # The effect pass: the frames that ran the blend unit are held to a bound rather
        # than to equality, because a PICA coefficient is a byte over 255 and
        # the DS's is a sixteenth. The verdict above already fails if the
        # bound is exceeded; this line is what says how close it came.
        approx=$(sed -n 's/^approx-frames //p' "$verifyrep")
        if [ "${approx:-0}" != "0" ]; then
            echo "      blend: $approx frames, widest channel $(sed -n 's/^approx-worst //p' "$verifyrep")"
        fi
    fi
    # Back to the ship path for everything below, and for whoever runs the
    # emulator by hand next: an absent present.txt is `gpu`, an absent bg.txt
    # is `gpu` too, and an absent input.txt gives the buttons back.
    rm -f "$verifycfg" "$bgcfg" "$replaycfg"

    # And the fault screen, through the probe the crt keeps for it: L+R+Y
    # reports an invented data abort at a guest address, Start ends the
    # process. Delivery of a *real* abort is not checked here and cannot be;
    # this emulator answers an unmapped read with zeros and carries on.
    shot=build/3ds/shots/run-fault.png
    if out=$(INPUT="wait:2 press:q press:w wait:1 hold:x:1 wait:2 shot:$shot \
                    release:q release:w hold:m:1 wait:2" \
             3ds/tests/azahar_shot.sh "$TARGET" build/3ds/shots/run-fault-boot.png \
             "${WARMUP:-12}" 2>&1); then
        ok "the fault screen draws and Start leaves: $shot"
    else
        bad "the fault screen draws and Start leaves" "$out"
    fi
    # And the hang detector, on a hang made on purpose. L+R+B stops
    # presenting frames and shortens the limit to three seconds; nothing else
    # can produce this state, because a port that hangs by accident is not
    # something a check can arrange. What is asserted is the report the
    # detector leaves on the card, the verdict, and the breadcrumb naming
    # where the frames stopped, which is this port's answer to the program
    # counter a desktop would print.
    hangrep=$HOME/.local/share/azahar-emu/sdmc/3ds/pokeplatinum/hang-report.txt
    rm -f "$hangrep"
    if ! out=$(INPUT="wait:3 press:q press:w hold:s:1 wait:12 \
                      release:q release:w" \
               3ds/tests/azahar_shot.sh "$TARGET" "" "${WARMUP:-12}" 2>&1); then
        bad "a hang says where it hung" "$out"
    elif [ "$(sed -n 's/^verdict //p' "$hangrep" 2>/dev/null)" != "HANG" ]; then
        bad "a hang says where it hung" \
            "no report at $hangrep; the detector did not fire"
    elif [ "$(sed -n 's/^where //p' "$hangrep")" != "the deliberate hang probe" ]; then
        bad "a hang says where it hung" \
            "fired, but the breadcrumb says $(sed -n 's/^where //p' "$hangrep")"
    else
        ok "a hang says where it hung: $(sed -n 's/^quiet-vblanks //p' "$hangrep") quiet vblanks, last seen in $(sed -n 's/^where //p' "$hangrep")"
    fi

    # And the sound. 3ds/src/3ds_snd_watch.c judges every source address the
    # game keyed a channel with and leaves the answer on the SD card, so the
    # check here is a boot and a grep. The verdict is three-valued and only
    # PASS passes: NONE means nothing was keyed, which is what a silent port
    # looks like from the outside and is the state this whole file exists to
    # tell apart from working sound.
    #
    # Its own boot, and a long one. The picture is on screen inside forty
    # seconds; the first sound defect measured here arrived at frame 370, which
    # is about ninety seconds of this emulator. A check that ran for the shot's
    # forty would have called the corrupted build clean. `frames` is asserted
    # for the same reason, a boot that died early would otherwise pass on a
    # handful of good keyons.
    report=$HOME/.local/share/azahar-emu/sdmc/3ds/pokeplatinum/snd-report.txt
    # 150 seconds at console speed; a third of that fast-forwarded, which
    # still covers more frames than the slow run ever did. The two move
    # together because the check also asserts a floor on frames.
    if [ -n "${SPEED:-}" ]; then
        SND_WAIT=${SND_WAIT:-60}
    else
        SND_WAIT=${SND_WAIT:-150}
    fi
    SND_FRAMES_MIN=${SND_FRAMES_MIN:-500}
    rm -f "$report"
    if ! out=$(INPUT="wait:3 hold:a:1 wait:$SND_WAIT" \
               3ds/tests/azahar_shot.sh "$TARGET" "" "${WARMUP:-12}" 2>&1); then
        bad "the game keys its channels inside the sound heap" "$out"
    elif [ ! -s "$report" ]; then
        bad "the game keys its channels inside the sound heap" \
            "the console left no report at $report"
    else
        v=$(sed -n 's/^verdict //p' "$report")
        f=$(sed -n 's/^frames //p' "$report")
        k=$(sed -n 's/^keyons //p' "$report")
        w=$(sed -n 's/^window //p' "$report")
        if [ "$v" != "PASS" ]; then
            bad "the game keys its channels inside the sound heap" \
                "verdict $v after $f frames" \
                "$(grep -E '^(stray|lost|unreadable|unread|sndaddr) ' "$report" \
                   | tr '\n' ' ')"
        elif [ "${f:-0}" -lt "$SND_FRAMES_MIN" ]; then
            bad "the game keys its channels inside the sound heap" \
                "PASS, but only $f frames, wanted $SND_FRAMES_MIN"
        else
            ok "the game keys its channels inside the sound heap: $w/$k in the window over $f frames"
        fi
    fi

    # The fallback: the same ELF packed WITHOUT --romfs, with the
    # cartridge left on the emulator's SD card instead. This is the shape a
    # netloaded build has, 3dslink would otherwise push 128 MB per
    # iteration, and the only way to know the fallback works is to take the
    # RomFS away and boot it. A pass here is the console reaching every one of
    # its ROM checks through sdmc:.
    #
    # Packed here rather than by a make target: the skinny 3dsx is a thing
    # this check needs and nothing else wants. The SD image is a symlink, so
    # the emulator's card costs no disk.
    skinny=build/3ds/pokeplatinum-skinny.3dsx
    sdcard=$HOME/.local/share/azahar-emu/sdmc/3ds/pokeplatinum
    if [ ! -f "$ROM" ]; then
        skipd "the SD card is the fallback when the 3dsx is skinny" "no $ROM"
    elif [ ! -d "$(dirname "$(dirname "$sdcard")")" ]; then
        skipd "the SD card is the fallback when the 3dsx is skinny" \
              "no emulator SD card at $sdcard"
    elif ! out=$(3dsxtool build/3ds/pokeplatinum.elf "$skinny" \
                   --smdh=build/3ds/pokeplatinum.smdh 2>&1); then
        bad "the SD card is the fallback when the 3dsx is skinny" "$out"
    else
        mkdir -p "$sdcard"
        ln -sf "$(pwd)/$ROM" "$sdcard/$(basename "$ROM")"
        # And the close, on this run because it is the only one in the section
        # that can perform one: every other boot either hands the console to
        # the game with A, after which Start is the game's button and not the
        # loader's, or ends on the fault screen, which leaves through
        # svcExitProcess() on purpose. Start here breaks the diagnostic loop
        # and goes through 3ds_apt.c's teardown, which is the only thing
        # that writes `closed 1` into the frame-time report.
        shot=build/3ds/shots/run-sdmc.png
        perfrep=$sdcard/perf-report.txt
        rm -f "$perfrep"
        if out=$(INPUT="wait:3 shot:$shot hold:m:1 wait:3" \
                 3ds/tests/azahar_shot.sh "$skinny" "" "${WARMUP:-12}" 2>&1); then
            if ! verdict=$(3ds/tests/shot_verdict.py "$shot" 2>&1); then
                bad "the SD card is the fallback when the 3dsx is skinny" "$verdict"
            elif [ "$(sed -n 's/^closed //p' "$perfrep" 2>/dev/null)" != "1" ]; then
                bad "the SD card is the fallback when the 3dsx is skinny" \
                    "$verdict, but Start left without the teardown writing $perfrep"
            else
                ok "the SD card is the fallback when the 3dsx is skinny, and Start closes it: $verdict"
            fi
        else
            bad "the skinny 3dsx boots" "$out"
        fi
    fi
else
    skipd "the emulator boots the 3dsx" "set TEST_EMU=1"
    skipd "the GPU draws the same picture the CPU blit did" "set TEST_EMU=1"
    skipd "the game draws a frame and it is not black" "set TEST_EMU=1"
    skipd "the fault screen draws and Start leaves" "set TEST_EMU=1"
    skipd "a hang says where it hung" "set TEST_EMU=1"
    skipd "the game keys its channels inside the sound heap" "set TEST_EMU=1"
    skipd "the SD card is the fallback when the 3dsx is skinny" "set TEST_EMU=1"
fi

# ----------------------------------------------------------------
printf '\n%d ok, %d failed, %d skipped\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]

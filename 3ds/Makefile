# 3ds/Makefile, Old 3DS homebrew build.
#
#   make -f 3ds/Makefile          write build/3ds/pokeplatinum.{3dsx,elf,smdh}
#   make -f 3ds/Makefile clean
#
# Invoked from the repo root, the same way pc/Makefile is. Official
# 3ds_rules wants a recursive make inside a BUILD directory so its
# %.elf recipe writes the .lst / .map next to the binary instead of
# in cwd. The stock hello-world flip (`BUILD == $(notdir $(CURDIR))`)
# does not work here: our BUILD is build/3ds, not a sibling named
# `build`, and CURDIR at the first invocation is the repo root.
#
# ROOT is derived from this file's path, not from cwd, abspath twice
# over, same reason as pc/Makefile (make matches targets textually).
# Flip on `$(CURDIR) == $(BUILD)`.
#
# Flags match 3ds-env.sh / 3ds_rules: ARMv6K hard-float, -mtp=soft,
# -mword-relocations, 3dsx.specs. The game is built at $(OPT), which is
# -O1 and was measured against -O2 (see OPT below); pc/hw is -O3. -O2
# inlines PMi_ReadRegister and the PM override then never runs (settled
# 5; the bug the PC port already paid for), so the five files the port
# replaces are compiled without automatic inlining; see NOINLINE_OBJS.
#
# The compiler is not the one on $PATH. This machine also has brew's
# arm-none-eabi-gcc 16.2.0, which is a bare-metal toolchain with no
# libctru, no 3dsx.specs and no 3DS crt: linking with it fails in ways
# that look like our bug. devkitARM's own base_tools prepends
# $(DEVKITPRO)/devkitARM/bin to PATH, so the include below already wins:
# what is added here is the check that it *did*, because silently
# building with the wrong compiler is the failure worth spending a
# subprocess on.
#
# Objects depend on $(CFLAGSTAMP), not on this file's mtime: editing a
# comment here must not rebuild everything, and changing -O1 must.

.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. source $$HOME/devkitpro/3ds-env.sh")
endif

# 3ds_rules needs DEVKITPRO; 3ds-env.sh sets both, but a shell with only
# DEVKITARM should not fail three includes deep.
ifeq ($(strip $(DEVKITPRO)),)
export DEVKITPRO := $(abspath $(DEVKITARM)/..)
endif

THISMK := $(abspath $(lastword $(MAKEFILE_LIST)))
ROOT   := $(abspath $(patsubst %/,%,$(dir $(THISMK)))/..)
BUILD  := $(ROOT)/build/3ds
TARGET := pokeplatinum
OUTPUT := $(BUILD)/$(TARGET)

# SMDH fields. Title is the PC SDL window string, UTF-8, including the
# e-acute. Author is wize-logic; the PC window has no CompanyName
# (a decompilation cannot honestly fill that field) but smdhtool needs
# a string and "Unspecified Author" is the 3ds_rules default we refuse.
APP_TITLE  := Pokémon Platinum
APP_AUTHOR := wize-logic

# The picture, and it has to be set BEFORE 3ds_rules: that file defaults
# APP_ICON to libctru's own hello-world icon and writes the %.smdh rule around
# whatever the name holds at include time. Both files are drawn by
# pc/gen_icon.py, which is the desktop and Windows builds' generator, one
# piece of artwork, procedural, owned by this repository. Nothing here is cut
# from the game's own art.
ICONDIR   := $(BUILD)/icon
APP_ICON  := $(ICONDIR)/icon48.png
BANNERPNG := $(ICONDIR)/banner.png

include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

# -fsigned-char here as well as in GAMEABI: these files are covered by host
# tests compiled with the system GCC, where a char is signed, so leaving the
# console on the ARM default would let a test pass over code that behaves
# differently on the console it is testing.
CFLAGS	:=	-g -Wall -O1 -mword-relocations \
			-ffunction-sections -fsigned-char \
			$(ARCH)

# 3ds/include is FIRST, always. It shadows SDK headers, and a shadow that is
# second on the search path is not a shadow. This is the PORT chain: the files
# under 3ds/src, which talk to libctru and never to the DS SDK.
#
# tools/armrec is last and holds one header. 3ds/src/armrec_mem_3ds.c provides
# armrec's memory functions on this console and has to be checked against their
# real prototypes; armrec_rt.h is stdint and stddef only, so it costs the port
# chain nothing and brings no DS SDK with it.
CFLAGS	+=	-I$(ROOT)/3ds/include -I$(CTRULIB)/include -I$(ROOT)/tools/armrec \
			-D__3DS__

ASFLAGS	:=	-g $(ARCH)
# --no-enum-size-warning, and only that one. The two chains disagree about
# enum width on purpose: game objects are -fno-short-enums so a struct lays
# out the way the DS laid it out, libctru and newlib are built the ARM EABI
# way, and ld takes the output's attribute from them. Measured: without this
# flag the link prints one warning per game object, which is 1,392 of them
# once the game link lands. Compiling 3ds/src with -fno-short-enums instead does not
# help, then all of our objects warn, because the attribute the output picks
# comes from libctru either way.
#
# The narrow option is the point. --no-warn-mismatch would also silence a
# float-ABI or wchar mismatch, and those would be real.
#
# --wrap on the four division helpers: 5.6. ARM has no divide instruction, so
# every `/` and `%` is a call into libgcc, and libgcc answers a zero divisor
# with +/-INT_MAX where the cartridge answers with the numerator. The wrappers
# in 3ds/src/3ds_div0_wrap.s are three instructions each and `__real_` reaches the
# original for the non-zero path; overriding __aeabi_idiv0 instead cannot work,
# because by the time it runs the numerator has been overwritten.
DIV0WRAP	:=	-Wl,--wrap=__aeabi_idiv -Wl,--wrap=__aeabi_uidiv \
			-Wl,--wrap=__aeabi_idivmod -Wl,--wrap=__aeabi_uidivmod

# --wrap on the bulk memory primitives: 8.2. The shadow moves every DS
# memory-map constant onto the slab except the VRAM windows, which cannot have
# a base (nine banks in five windows, per-16 KB placement) and stay DS
# addresses translated per access. These functions take a void* and write
# straight through it, so on this console they wrote to nothing. 3ds/src/
# 3ds_mi_host.c splits a range into contiguous host runs and hands each one to
# __real_; the bytes are still pc/src/pc_mi.c's, which is the point of not
# editing it. The Async forms are wrapped separately because --wrap redirects
# undefined references only, and theirs are resolved inside pc_dma.o.
# CARDi_ReadRom is in the list for the same reason and not because it is MI:
# The overlay loader hands it a DS address and the read goes out through the
# file system service, so the kernel writes 300 KB into a page the process
# does not have.
# 3ds/tests/mi_wrap.py checks that every name here has a wrapper behind it.
MIWRAPNAMES	:=	MI_CpuFill8 MI_CpuCopy8 \
			MIi_CpuClear16 MIi_CpuClear32 MIi_CpuClearFast \
			MIi_CpuCopy16 MIi_CpuCopy32 MIi_CpuCopyFast \
			MI_Copy16B MI_Copy32B MI_Copy36B MI_Copy48B MI_Copy64B \
			MI_Zero36B \
			MI_UncompressLZ8 MI_UncompressLZ16 \
			MI_DmaCopy16 MI_DmaCopy32 \
			MI_DmaFill16 MI_DmaFill32 \
			MI_DmaCopy16Async MI_DmaCopy32Async \
			MI_DmaFill16Async MI_DmaFill32Async \
			CARDi_ReadRom
MIWRAP	:=	$(foreach n,$(MIWRAPNAMES),-Wl,--wrap=$(n))

# --wrap on the two software renderers, so the frame breakdown can say how
# much of a frame each one costs. They are called through a function pointer
# from pc/src/pc_video.c, which both ports compile, so a timer around the call
# would have to live in shared code; the link is where this port already
# interposes on names it must not edit. 3ds/src/3ds_render_wrap.c holds them.
RENDERWRAP	:=	-Wl,--wrap=pc_video_set_renderer

# ...and the same problem for the 3D engine, which the frame breakdown was
# charging to the game. pc_gpu3d_vblank() is the whole per-frame 3D event,
# the polygon sort, the register latch and the rasterizer, and pc_video.c is
# its only caller, in another object, so --wrap reaches it where a wrap on
# pc_gpu3d_soft_render_frame() would not: pc_gpu3d.c defines that name weakly
# itself, and a reference an object also defines is not an undefined one.
GPU3DWRAP	:=	-Wl,--wrap=pc_gpu3d_vblank

# --wrap on Bg_GetCharPtr: it hands back a DS VRAM address that game code
# dereferences directly, which is a wild pointer on this console. See
# 3ds/src/3ds_bgptr_wrap.c for why the wrapper is on this function and not on
# the eighteen SDK accessors behind it.
BGPTRWRAP	:=	-Wl,--wrap=Bg_GetCharPtr

# The image's own span, under the names a glibc link gives it. pc/hw/pc_spu.c
# classifies an unreadable SOUNDxSAD as "a host image address" or "lost" and
# asks __executable_start and _end for the bounds; 3dsx.ld calls the same two
# addresses __start__ and __end__. This is a linker fact on both hosts, the
# PC port gets those names from its own linker script and not from a header,
# so the alias belongs in the link and not in a __3DS__ branch inside a file
# both ports compile (which would put pc/tests/run_tests.py in this port's
# gate). 7.13.
IMAGEBOUNDS	:=	-Wl,--defsym=__executable_start=__start__ \
			-Wl,--defsym=_end=__end__

LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,--no-enum-size-warning \
			$(DIV0WRAP) $(MIWRAP) $(RENDERWRAP) $(GPU3DWRAP) $(BGPTRWRAP) $(IMAGEBOUNDS) \
			-Wl,-Map,$(notdir $*.map)

# citro3d ahead of libctru, which is the order a static link needs: the GPU present's
# present path is the only thing in this port that calls it, and it resolves
# gsp, gx and linear-heap names out of libctru behind it. It ships in the same
# directory libctru does rather than in portlibs, so LIBPATHS is unchanged.
LIBS	:=	-lcitro3d -lctru -lm
LIBPATHS	:=	-L$(CTRULIB)/lib
LD	:=	$(CC)

#---------------------------------------------------------------------------------
# The GAME chain, decompiled sources, the SDK wraps, and pc/src.
#
# Two chains, not one, and the reason is settled: a translation unit that
# includes both <nitro.h> and <3ds.h> is the thing this port must not have,
# because the two sides disagree about enum width. So $(CTRULIB)/include
# appears on the PORT chain only, and game code reaches the console through
# this port's own headers instead.
#
# After 3ds/include the order is pc/Makefile's INCLUDES, unchanged. It is not
# an arbitrary list: $(GEN)/generated has to come AFTER $(ROOT)/include
# because both trees carry a game_records.h, include/'s is the GameRecords
# struct, generated/'s is the metang enum, and the ROM build resolves the
# bare name to include/ first. Reordering here would compile a different game.
#
# $(GEN) is the PC pipeline's output directory, used as-is. There is no second
# generator: if it is absent, the compile fails on a missing header,
# which is the honest failure.
SDKROOT     := $(ROOT)/subprojects
NITROSDK    := $(SDKROOT)/NitroSDK-4.2.30001
NITROSYSTEM := $(SDKROOT)/NitroSystem-071126.1
NITROWIFI   := $(SDKROOT)/NitroWiFi-2.1.30003
NITRODWC    := $(SDKROOT)/NitroDWC-2.2.30008
LIBVCT      := $(SDKROOT)/libvct-1.3.1
GEN         := $(ROOT)/build/pc/geninclude

GAMEINC := -I$(ROOT)/3ds/include \
           -I$(BUILD)/geninclude \
           -I$(ROOT)/pc/include \
           -I$(GEN) \
           -I$(ROOT)/include \
           -I$(ROOT)/asm \
           -I$(ROOT)/res \
           -I$(ROOT) \
           -I$(GEN)/generated \
           -I$(ROOT)/lib/crypto/include \
           -I$(ROOT)/lib/gds/include \
           -I$(ROOT)/lib/spl/include \
           -I$(ROOT)/lib/ppwlobby/include \
           -I$(NITROSDK)/include \
           -I$(NITROSYSTEM)/include \
           -I$(NITROWIFI)/include \
           -I$(NITRODWC)/include \
           -I$(NITRODWC)/include/gs \
           -I$(NITRODWC)/include/base \
           -I$(LIBVCT)/include \
           -I$(ROOT)/tools/armrec

# pc/Makefile's DEFINES, verbatim. PLATFORM_PC travels with them and the name
# is the only thing wrong with it: what it selects is the PORT's branch of a
# three-way choice (port / mwcc / ADS), a recorded NitroStaticInit pointer
# instead of a #pragma section, and named events in the ARM7 sound registers.
# Both are what this console needs too. Renaming it would edit pc/ for a
# cosmetic gain and break the PC build in the same commit.
GAMEDEFS := -DPM_KEEP_ASSERTS \
            -DGAME_VERSION=VERSION_PLATINUM \
            -DGAME_LANGUAGE=LANGUAGE_ENGLISH \
            -DPOKEPLATINUM_GENERATED_ENUM \
            -D_MSL_RESTRICT= \
            -DPOKEPLATINUM_REVISION=1 \
            -DSDK_CW_FORCE_EXPORT_SUPPORT -DSDK_TS -DSDK_4M -DSDK_ARM9 \
            -DSDK_CW -DSDK_FINALROM -DSDK_CODE_ARM -DNNS_FINALROM \
            -D_NITRO -DLINK_PPWLOBBY \
            -DPLATFORM_PC \
            -D__3DS__

# __3DS__ is on this chain as well as the port's, and it is not a copy of
# PLATFORM_PC: PLATFORM_PC selects the port's branch over mwcc's and ADS's,
# which this console shares, while __3DS__ is where a pc/src file needs THIS
# console specifically, pc_card_rom.c opens romfs:/ and has no pread. The
# compiler does not predefine it (devkitARM predefines only __DEVKITARM__ and
# __DEVKITPRO__) and 3ds_rules puts it on its own CFLAGS, which this file
# replaces. Measured before adding it: no newlib or devkitARM header keys on
# the name, so it reaches this port's code and nothing else, and the struct
# layout survey is unmoved.

# The ABI flags, measured and settled: -fno-short-enums so an enum is four
# bytes the way mwcc-for-ARM makes it, and _USE_LONG_TIME_T so newlib's time_t
# is the 32-bit one the DS headers assume. NOT -fpack-struct=4, which fixes
# the u64 rows by breaking everything else. The other half of that answer is
# the aligned(4) typedefs, which live in a header rather than a flag.
#
# -w, -fcommon, -fms-extensions, -fwrapv, -fno-strict-aliasing and gnu99 are
# pc/Makefile's, for pc/Makefile's reasons: this is the same source tree
# compiled by a different GCC.
#
# -fsigned-char because the ARM default is the other one. A bare `char` is
# signed on x86 and unsigned on ARM, and GCC follows the target: devkitARM
# defines __CHAR_UNSIGNED__ and the PC build does not. The ROM is built with
# mwcc's `-char signed` (meson.build), so signed is what this game's source
# means everywhere a char is compared or sign-extended, and without this
# flag every one of the 1,392 translation units here disagreed with both the
# cartridge and the PC port.
#
# It was found by hashing the two ports' frames over the same scripted
# session: identical polygon counts every frame, so the game was in lockstep,
# and different pixels from the moment anything was drawn.
GAMEABI := -fno-short-enums -fsigned-char -D_USE_LONG_TIME_T

# Three diagnostics GCC 14 promoted from warnings to errors. This is a
# compiler-version difference and not a source problem: the PC build is on GCC
# 13, where all three are warnings that -w hides, and the ROM build is on mwcc,
# which accepts them. devkitARM is GCC 16.1, so without this 128 game files
# stop compiling for reasons the same source does not have anywhere else.
#
# Named one at a time rather than -fpermissive, which would demote whatever
# GCC promotes next as well. Four of the six fire in this build, measured:
# 104 int-conversion, 9 implicit-function-declaration, 8 implicit-int, 1
# incompatible-pointer-types, and the other two are the ABI work's, which compiled
# the same tree with pc/Makefile's flags and found them. The list is here
# rather than in two places because 3ds/tests/abi_objects.sh reads it back
# through `permflags`: one list, two consumers, no drift.
GAMEPERM := -Wno-error=int-conversion \
            -Wno-error=implicit-function-declaration \
            -Wno-error=implicit-int \
            -Wno-error=incompatible-pointer-types \
            -Wno-error=return-mismatch \
            -Wno-error=declaration-missing-parameter-type

# The game's own optimisation level. -O1, and -O2 was tried and measured
# rather than assumed: 1,380 frames of the same boot at each level, window for
# window, identical picture digests. The game's own span went 8.65 -> 8.63 ms
# a frame and the whole frame 143.43 -> 143.41 ms, 0.2% of the part it
# touches, for 195 KB more text on a console with 16 KB of L1 and no L2. So
# the level is not the lever for the game's own code being half a 60 fps
# budget; it stays here as a knob and not as the ship build.
#
# -O2 does BUILD and RUN correctly (it did not before; see the exit hooks in
# 3ds/src/armrec_rt_3ds.c, which were undefined weak here, so -O2's tail-call
# form of a nop'd hook meant the function never returned). Anything raising it
# again should re-read that comment first.
OPT ?= -O1

# Per-path overrides of it, pc/Makefile's mechanism name for name, a level
# that behaves differently is bisected by path, and a bisect that cannot be
# run is a bisect that turns into a guess. Last -On on the command line wins,
# so an override is only an append.
#
#   make -f 3ds/Makefile OPT=-O2 OPT_subprojects=-O1
#   make -f 3ds/Makefile OPT=-O2 OPT_src/battle=-O1
#   make -f 3ds/Makefile OPT=-O2 OPT_src/berry_patches.c=-O1
#
# $1 is an absolute source path; the file, its directory, the first two
# components (subprojects/NitroSDK-...) and the top one are each looked up, so
# OPT_src=-O1 covers src/battle/foo.c as well.
src_rel = $(patsubst $(ROOT)/%,%,$(1))
opt_from_src = $(strip $(EXTRA_OPT) \
    $(OPT_$(src_rel)) \
    $(OPT_$(patsubst %/,%,$(dir $(src_rel)))) \
    $(OPT_$(word 1,$(subst /, ,$(src_rel)))/$(word 2,$(subst /, ,$(src_rel)))) \
    $(OPT_$(firstword $(subst /, ,$(src_rel)))))
EXTRA_OPT ?=

GAME_CFLAGS := -g $(OPT) -mword-relocations -ffunction-sections $(ARCH) \
               $(GAMEABI) $(GAMEPERM) \
               -std=gnu99 -fms-extensions -fno-strict-aliasing -fwrapv -fcommon -w \
               -include $(ROOT)/pc/include/pc_prelude.h \
               $(GAMEINC) $(GAMEDEFS)

# The extra force-include that only DECOMPILED GAME sources get. The ROM build
# gives include/pch/global_pch.h to the main executable and not to the SDK wrap
# sources (meson c_pch), pc/Makefile splits GAME_CFLAGS from SDK_CFLAGS the same
# way, and so does this file. The game C leans on it for <nitro.h>, <nnsys.h>,
# NELEMS and the global/ headers; an SDK source that saw it would be compiling
# against a different set of macros than the one it was written for.
#
# It is a separate variable rather than part of GAME_CFLAGS because the three
# files under 3ds/src that see the DS SDK are port code, not game code, and
# because `shadowflags` publishes GAME_CFLAGS for 3ds/tests/shadow_chain.sh,
# whose probe is a hand-written TU that includes what it means to test.
SRCPCH := -include $(ROOT)/include/pch/global_pch.h

CCVERSION := $(shell $(CC) --version 2>/dev/null | head -1)
ifeq (,$(findstring devkitARM,$(CCVERSION)))
$(error This is not devkitARM: "$(CCVERSION)" from $(shell command -v $(CC)). \
        source $$HOME/devkitpro/3ds-env.sh)
endif

# ccache in preprocessor mode only. Direct mode records which headers a
# compile used and misses a NEW header that now shadows one of them
# earlier on the search path; which is exactly what 3ds/include will do
# to the SDK once its shadows are on the path. The PC port paid for that once.
CCACHE := $(shell command -v ccache 2>/dev/null)
ifneq ($(CCACHE),)
ifeq (,$(findstring ccache,$(CC)))
CC := $(CCACHE) $(CC)
endif
endif
export CCACHE_NODIRECT = 1

# What the objects were built with. Not a file list and not this
# Makefile: the content is the flags themselves, so the stamp only moves
# when a flag does.
CFLAGSTAMP := $(BUILD)/.cflagstamp

# What the shadows were. A generated .d file lists the headers a compile DID
# open, so it cannot mention a header that did not exist yet, and a NEW file
# under 3ds/include is precisely a header that now shadows one the last
# compile opened somewhere else. Nothing in make's dependency graph notices
# that, and with ccache in the picture the stale object is served from cache
# rather than merely kept. So the stamp hashes the whole directory, names and
# contents both, and every object depends on it.
# 5.3. The overlay ordinals and the ROM link's memory-map values as constants
# rather than as absolute symbols, which a 3DSX cannot relocate. Derived from
# the two files the PC pipeline already generates, so there is still one
# answer to what an ordinal or a link address is. A prerequisite of the shadow
# stamp, which every object depends on, the generator only rewrites the file
# when its contents change, so this does not rebuild the tree on every run.
ABS_SRCS := $(ROOT)/build/pc/overlay_ids.c $(ROOT)/build/pc/lcf_syms.c
ABS_H    := $(BUILD)/geninclude/3ds_absolutes.h

SHADOWSTAMP := $(BUILD)/.shadowstamp
SHADOWS     := $(shell find $(ROOT)/3ds/include -type f 2>/dev/null | sort)

DEPSDIR	:=	$(BUILD)
VPATH	:=	$(ROOT)/3ds/src

CFILES	:=	$(notdir $(wildcard $(ROOT)/3ds/src/*.c))
SFILES	:=	$(notdir $(wildcard $(ROOT)/3ds/src/*.s))

# The PICA's own programs. picasso assembles a .v.pica into a .shbin and
# bin2s turns that into an object with `<name>_shbin` and `<name>_shbin_end`
# around it, 3ds/src/3ds_gpu.c declares those two symbols itself rather than
# including bin2s' generated header, which carries the size as a static.
#
# 3ds_rules already has the %.shbin rule and VPATH already reaches 3ds/src, so
# what is added below is the object step and this list.
PICAFILES	:=	$(notdir $(wildcard $(ROOT)/3ds/src/*.v.pica))
SHBINFILES	:=	$(PICAFILES:.v.pica=_shbin.o)

OFILES	:=	$(CFILES:.c=.o) $(SFILES:.s=.o) $(SHBINFILES)

# Files under 3ds/src that are GAME translation units: they see the DS SDK and
# they must not see libctru. Same implicit rule, different CFLAGS; which is
# also the enforcement, since a file that reaches for <3ds.h> from this list
# will not find it. Everything not named here is a port file.
GAMESRC	:=	3ds_ioreg.c 3ds_hwmem.c 3ds_ctrdg.c 3ds_sdk_statics.c \
		3ds_rom_hdr.c 3ds_os_context.c
GAMEOBJ	:=	$(GAMESRC:.c=.o)

# The probe is a game translation unit and nothing else: it includes an SDK
# name, resolves it through the shadow chain, and states what the chain has to
# be true for. It is compiled but never linked; there is no game to link it
# to until the game link, so it is a prerequisite of `all` rather than of the ELF.
# Its job in this build is to be the thing a new shadow rebuilds.
SHADOWPROBE := $(BUILD)/shadow_probe.o

# The first REAL game translation unit: pret's src/boot.c, untransformed, on
# the game chain with the PCH. The probe above is ours and says what the chain
# has to be true for; this one is the game's and says the chain works on code
# nobody wrote for this port.
#
# src/boot.c is the right file for it and not just the smallest. It reaches the
# shared work area through HW_ROM_HEADER_BUF, so the object is evidence rather
# than an assertion: the shadows moved that constant off its DS value, and the proof is
# that boot.o carries an undefined reference to armrec_shared_base instead of an
# absolute 0x027FFxxx nobody could have noticed. It is compiled, never linked;
# there is no game to link it to until 5.5.
GAMETU_SRC := $(ROOT)/src/boot.c
GAMETU     := $(BUILD)/gametu_boot.o

#---------------------------------------------------------------------------------
# The game. Everything above builds the port's own files; this builds pret's,
# the SDK's and the PC port's host layer, for ARM11.
#
# Source discovery is pc/Makefile's, list for list and exclusion for exclusion,
# because it is the same source tree and a second opinion about which files are
# in it would be a second game. What differs is only the compiler and the flags
# the ABI work settled.
#
# `make -f 3ds/Makefile game` compiles it. `make -f 3ds/Makefile link` also
# attempts the link and writes build/3ds/link.log and unresolved.txt. Neither
# is part of `all` yet: `all` still produces the 3dsx this port boots, and the game link
# is where the game becomes that binary.
#---------------------------------------------------------------------------------
OBJ := $(BUILD)/obj

# lib/ppwlobby is C++ and stays out, as on PC.
GAME_SRCS := $(shell find $(ROOT)/src $(ROOT)/lib/crypto $(ROOT)/lib/gds \
                          $(ROOT)/lib/spl -name '*.c' 2>/dev/null | sort)

# The ARM7 exclusion list comes from the SDK's own meson files, not from a
# filename pattern: rtc/src/control.c and pad/src/pad.c are ARM7 and say
# nothing about it. 86 files. Letting one through links the other processor's
# symbols into this image.
SDK_ARM7 := $(shell $(PYTHON) $(ROOT)/pc/gen_arm7_excludes.py \
              $(NITROSDK) $(NITROSYSTEM) $(NITROWIFI) $(NITRODWC) 2>/dev/null)
SDK_SRCS := $(filter-out $(SDK_ARM7), \
              $(shell find $(NITROSDK)/libraries $(NITROSYSTEM)/libraries \
                $(NITROWIFI)/libraries $(NITRODWC)/libraries \
                -name '*.c' 2>/dev/null | sort))

# pc/src and pc/hw come in whole apart from the files named here, and every
# file in pc/src has a row in 3ds/tests/pc_src_class.py saying which of the
# three it is: this port's own code (linked), a 3ds/src rewrite, or dropped.
# That table is checked against these two lists, so a file cannot quietly
# change class and a new pc/src file cannot arrive unclassified.
#
# PC_REPLACED is the rewrites. They are not weakened and cannot be: pc/src is
# the port's own strong code on both hosts, so two strong definitions of
# pc_input_init() is a multiple-definition error and not an override. The
# first three are here because the PC twin reaches guest memory with a cast
# that is correct exactly where a guest address is a host address.
#
# pc_div0.c is the odd one: it does not define a duplicate symbol, it does not
# compile at all, because its fix-up reads an x86 fault frame and steps over a
# `div` instruction. ARM has neither, and 3ds/src/3ds_div0_wrap.s gives the
# same answer three instructions earlier.
#
# The last three were compiling as failures until they were named here:
# pc_main.c wants mmap and sigaction, pc_view.c wants /dev/shm, and
# pc_os_context.c wants ucontext.h, and the rule below tolerated all three by
# writing a .skipped marker. A tolerated compile error reads as something to go
# and fix; each of these is answered by a 3ds/src file instead.
#
# pc_sym.c is replaced for the reason the others are not portable: it reads the
# running binary's own .symtab out of /proc/self/exe, and this console has
# neither. 3ds/src/3ds_sym.c answers the one caller that needs symbols here,
# out of a table carried in the image and filled in after the link. Every other
# name is unknown, and it says so.
PC_REPLACED := pc_input.c pc_state.c pc_guest_window.c pc_div0.c \
               pc_main.c pc_view.c pc_os_context.c pc_sym.c

# PC_DROPPED is the PC frontend and another operating system's code.
#
# The two pc_win_* files are one #if defined(_WIN32) with an empty tail, so
# they compiled here to an object with no symbols at all: dropping them
# changes no link and states what they are.
#
# The four labs and pc_diff.c came in with the frame-hook sweep, and they are not inert. They mint a
# save by driving the game, sweep every species, decode every message bank,
# play every sequence, and write a differential trace, each driven by an
# environment variable a console cannot set, each writing into a build
# directory, three of them calling exit() from inside a frame. pc/src calls
# their ten frame hooks unconditionally, so they were reachable and
# --gc-sections kept 43,718 bytes of text and 292,168 of .bss for code that
# cannot run here. 3ds/src/3ds_hooks.c answers the ten names with nothing.
PC_DROPPED := pc_win_fiber.c pc_win_ipc.c \
              pc_lab.c pc_audio_lab.c pc_sprite_lab.c pc_text_lab.c pc_diff.c

PC_EXCLUDED := $(PC_REPLACED) $(PC_DROPPED)
PC_SRCS := $(filter-out $(addprefix $(ROOT)/pc/src/,$(PC_EXCLUDED)), \
             $(wildcard $(ROOT)/pc/src/*.c))

# Objects and markers a build made before the exclusion existed. The link globs
# the object tree and `status` counts the markers, so a stale one of either
# reports a file this build does not have.
PC_REPLACED_LEFTOVERS := $(foreach f,$(PC_EXCLUDED:.c=), \
                           $(OBJ)/pc/$(f).o $(OBJ)/pc/$(f).o.skipped $(OBJ)/pc/$(f).o.err)
HW_SRCS := $(wildcard $(ROOT)/pc/hw/*.c)

GAME_OBJS := $(patsubst $(ROOT)/%.c,$(OBJ)/game/%.o,$(GAME_SRCS))
SDK_OBJS  := $(patsubst $(ROOT)/%.c,$(OBJ)/game/%.o,$(SDK_SRCS))
PC_OBJS   := $(patsubst $(ROOT)/pc/src/%.c,$(OBJ)/pc/%.o,$(PC_SRCS))
HW_OBJS   := $(patsubst $(ROOT)/pc/hw/%.c,$(OBJ)/pchw/%.o,$(HW_SRCS))

# The generated C the PC pipeline already produces, compiled here rather than
# regenerated: overlay ordinals from main.lsf, the linker-script symbols from
# the ROM's xMAP, the trainer AI bytecode, the weak trap stubs. All four are
# plain C over numbers the DS decided, so a second generator could only
# disagree. The rule, applied to more than headers.
GENC := $(ROOT)/build/pc/overlay_ids.c $(ROOT)/build/pc/lcf_syms.c \
        $(ROOT)/build/pc/trainer_ai_table.c $(ROOT)/build/pc/stubs.c \
        $(ROOT)/build/pc/mods_manifest.c
GEN_OBJS := $(patsubst $(ROOT)/build/pc/%.c,$(OBJ)/gen/%.o,$(wildcard $(GENC)))

# ------------------------------------------------------- the sound driver
#
# pc/arm7snd is the decompiled ARM7 SND driver, and pc/Makefile's three steps
# port here unchanged in shape: compile against the driver's own vendored
# headers into a directory OUTSIDE $(OBJ), derive a rename map with nm, then
# objcopy every name into the arm7_ namespace. The renaming is what keeps an
# unrenamed SND_CommandInit from overriding the ARM9's weakened one, and it is
# what makes pc/src/pc_arm7snd.c's arm7_* shims the driver's whole host
# surface. Its thirteen arm7_SND_* names were the game link's last walls.
#
# Two things differ from the PC build. There is no symbol prefix, ELF, not
# PE, so the map is name-to-arm7_name with nothing in front. And the include
# chain has 3ds/include ahead of pc/include, for nitro/types.h (the aligned(4)
# copy) and for 3ds/include/arm7snd/registers.h, which is where the driver's
# twenty-odd register literals stop being DS addresses.
SND7_SRCS := $(wildcard $(ROOT)/pc/arm7snd/src/*.c)
SND7_RAWDIR := $(BUILD)/arm7snd-raw
SND7_RAW  := $(patsubst $(ROOT)/pc/arm7snd/src/%.c,$(SND7_RAWDIR)/%.o,$(SND7_SRCS))
SND7_OBJS := $(patsubst $(SND7_RAWDIR)/%.o,$(OBJ)/arm7snd/%.o,$(SND7_RAW))
SND7_MAP  := $(SND7_RAWDIR)/ns.map

# The driver's own headers first, then this console's shadows, then the PC
# port's, then the SDK, pc/Makefile's chain with 3ds/include spliced in
# where a shadow has to win. Not GAMEINC: the driver is ARM7 code that was
# never compiled against the ARM9 SDK's search path, and handing it one would
# let an ARM9 header answer a name its own tree defines.
SND7_INC := -I$(ROOT)/3ds/include/arm7snd \
            -I$(ROOT)/pc/arm7snd/include \
            -I$(ROOT)/3ds/include \
            -I$(ROOT)/pc/include \
            -I$(NITROSDK)/include

SND7_CFLAGS := -g -O1 -mword-relocations -ffunction-sections $(ARCH) \
               $(GAMEABI) $(GAMEPERM) \
               -std=gnu99 -fms-extensions -fno-strict-aliasing -fwrapv -fcommon -w \
               -DPLATFORM_PC -D__3DS__ $(SND7_INC)

# The one generated file that cannot be borrowed: it is nm over the game
# objects, so it has to be nm over THESE objects. Names and sizes, never
# addresses, and the sizes are the ARM11 compiler's, which is the whole
# reason a copy of the PC one would be wrong.
OVSTATICS_C := $(BUILD)/overlay_statics.c
OVSTATICS_O := $(OBJ)/gen/overlay_statics.o

# armrec's runtime is the PC memory backend and this console replaces it; the 3DS file
# is already in $(OFILES). It is named here only to say it is not linked.
ALL_GAME_OBJS := $(GAME_OBJS) $(SDK_OBJS) $(PC_OBJS) $(HW_OBJS) $(GEN_OBJS) \
                 $(SND7_OBJS)

# No automatic inlining, for the objects whose calls the port must still be
# able to reach. Named once because the list below is not the only consumer.
NOAUTOINLINE := -fno-inline-functions -fno-inline-small-functions \
                -fno-inline-functions-called-once

# The SDK sources that write VRAMCNT. A store to a bank-control register is an
# assignment through a macro, and C cannot make a macro run code after one, so
# the remap hook fires at function exit; which needs the instrumentation.
VRAMCNT_SRCS := subprojects/NitroSDK-4.2.30001/libraries/gx/src/gx_vramcnt.c \
                subprojects/NitroSDK-4.2.30001/libraries/gx/src/gxstate.c \
                subprojects/NitroSDK-4.2.30001/libraries/os/src/os_china.c \
                subprojects/NitroSDK-4.2.30001/libraries/wvr/src/wvr.c \
                subprojects/NitroSDK-4.2.30001/libraries/mi/src/mi_wram.c
VRAMCNT_OBJS := $(patsubst %.c,$(OBJ)/game/%.o,$(VRAMCNT_SRCS))

# The five the port overrides and -O anything would inline away. Named at every
# level, not just at -O2: -O1 already inlines a small static and a
# called-once function, so the same five that pc/Makefile names are named here.
# Measured on PC as PMi_WaitBusy spinning forever with no ARM7 reply.
NOINLINE_OBJS := \
  $(OBJ)/game/subprojects/NitroSDK-4.2.30001/libraries/spi/src/pm.o \
  $(OBJ)/game/subprojects/NitroSDK-4.2.30001/libraries/spi/src/tp.o \
  $(OBJ)/game/subprojects/NitroSDK-4.2.30001/libraries/card/src/card_request.o \
  $(OBJ)/game/subprojects/NitroSDK-4.2.30001/libraries/mi/src/mi_dma.o \
  $(OBJ)/game/subprojects/NitroSystem-071126.1/libraries/snd/src/capture.o

PATCHES := $(ROOT)/pc/patches

# A second patch layer, this console's, stacked after pc/patches and authored
# against the result of applying it. It exists because the memory shadow has a
# cost the PC port does not pay: a moved constant is `armrec_<row>_base + n`,
# which is not a constant expression, so an SDK file that used one as a STATIC
# INITIALISER no longer compiles. Two files do, card_rom.c's
# `cardi_rom_header_addr` and g2di_BGManipulator.c's register-pointer table,
# and both stop being tables of constants and become tables filled once, by
# 3ds/src/3ds_sdk_statics.c, after armrec_mem_init() has a slab.
#
# Not pc/patches, because pc/patches applies to the PC build too and on that
# host these initialisers are correct as written. Not a shadow either: this is
# a change to a .c, and 3ds/include shadows headers.
PATCHES3DS := $(ROOT)/3ds/patches
PYTHON  ?= python3

# What the compile actually consumes. strip_asm's copy lives in the object
# tree, which breaks quote-includes relative to the original file, so the
# search path is pointed back at the source's own directory. The bare
# $(dir $<).. covers NitroWiFi's soc/src reaching into ../wcm/include.
SRCDIRS = -I$(dir $<) -I$(dir $<).. -I$(dir $<)../include -I$(dir $<)../../include

# --------------------------------------------------------------------- romfs
#
# 6.1. The cartridge rides inside the 3dsx. There is no card slot on this
# console and no second file to hand a user, so the ROM the game reads with
# CARDi_ReadRom is a RomFS member and the 3dsx is ~128 MB because of it.
# That is legal, 3dsxtool writes the RomFS after the code image and the
# loader mmaps it; nothing here is loaded into the 128 MB of FCRAM.
#
# The staging directory is build/3ds/romfs, NOT 3ds/romfs: `--romfs=<dir>`
# takes a whole directory and a copy of the ROM under a tracked path is a
# 128 MB image one `git add -A` away from the history. Everything under
# build/ is already ignored.
#
# The prerequisite is $(wildcard $(ROM)) and not $(ROM), so a missing image is
# this recipe's error message with the path in it rather than make's "no rule
# to make target". The sdmc copy is the fallback at RUN time; a build
# with no cartridge is still a build failure, because the alternative is a
# 3dsx that gets all the way to romfsInit before admitting it is empty.
ROM	?=	$(ROOT)/build/rom/pokeplatinum.us.nds
ROMFS	:=	$(BUILD)/romfs
ROMFSROM	:=	$(ROMFS)/$(notdir $(ROM))

_3DSXDEPS	:=	$(OUTPUT).smdh $(ROMFSROM)
_3DSXFLAGS	+=	--smdh=$(OUTPUT).smdh --romfs=$(ROMFS)

# ------------------------------------------------------------------ packaging
#
# 9.5. What the Home Menu needs that hbmenu does not. A 3DSX is launched
# inside hbmenu's own process and inherits its memory and its permissions; an
# installed title is a process of its own and has to declare both, which is
# what 3ds/pokeplatinum.rsf is for.
#
# The CIA is the skinny BUILD. It carries no cartridge and reads the image
# from sdmc:/3ds/pokeplatinum/, which is where `card` already puts it and
# which 3ds_rom.c already falls back to. Packing the image instead would make
# a 128 MB title to install on every rebuild, and a file that cannot be
# handed to anyone, because it would have the game inside it. This one is
# three megabytes and contains only this repository's own code.
#
# The first rule in a makefile is its default goal and these are the first
# rules with a real target, so say what the default is instead of letting the
# file's order decide, `make -f 3ds/Makefile` built the icon and stopped.
.DEFAULT_GOAL := all

GEN_ICON  := $(ROOT)/pc/gen_icon.py
BANNERWAV := $(ICONDIR)/silence.wav
BANNER    := $(OUTPUT).bnr
RSF       := $(ROOT)/3ds/pokeplatinum.rsf
MAKEROM   ?= makerom
BANNERTOOL ?= bannertool

# Grouped (&:), because one run of the generator writes both files and make
# would otherwise schedule a run per output.
$(APP_ICON) $(BANNERPNG) &: $(GEN_ICON)
	@mkdir -p $(ICONDIR)
	@$(PYTHON) $(GEN_ICON) --png $(ICONDIR)

# The Home Menu plays the banner's tune while the icon is selected. This one
# is a tenth of a second of silence: bannertool requires a tune and there is
# no music in this repository to give it, and the game's own is the game's.
$(BANNERWAV):
	@mkdir -p $(ICONDIR)
	@$(PYTHON) -c "import struct, sys; n = 4410; \
	  sys.stdout.buffer.write(b'RIFF' + struct.pack('<I', 36 + n) \
	    + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 1, 22050, 44100, 2, 16) \
	    + b'data' + struct.pack('<I', n) + bytes(n))" > $@

$(BANNER)	:	$(BANNERPNG) $(BANNERWAV)
	@if ! command -v $(BANNERTOOL) > /dev/null; then \
	   echo "cia: no $(BANNERTOOL) on PATH." >&2; \
	   echo "     it is not a devkitPro tool. Any build of it will do." >&2; \
	   exit 1; \
	 fi
	@$(BANNERTOOL) makebanner -i $(BANNERPNG) -a $(BANNERWAV) -o $@ > /dev/null

#---------------------------------------------------------------------------------
ifneq ($(CURDIR),$(BUILD))
#---------------------------------------------------------------------------------

.PHONY: all clean status send netload card card-back cia shadowflags gameflags permflags linkflags game link

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(THISMK)

game: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(THISMK) game

# What the link says, in pc/Makefile status's shape: defined against
# referenced over every object, toolchain __names dropped unless the tree's
# own assembly declares them. The log is the deliverable while the binary is
# not; the ROM header reader does.
# The marker is removed first. It was not, and a link that failed once went on
# reporting failure over a binary it had just written; which is how the first
# successful game link read as a failure.
link: $(BUILD)
	@rm -f $(BUILD)/link-failed
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(THISMK) game || true
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(THISMK) $(OUTPUT).elf \
	   2> $(BUILD)/link.log || touch $(BUILD)/link-failed
# arm7snd-raw is pruned from both sweeps. Those objects still spell the
# driver's symbols without the arm7_ prefix, and counting them would report
# SND_CommandInit as defined and its host imports as referenced; neither of
# which is true of anything the link sees.
	@$(NM) --defined-only $$(find $(OBJ) $(BUILD) -maxdepth 9 -name arm7snd-raw -prune -o -name '*.o' -print) 2>/dev/null \
	  | awk '$$2 ~ /[ATDBRWVGC]/ {print $$3}' | sort -u > $(BUILD)/defined.txt
# `$$1 == "U"`, not every undefined line: nm -u also prints WEAK undefined
# references (`w`), which resolve to zero and are never a link error. The
# ARM7-only China logo blobs are three of those, referenced weakly by
# os_china.c and defined nowhere on either host, counting them made
# unresolved.txt claim walls that cannot fail a link.
	@$(NM) -u $$(find $(OBJ) $(BUILD) -maxdepth 9 -name arm7snd-raw -prune -o -name '*.o' -print) 2>/dev/null \
	  | awk '$$1 == "U" {print $$2}' | sort -u > $(BUILD)/referenced.txt
	@grep -rh --include='*.s' --include='*.inc' \
	    -E '^[[:space:]]*\.?(global|extern|public|arm_func_start)[[:space:]]' \
	    $(ROOT)/lib $(ROOT)/subprojects/libsyscall 2>/dev/null \
	  | awk '{print $$2}' | sort -u > $(BUILD)/ownnames.txt
	@comm -13 $(BUILD)/defined.txt $(BUILD)/referenced.txt \
	  | awk 'NR == FNR { own[$$0] = 1; next } \
	         /^_GLOBAL/ { next } \
	         /^__/ && !($$0 in own) { next } \
	         { print }' $(BUILD)/ownnames.txt - > $(BUILD)/unresolved.txt || true
	@total=$$(find $(OBJ)/game -name '*.o' 2>/dev/null | wc -l); \
	 skipped=$$(find $(OBJ)/game -name '*.skipped' 2>/dev/null | wc -l); \
	 echo "C files compiled: $$total  skipped: $$skipped"
	@echo "unresolved symbols: $$(wc -l < $(BUILD)/unresolved.txt)"
	@if [ -f $(OUTPUT).elf ] && [ ! -f $(BUILD)/link-failed ]; then \
	   echo "link: ok"; \
	 else \
	   echo "link: FAILED, $(BUILD)/link.log"; \
	 fi
	@echo "  full list: $(BUILD)/unresolved.txt, $(BUILD)/link.log"

$(BUILD):
	@mkdir -p $@

clean:
	@echo clean ...
	@rm -fr $(BUILD)

# Netload to a 3DS on the LAN. hbmenu has to be showing and listening,
# Y on the homebrew menu, before this runs, or 3dslink pings until it
# gives up.
#
# The address is a make variable and not an environment one because a
# shell cannot export a name beginning with a digit, and this 3dslink
# has no environment fallback of its own: -a is the only way in.
#
#   make -f 3ds/Makefile send IP=192.168.1.23
send: all
	@if [ -z "$(IP)" ]; then \
	   echo "send: no address." >&2; \
	   echo "      make -f 3ds/Makefile send IP=<the 3DS's IPv4>" >&2; \
	   echo "      hbmenu must be showing and listening (Y) first." >&2; \
	   exit 1; \
	 fi
	3dslink -a $(IP) -r 5 $(OUTPUT).3dsx

# The same thing, five megabytes instead of a hundred and thirty-nine.
#
#   make -f 3ds/Makefile netload IP=<the 3DS's IPv4>
#
# The skinny 3dsx carries no cartridge and finds one at
# sdmc:/3ds/pokeplatinum/, which `card` has already put there, so once the
# card is staged this is the iteration loop, and 3dslink is not pushing the
# ROM over wifi on every edit. It refuses a build with no game in it for the
# same reason `card` does.
netload: $(OUTPUT).elf $(OUTPUT).smdh
	@if [ -z "$(IP)" ]; then \
	   echo "netload: no address." >&2; \
	   echo "         make -f 3ds/Makefile netload IP=<the 3DS's IPv4>" >&2; \
	   echo "         hbmenu must be showing and listening (Y) first." >&2; \
	   exit 1; \
	 fi
	@if ! $(PREFIX)nm $(OUTPUT).elf 2>/dev/null | grep -q ' T NitroMain$$'; then \
	   echo "netload: $(OUTPUT).elf has no game in it." >&2; \
	   echo "         make -f 3ds/Makefile game link -j\$$(nproc)" >&2; \
	   exit 1; \
	 fi
	@3dsxtool $(OUTPUT).elf $(BUILD)/$(TARGET)-skinny.3dsx \
	    --smdh=$(OUTPUT).smdh
	3dslink -a $(IP) -r 5 $(BUILD)/$(TARGET)-skinny.3dsx

# ---------------------------------------------------------------- the card
#
# Everything the console needs, onto a mounted SD card, in one command:
#
#   make -f 3ds/Makefile card SD=/mnt/d
#
# THE SKINNY 3DSX and not the fat one. The 3dsx that carries the cartridge in
# its own RomFS is 139 MB and has to be copied whole on every rebuild; the
# skinny one is 5 MB and finds the same image at
# sdmc:/3ds/pokeplatinum/, which is the fallback and is the shape a
# netloaded build has. The cartridge is copied once and then skipped, so a
# rebuild moves five megabytes.
#
# Under a fresh name every time, because some loaders cache a 3dsx by name and
# will happily run yesterday's build. The stable name is what hbmenu shows;
# the stamped one beside it is what to launch when a change refuses to appear.
#
# A REPLAY is opt-in, because the script wins while it is loaded: with an
# input.txt on the card the console drives itself and the buttons do nothing,
# which is what a measured run needs and is not what somebody who wants to
# play the game expects. So the default card is playable and REPLAY= arms a
# script, which is also how a stale one gets cleared.
#
#   make -f 3ds/Makefile card SD=/mnt/d                                 play it
#   make -f 3ds/Makefile card SD=/mnt/d REPLAY=pc/replays/new-game.txt  measure it
#   make -f 3ds/Makefile card SD=/mnt/d PRESENT=verify                  check the picture
#   make -f 3ds/Makefile card SD=/mnt/d BG=gpu                          ... with the background path's backgrounds
#   make -f 3ds/Makefile card SD=/mnt/d L3D=pica                        ... with the 3D rasterizer's rasterizer
#   make -f 3ds/Makefile card SD=/mnt/d L3D=pica P3D=edge               ... and its outline
SD      ?=
REPLAY  ?=
# Which present path the console takes: gpu, cpu or verify. Empty
# clears the file, which is `gpu`, the ship path. `verify` composes every
# frame with the CPU blit as well and leaves the byte compare in
# present-report.txt, which card-back brings home.
PRESENT ?=
# Which path composes the DS backgrounds: soft or gpu. Empty clears the
# file, which is `gpu`, the PICA, the ship path since the console priced it.
# `soft` is the software renderer, which is what a frame-parity run against the
# PC port needs; `gpu` with PRESENT=verify runs both and compares them frame by
# frame.
BG      ?=
# Which path draws the 3D layer: soft or pica. Empty clears the file,
# which is `soft`, the software rasterizer, still the ship path until the
# console prices `pica`.
L3D     ?=
# Bisect / dump knobs for the PICA rasterizer, written to p3d.txt. Empty
# clears the file: a leftover `dump` from a previous staging is how a
# cost run quietly became a dump run. Words are `dump`, `diff`, `edge`,
# `edgeflat`, `notex`, `nodepth`, `noblend`, `flat`, `nosplit`, `half=N`.
P3D     ?=
CARDDIR  = $(SD)/3ds/pokeplatinum
STAMP    = $(shell date -u +%m%d-%H%M)

# The ELF and the icon, not `all`: the 3dsx that `all` builds carries the
# 128 MB cartridge and would be written whole on every card flash, which is
# the copy this target exists to avoid.
card: $(OUTPUT).elf $(OUTPUT).smdh
	@if [ -z "$(SD)" ]; then 	   echo "card: no SD card." >&2; 	   echo "      make -f 3ds/Makefile card SD=/mnt/d" >&2; 	   exit 1; 	 fi
	@if [ ! -d "$(SD)/3ds" ]; then 	   echo "card: $(SD) has no /3ds, is that the 3DS's card?" >&2; 	   exit 1; 	 fi
	@if [ ! -f "$(ROM)" ]; then 	   echo "card: no cartridge at $(ROM), ninja -C build/rom" >&2; 	   exit 1; 	 fi
	@if ! $(PREFIX)nm $(OUTPUT).elf 2>/dev/null | grep -q ' T NitroMain$$'; then \
	   echo "card: $(OUTPUT).elf has no game in it." >&2; \
	   echo "      make -f 3ds/Makefile game link -j\$$(nproc)" >&2; \
	   exit 1; \
	 fi
	@mkdir -p "$(CARDDIR)"
	@3dsxtool $(OUTPUT).elf $(BUILD)/$(TARGET)-skinny.3dsx \
	    --smdh=$(OUTPUT).smdh
	@cp $(BUILD)/$(TARGET)-skinny.3dsx "$(SD)/3ds/$(TARGET).3dsx"
# The STAMPED copy is opt-in, and old ones are swept every time. It exists
# because some loaders cache a .3dsx by name and will launch yesterday's build
# from a file called today's; a stamped name defeats that. But one staging a
# day leaves a card with a row of near-identical executables on it and no way
# to tell which is which from the 3DS, which is its own way to test the wrong
# build. So: one file by default, STAMP=1 when a loader is actually caching,
# and the sweep runs either way.
	@rm -f "$(SD)/3ds/$(TARGET)"-[0-9]*-[0-9]*.3dsx
	@if [ -n "$(STAMPED)" ]; then \
	   cp $(BUILD)/$(TARGET)-skinny.3dsx "$(SD)/3ds/$(TARGET)-$(STAMP).3dsx"; \
	 fi
	@if cmp -s "$(ROM)" "$(CARDDIR)/$(notdir $(ROM))"; then 	   echo "  cart    : already there, unchanged"; 	 else 	   echo "  cart    : copying $$(stat -c %s $(ROM)) bytes, this takes a minute"; 	   cp "$(ROM)" "$(CARDDIR)/$(notdir $(ROM))"; 	 fi
	@if [ -f "$(CARDDIR)/$(notdir $(ROM)).sav" ]; then \
	   if [ -n "$(REPLAY)" ]; then \
	     echo "card: a CONTINUE save is on the card and REPLAY= is set." >&2; \
	     echo "      the script expects NEW GAME and will skip the title;" >&2; \
	     echo "      move the .sav aside first, or drop REPLAY=." >&2; \
	     exit 1; \
	   fi; \
	 fi
	@if [ -n "$(REPLAY)" ]; then \
	   if [ ! -f "$(REPLAY)" ]; then \
	     echo "card: no script at $(REPLAY)" >&2; exit 1; \
	   fi; \
	   cp "$(REPLAY)" "$(CARDDIR)/input.txt"; \
	 else \
	   rm -f "$(CARDDIR)/input.txt"; \
	 fi
	@if [ -n "$(PRESENT)" ]; then \
	   case "$(PRESENT)" in \
	     gpu|cpu|verify) ;; \
	     *) echo "card: PRESENT=$(PRESENT) is not gpu, cpu or verify" >&2; exit 1;; \
	   esac; \
	   echo "$(PRESENT)" > "$(CARDDIR)/present.txt"; \
	 else \
	   rm -f "$(CARDDIR)/present.txt"; \
	 fi
	@if [ -n "$(BG)" ]; then \
	   case "$(BG)" in \
	     soft|gpu) ;; \
	     *) echo "card: BG=$(BG) is not soft or gpu" >&2; exit 1;; \
	   esac; \
	   echo "$(BG)" > "$(CARDDIR)/bg.txt"; \
	 else \
	   rm -f "$(CARDDIR)/bg.txt"; \
	 fi
	@if [ -n "$(L3D)" ]; then \
	   case "$(L3D)" in \
	     soft|pica) ;; \
	     *) echo "card: L3D=$(L3D) is not soft or pica" >&2; exit 1;; \
	   esac; \
	   echo "$(L3D)" > "$(CARDDIR)/3d.txt"; \
	 else \
	   rm -f "$(CARDDIR)/3d.txt"; \
	 fi
	@if [ -n "$(P3D)" ]; then \
	   echo "$(P3D)" > "$(CARDDIR)/p3d.txt"; \
	 else \
	   rm -f "$(CARDDIR)/p3d.txt"; \
	 fi
	@rm -f "$(CARDDIR)/perf-report.txt" "$(CARDDIR)/snd-report.txt" \
	       "$(CARDDIR)/mem-report.txt" "$(CARDDIR)/hang-report.txt" \
	       "$(CARDDIR)/present-report.txt"
	@if [ -n "$(STAMPED)" ]; then \
	   echo "  app     : $(SD)/3ds/$(TARGET).3dsx (and -$(STAMP).3dsx)"; \
	 else \
	   echo "  app     : $(SD)/3ds/$(TARGET).3dsx (STAMPED=1 adds a dated copy)"; \
	 fi
	@if [ -n "$(REPLAY)" ]; then \
	   echo "  replay  : $$(grep -c . "$(REPLAY)") line(s) of $(notdir $(REPLAY)) as input.txt; the buttons will do nothing"; \
	 else \
	   echo "  replay  : none, so the buttons drive the game"; \
	 fi
	@if [ -f "$(CARDDIR)/$(notdir $(ROM)).sav" ]; then \
	   echo "  save    : kept ($(CARDDIR)/$(notdir $(ROM)).sav), CONTINUE"; \
	 else \
	   echo "  save    : none on the card"; \
	 fi
	@if [ -n "$(PRESENT)" ]; then \
	   echo "  present : $(PRESENT)"; \
	 else \
	   echo "  present : gpu (the PICA draws; PRESENT=cpu or verify to ask otherwise)"; \
	 fi
	@if [ -n "$(BG)" ]; then \
	   echo "  bg      : $(BG)"; \
	 else \
	   echo "  bg      : gpu (the PICA composes the backgrounds; BG=soft for the software 2D engine)"; \
	 fi
	@if [ -n "$(L3D)" ]; then \
	   echo "  3d      : $(L3D)"; \
	 else \
	   echo "  3d      : soft (the rasterizer draws the 3D layer and the PICA samples it)"; \
	 fi
	@if [ -n "$(P3D)" ]; then \
	   echo "  p3d     : $(P3D)"; \
	 else \
	   echo "  p3d     : off (no p3d.txt; P3D=edge / dump / diff to set one)"; \
	 fi
	@echo "  reports : cleared; the console writes them into $(CARDDIR)"
	@if [ -f "$(SD)/3ds/dspfirm.cdc" ]; then 	   echo "  sound   : dspfirm.cdc is on the card"; 	 else 	   echo "  sound   : NO dspfirm.cdc on the card; the game will run"; 	   echo "            silent. Universal Updater has DSP1; run it once on"; 	   echo "            the console and it writes $(SD)/3ds/dspfirm.cdc."; 	 fi

# What the console wrote, back off the card and into the tree, so the reports
# are read with the same tools the emulator's are.
#
#   make -f 3ds/Makefile card-back SD=/mnt/d
card-back:
	@if [ -z "$(SD)" ]; then 	   echo "card-back: no SD card. SD=/mnt/d" >&2; exit 1; 	 fi
	@mkdir -p $(BUILD)/hw
	@n=0; for f in perf-report.txt snd-report.txt mem-report.txt \
	               hang-report.txt present-report.txt $(TARGET).us.nds.sav; do 	   if [ -f "$(CARDDIR)/$$f" ]; then 	     cp "$(CARDDIR)/$$f" $(BUILD)/hw/$$f; 	     echo "  $$f -> build/3ds/hw/$$f ($$(stat -c %s "$(CARDDIR)/$$f") bytes)"; 	     n=$$((n + 1)); 	   fi; 	 done; 	 if [ "$$n" = 0 ]; then 	   echo "card-back: the console has written nothing to $(CARDDIR) yet." >&2; 	 fi

# 9.5. The installable title, for the Home Menu rather than hbmenu.
#
#   make -f 3ds/Makefile cia                 build/3ds/pokeplatinum.cia
#   make -f 3ds/Makefile cia SD=/mnt/d       ... and drop it where FBI looks
#
# TITLE ID 0x000400000FF58200, and the derivation is in the RSF so it can be
# recomputed instead of trusted. Install it with FBI (or any installer): the
# signature is makerom's own and only a console with signature patches will
# take it, which is the same console that runs hbmenu.
#
# It still needs `card` to have put the cartridge and dspfirm.cdc on the SD
# card; an installed title reads exactly the same files a 3dsx does. What it
# does not need is the 3dsx, and it survives a reboot into the Home Menu.
#
# The ELF and not `all`, for card's reason: `all` builds the 128 MB 3dsx and
# this target has nothing to do with it.
cia: $(OUTPUT).elf $(OUTPUT).smdh $(BANNER)
	@if ! command -v $(MAKEROM) > /dev/null; then \
	   echo "cia: no $(MAKEROM) on PATH." >&2; \
	   echo "     it is not a devkitPro tool; build it from Project_CTR." >&2; \
	   exit 1; \
	 fi
	@if ! $(PREFIX)nm $(OUTPUT).elf 2>/dev/null | grep -q ' T NitroMain$$'; then \
	   echo "cia: $(OUTPUT).elf has no game in it." >&2; \
	   echo "     make -f 3ds/Makefile game link -j\$$(nproc)" >&2; \
	   exit 1; \
	 fi
	@$(MAKEROM) -f cia -o $(OUTPUT).cia -elf $(OUTPUT).elf -rsf $(RSF) \
	   -icon $(OUTPUT).smdh -banner $(BANNER) -exefslogo
	@$(PYTHON) $(ROOT)/3ds/tests/cia_check.py $(OUTPUT).cia
	@if [ -n "$(SD)" ]; then \
	   if [ ! -d "$(SD)/3ds" ]; then \
	     echo "cia: $(SD) has no /3ds, is that the 3DS's card?" >&2; \
	     exit 1; \
	   fi; \
	   dir="$(SD)/cias"; \
	   if [ -d "$(SD)/cia" ] && [ ! -d "$$dir" ]; then dir="$(SD)/cia"; fi; \
	   mkdir -p "$$dir"; \
	   cp $(OUTPUT).cia "$$dir/$(TARGET).cia"; \
	   echo "  card    : $$dir/$(TARGET).cia, install it with FBI"; \
	 fi

# The game compile line, for anything that has to use the real one instead of
# a copy that drifts (3ds/tests/shadow_chain.sh). One line, nothing else on
# stdout, so a caller can put it straight into $(...).
shadowflags:
	@echo '$(GAME_CFLAGS)'

# The link flags that are this port's rather than the template's, for a check
# that has to read the real ones.
# Both halves, because 3ds/tests/link_walls.py reads the --defsym names back:
# A symbol the LINK defines is resolved even though no object defines it, and
# hardcoding that list in the test would be a second place to keep it right.
linkflags:
	@echo '$(DIV0WRAP) $(IMAGEBOUNDS)'

# The same line a decompiled game source is compiled with, GAME_CFLAGS plus
# the PCH. Separate from shadowflags because shadow_chain.sh's probe is written
# to include what it tests and must not have the PCH answering for it.
gameflags:
	@echo '$(GAME_CFLAGS) $(SRCPCH)'

# The diagnostics GCC 14 promoted to errors and this tree needs demoted.
# 3ds/tests/abi_objects.sh compiles the game with pc/Makefile's flags rather
# than these, so it asks for this one piece instead of keeping a copy.
permflags:
	@echo '$(GAMEPERM)'

status:
	@echo "target    : $(OUTPUT).3dsx"
	@if [ -f "$(OUTPUT).3dsx" ]; then \
	   stat -c '  built   : %s bytes, %y' "$(OUTPUT).3dsx"; \
	 else \
	   echo "  built   : absent"; \
	 fi
	@echo "romfs     : $(ROM)"
	@if [ -f "$(ROM)" ]; then \
	   stat -c '  cart    : %s bytes' "$(ROM)"; \
	 else \
	   echo "  cart    : ABSENT; the 3dsx will not build"; \
	 fi
	@if [ -f "$(ROMFSROM)" ]; then \
	   echo "  staged  : $(patsubst $(ROOT)/%,%,$(ROMFSROM))"; \
	 else \
	   echo "  staged  : not yet"; \
	 fi
	@echo "  fallback: sdmc:/3ds/pokeplatinum/$(notdir $(ROM)), for a 3dsx \
packed without --romfs"
	@echo "sources   : $(words $(CFILES)) C file(s) under 3ds/src"
	@echo "shadows   : $(words $(SHADOWS)) file(s) under 3ds/include"
	@echo "generated : $(if $(wildcard $(GEN)/.),$(GEN),ABSENT, run the PC pipeline before the game link)"
	@echo "game TU   : $(patsubst $(ROOT)/%,%,$(GAMETU_SRC)) -> $(if $(wildcard $(GAMETU)),$(patsubst $(ROOT)/%,%,$(GAMETU)),absent)"
	@echo "compiler  : $(shell command -v $(PREFIX)gcc)"
	@echo "            $(CCVERSION)"
	@echo "ccache    : $(if $(CCACHE),$(CCACHE), not installed) (CCACHE_NODIRECT=$(CCACHE_NODIRECT))"
	@echo "flags     : $(CFLAGS)"
	@echo "devkitpro : $(DEVKITPRO)"
	@echo "game      : $(words $(GAME_SRCS)) game + $(words $(SDK_SRCS)) SDK + \
$(words $(PC_SRCS)) pc/src + $(words $(HW_SRCS)) pc/hw source(s)"
	@if [ -d $(OBJ) ]; then \
	   echo "  objects : $$(find $(OBJ) -name '*.o' | wc -l) built, \
$$(find $(OBJ) -name '*.skipped' | wc -l) skipped"; \
	   find $(OBJ) -name '*.skipped' | sed 's|^$(OBJ)/|            |; s|\.o\.skipped$$|.c|' | sort; \
	 else \
	   echo "  objects : none, make -f 3ds/Makefile game"; \
	 fi
# The two unresolved numbers are different questions and neither replaces the
# other. `unresolved.txt` is pc/Makefile status's metric transplanted, defined
# against referenced over the objects alone, so every fopen a library
# resolves is in it, and it is the number comparable to the PC port's. The link
# log's is what ld could not find with the libraries in hand, and it is the one
# that has to reach zero. A name leaving the first list because it moved into
# newlib is expected; a name entering the second is a new wall, which is why
# 3ds/tests/link_walls.py refuses one without a rule.
	@if [ -f $(BUILD)/unresolved.txt ]; then \
	   echo "  unresolved: $$(wc -l < $(BUILD)/unresolved.txt) over the objects"; \
	   $(PYTHON) $(ROOT)/3ds/tests/link_walls.py 2>&1 | sed 's/^/            /'; \
	 else \
	   echo "  unresolved: not measured, make -f 3ds/Makefile link"; \
	 fi
	@echo "slab      : $(shell sed -n 's/.*GUEST_SLAB_BYTES == \(0x[0-9A-Fa-f]*\).*/\1/p' \
$(ROOT)/3ds/include/3ds_guest_map.h) bytes of guest memory, taken from the \
application heap at start-up"
	@echo "            what the console had left after it is on the app's own \
bottom line (APP free/region), which no build machine can answer"

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

.PHONY: all FORCE
all: $(OUTPUT).3dsx $(SHADOWPROBE) $(GAMETU)

$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)

# 5.3. The 3dsx IS the game: the same objects `link` reports on, linked into
# the binary this port ships. The self-test loop still runs in front of it and
# A hands the console over. There is no second ELF any more, one link, one
# set of flags, one thing to be wrong.
#
# The object list is a glob rather than $^, for the reason the separate game
# rule had it: a file that stops being compiled leaves its object behind, and
# the three pc/src files 3ds/src replaces came back as ten multiple-definition
# errors when they were merely dropped from the source list.
$(OUTPUT).elf	:	$(OFILES) $(ALL_GAME_OBJS) $(OVSTATICS_O) $(CFLAGSTAMP)
	@rm -f $(PC_REPLACED_LEFTOVERS)
	$(SILENTMSG) linking $(notdir $@)
	$(SILENTCMD)$(LD) -specs=3dsx.specs -g $(ARCH) -Wl,--no-enum-size-warning \
	  $(DIV0WRAP) $(MIWRAP) $(RENDERWRAP) $(GPU3DWRAP) $(BGPTRWRAP) $(IMAGEBOUNDS) -Wl,-Map,$(OUTPUT).map \
	  -o $@ $$(find $(OBJ) -name '*.o' | sort) $(OFILES) $(LIBPATHS) $(LIBS)
# 9.8. The overlay statics' addresses, into the binary that has them. This is
# a write into the file just linked and not a second link: the table is at its
# final size already, so only its contents change and nothing moves.
	@NM=$(PREFIX)nm $(PYTHON) $(ROOT)/3ds/gen_ov_addrs.py $@ $(OVSTATICS_C)

# 6.1. Copied, not linked: a hard link would let the next ROM build rewrite
# the RomFS member under a 3dsx that has already been packed. 128 MB moves
# once, when the image changes.
$(ROMFSROM)	:	$(wildcard $(ROM))
	@if [ ! -f "$(ROM)" ]; then \
	   echo "3ds: no cartridge image at $(ROM)" >&2; \
	   echo "     build the ROM first, or point ROM= at an image." >&2; \
	   exit 1; \
	 fi
	@rm -rf $(ROMFS)
	@mkdir -p $(dir $@)
	@echo "romfs ... $(notdir $@)"
	@cp -f "$(ROM)" $@

$(OFILES)	:	$(CFLAGSTAMP) $(SHADOWSTAMP)

# bin2s' object around an assembled shader. `-a 4` because DVLB_ParseFile
# reads the blob as u32, and the section is rodata like every other constant
# in the image. The .shbin itself comes from 3ds_rules' picasso rule.
%_shbin.o	:	%.shbin
	$(SILENTMSG) $(notdir $<)
# The `echo` is not decoration: bin2s ends its output without a newline and
# the assembler warns about the unterminated last line.
	$(SILENTCMD){ bin2s -a 4 $< ; echo ; } | $(AS) -o $@

$(GAMEOBJ)	:	CFLAGS := $(GAME_CFLAGS)

$(SHADOWPROBE)	:	$(ROOT)/3ds/tests/shadow_probe.c $(CFLAGSTAMP) $(SHADOWSTAMP)
	$(SILENTMSG) $(notdir $<)
	$(SILENTCMD)$(CC) -MMD -MP -MF $(DEPSDIR)/shadow_probe.d $(GAME_CFLAGS) \
	  -c $< -o $@ $(ERROR_FILTER)

# No strip_asm, no unstatic, no pc/patches: those come with the source pipeline, and this file needs
# none of them. Failing here is meant to be a compile error and not a skip.
$(GAMETU)	:	$(GAMETU_SRC) $(CFLAGSTAMP) $(SHADOWSTAMP)
	$(SILENTMSG) $(notdir $<)
	$(SILENTCMD)$(CC) -MMD -MP -MF $(DEPSDIR)/gametu_boot.d $(GAME_CFLAGS) $(SRCPCH) \
	  -c $< -o $@ $(ERROR_FILTER)

#--------------------------------------------------------------------- game
#
# The three transforms, in the order pc/Makefile applies them, on the
# same source tree and with the same patch files. Nothing under src/, lib/ or
# subprojects/ is edited: the game C must keep matching the ROM build and the
# subprojects are meson wraps a re-download would reset.
#
#   strip_asm.py   removes whole functions written as mwcc `asm {}` blocks so
#                  the portable rest of the file compiles; the stripped ones
#                  become loud undefined symbols
#   pc/patches     per-file diffs for mwcc-isms no transform can express,
#                  authored against what the compile consumes, strip_asm's
#                  copy where stripping worked, the pristine file otherwise
#   unstatic.py    the failure path only: mwcc's "header declares it, the
#                  definition says static" pattern, which GCC rejects
#
# A patch that no longer applies is a skip with a reason, not a silent
# fallback to the unpatched file: a patch exists because the file does not
# compile without it, and compiling it anyway would build a different game.
#
# Skip-tolerant like the PC build. A file that will not compile leaves a
# .skipped marker and its stderr in .err, is counted by `link`, and its
# symbols surface in the unresolved list rather than vanishing.
define GAME_COMPILE
	@mkdir -p $(dir $@)
	@if $(PYTHON) $(ROOT)/tools/armrec/strip_asm.py $< $@.stripped.c 2>/dev/null; \
	 then src=$@.stripped.c; else src=$<; fi; \
	 pfile="$(PATCHES)/$(patsubst $(ROOT)/%,%,$<).patch"; \
	 if [ -f "$$pfile" ]; then \
	   cp "$$src" $@.patched.c; \
	   if patch --silent --forward $@.patched.c "$$pfile" >/dev/null 2>$@.err; then \
	     src=$@.patched.c; \
	   else \
	     echo "  SKIP    $(patsubst $(ROOT)/%,%,$<) (pc/patches diff no longer applies)"; \
	     : > $@.skipped; rm -f $@; exit 0; \
	   fi; \
	 fi; \
	 pfile3="$(PATCHES3DS)/$(patsubst $(ROOT)/%,%,$<).patch"; \
	 if [ -f "$$pfile3" ]; then \
	   cp "$$src" $@.3ds.c; \
	   if patch --silent --forward $@.3ds.c "$$pfile3" >/dev/null 2>$@.err; then \
	     src=$@.3ds.c; \
	   else \
	     echo "  SKIP    $(patsubst $(ROOT)/%,%,$<) (3ds/patches diff no longer applies)"; \
	     : > $@.skipped; rm -f $@; exit 0; \
	   fi; \
	 fi; \
	 if $(CC) $(1) $(SRCDIRS) -c -o $@ $$src 2>$@.err; then \
	   rm -f $@.skipped $@.err $@.unstatic.c; \
	 elif $(PYTHON) $(ROOT)/tools/armrec/unstatic.py \
	        --cc "$(CC) $(1) $(SRCDIRS)" $$src $@.unstatic.c 2>/dev/null \
	      && $(CC) $(1) $(SRCDIRS) -c -o $@ $@.unstatic.c 2>/dev/null; then \
	   rm -f $@.skipped $@.err; \
	 else \
	   echo "  SKIP    $(patsubst $(ROOT)/%,%,$<)"; \
	   : > $@.skipped; rm -f $@; \
	 fi
endef

.SECONDEXPANSION:

# SDK objects are weakened after compiling, so a strong definition in 3ds/src
# or pc/src wins by linking alone. The game's own src/ and lib/ stay
# strong: nothing silently replaces hand-decompiled game code.
$(OBJ)/game/subprojects/%.o	:	$(ROOT)/subprojects/%.c $(CFLAGSTAMP) $(SHADOWSTAMP) $$(wildcard $(PATCHES)/subprojects/$$*.c.patch) $$(wildcard $(PATCHES3DS)/subprojects/$$*.c.patch)
	$(call GAME_COMPILE,$(GAME_CFLAGS) $(EXTRA_CFLAGS) $(call opt_from_src,$<))
	@test ! -f $@ || $(OBJCOPY) --weaken $(EXTRA_OBJCOPY) $@

$(OBJ)/game/%.o	:	$(ROOT)/%.c $(CFLAGSTAMP) $(SHADOWSTAMP) $$(wildcard $(PATCHES)/$$*.c.patch) $$(wildcard $(PATCHES3DS)/$$*.c.patch)
	$(call GAME_COMPILE,$(GAME_CFLAGS) $(SRCPCH) $(call opt_from_src,$<))

# The host layer. Same flags minus the PCH; it is not game code and the ROM
# build never gave it one. No transforms either: this tree is ours and a file
# here that needs strip_asm is a file in the wrong tree.
$(OBJ)/pc/%.o	:	$(ROOT)/pc/src/%.c $(CFLAGSTAMP) $(SHADOWSTAMP)
	@mkdir -p $(dir $@)
	@if $(CC) $(GAME_CFLAGS) $(call opt_from_src,$<) -c -o $@ $< 2>$@.err; then rm -f $@.skipped $@.err; \
	 else echo "  SKIP    $(patsubst $(ROOT)/%,%,$<)"; : > $@.skipped; rm -f $@; fi

# pc/hw AT -O3, and only pc/hw. Measured on this console: a frame costs about
# 210 ms, of which the blit is 4.5 and the buffer flush 0.1; the rest is the
# game and these four files, which compose two 256x192 screens a scanline at a
# time and rasterize the DS's polygons, in software, on a 268 MHz CPU. That is
# where the time is, so that is what is worth compiling hard.
#
# The rest of the game is at $(OPT). -O3 is kept for these four alone because
# it is where the time is, and because the inlining hazard at the top of this
# file does not apply to them: nothing in pc/hw overrides an SDK function;
# these are the melonDS-derived hardware models and they call into the port
# rather than the other way round.
HW_CFLAGS := $(patsubst $(OPT),-O3,$(GAME_CFLAGS))

$(OBJ)/pchw/%.o	:	$(ROOT)/pc/hw/%.c $(CFLAGSTAMP) $(SHADOWSTAMP)
	@mkdir -p $(dir $@)
	@if $(CC) $(HW_CFLAGS) -c -o $@ $< 2>$@.err; then rm -f $@.skipped $@.err; \
	 else echo "  SKIP    $(patsubst $(ROOT)/%,%,$<)"; : > $@.skipped; rm -f $@; fi

$(OBJ)/gen/%.o	:	$(ROOT)/build/pc/%.c $(CFLAGSTAMP) $(SHADOWSTAMP)
	@mkdir -p $(dir $@)
	@if $(CC) $(GAME_CFLAGS) -c -o $@ $< 2>$@.err; then rm -f $@.skipped $@.err; \
	 else echo "  SKIP    $(patsubst $(ROOT)/%,%,$<)"; : > $@.skipped; rm -f $@; fi

# The sound driver. No skip tolerance: this tree compiles on PC and a file
# that stops compiling here is a shadow that broke, not a transform that is
# missing.
$(SND7_RAWDIR)/%.o	:	$(ROOT)/pc/arm7snd/src/%.c $(CFLAGSTAMP) $(SHADOWSTAMP)
	@mkdir -p $(dir $@)
	$(SILENTMSG) $(notdir $<)
	$(SILENTCMD)$(CC) $(SND7_CFLAGS) -c -o $@ $< $(ERROR_FILTER)

# Everything the driver defines or imports moves, except the C library's and
# the two names the host deliberately shares with it. pc_spu_keyon_note() is
# pc/hw/pc_spu.c's, called from the keyon macro so the mixer's latch can see a
# retrigger it would otherwise diff away. armrec_io_base is 3ds/src/3ds_guest.c's
# and snd_sad_from_host() is 3ds/src/3ds_snd_addr.c's; both are this console's
# additions to that list, and both are reached from the register shadow. An
# arm7_ copy of either would be a second one nothing defines; which is how
# they were found, as a link error naming arm7_snd_sad_from_host.
# The rest of the map is the PC build's name for name.
$(SND7_MAP)	:	$(SND7_RAW) $(THISMK)
	@{ $(NM) --defined-only $(SND7_RAW) 2>/dev/null \
	     | awk '$$2 ~ /[ATDBRWVG]/ {print $$3}'; \
	   $(NM) -u $(SND7_RAW) 2>/dev/null | awk 'NF >= 2 {print $$2}'; } \
	  | sort -u \
	  | grep -vE '^(_GLOBAL_OFFSET_TABLE_|__.*|mem(cpy|set|move|cmp)|pc_spu_keyon_note|armrec_io_base|snd_sad_from_host)$$' \
	  | awk '{ print $$1 " arm7_" $$1 }' > $@
	@echo "  SND7NS  $$(wc -l < $@) sound-driver symbols moved into the arm7_ namespace"

$(OBJ)/arm7snd/%.o	:	$(SND7_RAWDIR)/%.o $(SND7_MAP)
	@mkdir -p $(dir $@)
	@$(OBJCOPY) --redefine-syms=$(SND7_MAP) $< $@

# The c library owns time(), and the game must not.
#
# NitroDWC's GameSpy port defines `time()` in gs/nonport.c, and it is not the
# C library's: it asserts OS_IsTickAvailable() and answers OS_GetTick(),
# because a DS does not know its timezone. Weakening it is not enough, a
# weak definition in an OBJECT still satisfies the reference, so the linker
# never extracts libc's time.o and every caller gets the game's. libctru's
# romfsInit() reads the clock while mounting, so the 3dsx with the game in it
# opened its RomFS, hit that assert before the SDK's OS existed, and was torn
# down with no screen and nothing in the log. `-u time` does not help for the
# same reason.
#
# So the name moves in the object instead, definition and internal references
# together: nonport.c's own callers keep the DS answer, everyone else gets the
# C library's. 3ds/tests/libc_shadow.py derives the whole set of game symbols
# that shadow a library name and fails on one that is not accounted for.
DWC_NONPORT := $(OBJ)/game/subprojects/NitroDWC-2.2.30008/libraries/gs/nonport.o
$(DWC_NONPORT)	:	EXTRA_OBJCOPY := --redefine-sym time=dwc_time

$(VRAMCNT_OBJS)	:	EXTRA_CFLAGS := -finstrument-functions
$(NOINLINE_OBJS)	:	EXTRA_CFLAGS := $(NOAUTOINLINE)

# After the game objects, because nm has to see them; not in ALL_GAME_OBJS,
# so listing them as its prerequisite is not a cycle. The link globs the
# object tree, so it is picked up once this has run.
$(OVSTATICS_O)	:	$(ROOT)/build/pc/overlay-map.txt $(ROOT)/pc/gen_overlay_statics.py $(ALL_GAME_OBJS)
	@mkdir -p $(dir $@)
	@$(PYTHON) $(ROOT)/pc/gen_overlay_statics.py \
	   --addr-table $(ROOT)/build/pc/overlay-map.txt $(OBJ)/game $(OVSTATICS_C)
	@$(CC) $(GAME_CFLAGS) -c -o $@ $(OVSTATICS_C)
	@echo "  OVSTAT  $$(grep -c '{ [0-9]' $(OVSTATICS_C)) overlay statics"

game: $(ALL_GAME_OBJS) $(OVSTATICS_O)
	@rm -f $(PC_REPLACED_LEFTOVERS)
	@echo "game: $$(find $(OBJ) -name '*.o' | wc -l) object(s), \
$$(find $(OBJ) -name '*.skipped' | wc -l) skipped"

# FORCE, so the stamps are re-derived every run; the write only happens when
# the text changed, so the objects only rebuild when the flags or the shadows
# did. The flag stamp carries GAME_CFLAGS as well now; the probe is built
# with them, and a flag that moves without rebuilding what it built is the
# whole reason this file exists.
#
# OPTSTAMP is every per-path override by name and value. Without it a bisect
# step changes nothing on the command line the stamp knows about, so make
# rebuilds nothing and the next boot answers about the previous build.
OPTSTAMP := $(foreach v,$(sort $(filter OPT_%,$(.VARIABLES))),$(v)=$($(v)))

$(CFLAGSTAMP)	:	FORCE
	@want="$(CC) | $(CFLAGS) | $(GAME_CFLAGS) | $(OPTSTAMP) | $(EXTRA_OPT) | $(SRCPCH) | $(ASFLAGS) | $(LDFLAGS) | $(DIV0WRAP) | $(MIWRAP) | $(RENDERWRAP) | $(GPU3DWRAP) | $(BGPTRWRAP) | $(LIBS) $(LIBPATHS)"; \
	 if [ ! -f $@ ] || [ "$$(cat $@)" != "$$want" ]; then \
	   printf '%s' "$$want" > $@; \
	 fi

# Names and contents. A shadow deleted moves the stamp as surely as one added:
# Both change which file an SDK name resolves to.
$(ABS_H)	:	$(ABS_SRCS) $(ROOT)/3ds/gen_absolutes.py
	@$(PYTHON) $(ROOT)/3ds/gen_absolutes.py $(ABS_SRCS) $@

$(SHADOWSTAMP)	:	FORCE $(ABS_H)
	@want=$$(md5sum $(SHADOWS) 2>/dev/null); \
	 if [ ! -f $@ ] || [ "$$(cat $@)" != "$$want" ]; then \
	   printf '%s' "$$want" > $@; \
	 fi

FORCE:

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

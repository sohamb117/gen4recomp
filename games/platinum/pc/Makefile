# Native PC build for pokeplatinum, modeled on pokediamond's pc/Makefile.
#
#   make -f pc/Makefile            build build/pc/pokeplatinum (link may not close yet)
#   make -f pc/Makefile status     honest numbers: skipped files, unresolved symbols, link state
#   make -f pc/Makefile headers    (re)generate the metang/fx_const headers, copy asset headers
#   make -f pc/Makefile clean
#
# This does not produce a ROM. It compiles the decompiled C (which for
# Platinum is essentially the whole game, the only ARM assembly in the tree
# is lib/crypto's RC4 and libsyscall's SVC thunks) plus the NitroSDK-family
# subproject C, natively for 32-bit x86.
#
#
# ABI decisions are inherited wholesale from pokediamond's port, where each
# was measured rather than assumed (see that Makefile's long comments):
#   -m32          long and pointers are 4 bytes, so guest struct layouts match
#                 the DS; i386 also 4-aligns long long the way legacy ARM does.
#   -fno-pie      default-PIE i386 reaches globals through a GOT that needs a
#                 prologue-established base register; absolute references are
#                 what identity-mapped guest memory wants anyway.
#   -no-pie -Wl,-Ttext-segment=0x10000000
#                 host text must not land on the identity-mapped guest regions
#                 (main RAM 0x02000000, ITCM 0x01FF8000, DTCM 0x027E0000).
#   -O1           the ship build. -O2 (`make o2`), UBSan (`make ubsan`)
#                 and ASan (`make asan`) are detectors, not the ship build.
#   pc/include first on the include path: it shadows nitro/types.h, where the
#                 SDK spells u32 "unsigned long" (64-bit on LP64 hosts).
#
# Requires gcc-multilib and libc6-dev-i386. The generated headers need the
# subprojects downloaded (`meson subprojects download`); the .naix asset
# headers additionally need a configured ROM build dir; see pc/gen_headers.py.

# Overridable (?=) where pc/Makefile.win retargets the same pipeline at
# i686-w64-mingw32: one COMPILE_RULE, one stub/lcf/arm7snd machinery, two
# toolchains. pokediamond duplicated its Makefile for Windows and pays for
# every pipeline fix twice; this tree parameterizes instead.
# abspath twice over: not just absolute but NORMALIZED, because make matches
# a command-line target against a rule TEXTUALLY, and a rule registered under
# ".../pokeplatinum/pc/../build/..." can never match the clean spelling a
# person (or a test) types. Measured: every single-object target said
# "Nothing to be done" while the full build worked perfectly.
ROOT     := $(abspath $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))/..)
BUILD    ?= $(ROOT)/build/pc

# Mods, pc/mods/README.md is the contract. MODS="name ..." selects them;
# empty is the port as shipped. A modded build gets a BUILD directory of its
# OWN, because the vanilla binary is the one every test and every pinned
# number measures, and a build that could leave modded objects where the
# vanilla link globs them would make "which port am I running" a question
# with a wrong answer available. An unknown name is an error now, not a mod
# that silently did not load.
MODS ?=
# Where a mod is looked up. Overridable so a mod can live in a tree of its own:
# A project that fuses with this port keeps its mod in its own repository and
# points MODS_DIR at it, and this tree is never written to. abspath for the same
# reason ROOT has it, the mod object rule below matches its prerequisite
# TEXTUALLY, so a relative or unnormalized spelling would compile nothing and
# say nothing. override, because that normalization has to reach a value given
# on the command line, which is where every out-of-tree use sets it.
MODS_DIR ?= $(ROOT)/pc/mods
override MODS_DIR := $(abspath $(MODS_DIR))
# Runtime content packages. A different variable from MODS on purpose:
# OpenMMO's fused build is MODS=openmmo and must not be cooked as a
# content package. cook reads PC_MODS if set, otherwise loadorder.txt.
PC_MODS ?=
PC_MODS_DIR ?= $(ROOT)/pc/mods
$(foreach m,$(MODS),$(if $(wildcard $(MODS_DIR)/$(m)),,\
    $(error MODS names '$(m)' but there is no $(MODS_DIR)/$(m))))
ifneq ($(strip $(MODS)),)
BUILD := $(BUILD)-mods
endif

OBJ      := $(BUILD)/obj
# Defined here rather than beside its rule: prerequisite lists expand when
# read, and the mods manifest names this stamp long before the rule appears.
MODSTAMP := $(BUILD)/.modstamp
# Defined here because object rules above the recipe still expand it.
CFLAGSTAMP := $(BUILD)/.cflagstamp
# Overridable for the same reason BUILD is: a retargeting makefile whose
# outputs must stay inside its own tree (pc/Makefile.wasm) points it there.
# The headers are host-independent, so the default stays shared.
GEN      ?= $(ROOT)/build/pc/geninclude
TARGET   ?= $(BUILD)/pokeplatinum
MAKEFILE := $(ROOT)/pc/Makefile
# What `make status` recurses into: the makefile the user actually invoked,
# which under pc/Makefile.win is that file and not this one.
THISMK   := $(abspath $(firstword $(MAKEFILE_LIST)))

ifeq ($(origin CC),default)
CC := gcc
endif
CCACHE := $(shell command -v ccache 2>/dev/null)
ifneq ($(CCACHE),)
ifeq (,$(findstring ccache,$(CC)))
CC := $(CCACHE) $(CC)
endif
endif
# ccache's direct mode records the include files a compile USED and misses a
# NEW header that now shadows one of them earlier on the search path, which
# is exactly what pc/include grows over time. Measured: the ioreg_*.h
# register shadows landed and every cached SDK object kept resolving the SDK
# originals; the 3D engine then decoded a command stream missing every
# macro-written register store. Preprocessor mode hashes what the compile
# actually consumes, so shadowing is always seen.
export CCACHE_NODIRECT = 1
PYTHON  ?= python3
NM      ?= nm
OBJCOPY ?= objcopy

OPT ?= -O1
# UBSan is a detector. A second BUILD directory keeps the ship -O1
# binary untouched. Halt is the default so the first hit is the finding;
# UBSAN_HALT=0 recovers so a catalog pass can see more than one.
# Signed overflow is already defined wrap (-fwrapv in BASEFLAGS).
UBSAN ?= 0
UBSAN_HALT ?= 1
ifeq ($(UBSAN),1)
  ifeq ($(UBSAN_HALT),1)
    SANITIZE := -fsanitize=undefined -fno-sanitize-recover=undefined
  else
    SANITIZE := -fsanitize=undefined
  endif
  # NANR/NCE streams are 2-byte packed. ARMv5TE loads them; i386 does too.
  # UBSan's alignment check is the only thing that cares. Game + SDK only:
  # pc/src keeps the check so a host-side misalign is still a finding.
  GAME_SANITIZE := -fno-sanitize=alignment
endif
# ASan is a detector. Second BUILD directory; same halt/catalog knobs
# as UBSan. i386 shadow is [0x20000000, 0x40000000); guest maps sit
# below 0x0A010000 and the image at 0x10000000. Mutual exclusion: one
# sanitizer per tree.
ASAN ?= 0
ASAN_HALT ?= 1
ifeq ($(ASAN),1)
  ifeq ($(UBSAN),1)
    $(error set only one of ASAN=1 and UBSAN=1; they are separate trees)
  endif
  ifeq ($(ASAN_HALT),1)
    SANITIZE := -fsanitize=address -fno-sanitize-recover=address
  else
    # Address does not recover by default (undefined does). Catalog
    # needs the recover flag, not just the absence of no-recover.
    SANITIZE := -fsanitize=address -fsanitize-recover=address
  endif
endif
SANITIZE ?=
GAME_SANITIZE ?=
# Last -On on the compile line wins, so an override is just appended.
#   make OPT=-O2 OPT_src/battle=-O1
#   make OPT=-O2 OPT_src/berry_patches.c=-O1
# Target-specific EXTRA_OPT is the same idea for one object:
#   $(OBJ)/game/src/foo.o: EXTRA_OPT := -O1
EXTRA_OPT ?=
# $1 is an absolute source path. File, its directory, the first two
# path components (subprojects/NitroSDK-...), then the top component
# (so OPT_src=-O1 covers src/battle/foo.c too).
src_rel = $(patsubst $(ROOT)/%,%,$(1))
opt_from_src = $(strip $(EXTRA_OPT) \
    $(OPT_$(src_rel)) \
    $(OPT_$(patsubst %/,%,$(dir $(src_rel)))) \
    $(OPT_$(word 1,$(subst /, ,$(src_rel)))/$(word 2,$(subst /, ,$(src_rel)))) \
    $(OPT_$(firstword $(subst /, ,$(src_rel)))))
ABI ?= -m32 -fno-pie
# PE symbol names carry a leading underscore; the SND7 namespace map and its
# keep-list have to speak whichever spelling the object format uses.
SYMPFX ?=

SDKROOT      := $(ROOT)/subprojects
NITROSDK     := $(SDKROOT)/NitroSDK-4.2.30001
NITROSYSTEM  := $(SDKROOT)/NitroSystem-071126.1
NITROWIFI    := $(SDKROOT)/NitroWiFi-2.1.30003
NITRODWC     := $(SDKROOT)/NitroDWC-2.2.30008
LIBVCT       := $(SDKROOT)/libvct-1.3.1

# Include order matters: pc/include shadows nitro/types.h; $(GEN) supplies
# generated/*.h, nitro/fx/fx_const.h and the copied res/ asset headers; the
# rest mirrors what meson.build hands the ROM compile (public_includes plus
# each dependency's exported include_directories).
# $(GEN)/generated (bare metang names like "badges.h") must come AFTER
# $(ROOT)/include: both trees have a game_records.h, include/'s is the
# GameRecords struct, generated/'s is the metang enum, and the ROM build
# resolves the bare name to include/ first, falling through to the generated
# dir only for names include/ does not have.
INCLUDES := -I$(ROOT)/pc/include \
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

# Compile-time plugins: staged overlays/patches first so later MODS win,
# then each mod's own include dirs so `#include "client.h"` works without
# a relative climb. Vanilla builds do not grow an -I.
MOD_INCLUDES :=
ifneq ($(strip $(MODS)),)
MOD_INCLUDES := $(foreach m,$(shell printf '%s\n' $(MODS) | tac),\
    $(if $(wildcard $(MODS_DIR)/$(m)/plugins/include),-I$(MODS_DIR)/$(m)/plugins/include) \
    $(if $(wildcard $(MODS_DIR)/$(m)/include),-I$(MODS_DIR)/$(m)/include))
INCLUDES := -I$(BUILD)/modinclude $(MOD_INCLUDES) $(INCLUDES)
$(shell mkdir -p $(BUILD)/modinclude)
endif

# The defines are the union of what meson.build passes the game compile and
# what the SDK wraps export through their dependency compile_args (each wrap's
# meson.build, "FOR CONSUMERS" section). SDK_CW is deliberate: headers pick
# their compiler branch with it; sources that then use real mwcc asm fail to
# compile and are counted, not hidden.
DEFINES := -DPM_KEEP_ASSERTS \
           -DGAME_VERSION=VERSION_PLATINUM \
           -DGAME_LANGUAGE=LANGUAGE_ENGLISH \
           -DPOKEPLATINUM_GENERATED_ENUM \
           -D_MSL_RESTRICT= \
           -DPOKEPLATINUM_REVISION=1 \
           -DSDK_CW_FORCE_EXPORT_SUPPORT -DSDK_TS -DSDK_4M -DSDK_ARM9 \
           -DSDK_CW -DSDK_FINALROM -DSDK_CODE_ARM -DNNS_FINALROM \
           -D_NITRO -DLINK_PPWLOBBY \
           -DPLATFORM_PC

# -fcommon: mwcc merges tentative definitions (lib/gds's gds_internal.h
# defines a variable at file scope in a header shared by four TUs); GCC >= 10
# defaults to -fno-common and turns that into "multiple definition" at link.
# What a second toolchain needs every translation unit compiled with and this
# file has no opinion about. Empty here, and it has to be read BEFORE the three
# *_CFLAGS below, which are simply-expanded: a retargeting makefile that
# appended to BASEFLAGS after its `include` would set a variable nothing reads
# again, and the flag would silently never reach a compile. pc/Makefile.arm is
# the caller, ARM emits no unwind tables unless asked, and without them the
# fault handler's backtrace() answers zero frames.
EXTRA_BASEFLAGS ?=
# Debug info and the common-symbol model, each a hook for one host that
# cannot take the default: wasm objects have no common symbols at all (clang
# refuses -fcommon there), and pc/Makefile.wasm resolves the duplicate
# tentative definitions instead; it also builds without DWARF, which
# wasm2c discards and which would triple the object tree. Everywhere else
# these are -g and -fcommon, exactly as before.
DEBUGFLAGS  ?= -g
COMMONFLAGS ?= -fcommon

BASEFLAGS := $(ABI) $(OPT) $(SANITIZE) $(EXTRA_BASEFLAGS) $(DEBUGFLAGS) -std=gnu99 -fms-extensions -fno-strict-aliasing -fwrapv $(COMMONFLAGS) \
             -include $(ROOT)/pc/include/pc_prelude.h $(INCLUDES) -w $(DEFINES)

# The ROM build force-includes include/pch/global_pch.h into every *game*
# translation unit (meson c_pch on the main executable); the game C leans on
# it for <nitro.h>, <nnsys.h>, NELEMS and friends. The SDK wrap sources do not
# see it there and must not see it here.
GAME_CFLAGS := $(BASEFLAGS) $(GAME_SANITIZE) -include $(ROOT)/include/pch/global_pch.h
SDK_CFLAGS  := $(BASEFLAGS) $(GAME_SANITIZE)
# The port's own code (pc/src, pc/hw, armrec_rt, the generated C). Same
# flags, except under pc/Makefile.win, where the DS trees have _WIN32
# hidden from their platform checks and this code must keep the real one.
PCSRC_CFLAGS := $(BASEFLAGS)

# pc/hw AT -O3 and with SSE2, and only pc/hw. These four files are the two
# hardware models; they compose two 256x192 screens a scanline at a time and
# rasterize the DS's polygons, per pixel, in software, and measurement puts
# most of a frame inside them: at the default resolution a walked overworld
# spends 3.4 ms of a 6.7 ms frame in the rasterizer, and each --hd3d step
# multiplies that. So this is what is worth compiling hard.
#
# The rest of the build stays at $(OPT) because -O2 there inlines an SDK
# function past the port's replacement of it and the game hangs before the
# first frame (see OPT above). That hazard does not reach here: nothing in
# pc/hw overrides anything; these are the melonDS-derived models, and they
# call into the port rather than the other way round. The 3DS build has
# compiled the same four files at -O3 for the same reason since it started.
#
# -msse2 is not a tuning knob, it is the vector unit: -m32 defaults to
# -march=i686, which is a 1995 instruction set with no SSE at all, so every
# per-pixel loop in here compiles to one pixel at a time. SSE2 is on every
# x86 made since 2001 and on every CPU that can run a 64-bit OS, which is the
# floor this port already stands on. No float or double appears in pc/hw, so
# the flag cannot move a rounding mode: it only lets the compiler pack four
# pixels into a register. -mfpmath is deliberately not set for the same
# reason it would not matter.
HW_OPT ?= -O3 -msse2
HW_CFLAGS := $(patsubst $(OPT),$(HW_OPT),$(PCSRC_CFLAGS))

# pc/src/pc_view.c is renderer arithmetic that lives outside pc/hw only for
# the seqlock's sake: the wide and HD composes are per-pixel passes over the
# whole published frame, and they were compiling at the ship -O1 while the
# rasterizer they feed ran at -O3. Appended, because the last -On on the
# line wins. It overrides no SDK symbol, so the -O2 inlining hazard that
# keeps game TUs at -O1 does not reach it.
$(OBJ)/pc/pc_view.o: PCSRC_CFLAGS := $(PCSRC_CFLAGS) $(HW_OPT)

# -pthread: pc/src/pc_workers.c splits a frame across cores. glibc 2.34 and
# later fold the thread functions into libc so this changes nothing there,
# but an older host needs the library named. pc/Makefile.win sets LDFLAGS of
# its own and must NOT gain this, on Windows the pool is Win32 calls, and
# linking winpthread would put a Vista-only import in an exe that has to
# start on XP.
LDFLAGS ?= $(ABI) $(SANITIZE) -no-pie -pthread -Wl,-Ttext-segment=0x10000000
LDLIBS  ?= -lm
# A hook the PE build fills: run after every object exists, before the link
# (pe_weak_promote, attribute-weak symbols with no strong definition do
# not resolve on PE at all; see tools/armrec/pe_weak_promote.py).
LINK_PREP ?= true

# ---------------------------------------------------------------- sources

# Game C: src/** plus the in-tree libraries. lib/ppwlobby is C++ and is
# excluded for now.
GAME_SRCS := $(shell find $(ROOT)/src $(ROOT)/lib/crypto $(ROOT)/lib/gds $(ROOT)/lib/spl -name '*.c' | sort)

# SDK C: the four NitroSDK-family wraps' library sources. libvct ships only
# prebuilt archives (no C), libsyscall only asm. NitroSDK/components is the
# ARM7 wireless blob and stays out, pokediamond learned the hard way that
# ARM7 code must not leak symbols into the ARM9 link.
#
# The blob is no longer the whole ARM7. Upstream #1247 made the ARM7 a compiled
# binary, which moved its C out of components/ and into libraries/, where this
# find looks. 86 ARM7 sources arrived in one merge and 77 of them failed to
# compile against ARM9 headers on the spot (`conflicting types for
# 'PM_SetLEDPattern'`); the other 9 would have compiled and quietly linked the
# other processor's symbols into this image, which is the leak decision 7
# exists to prevent and the half a skip count cannot see.
#
# The list comes from the SDK's own meson files rather than from a filename
# pattern, rtc/src/control.c and pad/src/pad.c are ARM7 and say nothing about
# it, for the same reason overlay IDs come from the lsf (decision 9).
SDK_ARM7 := $(shell $(PYTHON) $(ROOT)/pc/gen_arm7_excludes.py \
              $(NITROSDK) $(NITROSYSTEM) $(NITROWIFI) $(NITRODWC) 2>/dev/null)
SDK_SRCS := $(filter-out $(SDK_ARM7), \
              $(shell find $(NITROSDK)/libraries $(NITROSYSTEM)/libraries \
                $(NITROWIFI)/libraries $(NITRODWC)/libraries -name '*.c' 2>/dev/null | sort))

# A host that replaces or cannot compile a pc/src file names it here (bare
# file names). Empty for every host this file builds by itself.
PC_SRCS_EXCLUDE ?=
PC_SRCS := $(filter-out $(addprefix $(ROOT)/pc/src/,$(PC_SRCS_EXCLUDE)), \
             $(wildcard $(ROOT)/pc/src/*.c))

# pc/hw: the melonDS-derived GPU cores (see pc/hw/README.md for provenance
# and licence). Compiled like pc/src (strong symbols, never weakened) so
# they override armrec_rt.c's weak no-op hooks by linking alone.
HW_SRCS := $(wildcard $(ROOT)/pc/hw/*.c)

GAME_OBJS := $(patsubst $(ROOT)/%.c,$(OBJ)/game/%.o,$(GAME_SRCS))
SDK_OBJS  := $(patsubst $(ROOT)/%.c,$(OBJ)/game/%.o,$(SDK_SRCS))
PC_OBJS   := $(patsubst $(ROOT)/pc/src/%.c,$(OBJ)/pc/%.o,$(PC_SRCS))
HW_OBJS   := $(patsubst $(ROOT)/pc/hw/%.c,$(OBJ)/pchw/%.o,$(HW_SRCS))

OBJS := $(GAME_OBJS) $(SDK_OBJS) $(PC_OBJS) $(HW_OBJS)

# ---------------------------------------------------------- the sound driver
#
# pc/arm7snd: a decompiled ARM7 sound driver, measured to be drop-in. It
# compiles
# against its own vendored diamond-era headers, then every symbol it defines
# or imports moves into the arm7_ namespace, the ARM9's weakened SND_*
# command side keeps its names, and the driver reaches the host through
# arm7_-prefixed shims in pc/src/pc_snd.c. The keep-list is libc/libgcc
# only; everything else is either the driver's own or a shim's.
#
# Raw objects live OUTSIDE $(OBJ) on purpose: the link picks up every .o
# under $(OBJ), and an unrenamed SND_CommandInit would silently override the
# ARM9's weakened one.
SND7_SRCS := $(wildcard $(ROOT)/pc/arm7snd/src/*.c)
SND7_RAW  := $(patsubst $(ROOT)/pc/arm7snd/src/%.c,$(BUILD)/arm7snd-raw/%.o,$(SND7_SRCS))
SND7_OBJS := $(patsubst $(BUILD)/arm7snd-raw/%.o,$(OBJ)/arm7snd/%.o,$(SND7_RAW))
SND7_MAP  := $(BUILD)/arm7snd-raw/ns.map

SND7_CFLAGS := $(ABI) $(OPT) $(SANITIZE) $(GAME_SANITIZE) $(DEBUGFLAGS) -std=gnu99 -fms-extensions -fno-strict-aliasing \
               -fwrapv -w $(COMMONFLAGS) -DPLATFORM_PC \
               -I$(ROOT)/pc/arm7snd/include \
               -I$(ROOT)/pc/include \
               -I$(ROOT)/subprojects/NitroSDK-4.2.30001/include

$(BUILD)/arm7snd-raw/%.o: $(ROOT)/pc/arm7snd/src/%.c $(CFLAGSTAMP) $(SHADOWSTAMP)
	@mkdir -p $(dir $@)
	$(CC) $(SND7_CFLAGS) -c -o $@ $<

$(SND7_MAP): $(SND7_RAW) $(MAKEFILE)
	@{ $(NM) --defined-only $(SND7_RAW) 2>/dev/null \
	     | awk '$$2 ~ /[ATDBRWVG]/ {print $$3}'; \
	   $(NM) -u $(SND7_RAW) 2>/dev/null | awk 'NF >= 2 {print $$2}'; } \
	  | sort -u \
	  | grep -vE '^($(SYMPFX)_GLOBAL_OFFSET_TABLE_|__.*|$(SYMPFX)mem(cpy|set|move|cmp)|$(SYMPFX)pc_spu_keyon_note)$$' \
	  | awk -v p='$(SYMPFX)' '{ n = $$1; sub("^" p, "", n); print p n " " p "arm7_" n }' > $@
	@echo "  SND7NS  $$(wc -l < $@) sound-driver symbols moved into the arm7_ namespace"

# The rename is a hook because it is an object-format operation: wasm
# objects cannot have symbols renamed after the fact, so pc/Makefile.wasm
# recompiles the driver under a generated #define map instead.
SND7_RENAME ?= @$(OBJCOPY) --redefine-syms=$(SND7_MAP) $< $@
$(OBJ)/arm7snd/%.o: $(BUILD)/arm7snd-raw/%.o $(SND7_MAP)
	@mkdir -p $(dir $@)
	$(SND7_RENAME)

OBJS += $(SND7_OBJS)

# SDK_OVERLAY_<name>_ID are linker-script symbols on hardware; nothing in any
# object defines them, and both game C (FS_OVERLAY_ID) and the SDK refer to
# them by name. The lsf is the source of truth for overlay ordinals, the
# Nth Overlay block is ordinal N, the same value the SDK linker script would
# assign. gen_overlay_map.py came from pokeheartgold's tree, where the layout
# and the reasoning are documented; platinum only consumes the --ids-out half
# (there is no armrec translation step here to want the stem map).
OVERLAY_MAP   := $(BUILD)/overlay-map.txt
OVERLAY_IDS_C := $(BUILD)/overlay_ids.c
GEN_OVMAP     := $(ROOT)/tools/armrec/gen_overlay_map.py

$(OVERLAY_MAP): $(ROOT)/platinum.us/main.lsf $(GEN_OVMAP) $(MAKEFILE)
	@mkdir -p $(BUILD)
	@$(PYTHON) $(GEN_OVMAP) --ids-out $(OVERLAY_IDS_C) $(ROOT)/platinum.us/main.lsf > $@
	@test -z '$(SYMPFX)' || sed -i 's/\.global /.global $(SYMPFX)/; s/\.set /.set $(SYMPFX)/' $(OVERLAY_IDS_C)
	@echo "  OVMAP   $$(grep -c .set $(OVERLAY_IDS_C)) SDK_OVERLAY_*_ID symbols"

$(OVERLAY_IDS_C): $(OVERLAY_MAP) ;

$(OBJ)/pc/overlay_ids.o: $(OVERLAY_IDS_C)
	@mkdir -p $(dir $@)
	$(CC) $(PCSRC_CFLAGS) -c -o $@ $<

# ABSOLUTE_SYMS: whether the object format can carry an absolute symbol at
# all (`.set`). ELF and PE can; wasm cannot, and pc/Makefile.wasm turns the
# same two generated files into a header of constants instead.
ABSOLUTE_SYMS ?= 1
ifeq ($(ABSOLUTE_SYMS),1)
OBJS += $(OBJ)/pc/overlay_ids.o
endif

# Overlay .data/.bss snapshot table: names and object-nm sizes, not
# addresses. Generated AFTER the game objects exist so nm can see them,
# and kept out of $(OBJS) so this rule can list $(OBJS) as a prerequisite
# without a cycle. The link already globs $(OBJ), so the new object is
# picked up once this target has run. OVSTATICS_FLAGS is --addr-table on a
# host with no .symtab to read at runtime (wasm; the 3DS build's own rule).
OVERLAY_STATICS_C := $(BUILD)/overlay_statics.c
GEN_OVSTATICS     := $(ROOT)/pc/gen_overlay_statics.py
OVSTATICS_FLAGS   ?=

$(OBJ)/pc/overlay_statics.o: $(OVERLAY_MAP) $(GEN_OVSTATICS) $(OBJS)
	@mkdir -p $(dir $@) $(BUILD)
	@$(PYTHON) $(GEN_OVSTATICS) $(OVSTATICS_FLAGS) $(OVERLAY_MAP) $(OBJ)/game $(OVERLAY_STATICS_C)
	$(CC) $(PCSRC_CFLAGS) -c -o $@ $(OVERLAY_STATICS_C)
	@echo "  OVSTAT  $$(grep -c '{ [0-9]' $(OVERLAY_STATICS_C)) overlay statics"

# Weak trap stubs for the ~145 link-blocking symbols that belong to prebuilt
# or excluded wifi-era libraries (CPS/DGT/DWC/GameSpy/VCT/ppwlobby) plus a
# few asm-only SDK helpers, dead code for a single-player PC boot. Calling
# one prints the symbol and aborts. The list is checked in and explicit
# (pc/stubs.list): the generator never reads unresolved.txt or link.log, so
# a new genuinely-missing symbol still breaks the link.
STUBS_C := $(BUILD)/stubs.c

$(STUBS_C): $(ROOT)/pc/stubs.list $(ROOT)/pc/gen_stubs.py $(MAKEFILE)
	@mkdir -p $(BUILD)
	@$(PYTHON) $(ROOT)/pc/gen_stubs.py $(ROOT)/pc/stubs.list $@
	@echo "  STUBS   $$(grep -c '^PC_STUB_WEAK ' $@) weak trap stubs"

$(OBJ)/pc/stubs.o: $(STUBS_C)
	@mkdir -p $(dir $@)
	$(CC) $(PCSRC_CFLAGS) -I$(ROOT)/pc/include -c -o $@ $<

OBJS += $(OBJ)/pc/stubs.o

# The linker-command-file symbols the SDK's C reads as values, arena
# bounds, stack sizes, digest bounds. Their one honest source is the ROM
# link itself: build/rom/main.nef.xMAP (the decomp builds the real ROM, so
# the map carries the measured addresses). gen_lcf_syms.py transcribes an
# explicit list; SDK_OVERLAYTABLE_DIGEST is --define'd to zero because the
# ROM's own link never defines it; it is referenced only by digest-mode
# SDK C the ROM leaves out and this build compiles.
LCF_SYMS_C := $(BUILD)/lcf_syms.c
GEN_LCF    := $(ROOT)/tools/armrec/gen_lcf_syms.py
ROM_XMAP   := $(ROOT)/build/rom/main.nef.xMAP

$(LCF_SYMS_C): $(GEN_LCF) $(MAKEFILE) $(wildcard $(ROM_XMAP))
	@mkdir -p $(BUILD)
	@test -f $(ROM_XMAP) || { echo "pc/Makefile: $(ROM_XMAP) missing, configure and ninja build/rom first"; exit 1; }
	@$(PYTHON) $(GEN_LCF) $(ROM_XMAP) $@ \
	    SDK_MAIN_ARENA_LO SDK_SECTION_ARENA_DTCM_START \
	    SDK_SECTION_ARENA_EX_START SDK_SECTION_ARENA_ITCM_START \
	    SDK_STATIC_BSS_START SDK_STATIC_BSS_END SDK_AUTOLOAD_START \
	    SDK_AUTOLOAD_LIST SDK_AUTOLOAD_LIST_END SDK_AUTOLOAD_DTCM_START \
	    SDK_IRQ_STACKSIZE SDK_SYS_STACKSIZE \
	    SDK_OVERLAY_DIGEST SDK_OVERLAY_DIGEST_END \
	    --define SDK_OVERLAYTABLE_DIGEST=0
	@test -z '$(SYMPFX)' || sed -i 's/\.global /.global $(SYMPFX)/; s/\.set /.set $(SYMPFX)/' $@
	@echo "  LCFSYM  $$(grep -c .set $@) linker-script symbols from the xMAP"

$(OBJ)/pc/lcf_syms.o: $(LCF_SYMS_C)
	@mkdir -p $(dir $@)
	$(CC) $(PCSRC_CFLAGS) -c -o $@ $<

ifeq ($(ABSOLUTE_SYMS),1)
OBJS += $(OBJ)/pc/lcf_syms.o
endif

# The trainer AI's bytecode. It is data, not ARM, an offset-addressed
# program the battle engine interprets, and the ROM build already
# assembled it; gen_trainer_ai.py lifts the section into C. This replaces
# what was a zeroed stub in pc/stubs.list until 2026-08-11, when the first
# battle anyone reached dispatched an opcode out of the zeros and died:
# TrainerAI_EvalMoves, reading 0x20527020, reproduced from the session
# recording and fixed by this rule.
TRAI_C   := $(BUILD)/trainer_ai_table.c
TRAI_OBJ := $(ROOT)/build/rom/trainer_ai_script.o

$(TRAI_C): $(TRAI_OBJ) $(ROOT)/pc/gen_trainer_ai.py $(MAKEFILE)
	@mkdir -p $(BUILD)
	@test -f $(TRAI_OBJ) || { echo "pc/Makefile: $(TRAI_OBJ) missing, ninja -C build/rom first"; exit 1; }
	@$(PYTHON) $(ROOT)/pc/gen_trainer_ai.py $(TRAI_OBJ) $@

$(OBJ)/pc/trainer_ai_table.o: $(TRAI_C)
	@mkdir -p $(dir $@)
	$(CC) $(PCSRC_CFLAGS) -c -o $@ $<

OBJS += $(OBJ)/pc/trainer_ai_table.o

# The armrec runtime: identity-mapped guest memory (main RAM, tcms, IO,
# palette, VRAM windows) and the VRAM bank model. It is the memory half of
# the runtime only; there is no recompiled code here to feed it.
$(OBJ)/pc/armrec_rt.o: $(ROOT)/tools/armrec/armrec_rt.c $(ROOT)/tools/armrec/armrec_rt.h $(MAKEFILE)
	@mkdir -p $(dir $@)
	$(CC) $(PCSRC_CFLAGS) -c -o $@ $<

OBJS += $(OBJ)/pc/armrec_rt.o

# The enabled mods' own sources, and the manifest that names them. The
# manifest is a normal generated object, empty string when no mod is on,
# so one `pokeplatinum --help`-style question ("what am I running?") has one
# answer: the binary prints `pc-mods: <list>` at boot when the list is
# non-empty, and a vanilla binary carries the empty string rather than a
# second code path.
# Historical src/*.c stays one level (OpenMMO, existing mods). Nested
# plugins/src/**/*.c is additive. The default MODS list stays empty.
MOD_SRCS := $(foreach m,$(MODS),$(wildcard $(MODS_DIR)/$(m)/src/*.c)) \
            $(foreach m,$(MODS),$(shell find $(MODS_DIR)/$(m)/plugins/src -name '*.c' 2>/dev/null | sort))
MOD_OBJS := $(patsubst $(MODS_DIR)/%.c,$(OBJ)/mods/%.o,$(MOD_SRCS))
OBJS += $(MOD_OBJS)

$(BUILD)/mods_manifest.c: $(MODSTAMP)
	@printf 'const char pc_mods_manifest[] = "%s";\n' "$(strip $(MODS))" > $@

$(OBJ)/pc/mods_manifest.o: $(BUILD)/mods_manifest.c $(MAKEFILE)
	@mkdir -p $(dir $@)
	$(CC) $(PCSRC_CFLAGS) -c -o $@ $<
OBJS += $(OBJ)/pc/mods_manifest.o

.PHONY: all headers status test reach progress objlist cflags abi sync clean cook port

# The generated-symbol rules above precede `all` in the file, and make's
# default goal is the first target it parses, pin it so a bare `make -f
# pc/Makefile` still means "build the binary".
.DEFAULT_GOAL := all

all: $(TARGET)

# ---------------------------------------------------------------- headers

# Any change under pc/include must recompile the world: those headers shadow
# SDK ones, so make's per-file dependency tracking (which never saw the
# shadowed path) cannot know who is affected.
SHADOWSTAMP := $(BUILD)/.shadowstamp
$(SHADOWSTAMP): $(shell find $(ROOT)/pc/include -type f | sort)
	@mkdir -p $(BUILD)
	@touch $@

# Stamped so 109 metang runs do not tax every build. gen_headers.py itself
# skips up-to-date outputs; the stamp just avoids invoking python at all.
HDRSTAMP := $(GEN)/.stamp
headers: $(HDRSTAMP)
$(HDRSTAMP): $(wildcard $(ROOT)/generated/*.txt) $(ROOT)/generated/meson.build \
             $(ROOT)/pc/gen_headers.py
	$(PYTHON) $(ROOT)/pc/gen_headers.py $(GEN) $(ROOT)/build/rom
	@touch $@

# ---------------------------------------------------------------- compile

# Skip-tolerant, copied in spirit from pokediamond: a file that fails to
# compile is recorded as an empty .o.skipped marker (with the compiler's
# stderr kept beside it in .o.err), counted by `status`, and never silently
# dropped from the link, its symbols surface in the unresolved count
# instead. strip_asm.py (tools/armrec) gets first crack because a handful of
# SDK sources define whole functions as mwcc `asm {}` blocks; stripping just
# those lets the portable rest of the file compile, and the stripped functions
# turn into loud undefined symbols. unstatic.py runs on the failure path only,
# for mwcc's "header declares it, definition says static" pattern that GCC
# rejects outright (~40 files here; player_move.c is the canonical case).
# SRCDIRS: the compile runs on strip_asm's copy in the obj tree, which breaks
# quote-includes relative to the original file ("../include/util.h" in the
# NitroSDK library sources, bare private headers in the NNS ones). Point the
# search back at the source's own directory and the src/../include convention.
# The extra bare $(dir $<).. covers NitroWiFi's cross-library reach: soc/src
# includes "../wcm/include/wcm_cpsif.h", a path that only resolves one level
# above the including library's src/.
SRCDIRS = -I$(dir $<) -I$(dir $<).. -I$(dir $<)../include -I$(dir $<)../../include

# pc/patches: per-file unified diffs for mwcc-isms no generic transform can
# express (lvalue casts, token-paste tricks, encoding quirks). A patch lives at
# pc/patches/<path-of-source>.patch and is authored against what the compile
# ACTUALLY CONSUMES, strip_asm's copy when stripping succeeds, the pristine
# source otherwise (diff the build input, not the tree). The tree itself is
# never edited: game src must keep matching the ROM build, and subprojects/
# are meson wraps that a re-download would reset. A patch that stops applying
# fails loudly into the skip count with the reason in .o.err.
PATCHES := $(ROOT)/pc/patches

# pc/mods: the same mechanism, opt-in, stacked after the base patch,
# pc/mods/README.md is the contract. Each enabled mod may carry
# patches/<path-of-source>.patch or plugins/patches/<path>.patch (authored
# against what the compile consumes WITH the base patch already applied,
# mods applying in MODS order) and src/*.c / plugins/src/**/*.c compiled
# as game code. A mod patch that stops applying fails into the skip count
# naming its mod, exactly as a base patch does. Mod diffs use --fuzz=0;
# base pc/patches keep the default fuzz.
#
# THE STAMP is what makes switching mod sets safe: its content names the
# resolved MODS list and a digest of the enabled mods' files, it only moves
# when either changes, and every game object depends on it, so enabling,
# disabling, reordering or EDITING a mod recompiles what its patches could
# have touched, while `make` twice with one set stays a no-op. On change it
# also clears the mod-source objects, because the link globs the object tree
# and an object whose mod has left the set would otherwise go on linking.
FORCE:
$(MODSTAMP): FORCE
	@mkdir -p $(BUILD)
	@want="$(strip $(MODS)) $$( for m in $(MODS); do \
	         find $(MODS_DIR)/$$m -type f | sort | xargs md5sum; \
	       done | md5sum | cut -d' ' -f1 )"; \
	 if [ ! -f $@ ] || [ "$$(cat $@)" != "$$want" ]; then \
	   rm -rf $(OBJ)/mods; \
	   printf '%s' "$$want" > $@; \
	 fi
ifneq ($(strip $(MODS)),)
	@$(PYTHON) $(ROOT)/pc/modinclude.py --mods-dir "$(MODS_DIR)" \
	    --mods "$(strip $(MODS))" --dest "$(BUILD)/modinclude" \
	    --root "$(ROOT)" --gen "$(GEN)"
endif

# The patch-list stamp catches the one patch change the per-file prerequisite
# below cannot. That prerequisite is $(wildcard ...), so an absent patch is
# simply not a prerequisite; which is right for adding or editing one and
# Wrong for deleting one: make sees an object newer than its source and leaves
# the patched object in place. Measured, and it cost a run a bogus measurement:
# A patch was deleted, `make` reported 1392 compiled 0 skipped, and the binary
# still had the patch in it. The stamp holds the sorted list of patch paths,
# not their contents, which the per-file prerequisite already covers, so it
# moves only when one is added or removed, and then the world rebuilds.
PATCHSTAMP := $(BUILD)/.patchstamp
$(PATCHSTAMP): FORCE
	@mkdir -p $(BUILD)
	@want="$$(find $(PATCHES) -name '*.patch' | sort | md5sum | cut -d' ' -f1)"; \
	 if [ ! -f $@ ] || [ "$$(cat $@)" != "$$want" ]; then \
	   printf '%s' "$$want" > $@; \
	 fi

# Objects depend on the flags they were compiled with, not on pc/Makefile's
# mtime. A comment, an unused ASAN block, or a detector target must not
# rebuild 1392 files. Detector trees have their own BUILD, so their stamp
# includes SANITIZE. A recipe-only edit still needs `make clean`.
$(CFLAGSTAMP): FORCE
	@mkdir -p $(BUILD)
	@want="$(CC) $(ABI) $(OPT) $(HW_OPT) $(SANITIZE) $(GAME_SANITIZE) $(EXTRA_OPT) $(DEFINES) $(LDFLAGS)"; \
	 if [ ! -f $@ ] || [ "$$(cat $@)" != "$$want" ]; then \
	   printf '%s' "$$want" > $@; \
	 fi

define COMPILE_RULE
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
	 for m in $(MODS); do \
	   for sub in patches plugins/patches; do \
	     mp="$(MODS_DIR)/$$m/$$sub/$(patsubst $(ROOT)/%,%,$<).patch"; \
	     if [ -f "$$mp" ]; then \
	       tag=$$(printf '%s' "$$sub" | tr / _); \
	       cp "$$src" $@.mod-$$m-$$tag.c; \
	       if patch --silent --forward --fuzz=0 $@.mod-$$m-$$tag.c "$$mp" >/dev/null 2>$@.err; then \
	         src=$@.mod-$$m-$$tag.c; \
	       else \
	         echo "  SKIP    $(patsubst $(ROOT)/%,%,$<) (mod '$$m' patch no longer applies)"; \
	         : > $@.skipped; rm -f $@; exit 0; \
	       fi; \
	     fi; \
	   done; \
	 done; \
	 if $(CC) $(1) $(call opt_from_src,$<) $(SRCDIRS) -c -o $@ $$src 2>$@.err; then rm -f $@.skipped $@.err $@.unstatic.c; \
	 elif $(PYTHON) $(ROOT)/tools/armrec/unstatic.py --cc "$(CC) $(1) $(call opt_from_src,$<) $(SRCDIRS)" \
	        $$src $@.unstatic.c 2>/dev/null \
	      && $(CC) $(1) $(call opt_from_src,$<) $(SRCDIRS) -c -o $@ $@.unstatic.c 2>/dev/null; then rm -f $@.skipped $@.err; \
	 else \
	   echo "  SKIP    $(patsubst $(ROOT)/%,%,$<)"; \
	   : > $@.skipped; rm -f $@; \
	 fi
endef

# VRAMCNT writers get -finstrument-functions: a store to a bank-control
# register is `reg_GX_VRAMCNT_A = 0x80;` through a macro, and C cannot make
# a macro run code AFTER an assignment, so the remap hook fires at
# function exit instead. armrec_rt.c's __cyg_profile_func_exit (shared with
# pokediamond, where this arrangement and its reasoning live) calls
# armrec_vram_touch() there: the last moment the new mapping is still free
# to apply before the caller copies tiles through a window. Without this,
# a screen that remaps banks and loads its assets in the same frame puts
# the assets through the OLD windows, measured as the naming screen
# rendering tile garbage while every earlier screen (boot-time mapping,
# loads frames later) drew clean.
VRAMCNT_SRCS := subprojects/NitroSDK-4.2.30001/libraries/gx/src/gx_vramcnt.c \
                subprojects/NitroSDK-4.2.30001/libraries/gx/src/gxstate.c \
                subprojects/NitroSDK-4.2.30001/libraries/os/src/os_china.c \
                subprojects/NitroSDK-4.2.30001/libraries/wvr/src/wvr.c \
                subprojects/NitroSDK-4.2.30001/libraries/mi/src/mi_wram.c
$(patsubst %.c,$(OBJ)/game/%.o,$(VRAMCNT_SRCS)): \
    EXTRA_CFLAGS := -finstrument-functions

# The port overrides a handful of SDK functions with strong pc/src
# definitions; ELF weakens the whole SDK object after compile so the
# override wins. -O2 inlines same-TU callers and the call is gone, so
# weaken cannot recover it, measured 2026-08-14 as PMi_WaitBusy
# spinning forever (pm.c's own PMi_ReadRegister inlined, pc_pm.c never
# reached, no ARM7 reply). Windows already demotes these five at the
# assembly level for the same reason (PE weaks do not preempt). Keep
# the calls as calls; the rest of each file can still be -O2.
$(OBJ)/game/subprojects/NitroSDK-4.2.30001/libraries/spi/src/pm.o \
$(OBJ)/game/subprojects/NitroSDK-4.2.30001/libraries/spi/src/tp.o \
$(OBJ)/game/subprojects/NitroSDK-4.2.30001/libraries/card/src/card_request.o \
$(OBJ)/game/subprojects/NitroSDK-4.2.30001/libraries/mi/src/mi_dma.o \
$(OBJ)/game/subprojects/NitroSystem-071126.1/libraries/snd/src/capture.o: \
    EXTRA_CFLAGS := -fno-inline-functions -fno-inline-small-functions \
                    -fno-inline-functions-called-once

# SDK objects are weakened after compiling: every global definition in them
# becomes overridable by a strong definition in pc/src. This is the same
# linking model the pokediamond port runs on; there the recompiler emits
# machine-translated code weak and hand-written C wins; here the SDK's
# compiled C plays the machine-translated role. The game's own src/ and lib/
# stay strong: nothing should silently replace hand-decompiled game code.
# A file's patch is a prerequisite of its object, through secondary expansion
# so that only the patched file rebuilds. Without this a pc/patches diff was
# invisible to make: adding or editing one left the old object in place and
# the next `make` reported success having compiled nothing, measured when a
# new patch produced a binary with no trace of it. $(wildcard) is what makes
# an absent patch mean "no extra prerequisite" rather than a missing-file
# error, and it has to be deferred to $$ because a pattern rule's stem is not
# known at parse time. DELETING a patch is the case it cannot see, and
# $(PATCHSTAMP) above is what covers that. Mod patches need neither: the mod
# stamp already digests every file under an enabled mod.
.SECONDEXPANSION:

# SDK_WEAKEN is the weaken step as a hook: an object format with no
# post-compile weaken (wasm) supplies its own recipe for the same result.
SDK_WEAKEN ?= @test ! -f $@ || $(OBJCOPY) --weaken $@

$(OBJ)/game/subprojects/%.o: $(ROOT)/subprojects/%.c $(CFLAGSTAMP) $(HDRSTAMP) $(SHADOWSTAMP) $(MODSTAMP) $(PATCHSTAMP) $$(wildcard $(PATCHES)/subprojects/$$*.c.patch)
	$(call COMPILE_RULE,$(SDK_CFLAGS) $(EXTRA_CFLAGS))
	$(SDK_WEAKEN)

$(OBJ)/game/%.o: $(ROOT)/%.c $(CFLAGSTAMP) $(HDRSTAMP) $(SHADOWSTAMP) $(MODSTAMP) $(PATCHSTAMP) $$(wildcard $(PATCHES)/$$*.c.patch)
	$(call COMPILE_RULE,$(GAME_CFLAGS))

# A mod's own sources, compiled as game code so the game's headers and ABI
# are simply there. New code goes in new files; changing what an existing
# file compiles to is the patches' job; nothing silently replaces
# hand-decompiled game code, which is the same rule the SDK/pc-src override
# model already enforces in the other direction, and it is why there is
# no function-level override (a PE same-TU caller keeps the original).
$(OBJ)/mods/%.o: $(MODS_DIR)/%.c $(CFLAGSTAMP) $(HDRSTAMP) $(SHADOWSTAMP) $(MODSTAMP)
	@mkdir -p $(dir $@)
	$(CC) $(GAME_CFLAGS) $(MOD_INCLUDES) -c -o $@ $<

# Host-layer sources go through COMPILE_RULE so a mod patch on pc/src is
# not silently ignored. pc/hw stays a direct compile: that directory is
# the 3DS agent's this pass.
$(OBJ)/pc/%.o: $(ROOT)/pc/src/%.c $(CFLAGSTAMP) $(HDRSTAMP) $(SHADOWSTAMP) $(MODSTAMP) $(PATCHSTAMP) $$(wildcard $(PATCHES)/pc/src/$$*.c.patch)
	$(call COMPILE_RULE,$(PCSRC_CFLAGS))

$(OBJ)/pchw/%.o: $(ROOT)/pc/hw/%.c $(CFLAGSTAMP) $(HDRSTAMP) $(SHADOWSTAMP)
	@mkdir -p $(dir $@)
	$(CC) $(HW_CFLAGS) $(call opt_from_src,$<) -c -o $@ $<

# ---------------------------------------------------------------- link

# $(wildcard) at link time rather than $(OBJS): skipped files have no .o and
# must not abort the link with "no rule to make target".
#
# EXTRA_OBJS is for objects that do not live under $(OBJ) and so are invisible
# to that find, today just the Windows resource object, which the ELF build
# has no equivalent of.
EXTRA_OBJS ?=
# POST_LINK is LINK_PREP's other end: a hook that reads the finished binary.
# The PE build uses it to check the exe against the oldest Windows it claims
# to run on, which is a fact about the link and not about any one object.
POST_LINK ?= true
$(TARGET): $(OBJS) $(OBJ)/pc/overlay_statics.o $(EXTRA_OBJS)
	@mkdir -p $(BUILD)
	@# A skipped PORT object is a lie the link would tell at runtime: the
	@# game sweep's skip markers are a documented honesty mechanism for
	@# asm-shaped TUs, but pc/ and pchw/ are this port's own C and a skip
	@# there means a compile error somebody swallowed. It hid the Windows
	@# clock for weeks because a newer mingw's headers inline nanosleep and
	@# clock_gettime, pc_win_clock.c stopped compiling on that vintage,
	@# the marker stood in for the object, and every exe built there paced
	@# on the 15.6 ms tick (2026-08-27, the live channel's stutter).
	@sk=$$(find $(OBJ)/pc $(OBJ)/pchw -name '*.o.skipped' 2>/dev/null); \
	 if [ -n "$$sk" ]; then \
	   echo "link: port object(s) SKIPPED; the compile failed; read the"; \
	   echo "link: .err beside each marker:"; \
	   for m in $$sk; do echo "  $${m%.skipped}.err"; \
	     sed -n '1,4p' "$${m%.skipped}.err" 2>/dev/null | sed 's/^/    /'; done; \
	   exit 1; fi
	$(CC) $(LDFLAGS) -o $@ $$(find $(OBJ) -name '*.o' | sort) \
	    $(EXTRA_OBJS) $(LDLIBS)
	@$(POST_LINK)

# Objects are real targets, so a skipped file "succeeds" from make's view
# (the marker exists, the .o does not); re-running make retries it only when
# its source or this Makefile changes. That matches pokediamond's behaviour.
$(OBJS): | $(HDRSTAMP)

# ---------------------------------------------------------------- status

# The awk pipeline is pokediamond's `status` target: defined vs referenced
# over all objects, minus toolchain-owned __names (a __name counts as ours
# only if the tree's own assembly declares it, for Platinum that is
# lib/crypto/asm and subprojects/libsyscall/asm). When PC_MODS is set (or
# loadorder.txt exists) the binary itself prints the overlay line; status
# does not grow a second scanner.
status:
	@mkdir -p $(BUILD)
	@rm -f $(BUILD)/link-failed
	@$(MAKE) -s -f $(THISMK) all 2>$(BUILD)/link.log \
	  || touch $(BUILD)/link-failed
	@$(NM) --defined-only $$(find $(OBJ) -name '*.o') 2>/dev/null \
	  | awk '$$2 ~ /[ATDBRWVGC]/ {print $$3}' | sort -u > $(BUILD)/defined.txt
	@$(NM) -u $$(find $(OBJ) -name '*.o') 2>/dev/null \
	  | awk 'NF >= 2 {print $$2}' | sort -u > $(BUILD)/referenced.txt
	@grep -rh --include='*.s' --include='*.inc' -E '^[[:space:]]*\.?(global|extern|public|arm_func_start)[[:space:]]' \
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
	@if [ -n "$(strip $(MODS))" ]; then echo "mods: $(strip $(MODS))"; fi
	@if [ -f "$(TARGET)" ]; then \
	    if [ -n "$(strip $(PC_MODS))" ]; then \
	      "$(TARGET)" PC_SAVE=none PC_ROM=/no/such.nds \
	          PC_MODS="$(PC_MODS)" PC_MODS_DIR="$(PC_MODS_DIR)" \
	        2>&1 | grep '^modfs:' || true; \
	    elif [ -f "$(PC_MODS_DIR)/loadorder.txt" ]; then \
	      "$(TARGET)" PC_SAVE=none PC_ROM=/no/such.nds \
	          PC_MODS_DIR="$(PC_MODS_DIR)" \
	        2>&1 | grep '^modfs:' || true; \
	    fi; \
	 fi
	@echo "unresolved symbols: $$(wc -l < $(BUILD)/unresolved.txt)"
	@if [ -f $(TARGET) ] && [ ! -f $(BUILD)/link-failed ]; then \
	   echo "link: ok ($(TARGET))"; \
	 else \
	   echo "link: *** FAILED; there is no binary. $(BUILD)/link.log ***"; \
	   grep -m3 -E 'multiple definition|undefined reference|Error [0-9]' \
	     $(BUILD)/link.log 2>/dev/null | sed 's/^/  /' || true; \
	 fi
	@echo "  full list: $(BUILD)/unresolved.txt, $(BUILD)/link.log"

# ---------------------------------------------------------------- cook
#
# Shell the ROM-build tools over every enabled runtime package. Output is
# <pkg>/.cooked/; the running port overlays that tree. The boot path does
# not run this; it only checks the digest. An empty PC_MODS falls through
# to loadorder.txt, same as pc_modfs_boot.

# Stage appended generated tables only on a modded BUILD. Vanilla
# build/pc/geninclude is never a cook output.
COOK_MODINCLUDE :=
ifneq ($(abspath $(BUILD)),$(abspath $(ROOT)/build/pc))
COOK_MODINCLUDE := --modinclude "$(BUILD)/modinclude"
endif

cook:
	@$(PYTHON) $(ROOT)/pc/modcook.py --mods-dir "$(PC_MODS_DIR)" \
	  $(if $(strip $(PC_MODS)),--mods "$(PC_MODS)") \
	  $(COOK_MODINCLUDE)

# Extract content from another NDS ROM into a package. The ROM is
# required; a decomp files/ tree is not a source. Output is
# replace/<path> or narc/<dest>/<idx> the overlay already serves.
#   make -f pc/Makefile port ROM=heartgold.us.nds OUT=pc/mods/from-hg POKEMON=chikorita
port:
	@$(PYTHON) $(ROOT)/pc/modport.py \
	  $(if $(ROM),--rom "$(ROM)") \
	  $(if $(OUT),--out "$(OUT)") \
	  $(if $(POKEMON),--pokemon "$(POKEMON)") \
	  $(if $(AS),--as "$(AS)") \
	  $(if $(FILE),--file "$(FILE)") \
	  $(if $(NARC),--narc "$(NARC)") \
	  $(if $(MEMBER),--member "$(MEMBER)") \
	  $(if $(DEST),--dest "$(DEST)") \
	  $(if $(filter 1,$(LIST)),--list) \
	  $(if $(filter 1,$(IDENTIFY)),--identify)

# ------------------------------------------------------------------ icon
#
# One generator, three outputs: the .ico Explorer reads, the .rc that binds it
# into the Windows exes, and a C array of the same picture for
# SDL_SetWindowIcon where there is no resource section. Deliberately NOT under
# pc/include, a file there invalidates $(SHADOWSTAMP) and rebuilds all 1392
# translation units, and nothing in the game compile wants an icon.
ICONDIR   := $(BUILD)/icon
ICON_H    := $(ICONDIR)/pc_icon.h
ICON_NAME ?= $(basename $(notdir $(TARGET)))
# The Windows build overrides this with all three exes; each needs its own
# .rc because the version block names the file it sits in. Space-separated
# here, joined with commas for the generator.
ICON_EXES ?= $(ICON_NAME).exe
ICON_RCS  := $(patsubst %,$(ICONDIR)/%.rc,$(basename $(ICON_EXES)))
GEN_ICON  := $(ROOT)/pc/gen_icon.py
comma     := ,
empty     :=
space     := $(empty) $(empty)

# A grouped target (&:), not a stamp: one run of the generator produces all
# of these, and make needs to know that or it schedules one run per output.
# A stamp was tried first and was wrong in the one case that matters, delete
# a .rc and the stamp still looks current, so nothing regenerates it.
.PHONY: icon
icon: $(ICON_H) $(ICON_RCS)
$(ICON_H) $(ICON_RCS) &: $(GEN_ICON)
	@mkdir -p $(ICONDIR)
	@$(PYTHON) $(GEN_ICON) --out $(ICONDIR) --name $(ICON_NAME) \
	   --exes $(subst $(space),$(comma),$(ICON_EXES))

# ---------------------------------------------------------------- pcview

# The viewer is a NATIVE 64-bit build (no $(ABI)): the port cannot link the
# host's 64-bit SDL2, which is the whole reason the window is a second
# process over shm (pc/include/pc_view.h).
SDL2_CONFIG ?= sdl2-config
PCVIEW := $(BUILD)/pcview

.PHONY: pcview
pcview: $(PCVIEW)
$(PCVIEW): $(ROOT)/pc/sdl/pcview.c $(ROOT)/pc/include/pc_view.h \
           $(ROOT)/pc/sdl/pc_window_icon.h $(ICON_H) $(MAKEFILE)
	@mkdir -p $(BUILD)
	@command -v $(SDL2_CONFIG) >/dev/null 2>&1 || { \
	  echo "pcview needs SDL2 ($(SDL2_CONFIG) not on PATH)"; exit 1; }
	cc -O2 -Wall -I$(ICONDIR) $$($(SDL2_CONFIG) --cflags) -o $@ $< \
	   $$($(SDL2_CONFIG) --libs)

# The launcher, so a downloaded zip is playable without a terminal. Native
# 64-bit for the same reason the viewer is: it is an SDL2 program. It starts
# the other two and owns none of their behaviour, so it is built and shipped
# beside them rather than linked into either.
PCLAUNCH := $(BUILD)/pclaunch

.PHONY: pclaunch
pclaunch: $(PCLAUNCH)
$(PCLAUNCH): $(ROOT)/pc/sdl/pclaunch.c $(ROOT)/pc/sdl/pclaunch_font.h \
             $(ROOT)/pc/sdl/pc_window_icon.h $(ICON_H) $(MAKEFILE)
	@mkdir -p $(BUILD)
	@command -v $(SDL2_CONFIG) >/dev/null 2>&1 || { \
	  echo "pclaunch needs SDL2 ($(SDL2_CONFIG) not on PATH)"; exit 1; }
	cc -O2 -Wall -I$(ICONDIR) $$($(SDL2_CONFIG) --cflags) -o $@ $< \
	   $$($(SDL2_CONFIG) --libs)

# Everything a player needs, built: the port, the viewer and the launcher.
.PHONY: play
play: $(TARGET) $(PCVIEW) $(PCLAUNCH)
	@echo "  run $(PCLAUNCH)"

# ------------------------------------------------------------------ dist
#
# The one zip. Nothing here is published: the port binary is this tree's
# sources compiled and those sources decompile a commercial game, so `dist`
# builds something you keep. This repository ships source and tools, and
# build/ is gitignored, which is what keeps that true by construction.
#
# The binary carries its symbols deliberately; it is 53 MB unstripped and
# ~29 MB in the zip, and stripping would save half of that while costing the
# port's own --watch instrument its table and every crash report its function
# names. A download that is 15 MB smaller is not worth a bug report that says
# nothing.
DIST_ROOT ?= pokeplatinum-pc
DIST_ZIP  ?= $(BUILD)/dist/$(DIST_ROOT)-linux.zip
MKDIST    := $(ROOT)/pc/mkdist.py

.PHONY: dist
dist: $(TARGET) $(PCVIEW) $(PCLAUNCH)
	@$(PYTHON) $(MKDIST) --out $(DIST_ZIP) --root $(DIST_ROOT) \
	  --exec $(PCLAUNCH) --exec $(TARGET) --exec $(PCVIEW) \
	  --file $(ROOT)/pc/LICENSE:LICENSE.txt

# ---------------------------------------------------------------- harness

# Every test drives the REAL binary; two of them (plan, milestones) need no
# binary at all and run on a tree that will not build, which is exactly when
# someone is most likely to be reading the plan.
test:
	@$(PYTHON) $(ROOT)/pc/tests/run_tests.py

# -O2 is a detector, not the ship build. A second BUILD directory keeps the
# suite's -O1 binary untouched. PC_BIN points the runner at the detector.
# determinism + replay + the fast corpus: the three that would move if a
# hotter optimizer changed what the port does.
O2BUILD ?= $(ROOT)/build/pc-o2

.PHONY: o2
o2:
	$(MAKE) -f $(THISMK) OPT=-O2 BUILD=$(O2BUILD)
	PC_BIN=$(O2BUILD)/pokeplatinum $(PYTHON) $(ROOT)/pc/tests/run_tests.py \
	    determinism corpus

# UBSan is the same shape: second tree, same three tests, first hit dies.
UBSANBUILD ?= $(ROOT)/build/pc-ubsan

.PHONY: ubsan
ubsan:
	$(MAKE) -f $(THISMK) UBSAN=1 BUILD=$(UBSANBUILD)
	PC_BIN=$(UBSANBUILD)/pokeplatinum $(PYTHON) $(ROOT)/pc/tests/run_tests.py \
	    determinism corpus

# ASan is the same shape again. detect_leaks is off: the quarry is a
# wild write, not a leak catalog. halt_on_error follows ASAN_HALT.
# detect_odr_violation=0 is load-bearing, not a shrug: ARM7 objects
# are objcopy-renamed into arm7_* (SND7NS) so the ELF has one
# SNDi_DecibelSquareTable, but ASan's global metadata still carries
# the pre-rename C name and fires on the pair. Real ODR would have
# failed the link.
ASANBUILD ?= $(ROOT)/build/pc-asan
ASAN_OPTIONS_BASE := detect_leaks=0:abort_on_error=1:detect_odr_violation=0

.PHONY: asan
asan:
	$(MAKE) -f $(THISMK) ASAN=1 BUILD=$(ASANBUILD)
	ASAN_OPTIONS=$(ASAN_OPTIONS_BASE) \
	    PC_BIN=$(ASANBUILD)/pokeplatinum $(PYTHON) $(ROOT)/pc/tests/run_tests.py \
	    determinism corpus

# ----------------------------------------------------------------- bench
#
# Where a frame's time goes, over a fixed stretch of the recorded replay, run
# unpaced. BENCH_FRAMES and BENCH_SCRIPT override; the defaults are a segment
# long enough to include the overworld, which is the scene the question is
# usually about.
BENCH_FRAMES ?= 9000
BENCH_SCRIPT ?= $(ROOT)/pc/replays/new-game.txt

.PHONY: bench
bench: $(TARGET)
	@PC_SAVE=none PC_PACE=0 PC_BENCH=1 \
	 PC_FRAMES=$(BENCH_FRAMES) PC_INPUT=$(BENCH_SCRIPT) \
	 $(TARGET) 2>&1 >/dev/null | grep '^pc-bench' || true

# How far the port gets before it stops, and what stopped it: frames, the
# ending, and where. Any of the three moving is news; the frame count alone is
# not the signal.
#
# No default for REACH_FRAMES here. Pc/reach.sh owns the default, and a second
# copy of the number in this file is how pokediamond's `make reach` went on
# reporting "clean exit after 3000" for a wall its script had already been
# raised past. This only forwards an override.
reach:
	@$(if $(REACH_FRAMES),REACH_FRAMES=$(REACH_FRAMES) )$(ROOT)/pc/reach.sh $(if $(ROM),--rom $(ROM))

# Where things stand: symbols resolved, tasks done, milestones reached. Read
# out of the tree, so it cannot go stale.
progress:
	@$(ROOT)/pc/progress.sh

# Every object this build expects, one per line, for pc/sync_upstream.sh to diff
# the object tree against.
#
# The link globs $(OBJ) rather than using $(OBJS), deliberately, so a skipped
# file's absent .o does not abort it; which means an .o that has LEFT the build
# goes on linking as a ghost definition of a function nobody compiles. Two ways
# to leave: upstream deletes the source, or the source stops being enumerated
# (the ARM7 exclusion above dropped 86, nine of which had already compiled to
# .o files that would have kept linking). Neither is visible in any count the
# build prints, and no rule can remove them, because make only knows about
# targets it still has.
objlist:
	@printf '%s\n' $(OBJS)

# The exact flags a game translation unit is compiled with, for anything that
# needs to compile a header the way the port does.
#
# The point is that there is no second copy. A struct's field offsets are only
# meaningful under the ABI the port actually uses, on an LP64 host every
# pointer in a struct is four bytes too wide and shifts every field behind it.
# Anything carrying its own guess at these flags would be measuring a layout
# nothing is built with.
cflags:
	@printf '%s\n' $(GAME_CFLAGS)

# Does this build lay pret's structs out the way the ROM build's compiler does?
# Both compile the same headers, so a disagreement is the ABI and nothing else,
# and that the two agree is what the whole 32-bit host build rests on.
#
# Needs build/rom as well as build/pc: it reads the layouts out of the debug
# info mwcc and gcc already wrote into the objects, rather than compiling a
# probe of its own whose flags would be a second guess at both builds.
# NAME=Foo dumps one struct's two layouts side by side.
abi:
	@python3 $(ROOT)/pc/abi_layout.py $(if $(NAME),--dump $(NAME)) $(ABI_ARGS)

# ---------------------------------------------------- the differential oracle
#
# melonDS, built headless as a library and linked into a program that writes
# the same trace the port writes. MELONDS points at a checkout beside this
# repository by default and the target says so when there is not one.
#
# Three CMake options are off and all three are load-bearing rather than taste.
# ENABLE_JIT: a JIT in the oracle is a second implementation with its own bug
# surface. ENABLE_OGLRENDERER: there is no window and no GL context.
# ENABLE_GDBSTUB, which is the one that costs an hour if you get it wrong,
# melonDS turns it on with `add_definitions(-DGDBSTUB_ENABLED)` in its
# top-level CMakeLists, and add_definitions is directory-scoped, so a consumer
# outside that project does not inherit it from the `core` target.
# GDBSTUB_ENABLED puts a Gdb::GdbStub *member* inside ARM, so a library built
# with it and a caller built without it disagree about sizeof(ARM) and about
# the offset of every NDS member after ARM9. melon_host.cpp checks for that at
# startup and names it.
#
# 64-bit, deliberately: $(ABI) is not used here. The port is -m32 because its
# struct layouts have to match the DS; melonDS's core has no such requirement
# and its own build assumes a 64-bit host. The two programs share nothing but a
# text file.
MELONDS     ?= $(HOME)/GitHub/melonDS
MELON_BUILD := $(BUILD)/melonds
MELON_HOST  := $(BUILD)/pcdiff-melon
CXX         ?= g++
CMAKE       ?= cmake

# -fwrapv because melonDS's core sets it PUBLIC (`target_compile_options(core
# PUBLIC -fwrapv)`); the rest is what any consumer of the core needs.
MELON_CXXFLAGS := -std=c++17 -O2 -g -fwrapv -Wall \
                  -I$(MELONDS)/src -I$(MELON_BUILD)/src

MELON_SRCS := $(ROOT)/pc/diff/melon_host.cpp $(ROOT)/pc/diff/melon_platform.cpp

# Everything $(MELON_HOST) is built from, named once, so a test can ask what
# the oracle it is about to trust was built from rather than keep its own list.
# libteakra.a is on it because the link line uses it.
MELON_HOST_DEPS := $(MELON_SRCS) $(MELON_BUILD)/src/libcore.a \
                   $(MELON_BUILD)/src/teakra/src/libteakra.a $(THISMK)

.PHONY: melon diff melon-deps

$(MELON_BUILD)/src/libcore.a:
	@test -d $(MELONDS)/src || { \
	  echo "  melonDS is not at $(MELONDS)."; \
	  echo "  The differential runner needs it. Clone it, or point MELONDS"; \
	  echo "  at your checkout:"; \
	  echo "      make -f pc/Makefile melon MELONDS=/path/to/melonDS"; \
	  exit 1; }
	@echo "  CMAKE   melonDS core (no JIT, no GL, no GDB stub)"
	@$(CMAKE) -S $(MELONDS) -B $(MELON_BUILD) -DCMAKE_BUILD_TYPE=Release \
	    -DENABLE_JIT=OFF -DENABLE_OGLRENDERER=OFF -DENABLE_GDBSTUB=OFF \
	    -DBUILD_QT_SDL=OFF -DENABLE_LTO_RELEASE=OFF > $(BUILD)/melon-cmake.log 2>&1 \
	  || { echo "  cmake failed; see $(BUILD)/melon-cmake.log"; exit 1; }
	@$(CMAKE) --build $(MELON_BUILD) --target core -j$(shell nproc 2>/dev/null || echo 4) \
	    >> $(BUILD)/melon-cmake.log 2>&1 \
	  || { echo "  building melonDS core failed; see $(BUILD)/melon-cmake.log"; exit 1; }

# teakra comes out of the same cmake build as the core, so it has no build step
# of its own, but a prerequisite with no rule at all is a hard error on a tree
# that has not built melonDS yet, which would make `make melon` fail on a fresh
# checkout rather than run cmake. An empty recipe behind libcore.a says "this
# appears when that does".
$(MELON_BUILD)/src/teakra/src/libteakra.a: $(MELON_BUILD)/src/libcore.a ;

$(MELON_HOST): $(MELON_HOST_DEPS)
	@echo "  CXX     $(MELON_HOST)"
	@$(CXX) $(MELON_CXXFLAGS) -o $@ $(MELON_SRCS) \
	    $(MELON_BUILD)/src/libcore.a \
	    $(MELON_BUILD)/src/teakra/src/libteakra.a -lpthread -lm

melon: $(MELON_HOST)

# What the oracle was built from, one path per line. Printing it rather than
# duplicating it is the point: a test that checks the oracle is not stale reads
# this, so the list cannot drift from the rule.
melon-deps:
	@printf '%s\n' $(MELON_HOST_DEPS)

# Run both programs over the same ROM and report where they first disagree.
# FRAMES, SCRIPT, SAV and EVERY are optional; DIFF_ARGS passes anything else.
diff: $(MELON_HOST) $(TARGET)
	@python3 $(ROOT)/pc/diff/pcdiff.py \
	    $(if $(FRAMES),--frames $(FRAMES)) \
	    $(if $(EVERY),--every $(EVERY)) \
	    $(if $(SCRIPT),--input $(SCRIPT)) \
	    $(if $(SAV),--sav $(SAV)) \
	    $(DIFF_ARGS)

# Merge upstream pret and keep the merge only if the port still holds up. The
# script owns every default and every gate; this only forwards overrides, for
# the same reason `reach` does, a second copy of a number in this file is how
# a stale default outlives the script that was supposed to own it.
#
# SYNC_MAX=N limits the batch (0 or --all for everything); SYNC_ARGS passes the
# rest through (--dry-run, --abort, --no-reach).
sync:
	@$(if $(SYNC_MAX),UPSTREAM_MAX=$(SYNC_MAX) )$(ROOT)/pc/sync_upstream.sh $(SYNC_ARGS)

clean:
	rm -rf $(OBJ) $(TARGET) $(PCVIEW) $(PCLAUNCH) $(BUILD)/link.log $(BUILD)/defined.txt \
	  $(BUILD)/referenced.txt $(BUILD)/unresolved.txt $(BUILD)/link-failed


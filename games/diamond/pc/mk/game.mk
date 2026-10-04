# pc/mk/game.mk: the decompiled C of Diamond/Pearl (see pc/Makefile.wasm).
#
#   make -f pc/Makefile.wasm game-objs        every game/SDK C object
#   make -f pc/Makefile.wasm extracted-asm    the mwcc asm bodies, as .s
#   make -f pc/Makefile.wasm game-dupcheck    duplicate strong definitions
#
# Sources: exactly what arm9/Makefile compiles as C for the ARM9 (SRC_DIRS
# src lib lib/libnns/src lib/NitroSDK/src lib/MSL_C/src overlays/*/src),
# nothing excluded. A file that does not compile fails the build; there is
# no skip list.
#
# Objects: $(OBJ)/game/<path under arm9>.o, each through BRIDGE_COMPILE.
#
# Order of the source transforms (D's own; Platinum's COMPILE_RULE strips
# first and patches second):
#   arm9/<p>.c --pc/patches/arm9/<p>.c.patch--> $(BUILD)/patched/<p>.c
#              --strip_asm.py-->                $(BUILD)/prep/<p>.c  -> object
#              --pc/tools/dp_extract_asm.py-->  $(BUILD)/extracted/<p>.s
# Patches are authored against the PRISTINE source, so one patch serves both
# the C compile and the asm extraction (OS_exception.c needs both). Only
# files with mwcc `asm` bodies or a patch take this path; the rest compile
# from arm9/ directly.

A9 := $(ROOT)/arm9

GAME_SRC_DIRS := src lib lib/libnns/src lib/NitroSDK/src lib/MSL_C/src \
                 $(patsubst $(A9)/%,%,$(sort $(wildcard $(A9)/overlays/*/src)))
GAME_C   := $(foreach d,$(GAME_SRC_DIRS),$(sort $(wildcard $(A9)/$(d)/*.c)))
GAME_REL := $(patsubst $(A9)/%,%,$(GAME_C))
GAME_OBJS := $(GAME_REL:%.c=$(OBJ)/game/%.o)

# ------------------------------------------------------------------ flags
#
# arm9/Makefile's MWCFLAGS, translated:
#   -i ../include overlays/*/include ../files, -ir ../include-mw
#   lib/MSL_C/include lib/libnns/include lib/NitroSDK/include
# in that order. The -ir (recursive) dirs have no subdirectories, so -I is
# the whole translation. pc/include goes first: D-only header shadows.
#   -D$(GAME_VERSION) -D$(GAME_LANGUAGE) -DFS_IMPLEMENT, -enum int
# (= -fno-short-enums, in WASM_ABI).
#
# -funsigned-char: mwcc's default char is unsigned and arm9/Makefile does
# not say `-char signed` (pokeplatinum's meson.build does, which is why
# WASM_ABI, shared with the host layer, has -fsigned-char). Measured on the
# ROM: sub_02032A8C (unk_02031734.c) compares a plain `char` array with a
# u8 and mwcc loads it with ldrb, not ldrsb. It is the one D TU whose code
# depends on it (clang IR of all 297 TUs compared under both settings).
#
# u64/s64 alignment, the one struct-layout difference between mwcc and
# wasm32 clang: pc/include/nitro/types.h through the prelude.
GAME_INCLUDES := -I$(PCDIR)/include -I$(ROOT)/include \
                 $(addprefix -I,$(sort $(wildcard $(A9)/overlays/*/include))) \
                 -I$(ROOT)/files -I$(ROOT)/include-mw \
                 -I$(A9)/lib/MSL_C/include -I$(A9)/lib/libnns/include \
                 -I$(A9)/lib/NitroSDK/include
GAME_DEFINES := -D$(GAME_VERSION) -D$(GAME_LANGUAGE) -DFS_IMPLEMENT \
                -DPC_GAME_DP -DPLATFORM_PC
# Platinum's pc/include last (-idirafter), for the plain-C host interfaces
# game-side code shares with the runtime glue (pc_np_options.h); it never
# shadows a D or SDK header.
GAME_CFLAGS := $(WASM_ABI) -funsigned-char $(OPT) \
               -include $(PCDIR)/include/pc_prelude.h \
               $(GAME_INCLUDES) $(GAME_DEFINES) -idirafter $(PLAT)/pc/include

# MSL_C: Metrowerks' C library. Most of it is assembly, which armrec
# recompiles with HOST_LIBC_NAMES prefixed guest_ so wasi-libc stays the
# host's C library. Its two C files (abs, rand/srand) get the same names
# here; armrec's --guest-libc abs,rand,srand (DPArmrec) points the
# recompiled callers (`bl rand` in overlay 4, `bl abs` ...) at them. The
# game's own C keeps calling the host's abs (palette.c), same function.
GAME_MSL_RENAME := -Dabs=guest_abs -Drand=guest_rand -Dsrand=guest_srand

# src/filesystem.c declares `register u32 chunk_starts[3]` and subscripts
# it, which needs the array's address: mwcc allows that, clang refuses
# ("address of register variable requested"). `register` is only a hint;
# dropping it changes nothing else.
GAME_TU_EXTRA_src/filesystem.c := -Dregister=

# The VRAMCNT writers (the C ones; the assembly ones armrec hooks per store,
# ARMREC_VRAM_HOOK): -finstrument-functions, so armrec_rt.c's
# __cyg_profile_func_exit applies a bank remap before the caller copies
# through the new window; Platinum's pc/Makefile VRAMCNT_SRCS has the
# reasoning. irbridge.py nulls the return-address argument the wasm backend
# cannot produce; GAME_SDK_COMPILE's pass 1 (a plain compile, only for the
# symbol list) leaves the flag out for the same reason.
GAME_VRAMCNT_TUS := lib/NitroSDK/src/GX_vramcnt.c lib/NitroSDK/src/GX_state.c \
                    lib/NitroSDK/src/MI_wram.c
$(foreach t,$(GAME_VRAMCNT_TUS),$(eval GAME_TU_EXTRA_$(t) := -finstrument-functions))

# Overlay TUs know their overlay number (pc/include/sinit.h): the NN of
# arm9/overlays/NN/src, which is the ROM's overlay id (arm9.lsf OVERLAY_NN),
# leading zero dropped so 08/09 are not octal.
game_ovl = $(patsubst 0%,%,$(word 2,$(subst /, ,$(1))))

# Per-TU flags, from the path under arm9/ ($(1), with .c). -iquote keeps
# a quote include searching the source's own directory first, as mwcc does,
# also for the copies compiled from $(BUILD)/prep.
game_tu_flags = $(GAME_CFLAGS) -iquote$(dir $(A9)/$(1)) \
    $(if $(filter overlays/%,$(1)),-DPC_DP_OVERLAY=$(call game_ovl,$(1))) \
    $(if $(filter lib/MSL_C/%,$(1)),$(GAME_MSL_RENAME)) \
    $(GAME_TU_EXTRA_$(1))

GAME_DEPFLAGS = -MMD -MP -MF $@.d -MT $@

# ------------------------------------------------------------ flag stamp
#
# Objects depend on the flags they were compiled with (a stamp that moves
# only when they change), the D shadows, and their headers (.d files).
GAME_FLAGSTAMP := $(BUILD)/.game-flags
GAME_SHADOWS := $(shell find $(PCDIR)/include -type f)
.PHONY: FORCE
FORCE:
$(GAME_FLAGSTAMP): FORCE
	@mkdir -p $(dir $@)
	@want='$(CC) $(GAME_CFLAGS) $(GAME_MSL_RENAME)'; \
	 if [ ! -f $@ ] || [ "$$(cat $@)" != "$$want" ]; then printf '%s' "$$want" > $@; fi
GAME_DEPS := $(GAME_FLAGSTAMP) $(GAME_SHADOWS)

# ----------------------------------------------------- patches, asm in C
GAME_PATCHES := $(PCDIR)/patches
GAME_PATCHED := $(patsubst $(GAME_PATCHES)/arm9/%.patch,%,\
                  $(shell find $(GAME_PATCHES)/arm9 -name '*.c.patch' 2>/dev/null))
# Files holding mwcc `asm` function bodies (or a statement-level asm block):
# the same test strip_asm.py applies, 28 files today.
GAME_ASM_C := $(patsubst $(A9)/%,%,$(shell grep -lE \
    '^[[:space:]]*((static|[A-Z_]+)[[:space:]]+)?asm[[:space:]]|^[[:space:]]*asm[[:space:]]*$$' \
    $(GAME_C)))
GAME_PREP := $(sort $(GAME_ASM_C) $(GAME_PATCHED))
GAME_DIRECT := $(filter-out $(GAME_PREP),$(GAME_REL))

# The mwcc asm bodies, for armrec (pc/mk/armrec.mk). Functions the host
# layer replaces are listed in pc/host_overrides.txt (DPHost) and left out.
HOST_OVERRIDES := $(PCDIR)/host_overrides.txt
EXTRACTED_ASM := $(GAME_ASM_C:%.c=$(BUILD)/extracted/%.s)

# Asm-in-C that never runs on the host and is not replaced either: crt0.c's
# `_start`, the DS reset entry (wasi's _start is the module's entry and
# crt0 never runs; nothing in D references the name). Emitting it would put
# a second `_start` into the link next to wasi-libc's.
GAME_ASM_DEAD := _start

STRIP_ASM   := $(ARMREC)/strip_asm.py
EXTRACT_ASM := $(PCDIR)/tools/dp_extract_asm.py

$(BUILD)/patched/%.c: $(A9)/%.c $$(wildcard $(GAME_PATCHES)/arm9/$$*.c.patch)
	@mkdir -p $(dir $@)
	@cp $< $@.tmp
	@p=$(GAME_PATCHES)/arm9/$*.c.patch; \
	 if [ -f $$p ]; then patch --silent --forward $@.tmp $$p || \
	   { echo "pc/patches/arm9/$*.c.patch no longer applies" >&2; rm -f $@.tmp; exit 1; }; fi
	@mv $@.tmp $@

$(BUILD)/prep/%.c: $(BUILD)/patched/%.c $(STRIP_ASM)
	@mkdir -p $(dir $@)
	@$(PYTHON) $(STRIP_ASM) $< $@ 2>/dev/null

$(BUILD)/extracted/%.s: $(BUILD)/patched/%.c $(EXTRACT_ASM) $(ARMREC)/extract_asm.py \
                        $(ROM_XMAP) $(wildcard $(HOST_OVERRIDES)) $(GAME_FLAGSTAMP) \
                        $(PCDIR)/mk/game.mk
	@mkdir -p $(dir $@)
	$(PYTHON) $(EXTRACT_ASM) --cc "$(CC) $(call game_tu_flags,$*.c)" \
	    --xmap $(ROM_XMAP) --object $(notdir $*).o --overrides $(HOST_OVERRIDES) \
	    $(addprefix --drop ,$(GAME_ASM_DEAD)) --label arm9/$*.c $< $@

.PRECIOUS: $(BUILD)/patched/%.c $(BUILD)/prep/%.c

# ---------------------------------------------------------------- compile
#
# SDK sources (lib/: NitroSDK, libnns, MSL_C) are compiled weak, Platinum's
# two-pass weaken (games/platinum/pc/Makefile.wasm SDK_WEAKEN): pass 1
# lists the TU's own definitions, pass 2 recompiles with `#pragma weak` for
# each, so the host layer's strong definitions of SDK functions
# (OS_*, PXI_*, CARD_*, FS_StartOverlay, MI_*, ...) win at link without a
# list, and a same-TU caller is never inlined past one. Game code (src/,
# overlays/) is strong.
define GAME_SDK_COMPILE
@mkdir -p $(dir $@)
@$(CC) $(filter-out -finstrument-functions,$(call game_tu_flags,$*.c)) -c -o $@.pass1.o $<
 @$(NM) --defined-only -g $@.pass1.o 2>/dev/null | awk 'NF == 3 { print "#pragma weak " $$3 }' > $@.weak.h
@rm -f $@.pass1.o
$(call BRIDGE_COMPILE,$(call game_tu_flags,$*.c) $(GAME_DEPFLAGS) -include $@.weak.h)
endef

define GAME_COMPILE
$(if $(filter lib/%,$*.c),$(GAME_SDK_COMPILE),$(call BRIDGE_COMPILE,$(call game_tu_flags,$*.c) $(GAME_DEPFLAGS)))
endef

$(GAME_DIRECT:%.c=$(OBJ)/game/%.o): $(OBJ)/game/%.o: $(A9)/%.c $(GAME_DEPS) $(BRIDGE_DEPS)
	$(GAME_COMPILE)

$(GAME_PREP:%.c=$(OBJ)/game/%.o): $(OBJ)/game/%.o: $(BUILD)/prep/%.c $(GAME_DEPS) $(BRIDGE_DEPS)
	$(GAME_COMPILE)

-include $(wildcard $(GAME_OBJS:%=%.d))

# ------------------------------------------------- game-side host code
#
# pc/game/*.c: D-specific host code that works on the game's own structures
# (FieldSystem, Camera, TextPrinter, ...) and so compiles with the game's
# headers and flags, not the host layer's 4.2 ones: the runtime options'
# field half (pc_dp_field.c), the hooks pc/patches/arm9/*.s.patch call.
# Strong, through BRIDGE_COMPILE like any game TU.
GAME_PC_C    := $(sort $(wildcard $(PCDIR)/game/*.c))
GAME_PC_OBJS := $(patsubst $(PCDIR)/game/%.c,$(OBJ)/pcgame/%.o,$(GAME_PC_C))

$(OBJ)/pcgame/%.o: $(PCDIR)/game/%.c $(GAME_DEPS) $(BRIDGE_DEPS)
	@mkdir -p $(dir $@)
	$(call BRIDGE_COMPILE,$(GAME_CFLAGS) -iquote$(PCDIR)/game $(GAME_DEPFLAGS))

-include $(wildcard $(GAME_PC_OBJS:%=%.d))

OBJS += $(GAME_PC_OBJS)

OBJS += $(GAME_OBJS)

# ------------------------------------------------------------- targets
.PHONY: game-objs extracted-asm game-dupcheck
game-objs: $(GAME_OBJS)
extracted-asm: $(EXTRACTED_ASM)

# Two STRONG definitions of one name among the game objects would fail the
# link; weak ones (the whole SDK) resolve. Lists every such name and fails.
game-dupcheck: $(GAME_OBJS)
	@$(NM) -A --defined-only -g $(GAME_OBJS) 2>/dev/null | \
	 awk '{ t = $$(NF-1); n = $$NF; if (t != "W" && t != "V" && t != "w" && t != "v") c[n]++; } \
	      END { bad = 0; for (n in c) if (c[n] > 1) { print "duplicate strong definition: " n; bad = 1 } \
	            if (bad) exit 1; print "game objects: no duplicate strong definitions" }'

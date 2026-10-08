# pc/mk/game.mk: the decompiled C of HeartGold/SoulSilver (see
# pc/Makefile.wasm). games/diamond/pc/mk/game.mk is the model; what differs
# is where the sources come from and the flags.
#
#   make -f pc/Makefile.wasm game-objs        every game/SDK C object
#   make -f pc/Makefile.wasm extracted-asm    the mwcc asm bodies, as .s
#
# Sources: every object main.lsf links that has a .c (pc/tools/hg_lsf.py),
# nothing excluded: src/**, lib/NitroSDK/src/{os,mi}, and lib/dsprot/src
# (the ds_protect overlay's modules in the clear; see hg_lsf.py).
#
# Objects: $(OBJ)/game/<path>.o, each through BRIDGE_COMPILE.
#
# Source transforms, as on D/P:
#   <p>.c --pc/patches/<p>.c.patch--> $(BUILD)/patched/<p>.c
#         --strip_asm.py-->           $(BUILD)/prep/<p>.c  -> object
#         --dp_extract_asm.py-->      $(BUILD)/extracted/<p>.s
# An overlay TU's extracted assembly is written under
# $(BUILD)/extracted/overlays/NN/src/, the path armrec reads an overlay from
# (armrec.py overlay_of).

GAME_LIST := $(shell $(PYTHON) $(HG_LSF) $(LSF) c)
GAME_REL  := $(filter %.c,$(GAME_LIST))
GAME_OBJS := $(GAME_REL:%.c=$(OBJ)/game/%.o)
# path.c -> overlay id ("-" = static), for the extraction path and flags.
game_ovl_of = $(word 2,$(subst :, ,$(filter $(1):%,$(GAME_OVL_PAIRS))))
GAME_OVL_PAIRS := $(shell $(PYTHON) $(HG_LSF) $(LSF) c | awk '{ print $$1 ":" $$2 }')

# ------------------------------------------------------------------ flags
#
# common.mk's MWCFLAGS, translated: -i ./src -i ./include
# -i ./include/library -i files -I lib/include; DEFINES = GF_DEFINES
# (-D$(GAME_VERSION) -DGAME_REMASTER=0 -DENGLISH -DPM_KEEP_ASSERTS) plus
# GLB_DEFINES (-DSDK_ARM9 -DSDK_CODE_ARM -DSDK_FINALROM); lib/ objects get
# GLB_DEFINES only. -char signed (WASM_ABI's -fsigned-char), -enum int
# (-fno-short-enums), -lang c99 (gnu99: mwcc's extensions are on,
# -gccext,on).
#
# First on the path: Platinum's 4.2 SDK shadows (pc/include/nitro: the u64
# alignment in types.h, the coprocessor and geometry-port registers routed
# through the armrec runtime in ioreg_CP.h / ioreg_G3.h / ioreg_G3X.h), the
# same NitroSDK 4.2 headers HG/SS's lib/include carries.
GAME_SHADOW_DIR := $(BUILD)/shadow
GAME_INCLUDES := -I$(MYPC)/include -I$(GAME_SHADOW_DIR) \
                 -I$(ROOT)/src -I$(ROOT)/include -I$(ROOT)/include/library \
                 -I$(ROOT)/files -I$(ROOT)/lib/include
GAME_GF_DEFINES := -D$(GAME_VERSION) -DGAME_REMASTER=0 -D$(GAME_LANGUAGE) -DPM_KEEP_ASSERTS
GAME_GLB_DEFINES := -DSDK_ARM9 -DSDK_CODE_ARM -DSDK_FINALROM -DPC_GAME_HGSS -DPLATFORM_PC
GAME_CFLAGS := $(WASM_ABI) $(OPT) -include $(MYPC)/include/pc_prelude.h \
               $(GAME_INCLUDES) -idirafter $(PLAT)/pc/include

# The shadow directory holds copies of exactly Platinum's 4.2 SDK shadows
# (not the rest of Platinum's pc/include, whose host headers would shadow
# the game's own: pc/include/extras.h, ...), so a `#include_next` in them
# reaches lib/include.
GAME_SHADOW_SRC := nitro/types.h nitro/hw/ARM9/ioreg_CP.h nitro/hw/ARM9/ioreg_G3.h \
                   nitro/hw/ARM9/ioreg_G3X.h
GAME_SHADOW_H := $(GAME_SHADOW_SRC:%=$(GAME_SHADOW_DIR)/%)
$(GAME_SHADOW_DIR)/%: $(PLAT)/pc/include/%
	@mkdir -p $(dir $@)
	cp $< $@

# lib/dsprot builds on its own (lib/dsprot/Makefile: -i ./include, GLB
# defines only).
game_tu_flags = $(GAME_CFLAGS) -iquote$(dir $(ROOT)/$(1)) \
    $(if $(filter lib/dsprot/%,$(1)),-iquote$(ROOT)/lib/dsprot/include) \
    $(if $(filter lib/%,$(1)),,$(GAME_GF_DEFINES)) $(GAME_GLB_DEFINES)

GAME_DEPFLAGS = -MMD -MP -MF $@.d -MT $@

# ------------------------------------------------------------ flag stamp
GAME_FLAGSTAMP := $(BUILD)/.game-flags
GAME_SHADOWS := $(shell find $(MYPC)/include -type f 2>/dev/null)
.PHONY: FORCE
FORCE:
$(GAME_FLAGSTAMP): FORCE
	@mkdir -p $(dir $@)
	@want='$(filter-out $(CCACHE),$(CC)) $(GAME_CFLAGS) $(GAME_GF_DEFINES) $(GAME_GLB_DEFINES)'; \
	 if [ ! -f $@ ] || [ "$$(cat $@)" != "$$want" ]; then printf '%s' "$$want" > $@; fi
GAME_DEPS := $(GAME_FLAGSTAMP) $(GAME_SHADOWS) $(GAME_SHADOW_H)

# ----------------------------------------------------- patches, asm in C
GAME_PATCHES := $(MYPC)/patches
GAME_PATCHED := $(patsubst $(GAME_PATCHES)/%.patch,%,\
                  $(shell find $(GAME_PATCHES) -name '*.c.patch' 2>/dev/null))
GAME_ASM_C := $(patsubst $(ROOT)/%,%,$(shell cd $(ROOT) && grep -lE \
    '^[[:space:]]*((static|[A-Z_]+)[[:space:]]+)?asm[[:space:]]|^[[:space:]]*asm[[:space:]]*$$' \
    $(GAME_REL)))
GAME_PREP := $(sort $(GAME_ASM_C) $(GAME_PATCHED))
GAME_DIRECT := $(filter-out $(GAME_PREP),$(GAME_REL))

HOST_OVERRIDES := $(MYPC)/host_overrides.txt
# Where a TU's asm bodies go: overlays under extracted/overlays/NN/src.
game_extracted = $(BUILD)/extracted/$(if $(filter-out -,$(call game_ovl_of,$(1))),overlays/$(call game_ovl_of,$(1))/src/$(notdir $(1:%.c=%.s)),$(1:%.c=%.s))
EXTRACTED_ASM := $(foreach f,$(GAME_ASM_C),$(call game_extracted,$(f)))

# crt0's DS reset entry is assembly here (lib/asm/crt0.s, not linked as
# C), so no asm-in-C body is dead.
GAME_ASM_DEAD :=

STRIP_ASM   := $(ARMREC)/strip_asm.py
EXTRACT_ASM := $(PCDIR)/tools/dp_extract_asm.py

$(BUILD)/patched/%.c: $(ROOT)/%.c $$(wildcard $(GAME_PATCHES)/$$*.c.patch)
	@mkdir -p $(dir $@)
	@cp $< $@.tmp
	@p=$(GAME_PATCHES)/$*.c.patch; \
	 if [ -f $$p ]; then patch --silent --forward $@.tmp $$p || \
	   { echo "pc/patches/$*.c.patch no longer applies" >&2; rm -f $@.tmp; exit 1; }; fi
	@mv $@.tmp $@

$(BUILD)/prep/%.c: $(BUILD)/patched/%.c $(STRIP_ASM)
	@mkdir -p $(dir $@)
	@$(PYTHON) $(STRIP_ASM) $< $@ 2>/dev/null

# One extraction rule per asm-in-C TU (its output path depends on its
# overlay, which a pattern cannot say).
define GAME_EXTRACT_RULE
$(call game_extracted,$(1)): $(BUILD)/patched/$(1) $(EXTRACT_ASM) $(ARMREC)/extract_asm.py \
        $(ROM_XMAP) $(wildcard $(HOST_OVERRIDES)) $(GAME_FLAGSTAMP) $(MYPC)/mk/game.mk $(GAME_SHADOW_H)
	@mkdir -p $$(dir $$@)
	$(PYTHON) $(EXTRACT_ASM) --cc "$(CC) $(call game_tu_flags,$(1))" \
	    --xmap $(ROM_XMAP) --object $(notdir $(1:%.c=%.o)) --overrides $(HOST_OVERRIDES) \
	    $(addprefix --drop ,$(GAME_ASM_DEAD)) --label $(1) $$< $$@
endef
$(foreach f,$(GAME_ASM_C),$(eval $(call GAME_EXTRACT_RULE,$(f))))

.PRECIOUS: $(BUILD)/patched/%.c $(BUILD)/prep/%.c

# ---------------------------------------------------------------- compile
#
# SDK C (lib/: NitroSDK os/mi, dsprot) is compiled weak, the two-pass
# weaken of D/P's game.mk, so the host layer's strong OS_* definitions win
# at link. Game code (src/) is strong.
define GAME_SDK_COMPILE
@mkdir -p $(dir $@)
@$(CC) $(call game_tu_flags,$*.c) -c -o $@.pass1.o $<
@$(NM) --defined-only -g $@.pass1.o 2>/dev/null | awk 'NF == 3 { print "#pragma weak " $$3 }' > $@.weak.h
@rm -f $@.pass1.o
$(call BRIDGE_COMPILE,$(call game_tu_flags,$*.c) $(GAME_DEPFLAGS) -include $@.weak.h)
endef

define GAME_COMPILE
$(if $(filter lib/%,$*.c),$(GAME_SDK_COMPILE),$(call BRIDGE_COMPILE,$(call game_tu_flags,$*.c) $(GAME_DEPFLAGS)))
endef

$(GAME_DIRECT:%.c=$(OBJ)/game/%.o): $(OBJ)/game/%.o: $(ROOT)/%.c $(GAME_DEPS) $(BRIDGE_DEPS)
	$(GAME_COMPILE)

$(GAME_PREP:%.c=$(OBJ)/game/%.o): $(OBJ)/game/%.o: $(BUILD)/prep/%.c $(GAME_DEPS) $(BRIDGE_DEPS)
	$(GAME_COMPILE)

-include $(wildcard $(GAME_OBJS:%=%.d))

OBJS += $(GAME_OBJS)

# ------------------------------------------------------------- targets
.PHONY: game-objs extracted-asm game-dupcheck
game-objs: $(GAME_OBJS)
extracted-asm: $(EXTRACTED_ASM)

game-dupcheck: $(GAME_OBJS)
	@$(NM) -A --defined-only -g $(GAME_OBJS) 2>/dev/null | \
	 awk '{ t = $$(NF-1); n = $$NF; if (t != "W" && t != "V" && t != "w" && t != "v") c[n]++; } \
	      END { bad = 0; for (n in c) if (c[n] > 1) { print "duplicate strong definition: " n; bad = 1 } \
	            if (bad) exit 1; print "game objects: no duplicate strong definitions" }'

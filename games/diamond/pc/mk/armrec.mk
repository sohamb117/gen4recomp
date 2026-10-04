# pc/mk/armrec.mk: every ARM9 .s the ROM links, recompiled to C by armrec
# (games/platinum/tools/armrec/armrec.py) and compiled for wasm32.
#
#   make -f pc/Makefile.wasm armrec-objs       every armrec object (this slice)
#   make -f pc/Makefile.wasm armrec-classes    just $(ARMREC_CLASSES)
#
# Inputs are the ROM build's own ASM_DIRS (arm9/Makefile: asm, data, files,
# overlays/*/asm; data/ and files/ do not exist in this tree) plus the mwcc
# `asm` function bodies game.mk extracts from C into $(EXTRACTED_ASM).
# arm9/lib/syscall is deliberately not here: its .s are one-instruction SWI
# thunks (SVC_*), and the host layer defines every one of them in C
# (games/platinum/pc/src/pc_os_lite.c), so a recompiled `bl SVC_Div` is a
# boundary call like any other.
#
# armrec runs once over the whole set, because a .s alone does not say which
# call targets are recompiled and which are C: the symbol table is global.
# The outputs:
#   $(ARMREC_C)/<stem>.c      one per input; stem = basename unless two
#                             inputs share one (armrec's output_stems())
#   $(ARMREC_C)/armrec_init.c armrec_init_all() and armrec_overlay_data(id)
#   $(ARMREC_CLASSES)         F/D/B/X records for the bridge (bridge.mk)
#   $(ARMREC_OUT)/boundary.txt  calls that leave recompiled code, by callee
#   $(ARMREC_OUT)/report.txt    every construct armrec could not translate
# --wasm makes each boundary call a call to the bridge's c2u$NAME adapter
# and leaves armrec_externs.c / armrec_data_syms.c to the bridge.
#
# The generated C is compiled with plain $(CC): it calls nothing with a C
# prototype (the c2u$ adapters take armrec's uniform signature), so the IR
# rewrite has nothing to do there.

ARMREC_OUT     := $(BUILD)/armrec
ARMREC_C       := $(ARMREC_OUT)/c
ARMREC_OBJDIR  := $(OBJ)/armrec
ARMREC_CLASSES := $(ARMREC_OUT)/classes.txt
ARMREC_STAMP   := $(ARMREC_OUT)/armrec.stamp

ARMREC_S := $(sort $(wildcard $(ROOT)/arm9/asm/*.s)) \
            $(sort $(wildcard $(ROOT)/arm9/data/*.s)) \
            $(sort $(wildcard $(ROOT)/arm9/files/*.s)) \
            $(sort $(wildcard $(ROOT)/arm9/overlays/*/asm/*.s))
# armrec is given paths relative to $(ROOT) (it finds `.include "asm/..."`
# through the path's own arm9 component, as the ROM build's cwd does).
ARMREC_IN := $(patsubst $(ROOT)/%,%,$(ARMREC_S)) $(EXTRACTED_ASM)

# MSL functions game.mk compiles from C under a guest_ name (wasi-libc owns
# the bare one), and the host layer's replacements for asm functions.
ARMREC_GUEST_LIBC ?= abs,rand,srand
ARMREC_OVERRIDES  := $(wildcard $(PCDIR)/host_overrides.txt)

ifeq ($(GAME_VERSION),PEARL)
ARMREC_DEFS := --undef DIAMOND --define PEARL
else
ARMREC_DEFS := --define DIAMOND
endif

# --decomp-state is Platinum's table of which decompiled functions are
# Thumb; it says nothing true about Diamond, and on wasm a `.word` naming a
# C function is a table index that must not carry a Thumb bit anyway.
ARMREC_FLAGS := --wasm $(ARMREC_DEFS) --define ENGLISH \
                --include include --include arm9 --include . \
                --guest-libc $(ARMREC_GUEST_LIBC) \
                --decomp-state /dev/null --xmap $(ROM_XMAP) \
                $(if $(ARMREC_OVERRIDES),--host-override $(ARMREC_OVERRIDES))

# The stems armrec will write, from its own naming function.
ARMREC_STEMS := $(shell cd $(ROOT) && $(PYTHON) -c 'import sys; \
    sys.path.insert(0, "$(ARMREC)"); import armrec; \
    print(" ".join(armrec.output_stems(sys.argv[1:]).values()))' $(ARMREC_IN))
ARMREC_OBJS := $(addprefix $(ARMREC_OBJDIR)/,$(addsuffix .o,$(ARMREC_STEMS) armrec_init))

ARMREC_CFLAGS := $(WASM_ABI) $(OPT) -DPC_GAME_DP -I$(ARMREC)

OBJS += $(ARMREC_OBJS)
# Not built through BRIDGE_COMPILE (see the header).
BRIDGE_PLAIN_OBJS += $(ARMREC_OBJS)

.PHONY: armrec-objs armrec-classes
armrec-objs: $(ARMREC_OBJS)
armrec-classes: $(ARMREC_STAMP)

# Rewritten only when its content changes, so an armrec rerun that changes
# nothing the bridge reads does not rebuild every C translation unit.
$(ARMREC_CLASSES): $(ARMREC_STAMP) ;

# One armrec run writes every .c; a stamp stands for all of them (make 3.81
# has no grouped targets). --report and the stdout summary are the census.
$(ARMREC_STAMP): $(ARMREC_S) $(EXTRACTED_ASM) $(ARMREC)/armrec.py \
                 $(ROM_XMAP) $(ARMREC_OVERRIDES) \
                 $(PCDIR)/mk/armrec.mk
	@rm -rf $(ARMREC_C) && mkdir -p $(ARMREC_C)
	cd $(ROOT) && $(PYTHON) $(ARMREC)/armrec.py $(ARMREC_FLAGS) \
	    --out $(ARMREC_C) --classes $(ARMREC_CLASSES).tmp \
	    --boundary $(ARMREC_OUT)/boundary.txt \
	    --report $(ARMREC_OUT)/report.txt \
	    $(ARMREC_IN) > $(ARMREC_OUT)/summary.txt
	@cat $(ARMREC_OUT)/summary.txt
	cmp -s $(ARMREC_CLASSES).tmp $(ARMREC_CLASSES) \
	    && rm $(ARMREC_CLASSES).tmp \
	    || mv $(ARMREC_CLASSES).tmp $(ARMREC_CLASSES)
	@touch $@

$(ARMREC_C)/%.c: $(ARMREC_STAMP) ;

$(ARMREC_OBJDIR)/%.o: $(ARMREC_C)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ARMREC_CFLAGS) -c -o $@ $<

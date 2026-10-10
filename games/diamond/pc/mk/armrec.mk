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

ARMREC_S_PRISTINE := $(sort $(wildcard $(ROOT)/arm9/asm/*.s)) \
                     $(sort $(wildcard $(ROOT)/arm9/data/*.s)) \
                     $(sort $(wildcard $(ROOT)/arm9/files/*.s)) \
                     $(sort $(wildcard $(ROOT)/arm9/overlays/*/asm/*.s))

# Assembly patches: pc/patches/arm9/<p>.s.patch is applied to arm9/<p>.s
# (authored against the pristine file, like the .c patches game.mk applies)
# and armrec reads the copy at $(ARMREC_PATCHED)/arm9/<p>.s instead. The
# copy keeps an `arm9/overlays/NN/asm/` path component because that is how
# armrec knows which overlay a file belongs to (armrec.py overlay_of), and
# `.include` still resolves through --include arm9. A patch should stay
# size-neutral (replace a `bl X` with a `bl` to a host hook of the same
# shape): addresses are the ROM's, from the xMAP, and nothing in a patched
# function may need to move.
ARMREC_PATCHED  := $(ARMREC_OUT)/patched
ARMREC_SPATCHES := $(sort $(shell find $(PCDIR)/patches/arm9 -name '*.s.patch' 2>/dev/null))
ARMREC_SPATCHED_REL := $(patsubst $(PCDIR)/patches/arm9/%.patch,%,$(ARMREC_SPATCHES))
ARMREC_S := $(foreach f,$(ARMREC_S_PRISTINE),$(if $(filter $(patsubst $(ROOT)/arm9/%,%,$(f)),$(ARMREC_SPATCHED_REL)),$(ARMREC_PATCHED)/arm9/$(patsubst $(ROOT)/arm9/%,%,$(f)),$(f)))

$(ARMREC_PATCHED)/arm9/%.s: $(ROOT)/arm9/%.s $(PCDIR)/patches/arm9/%.s.patch
	@mkdir -p $(dir $@)
	@cp $< $@.tmp
	@patch --silent --forward $@.tmp $(PCDIR)/patches/arm9/$*.s.patch || \
	   { echo "pc/patches/arm9/$*.s.patch no longer applies" >&2; rm -f $@.tmp; exit 1; }
	@mv $@.tmp $@

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
# --overlay-dispatch: a call into another overlay at an address other
# overlays can occupy is dispatched by residency rather than bound by the
# name the disassembly gave it (armrec.py OVL_SYMS; the C side of the same
# class is pc/tools/dp_ovlabel_lint.py's).
ARMREC_FLAGS := --wasm $(ARMREC_DEFS) --define ENGLISH \
                --include include --include arm9 --include . \
                --guest-libc $(ARMREC_GUEST_LIBC) \
                --decomp-state /dev/null --xmap $(ROM_XMAP) --overlay-dispatch \
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
                 $(ROM_XMAP) $(ARMREC_OVERRIDES) $(ARMREC_SPATCHES) \
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

# Named, not a pattern: a .c reached only through a pattern rule is an
# intermediate file, which make deletes after the build that made it. The
# next parallel build then finds every armrec .c missing, and make 3.81
# loops forever over the armrec objects, each reported "The prerequisites
# ... are being made" with no job running (seen with armrec.stamp behind
# the FORCE'd .game-flags: -j4 no-op builds spun at full CPU for over half
# an hour with no child, where -j1 finished in seconds).
ARMREC_CS := $(addprefix $(ARMREC_C)/,$(addsuffix .c,$(ARMREC_STEMS) armrec_init))
$(ARMREC_CS): $(ARMREC_STAMP) ;

$(ARMREC_OBJDIR)/%.o: $(ARMREC_C)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ARMREC_CFLAGS) -c -o $@ $<

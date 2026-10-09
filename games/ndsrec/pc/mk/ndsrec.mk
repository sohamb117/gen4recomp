# pc/mk/ndsrec.mk: the ROM's ARM9 code, from the cartridge image to armrec
# objects. See pc/Makefile.wasm for the stages.
#
#   $(NDSREC_OUT)/sigdb.json        signatures learned from LEARN_ROM/XMAP
#   $(NDSREC_OUT)/primitives.syms   the primitive map for ROM
#   $(NDSREC_OUT)/asm/              ndsrec emit: arm9/asm/*.s and
#                                   arm9/overlays/<id>/asm/*.s (armrec reads
#                                   overlay membership from the path),
#                                   host_overrides.txt, emit.txt; then
#                                   pc/patches/$(VER)/ applied to it
#   $(NDSREC_OUT)/lcf.xmap          crt0's link values, in xMAP form, for
#                                   host.mk's launcher stack (ROM_XMAP)
#   $(ARMREC_C)/*.c, classes.txt    armrec --wasm over all of it
#
# Generated code stays in build/; the two reviewed BW caller snapshots are tracked.

# Assembly patches, per ROM: pc/patches/<VER>/<p>.s.patch is applied to the
# emitted <p>.s (paths from the assembly root, e.g. arm9/overlays/10/asm/
# ndsrec_ov010_002.s), as D/P's pc/patches/arm9/*.s.patch are to theirs:
# size-neutral, so every guest address and literal pool stays where the ROM
# has it (a `bl X` retargeted to host C, or an instruction pair replaced by
# `bl Hook; nop`). No fuzz: a patch whose context the emission no longer
# matches fails the build. pc/patches/<VER>/SHA256SUMS, when present, pins
# patched files to exact reviewed contents (`shasum -a 256 -c`, paths from
# the assembly root). A changed patch re-emits, so nothing is patched twice.
NDSREC_PATCH_DIR := $(MYPC)/patches/$(VER)
NDSREC_SPATCHES  := $(sort $(shell find $(NDSREC_PATCH_DIR) -name '*.s.patch' 2>/dev/null))
NDSREC_PATCH_SUMS := $(wildcard $(NDSREC_PATCH_DIR)/SHA256SUMS)

NDSREC_OUT     := $(BUILD)/ndsrec
NDSREC_ASM     := $(NDSREC_OUT)/asm
SIGDB          := $(NDSREC_OUT)/sigdb.json
PRIM_SYMS      := $(NDSREC_OUT)/primitives.syms
NDSREC_STAMP   := $(NDSREC_OUT)/asm.stamp
ROM_XMAP       := $(NDSREC_OUT)/lcf.xmap

ARMREC_OUT     := $(BUILD)/armrec
ARMREC_C       := $(ARMREC_OUT)/c
ARMREC_OBJDIR  := $(OBJ)/armrec
ARMREC_CLASSES := $(ARMREC_OUT)/classes.txt
ARMREC_STAMP   := $(ARMREC_OUT)/armrec.stamp

NDSREC_PY := $(wildcard $(NDSREC)/*.py)

$(SIGDB): $(LEARN_ROM) $(LEARN_XMAP) $(NDSREC)/primitives.txt $(NDSREC_PY)
	@mkdir -p $(dir $@)
	$(PYTHON) $(NDSREC)/sigs.py learn $(LEARN_ROM) $(LEARN_XMAP) \
	    $(NDSREC)/primitives.txt --out $@

$(PRIM_SYMS): $(ROM) $(SIGDB)
	$(PYTHON) $(NDSREC)/sigs.py match $(ROM) $(SIGDB) --out $@

$(ROM_XMAP): $(ROM) $(NDSREC_PY)
	@mkdir -p $(dir $@)
	$(PYTHON) $(NDSREC)/ndsrec.py lcf $(ROM) > $@.tmp && mv $@.tmp $@

$(NDSREC_STAMP): $(ROM) $(PRIM_SYMS) $(NDSREC_PY) $(NDSREC_SPATCHES) $(NDSREC_PATCH_SUMS)
	@rm -rf $(NDSREC_ASM)
	$(PYTHON) $(NDSREC)/ndsrec.py emit $(ROM) --symbols $(PRIM_SYMS) \
	    --out $(NDSREC_ASM)
	@for p in $(NDSREC_SPATCHES); do \
	   rel=$${p#$(NDSREC_PATCH_DIR)/}; rel=$${rel%.patch}; \
	   patch --silent --forward -F 0 $(NDSREC_ASM)/$$rel $$p || \
	     { echo "pc/patches/$(VER)/$$rel.patch no longer applies" >&2; exit 1; }; \
	 done
	$(if $(NDSREC_PATCH_SUMS),cd $(NDSREC_ASM) && shasum -a 256 -c --quiet $(NDSREC_PATCH_SUMS))
	@touch $@

# armrec over every file at once (the symbol table is global). Paths are
# relative to the assembly root so `arm9/overlays/<id>/asm/` names the
# overlay. No --xmap: every label carries its own address.
ARMREC_FLAGS := --wasm --decomp-state /dev/null \
                --host-override $(NDSREC_ASM)/host_overrides.txt

$(ARMREC_STAMP): $(NDSREC_STAMP) $(ARMREC)/armrec.py
	@rm -rf $(ARMREC_C) && mkdir -p $(ARMREC_C)
	cd $(NDSREC_ASM) && $(PYTHON) $(ARMREC)/armrec.py $(ARMREC_FLAGS) \
	    --out $(ARMREC_C) --classes $(ARMREC_CLASSES).tmp \
	    --boundary $(ARMREC_OUT)/boundary.txt \
	    --report $(ARMREC_OUT)/report.txt \
	    $$(cd $(NDSREC_ASM) && find arm9 -name '*.s' | sort) > $(ARMREC_OUT)/summary.txt
	@cat $(ARMREC_OUT)/summary.txt
	cmp -s $(ARMREC_CLASSES).tmp $(ARMREC_CLASSES) \
	    && rm $(ARMREC_CLASSES).tmp \
	    || mv $(ARMREC_CLASSES).tmp $(ARMREC_CLASSES)
	@touch $@

$(ARMREC_CLASSES): $(ARMREC_STAMP) ;

stage1: $(ARMREC_STAMP) $(ROM_XMAP)

# Stage 2 sees the generated C (make reads the directory at parse time).
ARMREC_OBJS := $(patsubst $(ARMREC_C)/%.c,$(ARMREC_OBJDIR)/%.o,$(wildcard $(ARMREC_C)/*.c))
ARMREC_CFLAGS := $(WASM_ABI) $(OPT) -DPC_GAME_DP -I$(ARMREC)

OBJS += $(ARMREC_OBJS)
BRIDGE_PLAIN_OBJS += $(ARMREC_OBJS)

$(ARMREC_OBJDIR)/%.o: $(ARMREC_C)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ARMREC_CFLAGS) -c -o $@ $<

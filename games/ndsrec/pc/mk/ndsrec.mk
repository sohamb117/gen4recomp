# pc/mk/ndsrec.mk: the ROM's ARM9 code, from the cartridge image to armrec
# objects. See pc/Makefile.wasm for the stages.
#
#   $(NDSREC_OUT)/sigdb.json        signatures learned from LEARN_ROM/XMAP
#   $(NDSREC_OUT)/primitives.syms   the primitive map for ROM
#   $(NDSREC_OUT)/asm/              ndsrec emit: arm9/asm/*.s and
#                                   arm9/overlays/<id>/asm/*.s (armrec reads
#                                   overlay membership from the path),
#                                   host_overrides.txt, emit.txt
#   $(NDSREC_OUT)/lcf.xmap          crt0's link values, in xMAP form, for
#                                   host.mk's launcher stack (ROM_XMAP)
#   $(ARMREC_C)/*.c, classes.txt    armrec --wasm over all of it
#
# Everything here is the ROM's code or data: build/ only, never committed.

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

$(NDSREC_STAMP): $(ROM) $(PRIM_SYMS) $(NDSREC_PY)
	@rm -rf $(NDSREC_ASM)
	$(PYTHON) $(NDSREC)/ndsrec.py emit $(ROM) --symbols $(PRIM_SYMS) \
	    --out $(NDSREC_ASM)
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

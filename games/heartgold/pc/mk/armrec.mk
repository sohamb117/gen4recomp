# pc/mk/armrec.mk: every ARM9 .s the HG/SS ROM links, recompiled to C by
# armrec (games/platinum/tools/armrec/armrec.py) and compiled for wasm32.
# games/diamond/pc/mk/armrec.mk is the model; read it first.
#
#   make -f pc/Makefile.wasm armrec-objs       every armrec object
#   make -f pc/Makefile.wasm armrec-classes    just $(ARMREC_CLASSES)
#
# Inputs: the assembly main.lsf links (pc/tools/hg_lsf.py: asm/, lib/asm,
# lib/{MSL_C,NitroDWC,NitroSDK}/asm), plus the mwcc `asm` bodies game.mk
# extracts from C. lib/syscall is not here, as on D/P: its SVC_* thunks
# are the host's C (pc_os_lite.c).
#
# Overlays. armrec learns a file's overlay from its path
# (arm9/overlays/NN/asm/, armrec.py overlay_of); pokeheartgold keeps every
# .s flat and says which overlay it belongs to only in main.lsf. Each
# overlay file is therefore staged at $(ARMREC_STAGE)/arm9/overlays/NN/asm/
# under its own basename (a symlink, or the patched copy), which keeps the
# xMAP's object name (the basename) and the include search (--include) as
# they are.
#
# Addresses: pokeheartgold's assembly names functions but carries no
# `; 0x...` comments, so every function and data label is placed from the
# ROM link's main.elf.xMAP (--xmap; armrec's place_from_xmap and the section
# origins parse_file reads).

ARMREC_OUT     := $(BUILD)/armrec
ARMREC_C       := $(ARMREC_OUT)/c
ARMREC_OBJDIR  := $(OBJ)/armrec
ARMREC_CLASSES := $(ARMREC_OUT)/classes.txt
ARMREC_STAMP   := $(ARMREC_OUT)/armrec.stamp
ARMREC_STAGE   := $(ARMREC_OUT)/stage

# `path ovl` pairs, ovl "-" for the static module and its autoloads.
ARMREC_LIST := $(shell $(PYTHON) $(HG_LSF) $(LSF) s | awk '{ print $$1 ":" $$2 }')
armrec_path = $(word 1,$(subst :, ,$(1)))
armrec_ovl  = $(word 2,$(subst :, ,$(1)))

# Assembly patches: pc/patches/<p>.s.patch applied to <p>.s (size-neutral,
# as on D/P: addresses are the xMAP's).
ARMREC_SPATCHES := $(sort $(shell find $(MYPC)/patches -name '*.s.patch' 2>/dev/null))
ARMREC_SPATCHED_REL := $(patsubst $(MYPC)/patches/%.patch,%,$(ARMREC_SPATCHES))

# Where armrec reads each file: the source itself (static, unpatched), or
# its staged copy/link.
armrec_staged = $(if $(filter -,$(call armrec_ovl,$(1))),$(ARMREC_STAGE)/$(call armrec_path,$(1)),$(ARMREC_STAGE)/arm9/overlays/$(call armrec_ovl,$(1))/asm/$(notdir $(call armrec_path,$(1))))
armrec_is_staged = $(or $(filter-out -,$(call armrec_ovl,$(1))),$(filter $(call armrec_path,$(1)),$(ARMREC_SPATCHED_REL)))
armrec_input = $(if $(call armrec_is_staged,$(1)),$(call armrec_staged,$(1)),$(ROOT)/$(call armrec_path,$(1)))
ARMREC_S := $(foreach e,$(ARMREC_LIST),$(call armrec_input,$(e)))
ARMREC_STAGED := $(foreach e,$(ARMREC_LIST),$(if $(call armrec_is_staged,$(e)),$(e)))

define ARMREC_STAGE_RULE
$(call armrec_staged,$(1)): $(ROOT)/$(call armrec_path,$(1)) $(wildcard $(MYPC)/patches/$(call armrec_path,$(1)).patch)
	@mkdir -p $$(dir $$@)
	@rm -f $$@ $$@.tmp
	@p=$(MYPC)/patches/$(call armrec_path,$(1)).patch; \
	 if [ -f $$$$p ]; then cp $$< $$@.tmp && \
	   { patch --silent --forward $$@.tmp $$$$p || \
	     { echo "pc/patches/$(call armrec_path,$(1)).patch no longer applies" >&2; rm -f $$@.tmp; exit 1; }; } && \
	   mv $$@.tmp $$@; \
	 else ln -s $$< $$@; fi
endef
$(foreach e,$(ARMREC_STAGED),$(eval $(call ARMREC_STAGE_RULE,$(e))))

ARMREC_IN := $(patsubst $(ROOT)/%,%,$(ARMREC_S)) $(EXTRACTED_ASM)

# The host layer's replacements for assembly functions: Platinum's SDK
# overrides the ROM-only core learned (tools/ndsrec/primitives.txt,
# `override`), as HG/SS names them; see the file.
ARMREC_OVERRIDES := $(MYPC)/host_overrides.txt
# MSL: no C file of MSL is linked (lib/MSL_C/src does not exist), so no
# function is compiled under a guest_ name.
ARMREC_GUEST_LIBC ?=

# MWASFLAGS: DEFINES (GF_DEFINES + GLB_DEFINES), -DSDK_ASM, and -DPM_ASM
# for asm/ (lib/ assembly takes no game define it would test). Include path
# in MWASFLAGS's order; include/ first, which is where armrec looks for
# config.h to force-include.
ARMREC_DEFS := --undef DIAMOND --define $(GAME_VERSION) \
               --define $(GAME_LANGUAGE) --define PM_KEEP_ASSERTS \
               --define SDK_ARM9 --define SDK_CODE_ARM --define SDK_FINALROM \
               --define SDK_ASM --define PM_ASM
ARMREC_INC := include . asm/include files lib/asm/include lib/NitroDWC/asm/include \
              lib/MSL_C/asm/include lib/NitroSDK/asm/include lib/syscall/asm/include \
              asm files/msgdata lib/include
ARMREC_FLAGS := --wasm $(ARMREC_DEFS) $(addprefix --include ,$(ARMREC_INC)) \
                $(if $(ARMREC_GUEST_LIBC),--guest-libc $(ARMREC_GUEST_LIBC)) \
                --decomp-state /dev/null --xmap $(ROM_XMAP) \
                --host-override $(ARMREC_OVERRIDES)

ARMREC_STEMS := $(shell cd $(ROOT) && $(PYTHON) -c 'import sys; \
    sys.path.insert(0, "$(ARMREC)"); import armrec; \
    print(" ".join(armrec.output_stems(sys.argv[1:]).values()))' $(ARMREC_IN))
ARMREC_OBJS := $(addprefix $(ARMREC_OBJDIR)/,$(addsuffix .o,$(ARMREC_STEMS) armrec_init))

ARMREC_CFLAGS := $(WASM_ABI) $(OPT) -DPC_GAME_DP -I$(ARMREC)

OBJS += $(ARMREC_OBJS)
BRIDGE_PLAIN_OBJS += $(ARMREC_OBJS)

.PHONY: armrec-objs armrec-classes armrec-stage
armrec-objs: $(ARMREC_OBJS)
armrec-classes: $(ARMREC_STAMP)
armrec-stage: $(ARMREC_S)

$(ARMREC_CLASSES): $(ARMREC_STAMP) ;

$(ARMREC_STAMP): $(ARMREC_S) $(EXTRACTED_ASM) $(ARMREC)/armrec.py \
                 $(ROM_XMAP) $(ARMREC_OVERRIDES) $(ARMREC_SPATCHES) \
                 $(MYPC)/mk/armrec.mk $(LSF)
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

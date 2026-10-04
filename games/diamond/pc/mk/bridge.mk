# pc/mk/bridge.mk: the typed C <-> recompiled bridge (IRBridge slice). See
# pc/Makefile.wasm and games/platinum/tools/armrec/{irbridge,gen_bridge}.py.
#
# wasm calls are typed, so decompiled C, host C and armrec's recompiled C
# (every function uint64_t f(u32 r0, u32 r1, u32 r2, u32 r3)) cannot call
# each other with mismatched prototypes the way i386 cdecl let them. Every C
# translation unit of the module is therefore compiled
#
#   clang -S -emit-llvm  ->  irbridge.py (rewrite against classes.txt)  ->
#   clang -c -x ir
#
# which turns a direct call to an asm function into a call to a generated
# marshalling wrapper aw$<sig>$F, an indirect call into ai$<sig> (native call
# for a C function pointer, armrec_dispatch for a guest address), and every
# other use of an asm function or data label into its guest address. Each TU
# leaves a record beside its object ($@.sigs), and gen_bridge.py turns the
# whole set into the C side: the wrappers, the c2u$ adapters recompiled code
# calls C through, the table-index -> adapter table armrec_dispatch uses for
# C function pointers, and armrec_ext_* / armrec_bind_externs().
#
# Contract with the other fragments:
#   $(call BRIDGE_COMPILE,<cflags>)  in a recipe with $< the .c and $@ the .o;
#                    the C->IR step gets <cflags> verbatim (so -MMD/-MF in it
#                    write the .d there), the IR->object step only its -O*.
#   $(BRIDGE_DEPS)   put in the prerequisites (after the .c) of every rule
#                    using BRIDGE_COMPILE: the rewrite reads classes.txt, so a
#                    fresh parallel build must order after it, and a changed
#                    classification must rebuild the TU.
#   BRIDGE_PLAIN_OBJS  every object in OBJS NOT built by BRIDGE_COMPILE
#                    (armrec's ARMREC_OBJS are excluded automatically); every
#                    other .o in OBJS must have its $@.sigs.
#
# Incremental under make 3.81: a TU's rewrite depends on the TU and
# classes.txt only; the generation reruns when any bridged object is newer
# than its stamp, and rewrites a generated .c only when its content changes,
# so the generated objects recompile only when the bridge really changed.

IRBRIDGE       := $(ARMREC)/irbridge.py
GEN_BRIDGE     := $(ARMREC)/gen_bridge.py
BRIDGE_CLASSES ?= $(BUILD)/armrec/classes.txt
BRIDGE_DEPS    := $(BRIDGE_CLASSES) $(IRBRIDGE)
BRIDGE_DIR     := $(BUILD)/bridge
BRIDGE_STAMP   := $(BRIDGE_DIR)/bridge.stamp
BRIDGE_GEN     := bridge_calls bridge_adapters bridge_externs
BRIDGE_GEN_C   := $(BRIDGE_GEN:%=$(BRIDGE_DIR)/%.c)
BRIDGE_GEN_OBJS := $(BRIDGE_GEN:%=$(BRIDGE_DIR)/%.o) $(BRIDGE_DIR)/armrec_bridge_wasm.o
BRIDGE_REPORT  := $(BRIDGE_DIR)/bridge_report.txt
BRIDGE_CFLAGS  ?= $(WASM_ABI) $(OPT) -DPC_GAME_DP -I$(ARMREC) \
                  -I$(PLAT)/pc/wasm/include -I$(NPROOT)/core/include

define BRIDGE_COMPILE
@mkdir -p $(dir $@)
$(CC) $(1) -S -emit-llvm -o $@.raw.ll $<
$(PYTHON) $(IRBRIDGE) --classes $(BRIDGE_CLASSES) $@.raw.ll $@.ll $@.sigs
$(CC) $(filter -O%,$(1)) -c -x ir -o $@ $@.ll
@rm -f $@.raw.ll $@.ll
endef

OBJS += $(BRIDGE_GEN_OBJS)
BRIDGE_PLAIN_OBJS += $(BRIDGE_GEN_OBJS)

# Deferred: every fragment has appended to OBJS by the time this expands
# (second expansion, enabled by the top makefile before the includes).
BRIDGE_OBJS = $(filter-out $(BRIDGE_PLAIN_OBJS) $(ARMREC_OBJS),$(filter %.o,$(OBJS)))

.PHONY: bridge-gen
bridge-gen: $(BRIDGE_STAMP)

# The object list goes through a file: a full module has hundreds of them.
$(BRIDGE_STAMP): $$(BRIDGE_OBJS) $(BRIDGE_CLASSES) $(GEN_BRIDGE)
	@mkdir -p $(BRIDGE_DIR)
	@rm -f $(BRIDGE_DIR)/sigs.list
	@$(foreach o,$(BRIDGE_OBJS),echo '$(o).sigs' >> $(BRIDGE_DIR)/sigs.list;)
	$(PYTHON) $(GEN_BRIDGE) --classes $(BRIDGE_CLASSES) --out $(BRIDGE_DIR) \
	    --sigs-list $(BRIDGE_DIR)/sigs.list
	@touch $@

# An empty recipe, so make re-reads the file's time after the stamp is
# remade and recompiles only what gen_bridge.py actually rewrote.
$(BRIDGE_GEN_C) $(BRIDGE_REPORT): $(BRIDGE_STAMP) ;

$(BRIDGE_DIR)/%.o: $(BRIDGE_DIR)/%.c $(ARMREC)/armrec_bridge.h
	$(CC) $(BRIDGE_CFLAGS) -c -o $@ $<

$(BRIDGE_DIR)/armrec_bridge_wasm.o: $(ARMREC)/armrec_bridge_wasm.c \
                                    $(ARMREC)/armrec_bridge.h $(ARMREC)/armrec_rt.h
	@mkdir -p $(dir $@)
	$(CC) $(BRIDGE_CFLAGS) -c -o $@ $<

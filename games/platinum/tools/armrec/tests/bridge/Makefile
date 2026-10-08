# The typed C <-> recompiled bridge, end to end, on a small module:
#
#   make -f games/platinum/tools/armrec/tests/bridge/Makefile [run]
#
# bridge_test.s goes through armrec.py --wasm (which also writes the
# classification), the C through Diamond's real BRIDGE_COMPILE and
# generation rules (games/diamond/pc/mk/bridge.mk), the runtime is the real
# armrec_rt.c built as Diamond builds it (-DPC_GAME_DP) plus the bridge
# runtime, and the module runs the way the real runtime runs a guest: wasm2c,
# compiled natively with a driver supplying the few WASI calls printf needs
# (run_w2c.c; the wabt wasm-interp here has no WASI, and node's crashes now
# and then on a 256 MiB initial memory). Outputs go to
# games/diamond/build/pc-wasm/bridgetest/.

HERE   := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
NPROOT := $(abspath $(HERE)/../../../../../..)
PLAT   := $(NPROOT)/games/platinum
ARMREC := $(PLAT)/tools/armrec
BUILD  := $(NPROOT)/games/diamond/build/pc-wasm/bridgetest

TOOLCHAINS ?= $(NPROOT)/.cache/toolchains
CC     := $(TOOLCHAINS)/wasi-sdk/bin/clang --target=wasm32-wasip1
WABT   := $(TOOLCHAINS)/wabt
PYTHON ?= python3
WASM_ABI := -fno-short-enums -fsigned-char -fno-common -std=gnu99 \
            -fms-extensions -fno-strict-aliasing -fwrapv -w
OPT    := -O2

NP_C_BASE := 0x0B000000
LDFLAGS := -Wl,--global-base=$(shell printf '%d' $(NP_C_BASE)) -Wl,--no-stack-first \
           -Wl,--initial-memory=268435456 -Wl,--max-memory=268435456 \
           -Wl,-z,stack-size=1048576

.DEFAULT_GOAL := run
.PHONY: run
.SECONDEXPANSION:

OBJS :=
BRIDGE_CLASSES := $(BUILD)/classes.txt
include $(NPROOT)/games/diamond/pc/mk/bridge.mk

# --- the assembly ----------------------------------------------------------
# bridge_msl.s has HG/SS msl.s's shape and is called from bridge_test.s.
ARMREC_OUT  := $(BUILD)/armrec
ARMREC_SRCS := bridge_test.s bridge_msl.s
ARMREC_OBJS := $(ARMREC_OUT)/bridge_test.o $(ARMREC_OUT)/bridge_msl.o \
               $(ARMREC_OUT)/armrec_init.o
OBJS += $(ARMREC_OBJS)

$(ARMREC_OUT)/stamp: $(addprefix $(HERE)/,$(ARMREC_SRCS)) $(ARMREC)/armrec.py
	@mkdir -p $(ARMREC_OUT)
	cd $(HERE) && $(PYTHON) $(ARMREC)/armrec.py --wasm --quiet \
	    --decomp-state /dev/null --out $(ARMREC_OUT) \
	    --classes $(BRIDGE_CLASSES).tmp $(ARMREC_SRCS)
	cmp -s $(BRIDGE_CLASSES).tmp $(BRIDGE_CLASSES) || mv $(BRIDGE_CLASSES).tmp $(BRIDGE_CLASSES)
	@touch $@
$(BRIDGE_CLASSES) $(ARMREC_OUT)/bridge_test.c $(ARMREC_OUT)/bridge_msl.c $(ARMREC_OUT)/armrec_init.c: $(ARMREC_OUT)/stamp ;

$(ARMREC_OUT)/%.o: $(ARMREC_OUT)/%.c
	$(CC) $(WASM_ABI) $(OPT) -DPC_GAME_DP -I$(ARMREC) -c -o $@ $<

# --- bridged C ---------------------------------------------------------------
TEST_CFLAGS := $(WASM_ABI) $(OPT) -DPC_GAME_DP -I$(ARMREC) \
               -I$(PLAT)/pc/wasm/include -I$(NPROOT)/core/include
C_OBJS := $(BUILD)/obj/bridge_main.o $(BUILD)/obj/bridge_c2.o \
          $(BUILD)/obj/test_rt.o $(BUILD)/obj/armrec_rt.o
OBJS += $(C_OBJS)

$(BUILD)/obj/%.o: $(HERE)/%.c $(BRIDGE_DEPS)
	$(call BRIDGE_COMPILE,$(TEST_CFLAGS))

$(BUILD)/obj/armrec_rt.o: $(ARMREC)/armrec_rt.c $(ARMREC)/armrec_rt.h $(BRIDGE_DEPS)
	$(call BRIDGE_COMPILE,$(TEST_CFLAGS))

# --- link and run --------------------------------------------------------------
$(BUILD)/bridgetest.wasm: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

W2C := $(BUILD)/w2c
$(W2C)/run: $(BUILD)/bridgetest.wasm $(HERE)/run_w2c.c
	@mkdir -p $(W2C)
	$(WABT)/bin/wasm2c $< -n bridgetest -o $(W2C)/bridgetest.c
	clang -O1 -w -I$(WABT)/include -I$(WABT)/share/wabt/wasm2c -I$(W2C) -o $@ \
	    $(W2C)/bridgetest.c $(WABT)/share/wabt/wasm2c/wasm-rt-impl.c \
	    $(WABT)/share/wabt/wasm2c/wasm-rt-mem-impl.c $(HERE)/run_w2c.c

run: $(W2C)/run
	$< > $(BUILD)/run.txt; s=$$?; cat $(BUILD)/run.txt; exit $$s
	@grep -q '^bridge test: \([0-9]*\)/\1 checks passed' $(BUILD)/run.txt

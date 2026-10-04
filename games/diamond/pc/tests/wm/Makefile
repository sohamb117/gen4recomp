# The WM model's unit test for Diamond/Pearl: games/platinum/pc/src/pc_wm.c
# and pc_pxi.c built as D builds them (PC_GAME_DP, the 4.2 host headers),
# driven by D's own ARM9 WM library: arm9/asm/WM_{system,standard,mp,sync,
# etc}.s as armrec recompiled them for the D module, so every command word
# and every record offset is the ROM's. Stations run under Platinum's
# harness (games/platinum/pc/tests/wm/run_wm_test.mjs; same exports).
#
#   make -C pc/tests/wm test                         (from games/diamond)
#   make -C pc/tests/wm test GAME_VERSION=PEARL
#
# Needs the recompiled C from a D guest build (build/pc-wasm/<ver>/armrec/c;
# `make -f pc/Makefile.wasm armrec-classes` writes it) and the ROM link's
# xMAP, which places D's WM command buffers.

DROOT   := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
NPROOT  := $(abspath $(DROOT)/../..)
PLAT    := $(NPROOT)/games/platinum
ARMREC  := $(PLAT)/tools/armrec
WASI_SDK ?= $(NPROOT)/.cache/toolchains/wasi-sdk
NODE    ?= node
SDK     := $(PLAT)/subprojects/NitroSDK-4.2.30001

GAME_VERSION ?= DIAMOND
ifeq ($(GAME_VERSION),PEARL)
VER := pearl
else
VER := diamond
endif
REC_C    := $(DROOT)/build/pc-wasm/$(VER)/armrec/c
ROM_XMAP := $(DROOT)/arm9/build/$(VER).us/arm9.elf.xMAP
OUT      := $(DROOT)/build/pc-wasm/wm-test-$(VER)

# D's ten 0x100-byte WM command buffers (WM_system.s .bss), from the link.
CMDBUF := $(shell awk '$$4 == "UNK_021D5EC0" { print "0x" $$1; exit }' $(ROM_XMAP) 2>/dev/null)
ifeq ($(CMDBUF),)
$(error UNK_021D5EC0 not found in $(ROM_XMAP); build the ROM first)
endif

CC := $(WASI_SDK)/bin/clang --target=wasm32-wasip1
# The host layer's flags and include order (pc/mk/host.mk): D's macro set
# over Platinum's NitroSDK 4.2 headers, fx_const.h from the guest build.
CFLAGS := -O2 -g -std=gnu99 -fms-extensions -fno-strict-aliasing -fwrapv -fno-common \
          -fno-short-enums -fsigned-char \
          -Wall -Wno-unknown-pragmas -Wno-unused-parameter \
          -include $(PLAT)/pc/include/pc_prelude.h \
          -I$(DROOT)/pc/include/host -I$(PLAT)/pc/wasm/include -I$(NPROOT)/core/include \
          -I$(PLAT)/pc/include -I$(DROOT)/build/pc-wasm/$(VER)/host/include -I$(SDK)/include \
          -I$(ARMREC) -I$(PLAT)/pc/src \
          -DPC_GAME_DP -DPLATFORM_PC -DSDK_CW_FORCE_EXPORT_SUPPORT -DSDK_TS -DSDK_4M -DSDK_ARM9 \
          -DSDK_CW -DSDK_FINALROM -DSDK_CODE_ARM -DNNS_FINALROM -D_NITRO -D_MSL_RESTRICT= \
          -DDP_WM_CMDBUF=$(CMDBUF)
# armrec's output compiles as pc/mk/armrec.mk compiles it.
REC_CFLAGS := -O2 -std=gnu99 -fno-short-enums -fsigned-char -fno-common -fwrapv -w \
              -DPC_GAME_DP -I$(ARMREC)
# The DS map (0x02000000-0x02800000: D's WM data and literals, the guest
# stack, system work at 0x027FFC00) lies below the C data, in a 64 MB memory.
LDFLAGS := -mexec-model=reactor -Wl,--global-base=50331648 -Wl,--no-stack-first \
           -Wl,--initial-memory=67108864 -Wl,--max-memory=67108864

REC_STEMS := WM_system WM_standard WM_mp WM_sync WM_etc
SRCS := $(DROOT)/pc/tests/wm/test_wm_dp.c $(DROOT)/pc/tests/wm/wm_dp_shim.c \
        $(PLAT)/pc/src/pc_wm.c $(PLAT)/pc/src/pc_pxi.c
OBJS := $(patsubst %.c,$(OUT)/%.o,$(notdir $(SRCS))) $(addprefix $(OUT)/rec_,$(addsuffix .o,$(REC_STEMS)))

vpath %.c $(sort $(dir $(SRCS)))

.PHONY: all test clean
all: $(OUT)/test_wm_dp.wasm

$(OUT)/rec_%.o: $(REC_C)/%.c | $(OUT)
	$(CC) $(REC_CFLAGS) -c -o $@ $<

$(OUT)/%.o: %.c $(ROM_XMAP) | $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/test_wm_dp.wasm: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(OUT):
	mkdir -p $@

test: $(OUT)/test_wm_dp.wasm
	$(NODE) $(PLAT)/pc/tests/wm/run_wm_test.mjs $<

clean:
	rm -rf $(OUT)

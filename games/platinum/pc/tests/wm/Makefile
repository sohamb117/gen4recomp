# The WM model's unit test: pc/src/pc_wm.c + pc/src/pc_pxi.c + the real
# NitroSDK ARM9 WM library, compiled for wasm32 exactly like the guest, run
# as several stations by run_wm_test.mjs (node).
#
#   make -C pc/tests/wm test        (from games/platinum)
#
# Needs build/pc-wasm/geninclude-wasm from a guest build (the prelude the
# guest compiles with); otherwise the SDK and the port's own headers.

WROOT   := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
NPROOT  := $(abspath $(WROOT)/../..)
WASI_SDK ?= $(NPROOT)/.cache/toolchains/wasi-sdk
NODE    ?= node
NP_INCLUDE ?= $(NPROOT)/core/include
SDK     := $(WROOT)/subprojects/NitroSDK-4.2.30001
OUT     := $(WROOT)/build/pc-wasm/wm-test

CC := $(WASI_SDK)/bin/clang --target=wasm32-wasip1
CFLAGS := -O2 -g -std=gnu99 -fms-extensions -fno-strict-aliasing -fwrapv -fno-common \
          -fno-short-enums -fsigned-char \
          -Wall -Wno-unknown-pragmas -Wno-unused-parameter \
          -I$(WROOT)/pc/wasm/include -I$(WROOT)/build/pc-wasm/geninclude-wasm -I$(NP_INCLUDE) \
          -include $(WROOT)/pc/wasm/include/pc_wasm_prelude.h \
          -include $(WROOT)/pc/include/pc_prelude.h \
          -I$(WROOT)/pc/include -I$(WROOT)/build/pc-wasm/geninclude -I$(WROOT)/include -I$(SDK)/include \
          -I$(WROOT)/pc/src -I$(SDK)/libraries/wm/include \
          -DSDK_CW_FORCE_EXPORT_SUPPORT -DSDK_TS -DSDK_4M -DSDK_ARM9 -DSDK_CW -DSDK_FINALROM \
          -DSDK_CODE_ARM -DNNS_FINALROM -D_NITRO -DPLATFORM_PC -D_MSL_RESTRICT=
# The DS map (system work at 0x027FFC00) lies below the C data, as in the
# guest link (core/cmake/NpGuestModule.cmake), in a smaller memory.
LDFLAGS := -mexec-model=reactor -Wl,--global-base=50331648 -Wl,--no-stack-first \
           -Wl,--initial-memory=67108864 -Wl,--max-memory=67108864

SRCS := $(WROOT)/pc/tests/wm/test_wm.c $(WROOT)/pc/tests/wm/wm_shim.c \
        $(WROOT)/pc/src/pc_wm.c $(WROOT)/pc/src/pc_pxi.c \
        $(SDK)/libraries/os/src/os_ownerInfo.c \
        $(addprefix $(SDK)/libraries/wm/src/,wm_system.c wm_standard.c wm_mp.c wm_sync.c wm_etc.c)
# SDK sources are compiled as the game compiles them: warnings are theirs.
SDK_SRCS := $(filter $(SDK)/%,$(SRCS))
OBJS := $(patsubst %.c,$(OUT)/%.o,$(notdir $(SRCS)))

vpath %.c $(sort $(dir $(SRCS)))

.PHONY: all test clean
all: $(OUT)/test_wm.wasm

$(OUT)/%.o: %.c | $(OUT)
	$(CC) $(CFLAGS) $(if $(filter $(SDK)/%,$(abspath $<)),-w) -c -o $@ $<

$(OUT)/test_wm.wasm: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(OUT):
	mkdir -p $@

test: $(OUT)/test_wm.wasm
	$(NODE) $(WROOT)/pc/tests/wm/run_wm_test.mjs $<

clean:
	rm -rf $(OUT)

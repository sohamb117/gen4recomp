# pc/mk/host.mk: the machine model, shared with Platinum, built for
# Diamond/Pearl. See pc/Makefile.wasm for the fragment contract.
#
#   make -f pc/Makefile.wasm host-objs        every host object (this slice)
#
# The host layer is Platinum's, compiled from its Platinum paths, never
# copied: games/platinum/pc/src (the SDK overrides, PXI responders, card,
# video, input, state), pc/hw (the 2D/3D/SPU models), pc/wasm/src (fibers,
# frame descriptor, fatal), tools/armrec/armrec_rt.c (guest memory,
# dispatch, overlays) and pc/arm7snd (the ARM7 sound driver, which is
# pokediamond's own arm7/lib SND code; see below). What D needs on top is
# pc/src here (pc_dp_*.c) and a handful of `#if defined(PC_GAME_DP)` lines in
# the shared files, each of which compiles to nothing for Platinum.
#
# HEADERS. Every host file compiles against Platinum's NitroSDK 4.2 headers,
# as it does for Platinum, not D's 3.2 ones: the host code is written to the
# 4.2 names, and what it shares with D's own compiled SDK was measured to lay
# out the same on wasm32 (both header sets, u64 at mwcc's 4-byte alignment
# as both builds' nitro/types.h shadows set it): OSContext (100 bytes; sp 56,
# lr 60, pc_plus4 64, sp_svc 68), OSThread (192), OSThreadInfo.current (4),
# OSThreadQueue (8), TPData (8), FSOverlayInfoHeader (ids 0..20) and
# FSOverlayInfo (44), OSSystemWork.real_time_clock (0x1E8), the PXI tags
# (RTC 5, TP 6, SOUND 7, PM 8, WM 10, FS 11, WVR 15; 32 tags), the RTC PXI
# word (command in bits 8-14, result bit 15, result in 0-7; D's
# arm9/asm/RTC_external.s RtcCommonCallback decodes the same), the card
# request codes 0..12, HW_ROM_HEADER_BUF and HW_BUTTON_XY_BUF. The
# disagreements are the card command block's chip spec, which
# include/host/pc_dp_card_common.h lays out the 3.2 way for pc_card_rom.c,
# and three points of the WM protocol, which pc_wm.c handles under
# PC_GAME_DP (its header has the field-by-field comparison).
# pc/include/host is searched first and holds only host-side shadows; D's
# game-side shadows (pc/include) are not on this path.
#
# THE SOUND DRIVER. pc/arm7snd is D's ARM7 sound driver: SND_alarm, bank,
# global, seq, util and work .c are byte-identical to D's arm7/lib/src, the
# others differ in 2-6 lines (register macros, the command-address floor,
# keyon), and all 23 shared headers are identical except mmap.h and
# registers.h. On D it is therefore the native pairing with the ARM9's SND
# command and shared-work layouts, not a port; it compiles exactly as
# Platinum's wasm build compiles it (raw, namespace map, renamed recompile,
# check_arm7ns.sh).
#
# Every C object here goes through BRIDGE_COMPILE (bridge.mk), except the
# sound driver (self-contained C reaching only its arm7_ shims and libc; no
# guest code address ever enters it) and the generated statics table, which
# are compiled plain and declared in BRIDGE_PLAIN_OBJS.

HOST_OBJ   := $(OBJ)/host
HOST_GEN   := $(BUILD)/host
HOST_NSDK  := $(PLAT)/subprojects/NitroSDK-4.2.30001
HOST_NSYS  := $(PLAT)/subprojects/NitroSystem-071126.1

HOST_DEFS := -DPC_GAME_DP -DPLATFORM_PC \
             -DSDK_CW_FORCE_EXPORT_SUPPORT -DSDK_TS -DSDK_4M -DSDK_ARM9 \
             -DSDK_CW -DSDK_FINALROM -DSDK_CODE_ARM -DNNS_FINALROM -D_NITRO \
             -D_MSL_RESTRICT=

HOST_INC := -I$(PCDIR)/include/host \
            -I$(PLAT)/pc/wasm/include \
            -I$(NPROOT)/core/include \
            -I$(PLAT)/pc/include \
            -I$(HOST_GEN)/include \
            -I$(HOST_NSDK)/include \
            -I$(HOST_NSYS)/include \
            -I$(ARMREC) \
            -I$(PLAT)/pc/src

HOST_CFLAGS := $(WASM_ABI) $(OPT) -include $(PLAT)/pc/include/pc_prelude.h \
               $(HOST_INC) $(HOST_DEFS)

# Per-object additions (target-specific below).
HOST_EXTRA :=

# ------------------------------------------------------------------ sources

# Not compiled for D, by file:
#   pc_win_fiber.c pc_win_ipc.c pc_win_clock.c   Windows halves (as on P wasm)
#   pc_os_context.c                     ucontext/mmap; pc/wasm/src replaces it
#   pc_lab.c pc_sprite_lab.c pc_text_lab.c pc_audio_lab.c pc_bgm_mute.c
#                                       drive Platinum's own game C
#   pc_probe2d.c                        reads Platinum's gSystem
#   pc_dwc_auth.c pc_dwc_connect.c      Platinum's DWC account and connect
#                                       Connect models; D runs its own
#                                       recompiled DWC
#   pc_dgt.c pc_crypto_rc4.c            replace Platinum-SDK DGT/RC4; D's DGT
#                                       is its own recompiled DGT_hash*.s, and
#                                       D links no CRYPTO_RC4
#   pc_selftest.c pc_div0.c             the self-tests of the two above and of
#                                       Platinum's ROM division routines
#   pc_boot_glue.c                      _start_AutoloadDoneCallback: D's is
#                                       crt0 asm, recompiled (overlay_13 takes
#                                       its address)
#   pc_np_field.c                       Platinum FieldSystem options; D's
#                                       field half is pc/game/pc_dp_field.c
# Their calls from the shared files are answered by src/pc_dp_hooks.c.
HOST_PC_EXCLUDE := pc_win_fiber.c pc_win_ipc.c pc_win_clock.c pc_os_context.c \
                   pc_lab.c pc_sprite_lab.c pc_text_lab.c pc_audio_lab.c \
                   pc_bgm_mute.c pc_probe2d.c pc_dwc_auth.c pc_dwc_connect.c \
                   pc_dgt.c pc_crypto_rc4.c pc_selftest.c pc_div0.c \
                   pc_boot_glue.c pc_np_field.c

HOST_PC_SRCS   := $(filter-out $(addprefix $(PLAT)/pc/src/,$(HOST_PC_EXCLUDE)), \
                    $(sort $(wildcard $(PLAT)/pc/src/*.c)))
HOST_HW_SRCS   := $(sort $(wildcard $(PLAT)/pc/hw/*.c))
HOST_WASM_SRCS := $(sort $(wildcard $(PLAT)/pc/wasm/src/*.c))
HOST_DP_SRCS   := $(sort $(wildcard $(PCDIR)/src/*.c))

HOST_OBJS := $(patsubst $(PLAT)/pc/src/%.c,$(HOST_OBJ)/pc/%.o,$(HOST_PC_SRCS)) \
             $(patsubst $(PLAT)/pc/hw/%.c,$(HOST_OBJ)/hw/%.o,$(HOST_HW_SRCS)) \
             $(patsubst $(PLAT)/pc/wasm/src/%.c,$(HOST_OBJ)/wasm/%.o,$(HOST_WASM_SRCS)) \
             $(patsubst $(PCDIR)/src/%.c,$(HOST_OBJ)/dp/%.o,$(HOST_DP_SRCS)) \
             $(HOST_OBJ)/armrec_rt.o

# ------------------------------------------------------- generated header

# nitro/fx/fx_const.h, which <nitro.h> includes: generated from the 4.2
# wrap's own table, as Platinum's pc/gen_headers.py does.
$(HOST_GEN)/include/nitro/fx/fx_const.h: $(HOST_NSDK)/gen/nitro/fx/gen_fx_const.py \
                                         $(HOST_NSDK)/gen/nitro/fx/fx_const.csv
	@mkdir -p $(dir $@)
	$(PYTHON) $(HOST_NSDK)/gen/nitro/fx/gen_fx_const.py \
	    $(HOST_NSDK)/gen/nitro/fx/fx_const.csv $@

# ---------------------------------------------------------------- objects

# Header dependencies come from the compiler (BRIDGE_COMPILE hands these
# flags to its C -> IR step verbatim); BRIDGE_DEPS orders every bridged
# compile after the classification it rewrites against.
HOST_DEPFLAGS = -MMD -MP -MF $@.d -MT $@
HOST_PRE := $(HOST_GEN)/include/nitro/fx/fx_const.h $(PCDIR)/mk/host.mk

$(HOST_OBJ)/pc/%.o: $(PLAT)/pc/src/%.c $(BRIDGE_DEPS) $(HOST_PRE)
	$(call BRIDGE_COMPILE,$(HOST_CFLAGS) $(HOST_EXTRA) $(HOST_DEPFLAGS))

$(HOST_OBJ)/hw/%.o: $(PLAT)/pc/hw/%.c $(BRIDGE_DEPS) $(HOST_PRE)
	$(call BRIDGE_COMPILE,$(HOST_CFLAGS) $(HOST_EXTRA) $(HOST_DEPFLAGS))

$(HOST_OBJ)/wasm/%.o: $(PLAT)/pc/wasm/src/%.c $(BRIDGE_DEPS) $(HOST_PRE)
	$(call BRIDGE_COMPILE,$(HOST_CFLAGS) $(HOST_EXTRA) $(HOST_DEPFLAGS))

$(HOST_OBJ)/dp/%.o: $(PCDIR)/src/%.c $(BRIDGE_DEPS) $(HOST_PRE)
	$(call BRIDGE_COMPILE,$(HOST_CFLAGS) $(HOST_EXTRA) $(HOST_DEPFLAGS))

$(HOST_OBJ)/armrec_rt.o: $(ARMREC)/armrec_rt.c $(BRIDGE_DEPS) $(HOST_PRE)
	$(call BRIDGE_COMPILE,$(HOST_CFLAGS) $(HOST_EXTRA) $(HOST_DEPFLAGS))

# pc/hw and pc_view.c at HW_OPT, as Platinum builds them (pc/Makefile: the
# rasterizer and the compose passes are where a frame's time goes). The
# last -O on the line wins.
$(patsubst $(PLAT)/pc/hw/%.c,$(HOST_OBJ)/hw/%.o,$(HOST_HW_SRCS)) \
$(HOST_OBJ)/pc/pc_view.o: HOST_EXTRA := $(HW_OPT)

# pc_os_lite.c defines __global_destructor_chain, the head of mwcc's
# static-destructor chain, for a Platinum link where nothing else does. In
# D the head is assembly data (arm9/asm/RUNTIME_CPLUS_StaticInitializers.s,
# 0x021D74C8), which the bridge hands every other C TU and the recompiled
# code by its guest address; a C definition beside it would be a second
# chain only this TU sees. Moved out of the way here, where it is dead.
$(HOST_OBJ)/pc/pc_os_lite.o: \
    HOST_EXTRA := -D__global_destructor_chain=pc_os_lite_unused_dtor_chain_dp

# The launcher thread's stack, from the ROM link's own values
# (src/pc_dp_boot.c). The xMAP lists linker-command-file symbols as
# `#>VALUE  NAME (linker command file)`.
host_xmap_value = $(shell awk '$$1 ~ /^\#>/ && $$2 == "$(1)" { print "0x" substr($$1, 3); exit }' $(ROM_XMAP) 2>/dev/null)
$(HOST_OBJ)/dp/pc_dp_boot.o: $(ROM_XMAP)
$(HOST_OBJ)/dp/pc_dp_boot.o: \
    HOST_EXTRA := -DPC_DP_SDK_AUTOLOAD_DTCM_START=$(call host_xmap_value,SDK_AUTOLOAD_DTCM_START) \
                  -DPC_DP_SDK_IRQ_STACKSIZE=$(call host_xmap_value,SDK_IRQ_STACKSIZE)

# ------------------------------------------------------- the sound driver

# Platinum's pc/Makefile(.wasm) recipe, unchanged in substance: compile raw
# (outside $(OBJ), never linked), collect every symbol the driver defines or
# references into a `#define sym arm7_sym` map (libc/compiler names and
# pc_spu_keyon_note kept), recompile under it, and check the result with
# Platinum's check_arm7ns.sh. The driver reaches the host only through the
# arm7_ shims in pc/src/pc_arm7snd.c.
HOST_SND7      := $(PLAT)/pc/arm7snd
HOST_SND7_SRCS := $(sort $(wildcard $(HOST_SND7)/src/*.c))
HOST_SND7_RAW  := $(patsubst $(HOST_SND7)/src/%.c,$(HOST_GEN)/arm7snd-raw/%.o,$(HOST_SND7_SRCS))
HOST_SND7_OBJS := $(patsubst $(HOST_SND7)/src/%.c,$(HOST_OBJ)/arm7snd/%.o,$(HOST_SND7_SRCS))
HOST_SND7_MAP  := $(HOST_GEN)/arm7snd-raw/ns.map
HOST_SND7_HDR  := $(HOST_GEN)/arm7snd-raw/ns.h
HOST_SND7_CFLAGS := $(WASM_ABI) $(OPT) -DPLATFORM_PC \
                    -I$(HOST_SND7)/include -I$(PLAT)/pc/include -I$(HOST_NSDK)/include
HOST_SND7_DEPS := $(PCDIR)/mk/host.mk

$(HOST_GEN)/arm7snd-raw/%.o: $(HOST_SND7)/src/%.c $(HOST_SND7_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(HOST_SND7_CFLAGS) -MMD -MP -MF $@.d -MT $@ -c -o $@ $<

$(HOST_SND7_MAP): $(HOST_SND7_RAW)
	@{ $(NM) --defined-only $(HOST_SND7_RAW) 2>/dev/null \
	     | awk '$$2 ~ /[ATDBRWVG]/ {print $$3}'; \
	   $(NM) -u $(HOST_SND7_RAW) 2>/dev/null | awk 'NF >= 2 {print $$2}'; } \
	  | sort -u \
	  | grep -vE '^(__.*|mem(cpy|set|move|cmp)|pc_spu_keyon_note)$$' \
	  | awk '{ print $$1 " arm7_" $$1 }' > $@.tmp && mv $@.tmp $@

$(HOST_SND7_HDR): $(HOST_SND7_MAP)
	@awk 'NF == 2 { print "#define " $$1 " " $$2 }' $< > $@.tmp && mv $@.tmp $@

$(HOST_OBJ)/arm7snd/%.o: $(HOST_SND7)/src/%.c $(HOST_SND7_HDR) $(HOST_SND7_DEPS) \
                         $(PLAT)/pc/wasm/check_arm7ns.sh
	@mkdir -p $(dir $@)
	$(CC) $(HOST_SND7_CFLAGS) -include $(HOST_SND7_HDR) -MMD -MP -MF $@.d -MT $@ \
	    -c -o $@.renamed $<
	@NM="$(NM)" sh $(PLAT)/pc/wasm/check_arm7ns.sh $@.renamed $(HOST_SND7_MAP)
	@mv $@.renamed $@

# ------------------------------------------------- overlay statics table

# pc_fs_overlay.c resets an overlay's writable C statics on every load
# after the first (on hardware the ROM copy is the reset; recompiled
# overlays get theirs from armrec_load_overlay). Platinum's generator, over
# D's overlay C objects: the map names each overlay TU the way the generator
# flattens an object path under $(OBJ)/game (game.mk puts
# arm9/overlays/NN/src/x.c at $(OBJ)/game/overlays/NN/src/x.o), with its
# overlay id, which is the directory number (arm9.lsf declares OVERLAY_00
# .. OVERLAY_86 in order). --addr-table: wasm has no symbol table at run
# time, so the addresses are written into the finished module after the
# link by Platinum's pc/wasm/patch_ov_addrs.py (the integrator's POST_LINK:
# patch_ov_addrs.py $(TARGET) $(WASM_MAP) $(HOST_OVSTATICS_C)).
HOST_OV_SRCS      := $(sort $(wildcard $(ROOT)/arm9/overlays/*/src/*.c))
HOST_OVMAP        := $(HOST_GEN)/overlay-map.txt
HOST_OVSTATICS_C  := $(HOST_GEN)/overlay_statics.c
HOST_OVSTATICS_O  := $(HOST_OBJ)/overlay_statics.o

host_ov_rel = $(patsubst $(ROOT)/arm9/%,%,$(1))
$(HOST_OVMAP): $(PCDIR)/mk/host.mk
	@mkdir -p $(dir $@)
	@printf '%s\n' $(foreach f,$(HOST_OV_SRCS),dp/$(subst /,_,$(call host_ov_rel,$(f))):$(word 2,$(subst /, ,$(call host_ov_rel,$(f))))) \
	  | tr ':' ' ' > $@.tmp && mv $@.tmp $@

$(HOST_OVSTATICS_C): $(HOST_OVMAP) $(PLAT)/pc/gen_overlay_statics.py $(GAME_OBJS)
	@mkdir -p $(dir $@)
	$(PYTHON) $(PLAT)/pc/gen_overlay_statics.py --addr-table $(HOST_OVMAP) $(OBJ)/game $@

$(HOST_OVSTATICS_O): $(HOST_OVSTATICS_C)
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c -o $@ $<

# ------------------------------------------------------------------ link

OBJS += $(HOST_OBJS) $(HOST_SND7_OBJS)
BRIDGE_PLAIN_OBJS += $(HOST_SND7_OBJS) $(HOST_OVSTATICS_O)
# Outside OBJS: it is generated from the game objects, which are in OBJS.
LINK_EXTRA_DEPS += $(HOST_OVSTATICS_O)
LINK_EXTRA_OBJS += $(HOST_OVSTATICS_O)

.PHONY: host-objs
host-objs: $(HOST_OBJS) $(HOST_SND7_OBJS)

-include $(addsuffix .d,$(HOST_OBJS) $(HOST_SND7_RAW) $(HOST_SND7_OBJS))

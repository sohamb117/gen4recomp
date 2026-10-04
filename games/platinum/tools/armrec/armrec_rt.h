/*
 * armrec runtime: guest ARM9 state and the memory model for recompiled code.
 *
 * The DS address space is identity mapped into the host process, so guest
 * address 0x02000000 lives at host address 0x02000000. A guest pointer and a
 * host pointer are then the same value, and recompiled code and hand-written
 * C pass pointers to each other for free. See armrec_mem_init().
 */

#ifndef ARMREC_RT_H
#define ARMREC_RT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Guest memory                                                       */
/* ------------------------------------------------------------------ */

/* DS ARM9 memory map regions we reserve. */
#define ARM_MAIN_RAM_BASE 0x02000000u
#define ARM_MAIN_RAM_SIZE 0x00400000u /* 4 MB */
#define ARM_SHARED_BASE   0x027E0000u
#define ARM_SHARED_SIZE   0x00020000u
#define ARM_WRAM_BASE     0x03000000u
#define ARM_WRAM_SIZE     0x00010000u
#define ARM_IO_BASE       0x04000000u
#define ARM_IO_SIZE       0x00100000u
#define ARM_PALETTE_BASE  0x05000000u
#define ARM_PALETTE_SIZE  0x00001000u
/*
 * The one region the console does not have.
 *
 * 0x02800000 to 0x03000000 is a main-RAM mirror on a DS and this port maps
 * none of it. The port needs a little guest-addressable memory of its own:
 * A host object's address does not fit the FIFO's 26-bit data field, and
 * SOUNDxSAD is 27 bits, so a wave in the port's own BSS is unreachable.
 * pc/src/pc_guest_window.c hands it out. Only part of the hole is mapped, so
 * a stray access elsewhere in it still faults.
 *
 * The ELF provides it rather than mmap, because some of what lives here is a
 * link-time object. pc/Makefile's PC_GUEST_BSS names the objects the linker
 * places here, and armrec_mem_init() must not map the region or an mmap would
 * replace the game's own statics with zeroed pages. Windows and wasm cannot
 * link-place anything there, so on both it is ordinary memory and only
 * pc_guest_window.c's run-time half exists.
 */
#define ARM_PORT_WINDOW_BASE 0x02A00000u
#define ARM_PORT_WINDOW_SIZE 0x00200000u /* 2 MB */
/* The name armrec_region_at() reports it under. pc_guest_window.c finds the
 * region by this name rather than repeating the address, so the base is
 * written down exactly once. */
#define ARMREC_PORT_WINDOW_NAME "port window"
/*
 * VRAM is not one region and not plain memory.
 *
 * Nine banks totalling 0xA4000 appear in five CPU-visible windows, wherever
 * VRAMCNT puts them, each window mirroring its contents to fill its slot. A
 * bank is in at most one window at a time, so the windows below are guest
 * addresses; the storage behind them is armrec_vram_bank_ptr()'s. The
 * per-window sizes are the addressable content, so a mirror is not hashed
 * several times over.
 */
#define ARM_VRAM_BASE     0x06000000u
#define ARM_VRAM_END      0x07000000u
#define ARM_VRAM_SIZE     (ARM_VRAM_END - ARM_VRAM_BASE)
#define ARM_VRAM_BLK      0x00004000u /* the mapping granularity, 16 KB */
#define ARM_VRAM_BANKS    9
#define ARM_VRAM_STORE    0x000A4000u /* = HW_LCDC_VRAM_SIZE; the nine banks */
#define ARM_OAM_BASE      0x07000000u
#define ARM_OAM_SIZE      0x00001000u
#define ARM_ITCM_BASE     0x01FF8000u
#define ARM_ITCM_SIZE     0x00008000u
/*
 * The ARM7's own memory.
 *
 * One region rather than two, because the two are contiguous and the ARM7's
 * linker script spans both: 0x037F8000 to 0x03800000 is the last mirror of
 * the 32 KB shared WRAM, where the ARM7's autoload section is placed, and
 * 0x03800000 to 0x03810000 is its private 64 KB IWRAM.
 *
 * Not aliased to the ARM9's WRAM at 0x03000000. On hardware the 32 KB is one
 * memory and WRAMCNT says which processor sees it; here the two are separate
 * storage, which is only right while the ARM9 does not use its window. It
 * does not: the six ARM9 literals that look like addresses in that range are
 * all PXI command payloads, and nothing writes WRAMCNT.
 */
#define ARM_ARM7_WRAM_BASE 0x037F8000u
#define ARM_ARM7_WRAM_SIZE 0x00018000u

/* Emulated stack lives at the top of main RAM, as on hardware. */
#define ARM_STACK_TOP 0x023E0000u

/*
 * The first I/O page is per-processor.
 *
 * 0x04000000 to 0x04000FFF is separate hardware on the two CPUs: separate
 * IME, IE, IF, four timers and four DMA channels, all at the same addresses.
 * armrec_cpu_switch() saves the page the outgoing processor was using and
 * restores the incoming one's. 4 KB is the span any per-CPU register was
 * found in; everything above it is one mapping.
 *
 * `pcdiff-melon --io-selftest` measures this. 83 halfwords are private and
 * writable, 1,963 are read-only or absent, and exactly two are visible
 * across, IPCSYNC and EXMEMCNT. Neither is shared storage, so neither is in
 * the mirror list below.
 */
#define ARM_IO_PERCPU_SIZE 0x00001000u
#define ARMREC_CPU_ARM9 0
#define ARMREC_CPU_ARM7 1

/* Which processor's I/O page, stack pointer and CPSR are live. */
extern int armrec_cpu;

/*
 * Make `cpu` the running processor: save the outgoing one's I/O page, stack
 * pointer and CPSR, copy the mirrored registers across, and install the
 * incoming one's. Returns 0, or -1 with armrec_mem_strerror() set. A switch to
 * the processor already running is a no-op and costs nothing. Measured at
 * 107 ns.
 */
int armrec_cpu_switch(int cpu);

/* The `cpu`'s I/O page: the live one at 0x04000000 if it is running, its saved
 * copy if it is not, so the host hardware model can reach the processor that
 * is not. NULL before armrec_mem_init(). */
void *armrec_io_page(int cpu);

/* The mirrored registers: halfwords both processors read the same value for,
 * copied to the incoming page on every switch. i in [0, armrec_io_mirror_count).
 */
int armrec_io_mirror_count(void);
uint32_t armrec_io_mirror_at(int i, const char **name);

/*
 * Reserve and zero the guest address space. Returns 0 on success, negative on
 * failure, which normally means the host has already mapped something in the
 * low address range. See armrec_mem_strerror().
 */
/*
 * A host stack at a fixed address; armrec_rt.c says why the port does not run
 * on the one the kernel gives it. Returns the low end of `size` bytes with an
 * unmapped guard page below, or NULL when the arena is exhausted. Nothing
 * frees one. Every host stack the port executes on comes from here, because a
 * pointer to a decompiled caller's local reaches guest memory, and guest
 * memory does not move. On wasm32 it is a 16-byte-aligned heap block with no
 * guard; armrec_rt.c says how much of the reason survives there.
 */
void *armrec_host_stack(size_t size);

int armrec_mem_init(void);
void armrec_mem_free(void);
const char *armrec_mem_strerror(void);

/* True once armrec_mem_init() has succeeded. */
extern int armrec_mem_ready;

/*
 * Walk the regions armrec_mem_init() maps. This is the one place that knows
 * what guest memory is, and two things outside it need that: pc_state.c
 * digests every region to compare one run against another, and a memory diff
 * against an emulator needs the same walk. Exposing the table beats copying
 * it, because a copy goes stale exactly when a region is added.
 *
 * Returns the number of regions; armrec_region_at() fills whichever of its
 * out-parameters are non-NULL and returns 0 for an index out of range.
 */
int armrec_region_count(void);
int armrec_region_at(int i, uint32_t *base, uint32_t *size, const char **name);

/* ------------------------------------------------------------------ */
/* VRAM bank mapping                                                  */
/* ------------------------------------------------------------------ */

/*
 * The one place guest memory is not a fixed mapping. VRAMCNT decides which of
 * the nine banks is reachable through which of the five windows, so the same
 * 16 KB can be at 0x06800000 one instant and 0x06000000 the next.
 *
 * Done by aliasing rather than indirection, because decompiled C reaches VRAM
 * through a plain u16* and there is no accessor to put a bank lookup behind.
 * The nine banks live in one shared backing object and each window is an mmap
 * of the slices VRAMCNT selects. 16 KB is a whole number of host pages, which
 * is what lets this be mmap rather than a software MMU.
 *
 * armrec_vram_touch() is the whole interface: "VRAMCNT may have changed". It
 * has to run after the store, which is why the decompiled side hooks function
 * exit: a macro cannot run code after an assignment through it.
 */
void armrec_vram_touch(void);

/* Host pointer to a bank's storage, 0 <= bank < 9. NULL before init. */
void *armrec_vram_bank_ptr(int bank);

/* A bank's size in bytes, and where LCDC puts it. Both from the sweep. */
uint32_t armrec_vram_bank_size(int bank);
uint32_t armrec_vram_bank_lcdc(int bank);

/* The guest address of bank b's VRAMCNT register. Not nine consecutive bytes:
 * WRAMCNT sits at 0x04000247, between G and H. */
uint32_t armrec_vram_cnt_addr(int bank);

/* How many times armrec_vram_touch() has actually remapped. For tests. */
unsigned long armrec_vram_remaps(void);

/*
 * The frame's render, bracketed. On POSIX these are no-ops, since every VRAM
 * view is the same memory by mmap. On Windows the 64 KB mapping granularity,
 * and on wasm32 the absence of any mapping, force a copy model (see
 * vram_copy() in armrec_rt.c), and these are where CPU writes reach the bank
 * store and capture writes reach the windows.
 */
void armrec_vram_render_begin(void);
void armrec_vram_render_end(void);

/*
 * Which bank, if any, guest address `a` currently reads, and at what offset
 * into it. Returns 0 when nothing is mapped there. A read of such an address
 * gives zero, as on hardware, and a write to it is refused loudly rather than
 * dropped.
 */
int armrec_vram_lookup(uint32_t a, int *bank, uint32_t *off);

/*
 * The two roles VRAMCNT can put a bank in that have no address in the ARM9's
 * map, and that a renderer therefore cannot reach through a window.
 *
 * armrec_vram_extpal() returns the 8 KB extended-palette slot of one of the
 * four palette regions, or NULL when no bank is mapped there, which a caller
 * must read as "all zeros" rather than "do not draw". Slots are 0 to 3 for
 * the two BG regions and 0 for the two OBJ ones. armrec_vram_bank_in_lcdc()
 * answers DISPCNT's VRAM display mode, which scans a bank out directly and
 * shows black if it is not in LCDC.
 */
enum { ARMREC_EXTPAL_ABG, ARMREC_EXTPAL_BBG, ARMREC_EXTPAL_AOBJ,
       ARMREC_EXTPAL_BOBJ };
void *armrec_vram_extpal(int which, int slot);
int armrec_vram_bank_in_lcdc(int bank);

/*
 * And the two the 3D engine reads. armrec_vram_texture() returns one of the
 * four 128 KB texture slots and armrec_vram_texpal() one of the eight 16 KB
 * texture-palette slots. NULL means no bank is mapped there and reads as
 * zeros. Slots 6 and 7 of the palette space have no bank that can reach them.
 */
#define ARMREC_TEX_SLOT_SIZE    0x20000u   /* 128 KB, four of them */
#define ARMREC_TEXPAL_SLOT_SIZE 0x4000u    /* 16 KB, eight of them */
void *armrec_vram_texture(int slot);
void *armrec_vram_texpal(int slot);

/* The VRAMCNT window: nine registers over 0x04000240-0x04000249, with
 * WRAMCNT at 0x247 inside it. A store anywhere in it is a possible remap. */
#define ARM_VRAM_CNT_BASE 0x04000240u
#define ARM_VRAM_CNT_SIZE 0x0Au
#define ARM_VRAM_CNT_HIT(a) \
    (__builtin_expect((uint32_t)((a) - ARM_VRAM_CNT_BASE) < ARM_VRAM_CNT_SIZE, 0))

/* ------------------------------------------------------------------ */
/* Memory accessors                                                   */
/* ------------------------------------------------------------------ */

/*
 * The one place guest memory is not memory: the maths coprocessor's control
 * and result registers, 0x04000280 to 0x040002B7. See armrec_cp_ptr() in
 * armrec_rt.c for the model. Everything else is a plain dereference.
 *
 * The window is one contiguous compare, and it is laid out so it can be:
 * DIVCNT (0x280), the divider's operands (0x290, 0x298, read through),
 * DIV_RESULT (0x2A0), DIVREM_RESULT (0x2A8), SQRTCNT (0x2B0) and SQRT_RESULT
 * (0x2B4). SQRT_PARAM at 0x2B8 is storage and stays outside.
 */
#define ARM_CP_READ_BASE 0x04000280u
#define ARM_CP_READ_SIZE 0x38u

uint32_t armrec_cp_read32(uint32_t a);
uint32_t armrec_cp_read16(uint32_t a);
uint32_t armrec_cp_read8(uint32_t a);

/*
 * The completion point, and the one function both halves of the port go
 * through. `addr` is a register in the block above that is about to be
 * touched. Whatever the unit owning it would produce from the operands
 * currently in memory is computed and stored at its real guest address, and a
 * plain pointer to that address comes back.
 *
 * A pointer rather than a value, because `reg_CP_DIVCNT = 0;` is a write
 * through a macro and cannot become a call, and three sites take the address
 * of a result register.
 *
 * The value is stored into the register rather than a private shadow, so a
 * caller keeping the pointer across a second division does not get the first
 * division's answer.
 */
void *armrec_cp_ptr(uint32_t addr);

#define ARM_CP_HIT(a) \
    (__builtin_expect((uint32_t)((a) - ARM_CP_READ_BASE) < ARM_CP_READ_SIZE, 0))

/*
 * ARMREC_CP_HOOK is per file, and that is measured rather than cautious.
 * Putting the window test on every recompiled load costs 3.2x the text, 625 KB
 * against 197 KB over thirty translation units. It is not the call: the same
 * test with `return 0` costs 574 KB, so what is expensive is turning every
 * load into a two-way branch. armrec.py defines this only for the files whose
 * assembly names an address in the window, which is 13 of 549.
 *
 * Sound because ARM cannot build 0x040002A0 with an immediate, so a function
 * that reads a result register has the address in its own literal pool or was
 * handed it. pc/tests/test_cp_div.c checks that no assembly file reaches the
 * window off a base it names some other way.
 */
#define ARM_HOSTPTR(a) ((void *)(uintptr_t)(uint32_t)(a))

/*
 * The second place guest memory is not memory: the geometry engine's command
 * ports and result registers. See pc/include/pc_gpu3d.h for the model.
 *
 * Two windows, because they are two different claims. 0x04000400 to
 * 0x040005CB is a FIFO: each store pushes a command or a parameter, so the
 * last store must not simply win. The hook is on the store and there is
 * nothing to read there. 0x04000600 to 0x040006A3 is the status and result
 * block, computed rather than stored, so it is hooked on the load as well.
 *
 * Per file for the coprocessor's reason. 0x04000440 can also be reached off a
 * base, so test_gpu3d walks every .s for both shapes and for the C call sites
 * that hand a port address to a function.
 */
#define ARM_GX_PORT_BASE 0x04000400u
#define ARM_GX_PORT_SIZE 0x1CCu          /* 0x0400 .. 0x05CB */
#define ARM_GX_REG_BASE  0x04000600u
#define ARM_GX_REG_SIZE  0x0A4u          /* 0x0600 .. 0x06A3 */

#define ARM_GX_PORT_HIT(a) \
    (__builtin_expect((uint32_t)((a) - ARM_GX_PORT_BASE) < ARM_GX_PORT_SIZE, 0))
#define ARM_GX_REG_HIT(a) \
    (__builtin_expect((uint32_t)((a) - ARM_GX_REG_BASE) < ARM_GX_REG_SIZE, 0))

void armrec_gx_store(uint32_t a, uint32_t v, int size);
uint32_t armrec_gx_load(uint32_t a, int size);

/*
 * A channel keyon, named by the ARM7 sound driver's own start sites;
 * see arm7/lib/include/registers.h (reg_SOUNDxCNT_KEYON) for why the SPU
 * latch cannot recover this event from register state alone. Weak no-op
 * default in armrec_rt.c; pc/hw/pc_spu.c has the real one.
 */
void pc_spu_keyon_note(int idx);

/*
 * The decompiled-C entry points, and the shape is forced rather than chosen.
 * `reg_G3_MTX_TRANS = x;` is an assignment through a macro and C gives no way
 * to run code after one, so registers.h hands back a staging word and the
 * command is committed by the next access of any kind.
 *
 * The staging area is 32 words, not one, because a caller may hand the port's
 * address to a block copy: G3_MultMtx33 sends nine words through MI_Copy36B.
 *
 * armrec_gx_reg() is the read side. It refreshes the whole status and result
 * block in guest memory before handing back a plain pointer into it, so a
 * block read of the clip matrix needs no hook of its own.
 */
uint32_t *armrec_gx_port(uint32_t addr);
void *armrec_gx_reg(uint32_t addr);

/*
 * The third place guest memory is not memory: the IPC block that connects the
 * two processors. See pc/include/pc_ipc.h for the model.
 *
 * Four registers and not one is storage. IPCSYNC (0x04000180) crosses: a
 * processor's output nibble is the other's input nibble, so a plain 16-bit
 * store also wipes the half the other processor owns. IPCFIFOCNT (0x04000184)
 * has a write-1-to-clear error bit and a write-only clear bit. IPCFIFOSEND
 * (0x04000188) is a push and IPCFIFORECV (0x04100000) is a pop.
 *
 * Two windows because they are a megabyte apart. Per file for the
 * coprocessor's reason; test_ipc re-derives that set two ways.
 */
#define ARM_IPC_REG_BASE  0x04000180u
#define ARM_IPC_REG_SIZE  0x0Cu          /* SYNC, FIFOCNT, FIFOSEND */
#define ARM_IPC_RECV_BASE 0x04100000u
#define ARM_IPC_RECV_SIZE 0x04u

#define ARM_IPC_HIT(a)                                                        \
    (__builtin_expect((uint32_t)((a) - ARM_IPC_REG_BASE) < ARM_IPC_REG_SIZE   \
                      || (uint32_t)((a) - ARM_IPC_RECV_BASE)                  \
                             < ARM_IPC_RECV_SIZE, 0))

void armrec_ipc_store(uint32_t a, uint32_t v, int size);
uint32_t armrec_ipc_load(uint32_t a, int size);

/*
 * The fourth place guest memory is not memory: the ARM7's SPI bus. See
 * pc/include/pc_spi.h for the model.
 *
 * Two registers, neither of them storage. SPICNT (0x040001C0) selects one of
 * three chips and releases its chip select as a side effect when the enable
 * bit is dropped, so a plain store loses what separates one transaction from
 * the next. SPIDATA (0x040001C2) is a shift register: a read gives the chip's
 * reply to the previous write.
 *
 * One window, four bytes wide. Per file for the coprocessor's reason, and the
 * set is four files, all arm7/asm/, because the ARM9 has no SPI bus.
 */
#define ARM_SPI_BASE 0x040001C0u
#define ARM_SPI_SIZE 0x04u               /* SPICNT and SPIDATA */

#define ARM_SPI_HIT(a)                                                        \
    (__builtin_expect((uint32_t)((a) - ARM_SPI_BASE) < ARM_SPI_SIZE, 0))

void armrec_spi_store(uint32_t a, uint32_t v, int size);
uint32_t armrec_spi_load(uint32_t a, int size);

/*
 * The fifth: the GBA slot's backup bus, 0x0A000000 to 0x0A00FFFF. With a
 * flash chip in the slot a store there is a command, not a store, and a load
 * in ID mode answers the chip's ID; pc/src/pc_agb_slot.c models the chip and
 * with the slot empty does the plain access. The bus is 8 bits wide and the
 * SDK reaches it only with ldrb/strb, so only the byte accesses are hooked.
 * Per file for the coprocessor's reason: the four CTRDG_flash_*.s of
 * Diamond/Pearl's recompiled SDK.
 */
#define ARM_AGB_BASE 0x0A000000u
#define ARM_AGB_SIZE 0x00010000u

#define ARM_AGB_HIT(a)                                                        \
    (__builtin_expect((uint32_t)((a) - ARM_AGB_BASE) < ARM_AGB_SIZE, 0))

void armrec_agb_store8(uint32_t a, uint32_t v);
uint32_t armrec_agb_load8(uint32_t a);

#ifdef ARMREC_CHECKED_MEM
uint32_t armrec_ld32(uint32_t a);
uint32_t armrec_ld16(uint32_t a);
uint32_t armrec_ld8(uint32_t a);
void armrec_st32(uint32_t a, uint32_t v);
void armrec_st16(uint32_t a, uint32_t v);
void armrec_st8(uint32_t a, uint32_t v);
#define ARM_LD32(a)    armrec_ld32(a)
#define ARM_LD16(a)    armrec_ld16(a)
#define ARM_LD8(a)     armrec_ld8(a)
#define ARM_ST32(a, v) armrec_st32((a), (v))
#define ARM_ST16(a, v) armrec_st16((a), (v))
#define ARM_ST8(a, v)  armrec_st8((a), (v))
#else
/*
 * Loads and stores are hooked independently, because the two hooks are for
 * different windows and a file can need one, both or neither. Inline functions
 * rather than macros wherever the address is evaluated twice (once for the
 * window test): generated code hands these expressions like `r4 + 0x18`, and
 * a macro would re-evaluate them.
 */
#if defined(ARMREC_CP_HOOK) || defined(ARMREC_GX_HOOK) || defined(ARMREC_IPC_HOOK) \
    || defined(ARMREC_SPI_HOOK) || defined(ARMREC_AGB_HOOK)
static inline uint32_t ARM_LD32(uint32_t a) {
#ifdef ARMREC_CP_HOOK
    if (ARM_CP_HIT(a)) return armrec_cp_read32(a);
#endif
#ifdef ARMREC_GX_HOOK
    if (ARM_GX_REG_HIT(a)) return armrec_gx_load(a, 4);
#endif
#ifdef ARMREC_IPC_HOOK
    if (ARM_IPC_HIT(a)) return armrec_ipc_load(a, 4);
#endif
#ifdef ARMREC_SPI_HOOK
    if (ARM_SPI_HIT(a)) return armrec_spi_load(a, 4);
#endif
    return *(uint32_t *)ARM_HOSTPTR(a);
}
static inline uint32_t ARM_LD16(uint32_t a) {
#ifdef ARMREC_CP_HOOK
    if (ARM_CP_HIT(a)) return armrec_cp_read16(a);
#endif
#ifdef ARMREC_GX_HOOK
    if (ARM_GX_REG_HIT(a)) return armrec_gx_load(a, 2);
#endif
#ifdef ARMREC_IPC_HOOK
    if (ARM_IPC_HIT(a)) return armrec_ipc_load(a, 2);
#endif
#ifdef ARMREC_SPI_HOOK
    if (ARM_SPI_HIT(a)) return armrec_spi_load(a, 2);
#endif
    return *(uint16_t *)ARM_HOSTPTR(a);
}
static inline uint32_t ARM_LD8(uint32_t a) {
#ifdef ARMREC_CP_HOOK
    if (ARM_CP_HIT(a)) return armrec_cp_read8(a);
#endif
#ifdef ARMREC_GX_HOOK
    if (ARM_GX_REG_HIT(a)) return armrec_gx_load(a, 1);
#endif
#ifdef ARMREC_IPC_HOOK
    if (ARM_IPC_HIT(a)) return armrec_ipc_load(a, 1);
#endif
#ifdef ARMREC_SPI_HOOK
    if (ARM_SPI_HIT(a)) return armrec_spi_load(a, 1);
#endif
#ifdef ARMREC_AGB_HOOK
    if (ARM_AGB_HIT(a)) return armrec_agb_load8(a);
#endif
    return *(uint8_t *)ARM_HOSTPTR(a);
}
#else
/* Identity mapping makes these plain dereferences, which is the whole point. */
#define ARM_LD32(a)    (*(uint32_t *)ARM_HOSTPTR(a))
#define ARM_LD16(a)    ((uint32_t) * (uint16_t *)ARM_HOSTPTR(a))
#define ARM_LD8(a)     ((uint32_t) * (uint8_t *)ARM_HOSTPTR(a))
#endif

#if defined(ARMREC_VRAM_HOOK) || defined(ARMREC_GX_HOOK) || defined(ARMREC_IPC_HOOK) \
    || defined(ARMREC_SPI_HOOK) || defined(ARMREC_AGB_HOOK)
/*
 * A store into 0x04000240 to 0x04000249 remaps VRAM, so it has to be seen.
 * The hook is per file for the same reason the CP one is, and it is on the
 * store because that is what changes the mapping.
 *
 * A store into the geometry engine's windows is the other case, and it comes
 * first because it does not fall through to memory at all: a FIFO push is not
 * a store, and letting it also land at the register's address would leave the
 * last word of every display list sitting in the I/O page.
 */
static inline void ARM_ST32(uint32_t a, uint32_t v) {
#ifdef ARMREC_GX_HOOK
    if (ARM_GX_PORT_HIT(a) || ARM_GX_REG_HIT(a)) { armrec_gx_store(a, v, 4); return; }
#endif
#ifdef ARMREC_IPC_HOOK
    if (ARM_IPC_HIT(a)) { armrec_ipc_store(a, v, 4); return; }
#endif
#ifdef ARMREC_SPI_HOOK
    if (ARM_SPI_HIT(a)) { armrec_spi_store(a, v, 4); return; }
#endif
    *(uint32_t *)ARM_HOSTPTR(a) = v;
#ifdef ARMREC_VRAM_HOOK
    if (ARM_VRAM_CNT_HIT(a) || ARM_VRAM_CNT_HIT(a + 3)) armrec_vram_touch();
#endif
}
static inline void ARM_ST16(uint32_t a, uint32_t v) {
#ifdef ARMREC_GX_HOOK
    if (ARM_GX_PORT_HIT(a) || ARM_GX_REG_HIT(a)) { armrec_gx_store(a, v, 2); return; }
#endif
#ifdef ARMREC_IPC_HOOK
    if (ARM_IPC_HIT(a)) { armrec_ipc_store(a, v, 2); return; }
#endif
#ifdef ARMREC_SPI_HOOK
    if (ARM_SPI_HIT(a)) { armrec_spi_store(a, v, 2); return; }
#endif
    *(uint16_t *)ARM_HOSTPTR(a) = (uint16_t)v;
#ifdef ARMREC_VRAM_HOOK
    if (ARM_VRAM_CNT_HIT(a) || ARM_VRAM_CNT_HIT(a + 1)) armrec_vram_touch();
#endif
}
static inline void ARM_ST8(uint32_t a, uint32_t v) {
#ifdef ARMREC_GX_HOOK
    if (ARM_GX_PORT_HIT(a) || ARM_GX_REG_HIT(a)) { armrec_gx_store(a, v, 1); return; }
#endif
#ifdef ARMREC_IPC_HOOK
    if (ARM_IPC_HIT(a)) { armrec_ipc_store(a, v, 1); return; }
#endif
#ifdef ARMREC_SPI_HOOK
    if (ARM_SPI_HIT(a)) { armrec_spi_store(a, v, 1); return; }
#endif
#ifdef ARMREC_AGB_HOOK
    if (ARM_AGB_HIT(a)) { armrec_agb_store8(a, v); return; }
#endif
    *(uint8_t *)ARM_HOSTPTR(a) = (uint8_t)v;
#ifdef ARMREC_VRAM_HOOK
    if (ARM_VRAM_CNT_HIT(a)) armrec_vram_touch();
#endif
}
#else
#define ARM_ST32(a, v) (*(uint32_t *)ARM_HOSTPTR(a) = (uint32_t)(v))
#define ARM_ST16(a, v) (*(uint16_t *)ARM_HOSTPTR(a) = (uint16_t)(v))
#define ARM_ST8(a, v)  (*(uint8_t *)ARM_HOSTPTR(a) = (uint8_t)(v))
#endif
#endif

/* Signed loads sign-extend into a 32-bit register. */
#define ARM_LD16S(a) ((uint32_t)(int32_t)(int16_t)ARM_LD16(a))
#define ARM_LD8S(a)  ((uint32_t)(int32_t)(int8_t)ARM_LD8(a))

/* ------------------------------------------------------------------ */
/* Stack pointer                                                      */
/* ------------------------------------------------------------------ */

/*
 * r13 is global: recompiled functions share one emulated stack, exactly as on
 * hardware. r0 to r12 and lr are per-function C locals.
 */
extern uint32_t armrec_sp;
#define ARM_SP armrec_sp

/* ------------------------------------------------------------------ */
/* Flag helpers                                                       */
/* ------------------------------------------------------------------ */

#define ARM_NZ(nf, zf, x)          \
    do {                           \
        uint32_t _t = (uint32_t)(x); \
        (nf) = _t >> 31;           \
        (zf) = (_t == 0);          \
    } while (0)

/* Carry out of an unsigned add; overflow out of a signed add. */
#define ARM_ADD_C(a, b, r) ((uint32_t)(r) < (uint32_t)(a))
#define ARM_ADD_V(a, b, r) ((((uint32_t)(a) ^ (uint32_t)(r)) & ((uint32_t)(b) ^ (uint32_t)(r))) >> 31)
/* ARM subtract sets C = NOT borrow, i.e. C = (a >= b) unsigned. */
#define ARM_SUB_C(a, b, r) ((uint32_t)(a) >= (uint32_t)(b))
#define ARM_SUB_V(a, b, r) ((((uint32_t)(a) ^ (uint32_t)(b)) & ((uint32_t)(a) ^ (uint32_t)(r))) >> 31)

/* Rotate right, the ARM barrel shifter primitive. */
static inline uint32_t arm_ror(uint32_t v, uint32_t n) {
    n &= 31;
    return n ? ((v >> n) | (v << (32 - n))) : v;
}

/* Register-amount shifts use the full 8-bit amount, so >=32 is meaningful. */
static inline uint32_t arm_lsl_r(uint32_t v, uint32_t n) { return n >= 32 ? 0 : v << n; }
static inline uint32_t arm_lsr_r(uint32_t v, uint32_t n) { return n >= 32 ? 0 : v >> n; }
static inline uint32_t arm_asr_r(uint32_t v, uint32_t n) {
    return n >= 32 ? (uint32_t)((int32_t)v >> 31) : (uint32_t)((int32_t)v >> n);
}
static inline uint32_t arm_ror_r(uint32_t v, uint32_t n) { return arm_ror(v, n & 31); }

/* Shifter carry-out for register-amount shifts (0 amount leaves C alone; the
 * generated code handles that case by not emitting an update). */
static inline uint32_t arm_lsl_rc(uint32_t v, uint32_t n, uint32_t oldc) {
    if (n == 0) return oldc;
    if (n > 32) return 0;
    return (v >> (32 - n)) & 1;
}
static inline uint32_t arm_lsr_rc(uint32_t v, uint32_t n, uint32_t oldc) {
    if (n == 0) return oldc;
    if (n > 32) return 0;
    return (v >> (n - 1)) & 1;
}
static inline uint32_t arm_asr_rc(uint32_t v, uint32_t n, uint32_t oldc) {
    if (n == 0) return oldc;
    if (n >= 32) return v >> 31;
    return (v >> (n - 1)) & 1;
}

/* Count leading zeros (clz, ARMv5). */
static inline uint32_t arm_clz(uint32_t v) {
    uint32_t n = 0;
    if (v == 0) return 32;
    while (!(v & 0x80000000u)) { v <<= 1; n++; }
    return n;
}

/* ------------------------------------------------------------------ */
/* Control transfer                                                   */
/* ------------------------------------------------------------------ */

/*
 * Recompiled functions all have this signature. The return value packs r0 in
 * the low half and r1 in the high half so 64-bit ARM returns survive.
 */
typedef uint64_t (*armrec_fn)(uint32_t, uint32_t, uint32_t, uint32_t);

/*
 * Recompiled functions are emitted under their real symbol names, so the
 * linker wires up all three directions for free:
 *
 *   recompiled -> recompiled     direct C call
 *   decompiled C -> recompiled   the C prototype binds to the recompiled body
 *   recompiled -> decompiled C   the call binds to the hand-written C
 *
 * Decompiling a function is then just deleting its .s file and rebuilding.
 * AAPCS passes the first four integer or pointer arguments in r0 to r3 and
 * returns in r0 and r1, matching these four parameters and the packed return
 * value. Arguments past the fourth need ARMREC_CALL_EXT below.
 */
#define ARM_RETURN()  return (((uint64_t)r1 << 32) | (uint64_t)r0)
/* Not every function touches every register; keep -Wunused quiet. */
#define ARMREC_UNUSED()                                                       \
    do {                                                                      \
        (void)a0; (void)a1; (void)a2; (void)a3;                               \
        (void)r0; (void)r1; (void)r2; (void)r3; (void)r4; (void)r5;           \
        (void)r6; (void)r7; (void)r8; (void)r9; (void)r10; (void)r11;         \
        (void)r12; (void)lr;                                                  \
        (void)nf; (void)zf; (void)cf; (void)vf;                               \
    } while (0)
#define ARMREC_CALL(f, a0, a1, a2, a3) (f)((a0), (a1), (a2), (a3))
#define ARM_TAILCALL(f, a0, a1, a2, a3) return (f)((a0), (a1), (a2), (a3))
/*
 * The link register has a value, and it is this.
 *
 * A recompiled function returns by returning: `bx lr` and `pop {pc}` become
 * ARM_RETURN(), and the guest's lr is never consulted. That works until a
 * function saves lr and comes back through a different register:
 *
 *     push {r4, r5, r6, lr}
 *     ...
 *     pop  {r4, r5, r6}
 *     pop  {r3}            ; the saved lr, now in r3
 *     bx   r3              ; ...and this is the return
 *
 * Nothing about `bx r3` says "return", so armrec translates it as the
 * indirect branch it is, and lr started at 0. Initialising lr to a value that
 * cannot be a code address makes a branch to it a return, and the value
 * travels through the guest stack like any other, so it survives spills and
 * reloads. Bit 0 is the Thumb bit on a `bx`, so the comparison masks it.
 */
#define ARMREC_LR_SENTINEL 0xFFFFFFF0u
#define ARM_TAILCALL_IND(x, a0, a1, a2, a3)                                   \
    do {                                                                      \
        uint32_t armrec_tc_ = (uint32_t)(x);                                  \
        if ((armrec_tc_ & ~1u) == ARMREC_LR_SENTINEL) ARM_RETURN();           \
        return armrec_dispatch(armrec_tc_, (a0), (a1), (a2), (a3));           \
    } while (0)

/* ------------------------------------------------------------------ */
/* The C boundary: arguments past the fourth                          */
/* ------------------------------------------------------------------ */

/*
 * ARM passes arguments 1 to 4 in r0 to r3 and the rest on the stack. A
 * recompiled function's stack is armrec_sp; a decompiled C callee reads the
 * host stack. So the four parameters above deliver a call into decompiled C
 * correctly only while it has four parameters or fewer.
 *
 * The fix rests on the port being 32-bit. i386 System V passes every argument
 * on the stack as a sequence of 4-byte words, and for the types this tree
 * uses that sequence is word for word the sequence ARM builds, once ARM's
 * first four words are taken out of the registers. So a host argument list of
 * r0, r1, r2, r3, [sp+0], [sp+4], ... hands the callee exactly the words the
 * ARM caller built, and the callee's own declaration decides how many it
 * reads.
 *
 * i386 is cdecl, so passing more words than the callee declares is harmless.
 * ARMREC_EXT_STACK_WORDS is a ceiling rather than a count, and
 * test_abi_boundary re-derives the widest callee from DWARF and fails if it
 * exceeds this. Do not lower it without moving that test.
 */
#define ARMREC_EXT_STACK_WORDS 16

typedef uint64_t (*armrec_extfn)(uint32_t, uint32_t, uint32_t, uint32_t,
                                 uint32_t, uint32_t, uint32_t, uint32_t,
                                 uint32_t, uint32_t, uint32_t, uint32_t,
                                 uint32_t, uint32_t, uint32_t, uint32_t,
                                 uint32_t, uint32_t, uint32_t, uint32_t);

#define ARMREC_STACK_ARGS                                                     \
    ARM_LD32(ARM_SP +  0u), ARM_LD32(ARM_SP +  4u),                           \
    ARM_LD32(ARM_SP +  8u), ARM_LD32(ARM_SP + 12u),                           \
    ARM_LD32(ARM_SP + 16u), ARM_LD32(ARM_SP + 20u),                           \
    ARM_LD32(ARM_SP + 24u), ARM_LD32(ARM_SP + 28u),                           \
    ARM_LD32(ARM_SP + 32u), ARM_LD32(ARM_SP + 36u),                           \
    ARM_LD32(ARM_SP + 40u), ARM_LD32(ARM_SP + 44u),                           \
    ARM_LD32(ARM_SP + 48u), ARM_LD32(ARM_SP + 52u),                           \
    ARM_LD32(ARM_SP + 56u), ARM_LD32(ARM_SP + 60u)

/*
 * -DARMREC_NO_STACK_ARGS puts the defect back, and it is not a debugging
 * switch: test_abi_boundary builds the same generated code both ways and
 * requires the arguments to be wrong without it. A test written against a bug
 * that is already fixed passes either way.
 */
#ifdef ARMREC_NO_STACK_ARGS
#define ARMREC_CALL_EXT(f, a0, a1, a2, a3) ARMREC_CALL(f, a0, a1, a2, a3)
#else
#define ARMREC_CALL_EXT(f, a0, a1, a2, a3)                                    \
    ((armrec_extfn)(f))((a0), (a1), (a2), (a3), ARMREC_STACK_ARGS)
#endif
#define ARM_TAILCALL_EXT(f, a0, a1, a2, a3)                                   \
    return ARMREC_CALL_EXT(f, a0, a1, a2, a3)

/* ------------------------------------------------------------------ */
/* The same boundary in the other direction                           */
/* ------------------------------------------------------------------ */

/*
 * Decompiled C calling a recompiled function with more than four argument
 * words pushes them on the host stack, where the recompiled body cannot see
 * them. The defect this closes: a seven-word call to OS_SetPeriodicAlarm
 * arrived with words six and seven zeroed, so the alarm that wakes the sound
 * thread was armed with a NULL callback.
 *
 * icall_thunk.py rewrites a direct `call NAME` to a recompiled function into
 * `call armrec_stk_NAME`, a generated trampoline that is one line each:
 *
 *     uint64_t armrec_stk_F(a0..a3, s0..s15)
 *     { return armrec_stkargs_call((armrec_extfn)F, a0..a3, s0..s15); }
 *
 * It needs no arity. i386 cdecl lets a caller pass more words than the callee
 * reads, so this reads a fixed ARMREC_EXT_STACK_WORDS past the fourth and
 * stores all of them below the emulated stack pointer. It forwards the words
 * as well, because the name may resolve to a decompiled definition instead.
 * The 64 bytes go below ARM_SP and ARM_SP is put back, which is what an ARM
 * caller's outgoing-argument area is.
 */
#define ARMREC_STK_PARAMS                                                     \
    uint32_t s0, uint32_t s1, uint32_t s2, uint32_t s3,                       \
    uint32_t s4, uint32_t s5, uint32_t s6, uint32_t s7,                       \
    uint32_t s8, uint32_t s9, uint32_t s10, uint32_t s11,                     \
    uint32_t s12, uint32_t s13, uint32_t s14, uint32_t s15
#define ARMREC_STK_FORWARD                                                    \
    s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, s12, s13, s14, s15

uint64_t armrec_stkargs_call(armrec_extfn fn, uint32_t a0, uint32_t a1,
                             uint32_t a2, uint32_t a3, ARMREC_STK_PARAMS);

/*
 * A call that has not returned still has its words below the emulated stack
 * pointer, and the ARM7 is parked inside one for most of the run. These put
 * them back and re-apply them, and they nest, so anything reading guest
 * memory as state brackets itself with the pair. pc_state_digest_span() is
 * the one place that does it.
 *
 * Diagnostics that read guest memory as memory, such as --watch, deliberately
 * do not: they are for looking at what is actually there.
 */
void armrec_stkargs_settle(void);
void armrec_stkargs_resume(void);
int armrec_stkargs_live_count(void);

/*
 * A callee whose ABI the word sequence above cannot express, named by
 * armrec.py --abi-trap. There is exactly one today: a decompiled C function
 * returning a struct larger than a register. i386 gives it a hidden result
 * pointer that the callee pops with `ret $4`, so the recompiled caller's own
 * stack cleanup then runs four bytes short and corrupts its frame. Trapping
 * is right because the call site is real but nothing exercises it yet.
 */
void armrec_abi_trap(const char *name, const char *why);
#define ARMREC_CALL_ABI_TRAP(name, why) \
    (armrec_abi_trap((name), (why)), (uint64_t)0)

/* Register a guest address to recompiled function mapping, for code that is
 * always resident. Two different functions at one address is a hard error:
 * silently keeping the first is what hid the overlay collision for the life
 * of this project. */
void armrec_register(uint32_t addr, armrec_fn fn, const char *name);

/*
 * Register every decompiled function whose name encodes a guest address.
 *
 * Generated by tools/armrec/gen_decomp_syms.py from the link's own symbols
 * and called by pc_main.c after armrec_init_all(). Decompiling a function
 * deletes the .s that said where it lived, so without this nothing reaching
 * it through a stored guest address resolves.
 *
 * Weakly defined as a no-op in armrec_rt.c, because most tests link the
 * runtime without the generated file.
 */
void armrec_register_decompiled(void);
extern const int armrec_decomp_sym_count;

/* ------------------------------------------------------------------ */
/* Overlays: a guest address means whatever is resident there          */
/* ------------------------------------------------------------------ */

/*
 * 87 overlays share 11 load addresses, 33 of them starting at 0x021D74E0,
 * because on hardware only one occupies a window at a time. A guest address
 * inside a window therefore identifies one function per claimant, and which
 * is meant depends on what is loaded.
 *
 * armrec_register_overlay() reserves a slot owned by `ovl`. The slot exists
 * from startup so a dispatch to a non-resident address can say who owns it.
 * Nothing is ever removed: residency is one byte in an array beside it, so
 * loading cannot disturb the probe chain.
 *
 * armrec_load_overlay() takes the window from its caller, because the ROM's
 * overlay table is the one thing that knows it. Loading evicts every resident
 * overlay whose live range intersects the incoming one, which is the game's
 * own CanOverlayBeLoaded() test. It is an interval relation, not a set of
 * windows: 116 pairs at different load addresses intersect.
 */
#define ARMREC_MAX_OVERLAYS 128

void armrec_register_overlay(uint32_t addr, armrec_fn fn, const char *name,
                             int ovl);

/* Make overlay `id` resident over [ram_address, ram_address + total_size),
 * evicting whatever it overlaps, and write its static data into the window. */
void armrec_load_overlay(int id, uint32_t ram_address, uint32_t total_size);

/* Drop overlay `id`. Its addresses then abort by name instead of resolving to
 * code whose data is gone. */
void armrec_unload_overlay(int id);

int armrec_overlay_resident(int id);

/*
 * The generated data half, one call per overlay (armrec.py emits it into
 * armrec_init.c). Weak here so the runtime links on its own, in which case an
 * overlay becomes resident and simply has no data to write.
 */
void armrec_overlay_data(int id);

/* Copy a static data blob into guest memory at load time. */
void armrec_load_data(uint32_t addr, const void *src, uint32_t len);

/* Synthetic guest addresses for symbols that live in hand-decompiled C, so
 * that recompiled code can still take their address and call through it. */
uint32_t armrec_extern_addr(const char *name, void *fn);

/* Look up and call by guest address (BX/BLX to a register, function pointers
 * loaded out of data tables, etc.). Traps if the address is unknown. */
uint64_t armrec_dispatch(uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);

#if defined(__wasm__) && defined(PC_GAME_DP)
/* Diamond/Pearl on wasm32: values below ARMREC_WASM_FNPTR_END are C function
 * pointers (table indices), resolved through the bridge's adapter table;
 * armrec_bridge.h. */
#include "armrec_bridge.h"
#endif

/* True if `addr` falls inside one of the DS regions armrec_mem_init() maps. */
int armrec_is_guest_addr(uint32_t addr);

/* True if `addr` is inside this program's own loaded text, i.e. is a host
 * function pointer rather than a guest address. Identity mapping makes the two
 * indistinguishable by value, so the linker's own bounds are what settle it. */
int armrec_is_host_text(uint32_t addr);

/* Is this whole span inside the port's own loaded image: text, rodata and
 * initialised data? pc/src/pc_dma.c is the caller, for a DMA source that is a
 * decompiled `static const`. */
int armrec_host_data_span_ok(uint32_t addr, uint32_t len);

/*
 * True if the whole of [addr, addr+len) lies inside one mapped region.
 *
 * Two calls to armrec_is_guest_addr() on the ends is not the same claim and
 * would pass on a span that straddles a hole, and there is a 4 MB one between
 * main RAM's end and the shared work area. Anything that walks a range rather
 * than touching one address wants this.
 *
 * A zero length is true anywhere from a region's base to its end inclusive;
 * callers that care about the address itself pass a length.
 */
int armrec_guest_span_ok(uint32_t addr, uint32_t len);

/*
 * Call a code address that may be either a guest address or a host function
 * pointer, and say which it was if it is neither.
 *
 * armrec_dispatch() answers the first case only. The second arises wherever
 * decompiled C hands a function pointer to something that will later call it
 * through a stored word, such as OS_CreateThread's `func` argument. A
 * decompiled C function's address is a host address, so identity mapping
 * makes it directly callable; a recompiled function's guest address is not
 * its host address and only the table knows the mapping.
 */
uint64_t armrec_call_code(uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2,
                          uint32_t a3);

/*
 * The same resolution handed back instead of called, for when the arguments
 * are already where the callee wants them.
 *
 * Decompiled guest C calls a function pointer with C's own `p(a, b)`, so
 * nothing needs marshalling; only the target is wrong, because a word read
 * out of guest memory is a guest address. icall_thunk.py rewrites every
 * indirect call in guest C to go through armrec_icall, which asks this for
 * the host entry point and jumps to it with the frame untouched. Never
 * returns NULL: an unknown address aborts by name.
 */
void *armrec_resolve_code(uint32_t addr);

/* The i386 thunks icall_thunk.py emits calls to. Declared for the linker's
 * sake and for tests; they are not C-callable. See armrec_rt.c. */
void armrec_icall(void);
void armrec_icall_tail(void);

/* Resolve a guest address to a name, for diagnostics. Never NULL. */
const char *armrec_name_of(uint32_t addr);

/* Whether resident code owns a code address: what armrec_dispatch would call
 * rather than abort on. armrec_name_of also names non-resident claimants. */
int armrec_code_live(uint32_t addr);

/* Called when recompiled code reaches something we cannot execute. */
void armrec_trap(const char *fn, const char *what) __attribute__((noreturn));

/* Software interrupt (BIOS call). Implemented by the platform layer. */
uint32_t armrec_swi(uint32_t num, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3);

/* Coprocessor access (CP15: caches, TCM, protection unit). Platform layer. */
uint32_t armrec_mrc(uint32_t cp, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2);
void armrec_mcr(uint32_t cp, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2, uint32_t v);

/* CPSR/SPSR access. The port runs everything in one mode, so these are
 * bookkeeping only; the interrupt-enable bit is the part that matters. */
uint32_t armrec_mrs(int spsr);
void armrec_msr(int spsr, uint32_t mask, uint32_t v);

/* ------------------------------------------------------------------ */
/* Instrumentation                                                    */
/* ------------------------------------------------------------------ */

/* Set ARMREC_TRACE=1 in the environment to log every dispatched call. */
extern int armrec_trace;

#ifdef __cplusplus
}
#endif

#endif /* ARMREC_RT_H */

/*
 * The GBA machine under a decomp compiled to wasm32 (games/gba-common).
 *
 * Guest memory is the GBA map: EWRAM 0x02000000, IWRAM 0x03000000, I/O
 * 0x04000000, palette 0x05000000, VRAM 0x06000000, OAM 0x07000000 and the
 * player's ROM at 0x08000000, all plain linear memory. The game's code is
 * native wasm; the bridge (tools/gbabridge.py) turns its volatile accesses
 * into gba_vload/gba_vstore, its function pointers into GBA code addresses
 * (gba_dispatch) and its data into ROM/RAM addresses.
 *
 * Time is counted in scanlines: the game runs between two VBlank waits and
 * the machine steps lines (PPU line, HBlank DMA/IRQ, timers, VCount match,
 * VBlank at line 160 where the frame goes to the host) whenever the game
 * waits: VBlankIntrWait, or a poll of VCOUNT/DISPSTAT.
 *
 * Port TUs are compiled with the decomp's headers and bridged like the
 * game's own, so a port function named like a cartridge one (CpuSet,
 * SoundMain, ply_note) gets that function's address.
 */
#ifndef GBA_PORT_H
#define GBA_PORT_H

#include <stdint.h>

#define GBA_EWRAM 0x02000000u
#define GBA_IWRAM 0x03000000u
#define GBA_IO 0x04000000u
#define GBA_PAL 0x05000000u
#define GBA_VRAM 0x06000000u
#define GBA_OAM 0x07000000u
#define GBA_ROM 0x08000000u

#define GBA_W 240
#define GBA_H 160
#define GBA_LINES 228
#define GBA_LINE_CYCLES 1232u

#define GBA_PTR(a) ((uint8_t *)(uintptr_t)(a))
#define IO16(off) (*(uint16_t *)(uintptr_t)(GBA_IO + (off)))
#define IO32(off) (*(uint32_t *)(uintptr_t)(GBA_IO + (off)))

/* I/O register offsets the machine itself looks at */
enum {
    R_DISPCNT = 0x000, R_DISPSTAT = 0x004, R_VCOUNT = 0x006,
    R_BG0CNT = 0x008, R_BG0HOFS = 0x010, R_BG2PA = 0x020, R_BG2X = 0x028, R_BG2Y = 0x02C,
    R_BG3PA = 0x030, R_BG3X = 0x038, R_BG3Y = 0x03C,
    R_WIN0H = 0x040, R_WIN1H = 0x042, R_WIN0V = 0x044, R_WIN1V = 0x046, R_WININ = 0x048, R_WINOUT = 0x04A,
    R_MOSAIC = 0x04C, R_BLDCNT = 0x050, R_BLDALPHA = 0x052, R_BLDY = 0x054,
    R_SOUND1CNT_L = 0x060, R_SOUNDCNT_L = 0x080, R_SOUNDCNT_H = 0x082, R_SOUNDCNT_X = 0x084,
    R_SOUNDBIAS = 0x088, R_WAVE_RAM = 0x090, R_FIFO_A = 0x0A0, R_FIFO_B = 0x0A4,
    R_DMA0SAD = 0x0B0, R_TM0CNT = 0x100, R_SIOCNT = 0x128, R_KEYINPUT = 0x130, R_KEYCNT = 0x132,
    R_RCNT = 0x134, R_IE = 0x200, R_IF = 0x202, R_WAITCNT = 0x204, R_IME = 0x208,
};

enum {
    IRQ_VBLANK = 1 << 0, IRQ_HBLANK = 1 << 1, IRQ_VCOUNT = 1 << 2, IRQ_TIMER0 = 1 << 3,
    IRQ_TIMER3 = 1 << 6, IRQ_SERIAL = 1 << 7, IRQ_DMA0 = 1 << 8, IRQ_KEYPAD = 1 << 12, IRQ_GAMEPAK = 1 << 13,
};

/* A decomp static by object and name (gbabridge.py maps the symbol to the
 * ELF's local): GBA_LOCAL(overworld, CB1_Overworld) is its address. */
#define GBA_LOCAL(file, name) ((uint32_t)(uintptr_t)__gba_local__##file##__##name)
#define GBA_LOCAL_DECL(file, name) extern char __gba_local__##file##__##name[]

/* per-game constants and hooks the port needs (games/<game>/pc/src) */
typedef struct gba_game_info {
    const char *name;
    const char *game_code;  /* ROM header 0xAC, e.g. "BPEE" */
    uint32_t save_size;     /* flash bytes */
    uint32_t intr_table;    /* address of gIntrTable */
    void (*agb_main)(void);
    uint32_t *callback2;    /* &gMain.callback2 */
    /* fills NP_STAT_FIELD_READY, NP_STAT_MAP_ID, NP_STAT_IN_BATTLE */
    void (*status)(uint32_t *status);
    /* the game's own save, run from the main loop; returns NP_QS_* */
    uint32_t (*quicksave)(void);
} gba_game_info;
extern const gba_game_info gba_game;

/* gba_io.c */
extern uint32_t gba_vcount;         /* current scanline */
extern uint64_t gba_frames;         /* VBlanks since boot */
extern uint64_t gba_cycles;         /* 16.78 MHz cycles since boot, at line granularity */
extern uint32_t gba_keys;           /* NP_KEY_* held this frame */
void gba_io_reset(void);
void gba_io_write16(uint32_t off, uint16_t v);
uint16_t gba_io_read16(uint32_t off);
void gba_step_line(void);
void gba_raise_irq(uint16_t bits);
void gba_check_irqs(void);
void gba_wait_irq(uint16_t flags, int discard_old);
void gba_dma_run(int ch);

/* gba_ppu.c */
extern uint32_t gba_screen[GBA_H * GBA_W];
void gba_ppu_line(int y);
void gba_ppu_vblank(void);
void gba_ppu_reload_affine(int bg);

/* gba_apu.c */
void gba_apu_reset(void);
void gba_apu_write(uint32_t off, uint16_t v);
void gba_apu_frame(void);
void gba_apu_pcm(const int8_t *left, const int8_t *right, uint32_t n, uint32_t rate);

/* gba_main.c: the frame boundary with the host */
void gba_frame_end(void);
void gba_soft_reset(void);
__attribute__((noreturn)) void gba_fatal(const char *fmt, ...);
void gba_log(const char *fmt, ...);

/* gba_flash.c */
void gba_flash_boot(void);
void gba_flash_publish(void);

/* gba_rtc.c */
int64_t gba_rtc_seconds(void);

/* generated dispatch table */
uint32_t gba_dispatch(uint32_t addr, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                      uint32_t);
int gba_is_code(uint32_t addr);
#define gba_call0(addr) ((void)gba_dispatch((addr), 0, 0, 0, 0, 0, 0, 0, 0))

#endif

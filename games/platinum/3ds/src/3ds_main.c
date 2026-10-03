/*
 * 3ds/src/3ds_main.c: first binary this tree produces.
 *
 * libctru stub grown into the guest slab's gate: both screens, apt/HID loop, Start or
 * Select returns to hbmenu. No game code and no NitroMain. What it does is
 * take the guest slab, run every model built on it, translation, the port
 * window, sound source addresses, the two I/O pages, the VRAM banks, leave
 * guest memory the way a reset console leaves it, and print one line that says
 * whether all of that passed. Later phases grow this file into the crt
 * without replacing the loop.
 *
 * Why the self-tests run here and not only on the host. Every model in this
 * port is pure C and 3ds/tests/run.sh runs all of them on a build machine,
 * which is where a failure is readable. What the console adds is that libctru's
 * heap really handed over 8 MB, that the ARM11's alignment and struct layout
 * agree with the host's, and that nothing in the toolchain moved a table,
 * claims a host run cannot make. It costs a few milliseconds once.
 *
 * The first build printed to a PrintConsole on each screen. The next took the consoles away:
 * A console owns its screen, consoleInit reformats it to RGB565 and clears
 * it every frame it is written to, so a console and a blit cannot share an
 * LCD, and the blit is what the port needs. Text now goes through the 5x7
 * font in 3ds_view.c, into the letterbox band above and below the picture.
 *
 * The two surfaces here are the placeholder producer. The sound work points
 * `view_present` at pc_video's two DS screens instead and nothing below the
 * seam changes.
 *
 * aptMainLoop returns 0 when Home or close should tear us down. Do not hang
 * in that case, fall out and gfxExit. KEY_START is the documented hbmenu
 * exit; KEY_SELECT is the alternative so a loader that eats Start still has
 * a way out.
 */

#include <3ds.h>
#include <stdio.h>
#include <string.h>

/* armrec's memory API, for the init this file now goes through; the memory backend's
 * armrec_mem_3ds.c is what answers it on this console. */
#include "armrec_rt.h"

#include "armrec_mem_3ds.h"
#include "3ds_apt.h"
#include "3ds_audio.h"
#include "3ds_cp.h"
#include "3ds_cpu.h"
#include "3ds_ctrdg.h"
#include "3ds_div0.h"
#include "3ds_fault.h"
#include "3ds_frame.h"
#include "3ds_gpu.h"
#include "3ds_guest.h"
#include "3ds_init.h"
#include "3ds_input.h"
#include "3ds_io.h"
#include "3ds_hostmap.h"
#include "3ds_mi_host.h"
#include "3ds_hwmem.h"
#include "3ds_ioreg.h"
#include "3ds_mem.h"
#include "3ds_os_context.h"
#include "3ds_rom.h"
#include "3ds_rom_hdr.h"
#include "3ds_sym.h"
#include "3ds_sdk_statics.h"
#include "3ds_snd_addr.h"
#include "3ds_snd_watch.h"
#include "3ds_state.h"
#include "3ds_effect.h"
#include "3ds_gpu2d.h"
#include "3ds_layer3d.h"
#include "3ds_gpu3d.h"
#include "3ds_pica3d.h"
#include "3ds_tex3d.h"
#include "3ds_tile.h"
#include "3ds_view.h"
#include "3ds_vram.h"
#include "3ds_watchdog.h"
#include "3ds_window.h"

/*
 * The whole game runs on this stack. Libctru gives a 3dsx 32 KB by default,
 * which is a sensible size for a homebrew app and is not one for this: there
 * is one virtual CPU here, the SDK's scheduler switches contexts rather than
 * threads, so every guest thread, the DS SDK under it and the port's own host
 * models are all nested on the crt's one stack.
 *
 * 32 KB was measured too small the first time a player saved. The save's
 * backup copy wanted a 65 KB frame, the prologue moved the stack pointer
 * straight out of the mapped region, every store of that frame went nowhere,
 * and the return branched to zero, with no fault, because this emulator
 * answers an unmapped read with zeros. That frame is gone, but the margin it
 * ate through was never there to begin with: the deepest frame left is 7,488
 * bytes, which is a quarter of the old stack in a single call.
 *
 * 256 KB is insurance and not a measurement; nothing here knows the game's
 * true deepest path, but it is 0.4% of the console's memory against a
 * failure mode that produces a branch to zero rather than a diagnosable
 * fault.
 */
unsigned int __stacksize__ = 256 * 1024;

/*
 * 192 KB each, in .bss. That is the DS's own picture size and it is not
 * negotiable; what it is not is the guest slab (the guest slab), which is a
 * different 8.02 MB and does not live in the ELF.
 */
static uint32_t sTopSurface[VIEW_DS_WIDTH * VIEW_DS_HEIGHT];
static uint32_t sBottomSurface[VIEW_DS_WIDTH * VIEW_DS_HEIGHT];

/* Where the fault probe's read lands, so the compiler cannot decide the read
 * never happened. */
static volatile uint16_t sFaultProbe;

/*
 * The game's entry point, weak because the 3dsx does not carry the game yet:
 * 3dsxtool refuses the full link over the absolute symbols the overlay
 * ordinals and the SDK's linker-script addresses are, so this binary is
 * 3ds/src alone and the honest answer to A is that there is no game in it.
 * With the game linked in, the same code hands the console over.
 */
extern void NitroMain(void) __attribute__((weak));

/*
 * The crt in front of NitroMain.
 *
 * Everything above has already happened by the time this runs: the graphics,
 * the fault handler, the slab, the SDK statics, and every model's self-test.
 * What is left is pc_main.c's own start-up order, which lives in host_init(),
 * and the game's entry point.
 *
 * It is a key press and not a timer. The diagnostic loop is what the
 * emulator gate photographs and what the fault probes need a console for, so
 * a boot that walked into the game after N frames would take both away. A
 * says go, and the top line says so.
 *
 * NitroMain is weak. A binary with no game in it is a real state during
 * bring-up (3ds/src alone links and runs) and the honest answer there is
 * to say there is no game rather than to fail to link.
 *
 * Returns a line for the screen when the hand-over did not happen, and NULL
 * when it did. A console has no stderr anybody reads.
 */
static const char *game_boot(void)
{
    if (NitroMain == NULL) {
        return "NO GAME IN THIS BINARY";
    }
    if (host_init() != 0) {
        static char msg[64];

        snprintf(msg, sizeof msg, "HOST INIT FAILED AT %s, STEP %d",
                 host_init_failed() != NULL ? host_init_failed() : "?",
                 host_init_ran());
        return msg;
    }

    NitroMain();

    /*
     * The DS's own NitroMain does not return; it ends in the game's main
     * loop, so arriving here at all is a finding and not a shutdown path.
     */
    return "NITROMAIN RETURNED";
}

int main(int argc, char **argv)
{
    char lineTop[64];
    char lineBottom[64];
    unsigned long frame = 0;
    const struct mem_slab *slab;
    int slabOk;
    int armRan = 0;
    int armBad = 0;
    int digRan = 0;
    int digBad = 0;
    int xlatRan = 0;
    int xlatBad = 0;
    int winRan = 0;
    int winBad = 0;
    int sndRan = 0;
    int sndBad = 0;
    int watchRan = 0;
    int watchBad = 0;
    int audRan = 0;
    int audBad = 0;
    int ioRan = 0;
    int ioBad = 0;
    int regRan = 0;
    int regBad = 0;
    int memRan = 0;
    int memBad = 0;
    int cartRan = 0;
    int cartBad = 0;
    int vramRan = 0;
    int vramBad = 0;
    int rstRan = 0;
    int rstBad = 0;
    int sdkRan = 0;
    int sdkBad = 0;
    int divRan = 0;
    int divBad = 0;
    int cpRan = 0;
    int cpBad = 0;
    int cpuRan = 0;
    int cpuBad = 0;
    int ctxRan = 0;
    int ctxBad = 0;
    int aptRan = 0;
    int aptBad = 0;
    int wdRan = 0;
    int wdBad = 0;
    int mapRan = 0;
    int mapBad = 0;
    int miRan = 0;
    int miBad = 0;
    int romRan = 0;
    int romBad = 0;
    int hdrRan = 0;
    int hdrBad = 0;
    int ovRan = 0;
    int tileRan = 0;
    int tileBad = 0;
    int bgRan = 0;
    int bgBad = 0;
    int fxRan = 0;
    int fxBad = 0;
    int gxRan = 0;
    int gxBad = 0;
    int l3dRan = 0;
    int l3dBad = 0;
    int texRan = 0;
    int texBad = 0;
    int picaRan = 0;
    int picaBad = 0;
    int ovBad = 0;
    int selfRan;
    int selfBad;
    int paceMarked = 0;
    const char *bootMsg = NULL;

    (void)argc;
    (void)argv;

    gfxInitDefault();

    /*
     * And the PICA, which draws every frame from here on. Before the
     * DSP because it is the other half of the screens, and BEFORE
     * watchdog_arm() because C3D_Init takes libctru's one VBlank0 callback
     * slot for a frame counter this port does not use, and the hang
     * detector wants it back. Failure is not fatal: 3ds_frame.c falls back to
     * the CPU blit, which is still in the binary as the oracle the GPU present is
     * checked against.
     */
    (void)gpu_init();

    /*
     * The DSP, beside the screens and for the same reason: it is a console
     * service this port needs open before anything makes a sound, not one of
     * pc_main.c's host models. It comes up whether or not there is a game in
     * this binary, with one, it registers the mixer's sink; without one, it
     * measures how long the DSP takes to play a buffer of a known length,
     * which is what says the rate and the sample count are right.
     * Failure is not fatal: a console with no DSP firmware runs silent.
     */
    (void)audio_init();

    /*
     * And the system's own three interruptions, Home, sleep and close;
     * which can arrive from here on and which every exit path out of this
     * binary now goes through. After the two services it tears down,
     * because the hook can fire the moment it is installed.
     */
    apt_install();

    /*
     * And the hang detector, on the console's own VBlank rather than a thread
     * of its own. Armed here, before the self-tests, so a boot that
     * stops inside one of them is reported too; there is no presented frame
     * during any of it and the limit is twenty seconds.
     */
    watchdog_arm(0);
    watchdog_where("boot self-tests");

    /*
     * The thread guest code is allowed to run on, taken before anything here
     * hands work to libctru. It is this one (the crt's) and every
     * guest thread the SDK's scheduler dispatches runs on it, because that
     * scheduler switches contexts and not threads.
     */
    cpu_bind_main();

    /*
     * Before the slab, because a fault while taking it is exactly the kind
     * this console otherwise reports as a frozen screen. The handler draws,
     * so it goes in after the graphics and not before. 4.4.
     */
    fault_install();

    /*
     * Before anything else asks the heap for a large block: the slab is the
     * one allocation that cannot fail late or move, and taking it first means
     * a shortfall shows up as a refusal to start rather than as a mystery
     * three phases from now. A failure is not fatal *here*, the loop still
     * runs so the message can be read off the screen, but nothing that
     * needs guest memory may run after it.
     *
     * armrec_mem_init() rather than mem_init(), and that is 4.1: the slab, the
     * translator, the two I/O pages and the VRAM map all come up behind the
     * function pc_main.c already calls on PC. The game link keeps this call and drops
     * everything under it.
     */
    slabOk = armrec_mem_init() == 0;
    slab = mem_info();

    /*
     * The translator is the slab's only interface, so it is tested here on
     * the console rather than only on the host: the same guest_selftest()
     * 3ds/tests/guest_xlat.c runs, on the memory libctru actually gave us.
     * It scribbles over every region, so the slab is zeroed again after it,
     * guest memory has to start the way a DS starts, and the game link will drop
     * this call rather than work around it.
     */
    if (slabOk) {
        /*
         * The region table first, because it is the only check here that reads
         * guest memory without writing any: it says the nine rows, the five
         * VRAM windows and the port window are where armrec's callers will
         * look for them, before anything below scribbles on them.
         */
        armBad = armrec_mem_selftest(&armRan);
        /*
         * And the digest, which is the other read-only one and has to run
         * while guest memory is still the way armrec_mem_init() left it: its
         * first check is that a zeroed map hashes to the value the PC port
         * hashes it to. Several passes over 8 MB of FNV cost about half a
         * second here, once, and the game link drops this call with the rest.
         */
        digBad = state_selftest(&digRan);
        /*
         * And the one row where a wrong address does not fault: 0x08000000 is
         * a GBA cartridge on a DS and this console's heap window here, so the
         * SDK's cartridge check would decide from whatever the heap held. It
         * reads an empty slot now, and this says so from inside a translation
         * unit compiled against the SDK's own macros. Read-only, and third,
         * because it is a claim about *zeros*: guest_selftest() below writes
         * a byte at each end of every region, and one of those ends is the
         * module-ID image word the cartridge check reads.
         */
        cartBad = ctrdg_selftest(&cartRan);
        xlatBad = guest_selftest(&xlatRan);
        /*
         * And the allocator that sits on top of it, for the same reason: the
         * port window is where every object the SPU has to reach will live,
         * and "it works on the host" is not the claim this port needs. It
         * fills the window and hands it back, so it runs before the slab is
         * zeroed and before anything real is allocated.
         */
        winBad = window_selftest(&winRan);
        /*
         * And the rule the window exists to keep: a wave's guest address has
         * to survive SOUNDxSAD's 27 bits, and a source address has to find the
         * bytes again. The sound work is where the SPU calls this; running it here
         * means the invariant is proved on the console before anything depends
         * on it.
         */
        sndBad = snd_addr_selftest(&sndRan);
        /*
         * And the same rule against the addresses the *game* keys channels
         * with, which is the check that runs for the whole boot rather than
         * here. What runs here is its classifier, over a log this
         * planted: a wave in the window passes, one in main RAM fails even
         * though the SPU could read it, and a host pointer put through
         * SOUNDxSAD's own mask fails whichever way it lands. It runs before
         * the game so the log it borrows is still empty.
         */
        watchBad = snd_watch_selftest(&watchRan);
        /*
         * And the far end of the same path: the DSP the mixer's samples go
         * out through. What is checked is the probe audio_init() already ran:
         * A buffer of a known length took the number of frames its length
         * says, which is the one thing libctru's own documentation leaves
         * ambiguous and the one that would put every sound an octave out.
         */
        audBad = audio_selftest(&audRan);
        /*
         * And the two I/O pages, which are the one place a slab row is not the
         * whole truth about an address: what 0x04000000 holds depends on which
         * processor is asking. Nothing in this game switches; the model is
         * checked before anything needs it, not after.
         */
        ioBad = io_selftest(&ioRan);
        /*
         * And the shadow that puts the game's registers in that row. This is
         * the only check here that runs in a translation unit compiled against
         * the DS SDK rather than libctru: it takes the address of real `reg_*`
         * macros and compares each against the translator. Reading the shadow
         * header cannot tell you the compiler opened it; this can.
         */
        regBad = ioreg_selftest(&regRan);
        /*
         * And the other half of the same shadow: the DS memory map. Main RAM
         * and the shared work area are 8 MB apart in the SDK's arithmetic and
         * are two separate rows here, so every constant in the shared area is
         * checked against the translator one at a time.
         */
        memBad = hwmem_selftest(&memRan);
        /*
         * And VRAM, which is the one region a slab row does not describe on
         * its own: nine banks, five windows, and VRAMCNT deciding which
         * address reaches which bank. The check that matters is the one the
         * console runs here, a marker written through the LCDC window and
         * read back through the BG window after the bank moves.
         */
        vramBad = vram_selftest(&vramRan);
        /*
         * And the divider, which is the other place a slab row is not
         * storage: the game's FX_Div, FX_Inv and FX_Sqrt read result
         * registers that nothing would ever have written. It runs inside the
         * slab block because every one of its registers is in the I/O row,
         * and it leaves them zeroed.
         */
        cpBad = cp_selftest(&cpRan);
        /*
         * And the memoised translator the 2D renderer reads through.
         * Inside the slab block because every claim it makes is about
         * translations, and before the zeroing below because it writes
         * nothing; it compares its own answers against armrec_host_ptr()'s.
         */
        mapBad = hostmap_selftest(&mapRan);
        /*
         * And the walk the bulk memory primitives go through. It is
         * here because it needs the bank model above it, its interesting
         * cases are a fill across a window with a bank missing and a copy
         * across a seam, and it leaves every bank disabled again.
         */
        miBad = mi_host_selftest(&miRan);
        /*
         * Everything above scribbles on guest memory, so it is zeroed once,
         * here, and *then* pc_input_init() puts the two words a reset console
         * starts with back, KEYINPUT 0x03FF and the ARM7's X/Y/pen halfword
         * 0x2C00, the same values and the same masks pc/src/pc_input.c
         * publishes. 3ds/src/3ds_input.c is what answers that name here,
         * and the input path is where the console's own buttons reach it.
         */
        memset(slab->base, 0, slab->bytes);
        rstBad = input_selftest(&rstRan);
        /*
         * The two SDK statics whose initialisers stopped being constant when
         * the shadow moved their addresses onto the slab. They live in
         * .data rather than in guest memory, so the zeroing above does not
         * touch them, but they do need a bound slab, and they need to be
         * written before any SDK code runs, which on this binary is never and
         * once the game links is immediately after this block.
         */
        sdk_statics_publish();
        sdkBad = sdk_statics_check();
        sdkRan = sdk_statics_present() ? 9 : 0;
    }

    /*
     * Division by zero, which is the one model in this tree with no host twin:
     * The build machine is x86, where these same expressions raise #DE and
     * kill the process before anything can be compared. It touches no guest
     * memory, so it runs outside the slab block and runs even when the slab
     * did not come up. 5.6.
     */
    divBad = div0_selftest(&divRan);

    /*
     * And the one virtual CPU, which is the other check with no host twin:
     * A build machine has no thread ids, so cpu_selftest() answers 0 checks
     * there and these three run only here. Outside the slab block; it
     * touches no guest memory, and after cpu_bind_main() above, which is
     * half of what it is checking.
     */
    cpuBad = cpu_selftest(&cpuRan);

    /*
     * And the context switch, which is the other model with no host twin and
     * the one that has to run where a stack can really be swapped: a build
     * machine would be putting x86 behind ARM assembly. It allocates one
     * 128 KB host stack, ping-pongs between two contexts and leaves both.
     * Outside the slab block, the guest stack address it is handed is
     * recorded and never used as a stack. 7.10.
     */
    ctxBad = os_context_selftest(&ctxRan);

    /*
     * And what happens when the system takes the console away. Outside
     * the slab block because it touches no guest memory: what it drives is the
     * bookkeeping a Home press, a closed lid and a close request all go
     * through; the host interval that has to come off the frame-time report,
     * and the guest VBlank count that has to be the same on both sides of the
     * suspension. It leaves its own counters at zero, so a real suspension
     * later in the run is still the first one.
     */
    aptBad = apt_selftest(&aptRan);

    /*
     * And the hang detector's decision, beside it because they share a
     * question: what stops the frames. A suspension does and is not a hang; a
     * scene presenting every ninth VBlank of a ten-VBlank limit does not and
     * runs forever. Outside the slab block, and it leaves itself disarmed;
     * the arming above is what the run uses.
     */
    wdBad = watchdog_selftest(&wdRan);
    watchdog_arm(0);

    /*
     * And the cartridge the 3dsx carries. Outside the slab block for the same
     * reason as the division check; it touches no guest memory, it is a
     * mount and a file, and it runs here rather than in run.sh because a
     * build machine has no RomFS to mount: whether --romfs packed the right
     * image is a question only the console can answer.
     */
    romBad = rom_selftest(&romRan);

    /*
     * And the header out of it, at the address the firmware would have left
     * it: guest 0x027FFE00, through the translator, read back through the
     * SDK's own struct from a translation unit compiled against the SDK. It
     * needs the mount above and a slab, and it puts both the buffer and the
     * 160 bytes above it back the way it found them; those 160 are the
     * lock words and the button buffer, which is the whole point of the
     * check. 6.3.
     */
    if (slabOk && romBad == 0) {
        hdrBad = rom_hdr_selftest(&hdrRan);
    }

    /*
     * And the overlay statics' addresses, which have to survive the
     * loader moving the image. They are written into the link as differences
     * from the table that holds them precisely so they can; this is where
     * that is checked, because a build machine has no relocation to apply.
     * Outside the slab block: they are host addresses in this image and touch
     * no guest memory.
     */
    ovBad = ov_addr_selftest(&ovRan);

    /*
     * And the tile expander, which is bit arithmetic over caller
     * buffers and needs no slab. It runs on the console for the reason every
     * pure model here does: the ARM11's char is unsigned by default and its
     * loads are alignment-sensitive, and a build machine's are neither.
     * Whether its colours are the software renderer's is a different question
     * and 3ds/tests/gpu2d_render.c is where that one is asked.
     */
    tileBad = tile_selftest(&tileRan);

    /*
     * And the 2D survey's predicate, which is the one part of that file
     * that decides anything: it says whether a frame's registers put it inside
     * the subset the GPU path draws. Pure arithmetic over a register set, so
     * it runs here beside the expander and needs no guest memory either.
     */
    bgBad = gpu2d_selftest(&bgRan);

    /*
     * And the effect pass's colour arithmetic, which is the DS's blend unit and its
     * master fade folded into a palette: the two brightness effects and the
     * fade, checked against pc_gpu2d.c's own pc_gpu2d_present_px() over every
     * halfword a palette can hold. A rounding step wrong here is a rounding
     * step wrong on every pixel of a faded screen.
     */
    fxBad = effect_selftest(&fxRan);

    /*
     * And the 3D survey's own arithmetic: what a DS texture image
     * parameter describes and what it weighs, and the working-set table that
     * counts one frame's distinct textures. Pure arithmetic over words the
     * geometry engine produced, so it runs here with the rest and needs no
     * polygon list.
     */
    gxBad = gpu3d_selftest(&gxRan);

    /*
     * And the 3D layer's expansion: six bits a channel out to eight,
     * which has to be the software compositor's own arithmetic to the bit or
     * every frame with 3D on it differs from the oracle by a rounding step.
     */
    l3dBad = layer3d_selftest(&l3dRan);

    /*
     * And the 3D texture converter's own arithmetic: the colour
     * expansion, which is NOT the tile expander's, a texture palette entry
     * has no sixth green bit and reaches a whiter white, plus the shape and
     * slot fields a TexParam carries. Whether the converted texels are the
     * software renderer's own is the question 3ds/tests/tex3d_convert.c asks,
     * because answering it needs a texture in guest memory and this does not.
     */
    texBad = tex3d_selftest(&texRan);

    /*
     * And the PICA vertex transform (the 3D producer): a screen pixel out of the
     * geometry engine, multiplied through by W, divided by W again the way the
     * PICA will, has to come back the same pixel, plus the cull inversion,
     * which the DS states as what to KEEP and the PICA as what to throw away.
     * Float arithmetic, so it runs on the console for the reason every pure
     * model here does: this ARM11 is not a build machine's FPU.
     */
    picaBad = pica3d_selftest(&picaRan);

    /*
     * One total, computed once: every count above is fixed by the time the
     * loop starts, and the corner block has to carry the verdict from the
     * first frame a screenshot could catch. A run.sh check reads that block
     * out of the emulator's PNG, without it the suite reported 35 ok while
     * the console was drawing SELF FAIL, which is the one kind of green that
     * is worse than a red.
     */
    selfRan = armRan + digRan + xlatRan + winRan + sndRan + audRan + ioRan
              + regRan + memRan + cartRan + vramRan + cpRan + rstRan
              + sdkRan + divRan + romRan + hdrRan + cpuRan + ctxRan
              + mapRan + miRan + watchRan + aptRan + wdRan + ovRan + tileRan
              + bgRan + fxRan + gxRan + l3dRan + texRan + picaRan;
    selfBad = armBad + digBad + xlatBad + winBad + sndBad + audBad + ioBad
              + regBad + memBad + cartBad + vramBad + cpBad + rstBad
              + sdkBad + divBad + romBad + hdrBad + cpuBad + ctxBad
              + mapBad + miBad + watchBad + aptBad + wdBad + ovBad + tileBad
              + bgBad + fxBad + gxBad + l3dBad + texBad + picaBad;

    view_test_pattern(sTopSurface, VIEW_DS_WIDTH, VIEW_DS_HEIGHT,
                      VIEW_COLOUR_TOP, VIEW_COLOUR_EDGE,
                      (slabOk && selfBad == 0) ? VIEW_COLOUR_MARK
                                               : VIEW_COLOUR_MARK_BAD);
    view_test_pattern(sBottomSurface, VIEW_DS_WIDTH, VIEW_DS_HEIGHT,
                      VIEW_COLOUR_BOTTOM, VIEW_COLOUR_EDGE,
                      (slabOk && selfBad == 0) ? VIEW_COLOUR_MARK
                                               : VIEW_COLOUR_MARK_BAD);
    fault_set_surfaces(sTopSurface, sBottomSurface);

    /*
     * frame_running() rather than aptMainLoop() directly, and no
     * hidScanInput() here: both moved into frame_present(), so this loop
     * and the game's OS_Halt end a frame the same way instead of keeping two
     * sequences that can drift. The first iteration runs before any scan, so
     * hidKeysDown() is 0 on it; which costs a button press nobody made.
     */
    while (frame_running()) {
        u32 kDown;
        u32 kHeld;

        kDown = hidKeysDown();
        kHeld = hidKeysHeld();

        if (kDown & (KEY_START | KEY_SELECT)) {
            break;
        }

        /*
         * A hands the console to the game. Only from a passing
         * console: host_init() and NitroMain run on top of every model
         * checked above, and starting the game over a failed one would
         * produce evidence about this port rather than about the game.
         */
        if ((kDown & KEY_A) && bootMsg == NULL) {
            if (!slabOk || selfBad != 0) {
                bootMsg = "SELF FAILED, NOT STARTING THE GAME";
            } else {
                bootMsg = game_boot();
            }
        }

        /*
         * The fault path's two probes, because it has two halves and only one
         * of them can be seen here. 4.4.
         *
         * L+R+Y shows the report: the same fault_report() the handler calls,
         * with a data abort invented at a guest address, so the screen says
         * GUEST 02003F00 and Start ends the process. That is the half this
         * emulator can check.
         *
         * L+R+X is the real thing; a read through a translation that
         * answered NULL, which is what an unmapped VRAM address gives. On
         * hardware that is a data abort and the handler draws. On Azahar
         * 2126.0 it is measured to return zeros and carry on, so this button
         * looks like it did nothing, and that is the finding rather than a
         * bug in it.
         */
        if ((kHeld & (KEY_L | KEY_R)) == (KEY_L | KEY_R) && (kDown & KEY_Y)) {
            void *at = armrec_host_ptr(0x02003F00u);

            fault_report(FAULT_DATA_ABORT, (uint32_t)(uintptr_t)at, 0x00000005u,
                         (uint32_t)(uintptr_t)&main, 0u, 0u);
        }
        if ((kHeld & (KEY_L | KEY_R)) == (KEY_L | KEY_R) && (kDown & KEY_X)) {
            const volatile uint16_t *bad =
                (const volatile uint16_t *)armrec_host_ptr(0x06000000u);

            sFaultProbe = *bad;
        }

        /*
         * The pacing check, which is the one self-test that cannot run
         * before the loop: it overruns a frame on purpose and then watches
         * the frames after it. It reports through the same SELF total below.
         */
        watchdog_where("diagnostic loop");

        /*
         * L+R+B stops presenting frames on purpose, which is the only way to
         * see the hang detector do its job: a port that hangs by accident is
         * not something a check can arrange. The limit is shortened to three
         * seconds first, because the real twenty is a check nobody would run.
         * 3ds/tests/run.sh presses this and then reads the report off the
         * card. 9.7.
         */
        if ((kHeld & (KEY_L | KEY_R)) == (KEY_L | KEY_R) && (kDown & KEY_B)) {
            watchdog_arm(180u);
            watchdog_where("the deliberate hang probe");
            for (;;) {
                /* Nothing. The GSP event thread runs at one priority above
                 * this one, so it is not starved by the spin. */
            }
        }

        pace_step();
        if (pace_bad() != 0 && !paceMarked) {
            /*
             * Repaint the corner block red. The marker is what
             * 3ds/tests/shot_verdict.py reads, and it was painted amber
             * before the loop; a check that fails during the loop and
             * leaves it amber is a failure that reads as a pass.
             */
            view_test_pattern(sTopSurface, VIEW_DS_WIDTH, VIEW_DS_HEIGHT,
                              VIEW_COLOUR_TOP, VIEW_COLOUR_EDGE,
                              VIEW_COLOUR_MARK_BAD);
            view_test_pattern(sBottomSurface, VIEW_DS_WIDTH, VIEW_DS_HEIGHT,
                              VIEW_COLOUR_BOTTOM, VIEW_COLOUR_EDGE,
                              VIEW_COLOUR_MARK_BAD);
            paceMarked = 1;
        }

        if (slabOk) {
            /*
             * One total, not seven counts. The bottom screen's band is 53
             * characters of the 5x7 font and the per-model list had already
             * filled it; it never said which model failed either, because
             * those were the numbers of checks *run*. So the top line carries
             * the total and the verdict, and the bottom line below names the
             * failing model when there is one.
             */
            /* NODSP is not a failure: a console with no dumped sound
             * firmware runs the game silent, and the line says so rather
             * than leaving it to be discovered by listening. */
            snprintf(lineTop, sizeof lineTop, "SLAB %08lX SELF %d %s%s F:%06lu%s",
                     (unsigned long)(uintptr_t)slab->base,
                     selfRan + pace_ran(),
                     (selfBad + pace_bad()) == 0 ? "PASS" : "FAIL",
                     audio_ready() ? "" : " NODSP", frame,
                     (bootMsg == NULL && NitroMain != NULL) ? " A:GAME" : "");
            /*
             * The register check is the one model the host cannot run, so
             * when it fails the bottom line gives up the memory report and
             * says which check went first instead. Everything else here can
             * be reproduced by 3ds/tests/run.sh on a machine with a debugger.
             */
            if (pace_bad() != 0) {
                snprintf(lineBottom, sizeof lineBottom,
                         "PACE FAILED %d OF %d, FIRST AT %d, MEAN %lu",
                         pace_bad(), pace_ran(), pace_first_failure(),
                         pace_mean_ticks());
            } else if (regBad != 0) {
                snprintf(lineBottom, sizeof lineBottom,
                         "IOREG FAILED %d OF %d, FIRST AT %d",
                         regBad, regRan, ioreg_first_failure());
            } else if (memBad != 0) {
                snprintf(lineBottom, sizeof lineBottom,
                         "HWMEM FAILED %d OF %d, FIRST AT %d",
                         memBad, memRan, hwmem_first_failure());
            } else if (cartBad != 0) {
                snprintf(lineBottom, sizeof lineBottom,
                         "CTRDG FAILED %d OF %d, FIRST AT %d",
                         cartBad, cartRan, ctrdg_first_failure());
            } else if (romBad != 0) {
                snprintf(lineBottom, sizeof lineBottom,
                         "ROMFS FAILED %d OF %d, FIRST AT %d",
                         romBad, romRan, rom_first_failure());
            } else if (hdrBad != 0) {
                snprintf(lineBottom, sizeof lineBottom,
                         "ROMHDR FAILED %d OF %d, FIRST AT %d",
                         hdrBad, hdrRan, rom_hdr_first_failure());
            } else if (divBad != 0) {
                snprintf(lineBottom, sizeof lineBottom,
                         "DIV0 FAILED %d OF %d, FIRST AT %d",
                         divBad, divRan, div0_first_failure());
            } else if (cpuBad != 0) {
                snprintf(lineBottom, sizeof lineBottom,
                         "ONE CPU FAILED %d OF %d", cpuBad, cpuRan);
            } else if (mapBad != 0) {
                snprintf(lineBottom, sizeof lineBottom,
                         "HOSTMAP FAILED %d OF %d", mapBad, mapRan);
            } else if (miBad != 0) {
                snprintf(lineBottom, sizeof lineBottom,
                         "MI WALK FAILED %d OF %d", miBad, miRan);
            } else if (ctxBad != 0) {
                snprintf(lineBottom, sizeof lineBottom,
                         "OSCONTEXT FAILED %d OF %d, FIRST AT %d",
                         ctxBad, ctxRan, os_context_first_failure());
            } else if (audBad != 0) {
                /* A line of its own because one of its two failures is not a
                 * code fault: a console with no DSP firmware says ABSENT, and
                 * the probe frame count is what a wrong sample rate moves. */
                snprintf(lineBottom, sizeof lineBottom,
                         "AUDIO FAILED %d OF %d, DSP %s, PROBE %d",
                         audBad, audRan, audio_ready() ? "UP" : "ABSENT",
                         audio_probe_frames());
            } else if (wdBad != 0) {
                snprintf(lineBottom, sizeof lineBottom,
                         "WATCHDOG FAILED %d OF %d", wdBad, wdRan);
            } else if (aptBad != 0) {
                snprintf(lineBottom, sizeof lineBottom,
                         "APT SUSPEND FAILED %d OF %d", aptBad, aptRan);
            } else if (watchBad != 0) {
                /* Its own line because the band's letter list is full. The
                 * counters are back at zero by now; the check ends by
                 * resetting them so the game's own measurement starts clean,
                 * so what this can say is which model and how much of it. */
                snprintf(lineBottom, sizeof lineBottom,
                         "SND WATCH FAILED %d OF %d", watchBad, watchRan);
            } else if (selfBad != 0) {
                /*
                 * Which model, in the order they ran. Every one of these can
                 * be reproduced by 3ds/tests/run.sh on a build machine, so a
                 * letter and a count is enough to say where to look.
                 */
                snprintf(lineBottom, sizeof lineBottom,
                         "SELF FAILED A%d D%d X%d W%d S%d I%d V%d C%d R%d K%d",
                         armBad, digBad, xlatBad, winBad, sndBad, ioBad,
                         vramBad, cpBad, rstBad, sdkBad);
            } else {
                snprintf(lineBottom, sizeof lineBottom,
                         "APP %luK/%luM HEAP %luK/%luK LIN %luK",
                         (unsigned long)(slab->app_free_after / 1024),
                         (unsigned long)(slab->app_region / (1024 * 1024)),
                         (unsigned long)(slab->heap_used / 1024),
                         (unsigned long)(slab->heap_size / 1024),
                         (unsigned long)(slab->linear_free / 1024));
            }
        } else {
            snprintf(lineTop, sizeof lineTop, "SLAB FAILED F:%06lu", frame);
            snprintf(lineBottom, sizeof lineBottom, "%s", armrec_mem_strerror());
        }

        /*
         * What the hand-over said, over whatever the block above chose. It
         * only ever holds a message when the game did NOT take the console,
         * so it is the last thing that happened and the thing to read.
         */
        if (bootMsg != NULL) {
            snprintf(lineBottom, sizeof lineBottom, "%s", bootMsg);
        }

        frame_present(sTopSurface, sBottomSurface, lineTop, lineBottom, 0);
        frame = frame_count();
    }

    armrec_mem_free();
    /*
     * The report, the DSP, the cartridge and the screens, through the one
     * sequence the guest path and the system's own close also take. It
     * is registered with atexit() as well, so returning from here without it
     * would still be clean; it is called anyway because the slab above is
     * already gone and the order should read the same in both loops.
     */
    apt_shutdown();
    return 0;
}

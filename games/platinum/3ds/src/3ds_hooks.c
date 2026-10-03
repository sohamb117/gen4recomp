/*
 * 3ds/src/3ds_hooks.c: the frame hooks pc/src calls into the labs, answered by
 * a console that does not host them.
 *
 * pc/src's per-frame path calls ten functions that belong to the PC port's
 * build-machine workflow: nine lab entry points out of pc_video_frame_end()
 * and the differential checkpoint out of OS_Halt(). Every one of those calls
 * is unconditional, because the lab decides for itself from an environment
 * variable that it has nothing to do, so as long as the calls are there the
 * labs are reachable and --gc-sections keeps 43,718 bytes of text and 292,168
 * bytes of .bss in the image for code that cannot run here.
 *
 * The eleventh is not a frame hook and was missed until someone measured the
 * link. A patch puts a pc_lab_boot() call in the game's own boot path, so it
 * is not reached through pc_video.c at all and was not in the frame-hook
 * sweep. It is a strong undefined reference today; the linker does not
 * complain only because nothing calls NitroMain yet.
 *
 * The labs mint a save by driving the game, sweep every species through the
 * sprite path, decode every message bank, and play every sequence, each driven
 * by a PC_LAB* variable, each writing files into a build directory, and three
 * of them calling exit() from inside a frame. A console has no environment to
 * set, no build directory, and nothing that survives exit() mid-frame. The
 * answer for saves is to copy one the PC port made.
 *
 * No-ops and not #ifdef in pc/src. The alternative was a __3DS__ branch around
 * the call sites, which edits a file both ports compile for a change that buys
 * nothing: the call sites are correct, they are simply calling something this
 * host does not have.
 *
 * Not weak, either. A weak definition would let a lab that came back later win
 * by linking, which sounds like a courtesy and is really a way for one of
 * these files to return without anybody deciding it should. Strong definitions
 * make putting a lab back a multiple-definition error and therefore a
 * decision.
 *
 * The classification these ten come out of is 3ds/tests/pc_src_class.py.
 */

#include "3ds_hooks.h"

/* pc_video.c declares these at its call site rather than in a header, so
 * there is no header to include; the signature is the one it declares. */

void pc_lab_frame(unsigned long long frame)
{
    (void)frame;
}

/*
 * 0 is "no lab mode", which is the answer that makes main.c take the game's
 * own intro. The PC file returns 1 or 2 to mean "a lab has already put a save
 * in place, skip to it", and neither can happen here.
 */
int pc_lab_boot(void *saveData)
{
    (void)saveData;
    return 0;
}

void pc_lab_battle_frame(unsigned long long frame)
{
    (void)frame;
}

void pc_lab_save_frame(unsigned long long frame)
{
    (void)frame;
}

void pc_lab_grow_frame(unsigned long long frame)
{
    (void)frame;
}

void pc_lab_contest_frame(unsigned long long frame)
{
    (void)frame;
}

void pc_lab_underground_frame(unsigned long long frame)
{
    (void)frame;
}

void pc_lab_sprite_frame(unsigned long long frame)
{
    (void)frame;
}

void pc_lab_text_frame(unsigned long long frame)
{
    (void)frame;
}

void pc_lab_audio_frame(unsigned long long frame)
{
    (void)frame;
}

void pc_diff_frame(unsigned long long frame)
{
    (void)frame;
}

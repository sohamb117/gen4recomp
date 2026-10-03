/*
 * The ten pc/src frame hooks this console answers with nothing. See
 * 3ds_hooks.c for why they are here rather than #ifdef'd out at the call
 * site, and 3ds/tests/pc_src_class.py for the classification.
 *
 * Nothing in 3ds/src calls these (pc/src does) so this header exists to
 * give the definitions a prototype and to keep -Wmissing-prototypes quiet.
 */

#ifndef POKEPLATINUM_3DS_HOOKS_H
#define POKEPLATINUM_3DS_HOOKS_H

void pc_lab_frame(unsigned long long frame);
void pc_lab_battle_frame(unsigned long long frame);
void pc_lab_save_frame(unsigned long long frame);
void pc_lab_grow_frame(unsigned long long frame);
void pc_lab_contest_frame(unsigned long long frame);
void pc_lab_underground_frame(unsigned long long frame);
void pc_lab_sprite_frame(unsigned long long frame);
void pc_lab_text_frame(unsigned long long frame);
void pc_lab_audio_frame(unsigned long long frame);

void pc_diff_frame(unsigned long long frame);

/*
 * The eleventh, and the only one that is not a frame hook: pc/patches puts a
 * call to it in src/main.c, so it is the game's own boot path that asks. See
 * 3ds_hooks.c for what 0 means.
 */
int pc_lab_boot(void *saveData);

#endif /* POKEPLATINUM_3DS_HOOKS_H */

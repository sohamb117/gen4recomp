/*
 * wasm32 shadow of include/list_menu.h: the same header, laid out as mwcc
 * lays it out.
 *
 * struct ListMenuTemplate has two u8 bitfields at 0x18 (fillValue,
 * cursorShadowPal) followed by u16 bitfields (lettersSpacing ... cursorKind).
 * mwcc starts a new u16 unit for them at 0x1A, as the header's offsets say;
 * wasm32 clang (SysV rules) packs the first ones into the free byte 0x19.
 * Templates built by the recompiled assembly come from ROM data in mwcc's
 * layout (ov83_0223AE00, the Mystery Gift menus: 0x1A = 0x0080, so
 * scrollMultiple 1 and fontId 0), which list_menu.c then read as
 * itemVerticalPadding 0 from 0x19 and fontId 32 from 0x1A bits 2-7: the
 * font's height came back 0 and every item printed on line 0.
 *
 * Microsoft bitfield rules (a bitfield whose type size differs from the
 * previous one starts a new unit of its own type) give mwcc's layout here.
 * This is the only D/P struct with adjacent bitfields of different sizes
 * (every header and source under include/ and arm9/ scanned), so the
 * pragma covers just this header. Its own includes come first, outside it.
 *
 * Reached through -I$(PCDIR)/include ahead of include/: the two includers
 * (arm9/src/list_menu.c, arm9/overlays/59/include/ov59_Intro.h) live
 * outside include/, so a quote include never finds the original first.
 */
#ifndef PC_DP_LIST_MENU_SHADOW_H
#define PC_DP_LIST_MENU_SHADOW_H

#include "list_menu_cursor.h"
#include "list_menu_items.h"

#pragma ms_struct on
#include_next "list_menu.h"
#pragma ms_struct off

#endif /* PC_DP_LIST_MENU_SHADOW_H */

/*
 * 3ds/src/3ds_bgptr_wrap.c: Bg_GetCharPtr() answers with a host pointer.
 *
 * The bug this fixes, as seen. On the emulator the message box drew as a bare
 * white rectangle with no frame and no scroll cursor, over a background that
 * stayed black, where the desktop port drew the framed box and the beige
 * field. The emulator's log named it: 1,792 reads of addresses like
 * 0x0601FD80 that nothing is mapped at, all from two functions in
 * src/render_window.c.
 *
 * WHY. Bg_GetCharPtr() returns G2_GetBG0CharPtr() and friends, which compute
 * a DS VRAM address from the background control registers. Game code then
 * dereferences it, render_window.c memcpy's window-frame tiles straight out
 * of it. On the desktop that is correct, because a guest address is a host
 * address there. Here it is a wild pointer: reads answer zeros and writes go
 * nowhere, so tiles blitted through it silently never arrive, which is
 * exactly a frame that does not draw.
 *
 * Why the wrapper is here and not on the SDK accessors. G2_GetOBJCharPtr()
 * and its family are also used as integers, ov19_021DA270.c adds one to a
 * VRAM base, so converting all eighteen of them would fix the dereferences
 * and break the arithmetic. Bg_GetCharPtr() has five callers and every one
 * either dereferences the result or hands it to MI_CpuFill8 / MI_CpuClear32,
 * and those are already wrapped: 3ds_mi_host.c asks mi_host_is_guest(), every
 * guest row is below 0x08000000 and every host address at or above, so a host
 * pointer passes through them untouched. Both kinds of caller are served.
 *
 * --wrap reaches this one. It redirects undefined references only, and the
 * five callers are all in other objects; the definition inside bg_window.c is
 * not affected, which is what keeps this from recursing.
 */

#include <stddef.h>
#include <stdint.h>

#include "3ds_guest.h"

extern void *__real_Bg_GetCharPtr(uint8_t bgLayer);

void *__wrap_Bg_GetCharPtr(uint8_t bgLayer)
{
    void *guest = __real_Bg_GetCharPtr(bgLayer);

    /*
     * A layer with no character base answers NULL and must go on answering
     * NULL: the callers test for it, and armrec_host_ptr(0) is a different
     * question with a different answer.
     */
    if (guest == NULL) {
        return NULL;
    }
    return armrec_host_ptr((uint32_t)(uintptr_t)guest);
}

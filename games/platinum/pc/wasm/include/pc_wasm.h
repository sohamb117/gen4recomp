/* The wasm host layer's shared pieces (pc/wasm/src/pc_wasm_host.c).
 * Only meaningful in a wasm32 compile; core/include/np_guest_abi.h is the
 * contract with the native runtime. */
#ifndef PC_WASM_H
#define PC_WASM_H

#if defined(__wasm__)

#include <np_guest_abi.h>

/* The one frame descriptor, at a fixed guest address for the life of the
 * module. magic/version are set statically; the screen/audio/frame fields
 * are filled by pc_view.c before every np_host_vblank, the save fields by
 * pc_card_rom.c, the input fields read back after vblank returns. */
extern np_frame_desc pc_wasm_frame;

/* Fatal: write msg to stderr and hand it to np_host_trap. Never returns. */
void pc_wasm_fatal(const char *msg) __attribute__((noreturn));
/* printf-shaped pc_wasm_fatal. */
void pc_wasm_fatalf(const char *fmt, ...)
    __attribute__((noreturn, format(printf, 1, 2)));

#endif /* __wasm__ */

#endif /* PC_WASM_H */

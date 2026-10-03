/* Loud trap for the PC build's link stubs.
 *
 * A handful of symbols the link needs belong to libraries that are dead
 * code for a single-player PC boot (wifi, lobby, voice chat, and a few
 * asm-only SDK helpers). Each gets a weak stub whose body is a call to
 * pc_trap_unreached(): reaching one at runtime is a bug, and the trap
 * makes it impossible to miss; it names the symbol, says why it was
 * stubbed, and aborts. It never returns.
 */
#ifndef PC_TRAP_H
#define PC_TRAP_H

void pc_trap_unreached(const char *sym, const char *why) __attribute__((noreturn));

#endif /* PC_TRAP_H */

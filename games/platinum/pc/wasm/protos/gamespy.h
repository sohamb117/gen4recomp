/*
 * The same fix as pc/wasm/protos/game.h for two NitroDWC GameSpy TUs (wifi,
 * unreachable in a single-player boot, fixed anyway so the link has no
 * unexplained signature mismatch):
 *   sb_queryengine.c calls SBServerParseQR2FullKeysSingle (sb_server.c:520,
 *     void) undeclared;
 *   every libraries/gs TU reaches __msl_assertion_failed
 *     (pc/src/pc_os_lite.c:505, void) through gsAssert.h's assert macro,
 *     undeclared (33 of them).
 */
#ifndef POKEPLATINUM_WASM_PROTOS_GAMESPY_H
#define POKEPLATINUM_WASM_PROTOS_GAMESPY_H

void __msl_assertion_failed(const char *condition, const char *filename,
                            const char *funcname, int lineno);

#ifdef PC_WASM_PROTO_SBSERVER
#include "gs/serverbrowsing/sb_serverbrowsing.h"
void SBServerParseQR2FullKeysSingle(SBServer server, char *data, int len);
#endif

#endif

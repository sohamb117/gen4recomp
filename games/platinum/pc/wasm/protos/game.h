/*
 * Prototypes for functions a few game TUs call without a declaration in
 * scope. Force-included (pc/Makefile.wasm, "implicit declarations") into
 * exactly those TUs, after global_pch.h; each TU selects its own section with
 * a -DPC_WASM_PROTO_* flag, so nothing else reaches it.
 *
 * C89 implicit declaration types each call `int f()`. Every definition here
 * returns void, so on i386 cdecl the caller merely ignores a garbage eax. A
 * wasm call is typed: wasm-ld reports "function signature mismatch" and
 * replaces the call with a trap, so these calls (selecting a battle move
 * among them) would kill the guest. The prototypes are the definitions' own;
 * nothing else in the TU changes. pc/wasm/check_module.py fails the link on
 * any such thunk outside the generated trap stubs, which is how these were
 * found.
 */
#ifndef POKEPLATINUM_WASM_PROTOS_GAME_H
#define POKEPLATINUM_WASM_PROTOS_GAME_H

#ifdef PC_WASM_PROTO_HEAP           /* applications/pokedex/pokedex_panel.c, password_word_bank.c */
#include "heap.h"
#endif

#ifdef PC_WASM_PROTO_CONTEST        /* contest.c */
/* include/link_contest_records.h itself cannot be included here: its guard
 * is POKEPLATINUM_CONTEST_H, the same as include/contest.h's, which is why
 * contest.c never includes it. The prototype is that header's line 15. */
#include "generated/pokemon_contest_types.h"
#include "savedata.h"
void LinkContestRecords_IncrementSavaData(SaveData *saveData, enum PokemonContestType contestType, int placement);
#endif

#ifdef PC_WASM_PROTO_UNDERGROUND    /* underground/text_printer.c */
void UndergroundMan_RemovePrinters(void); /* include/underground/manager.h:79 */
#endif

#ifdef PC_WASM_PROTO_BATTLE         /* battle/battle_display.c */
/* src/battle/battle_controller.c:747,778,839; declared in no header. */
#include "struct_decls/battle_system.h"
void BattleController_EmitSelectedCommand(BattleSystem *battleSys, int battler, int command);
void BattleController_EmitSelectedMove(BattleSystem *battleSys, int battler, int command);
void BattleController_EmitSelectedTarget(BattleSystem *battleSys, int battler, int command);
#endif

#endif

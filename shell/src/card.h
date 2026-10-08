/*
 * Trainer Card and Pokedex diploma images for sharing, drawn from save data
 * with the shell's embedded 8x8 font onto a plain pixel buffer.
 *
 * The layouts are nativeplat's own (no game graphics are copied or decoded),
 * and the renderer is SDL-free so the unit tests can render both pages and
 * check them without a window.
 */
#ifndef NP_SHELL_CARD_H
#define NP_SHELL_CARD_H

#include <stdbool.h>
#include <stdint.h>

#define NP_CARD_W 768
#define NP_CARD_H 576
#define NP_CARD_PARTY_MAX 6

typedef enum np_card_kind { NP_CARD_TRAINER, NP_CARD_DIPLOMA } np_card_kind;

typedef struct np_card_info {
    const char *game; /* "Platinum", ... */
    char name[64];    /* UTF-8; letters outside ASCII are approximated */
    uint16_t tid;
    bool female;
    uint32_t money;
    uint8_t badges; /* bitmask */
    const char *const *badge_names; /* the region's eight; NULL: Sinnoh's */
    uint16_t play_hours;
    uint8_t play_minutes;
    uint16_t dex_seen, dex_caught; /* national numbering */
    uint16_t dex_total;            /* species in the game's dex; 0: 493 */
    bool national_dex;
    int party_count;
    char party[NP_CARD_PARTY_MAX][48];      /* nickname, species or "Egg" */
    uint8_t party_level[NP_CARD_PARTY_MAX]; /* 0: not shown (eggs) */
    int year, month, day;                   /* issue date (diploma) */
} np_card_info;

/* Renders `kind` into px (NP_CARD_W * NP_CARD_H pixels, 0x00RRGGBB). */
void np_card_render(np_card_kind kind, const np_card_info *info, uint32_t *px);

#endif

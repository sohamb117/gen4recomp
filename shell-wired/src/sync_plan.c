/* Folder sync decisions and the per-slot state lines (see sync_plan.h). */
#include "sync_plan.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

np_sync_action np_sync_decide(const np_sync_side *local, const np_sync_side *remote, const uint8_t *base)
{
    if (local->kind == NP_SYNC_ABSENT)
        return remote->kind == NP_SYNC_ABSENT ? NP_SYNC_SAME : NP_SYNC_PULL;
    if (remote->kind == NP_SYNC_ABSENT)
        return NP_SYNC_PUSH;
    /* Both files exist. A fresh slot never replaces a save. */
    if (local->kind == NP_SYNC_EMPTY)
        return remote->kind == NP_SYNC_EMPTY ? NP_SYNC_SAME : NP_SYNC_PULL;
    if (remote->kind == NP_SYNC_EMPTY)
        return NP_SYNC_PUSH;
    if (!memcmp(local->hash, remote->hash, 20))
        return NP_SYNC_SAME;
    if (base && !memcmp(local->hash, base, 20))
        return NP_SYNC_PULL; /* only the folder's copy changed */
    if (base && !memcmp(remote->hash, base, 20))
        return NP_SYNC_PUSH; /* only this device's copy changed */
    return NP_SYNC_CONFLICT;
}

int np_sync_conflict_name(const char *slot, int year, int month, int day, const char *const *taken, int ntaken,
                          char out[NP_SLOT_NAME_MAX + 1])
{
    char suffix[32];
    int sl = snprintf(suffix, sizeof suffix, " (conflict %04d-%02d-%02d)", year, month, day);
    if (sl < 0 || sl >= NP_SLOT_NAME_MAX)
        return -1;
    char base[NP_SLOT_NAME_MAX + 1];
    size_t keep = strlen(slot);
    if (keep > (size_t)(NP_SLOT_NAME_MAX - sl))
        keep = (size_t)(NP_SLOT_NAME_MAX - sl);
    while (keep && slot[keep - 1] == ' ')
        keep--; /* no double space before the suffix */
    memcpy(base, slot, keep);
    memcpy(base + keep, suffix, (size_t)sl + 1);
    if (np_slot_name_problem(base))
        return -1;
    return np_slot_unique(base, taken, ntaken, out);
}

static void hex(const uint8_t *h, char out[41])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        out[2 * i] = digits[h[i] >> 4];
        out[2 * i + 1] = digits[h[i] & 15];
    }
    out[40] = '\0';
}

static int unhex(const char *s, size_t len, uint8_t *h)
{
    if (len != 40)
        return -1;
    for (int i = 0; i < 40; i++) {
        char c = s[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0)
            return -1;
        if (i & 1)
            h[i / 2] = (uint8_t)(h[i / 2] | v);
        else
            h[i / 2] = (uint8_t)(v << 4);
    }
    return 0;
}

int np_sync_record_parse(const char *line, const char *const *game_ids, int ngames, np_sync_record *r)
{
    const char *f[8];
    size_t len[8];
    int nf = 0;
    const char *p = line;
    for (;;) {
        const char *end = p + strcspn(p, "\t\r\n");
        if (nf == 8)
            return -1;
        f[nf] = p;
        len[nf++] = (size_t)(end - p);
        if (*end != '\t')
            break;
        p = end + 1;
    }
    if (nf != 8)
        return -1;
    memset(r, 0, sizeof *r);
    r->game = -1;
    for (int g = 0; g < ngames; g++)
        if (strlen(game_ids[g]) == len[0] && !memcmp(game_ids[g], f[0], len[0]))
            r->game = g;
    if (r->game < 0 || len[1] == 0 || len[1] > NP_SLOT_NAME_MAX || len[7] > NP_SLOT_NAME_MAX)
        return -1;
    memcpy(r->slot, f[1], len[1]);
    memcpy(r->conflict, f[7], len[7]);
    if (np_slot_name_problem(r->slot) || (r->conflict[0] && np_slot_name_problem(r->conflict)))
        return -1;
    if (unhex(f[2], len[2], r->base))
        return -1;
    char num[32];
    uint64_t *sizes[2] = {&r->local_size, &r->remote_size};
    int64_t *times[2] = {&r->local_mtime, &r->remote_mtime};
    for (int k = 0; k < 4; k++) {
        size_t n = len[3 + k];
        if (n == 0 || n >= sizeof num)
            return -1;
        memcpy(num, f[3 + k], n);
        num[n] = '\0';
        char *e;
        if (k % 2 == 0)
            *sizes[k / 2] = strtoull(num, &e, 10);
        else
            *times[k / 2] = strtoll(num, &e, 10);
        if (*e)
            return -1;
    }
    return 0;
}

int np_sync_record_format(const np_sync_record *r, const char *const *game_ids, char *out, size_t n)
{
    char h[41];
    hex(r->base, h);
    return snprintf(out, n, "%s\t%s\t%s\t%" PRIu64 "\t%" PRId64 "\t%" PRIu64 "\t%" PRId64 "\t%s\n", game_ids[r->game],
                    r->slot, h, r->local_size, r->local_mtime, r->remote_size, r->remote_mtime, r->conflict);
}

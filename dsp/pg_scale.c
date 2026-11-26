/* Phrase Gen — ScaleQuantizer.  See pg_scale.h. */
#include "pg_scale.h"

static int pc_of(const pg_scale_t *s, int note) {
    return ((note - s->root) % 12 + 12) % 12;
}

void pg_scale_init(pg_scale_t *s, int root, uint16_t mask) {
    mask &= 0xFFFu;
    if (mask == 0) mask = 1;            /* never an empty scale */
    s->root = ((root % 12) + 12) % 12;
    s->mask = mask;
    s->degrees = 0;
    for (int i = 0; i < 12; i++) {
        if (mask & (1u << i)) s->offsets[s->degrees++] = (int8_t)i;
    }
}

int pg_scale_contains(const pg_scale_t *s, int note) {
    if (note < 0 || note > 127) return 0;
    return (s->mask >> pc_of(s, note)) & 1u;
}

int pg_scale_quantize(const pg_scale_t *s, int note) {
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    for (int d = 0; d <= 12; d++) {
        if (pg_scale_contains(s, note - d)) return note - d;   /* ties: down */
        if (pg_scale_contains(s, note + d)) return note + d;
    }
    return note;  /* unreachable: mask is never empty */
}

int pg_scale_degree(const pg_scale_t *s, int note) {
    if (!pg_scale_contains(s, note)) return -1;
    int pc = pc_of(s, note);
    for (int i = 0; i < s->degrees; i++) {
        if (s->offsets[i] == pc) return i;
    }
    return -1;
}

int pg_scale_shift(const pg_scale_t *s, int note, int n) {
    note = pg_scale_quantize(s, note);
    while (n > 0) {
        int next = note + 1;
        while (next <= 127 && !pg_scale_contains(s, next)) next++;
        if (next > 127) break;
        note = next;
        n--;
    }
    while (n < 0) {
        int next = note - 1;
        while (next >= 0 && !pg_scale_contains(s, next)) next--;
        if (next < 0) break;
        note = next;
        n++;
    }
    return note;
}

int pg_scale_degree_near(const pg_scale_t *s, int degree, int near) {
    if (degree < 0 || degree >= s->degrees) return pg_scale_quantize(s, near);
    int pc = (s->root + s->offsets[degree]) % 12;
    int best = -1, best_dist = 1000;
    for (int n = pc; n <= 127; n += 12) {
        int d = n > near ? n - near : near - n;
        if (d < best_dist) { best = n; best_dist = d; }
    }
    return best;
}

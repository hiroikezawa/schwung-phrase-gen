/*
 * Phrase Gen — tiny deterministic RNG (xorshift32) and weighted choice.
 * Deterministic on purpose: a seed reproduces a pattern exactly, which is what
 * makes the host CLI useful for tuning and what a future "Random Seed" knob
 * will set.
 */
#ifndef PG_RNG_H
#define PG_RNG_H

#include <stdint.h>

typedef struct { uint32_t s; } pg_rng_t;

static inline void pg_rng_seed(pg_rng_t *r, uint32_t seed) {
    /* splitmix-style scramble so small seeds (1, 2, 3...) diverge at once */
    seed += 0x9E3779B9u;
    seed ^= seed >> 16; seed *= 0x85EBCA6Bu;
    seed ^= seed >> 13; seed *= 0xC2B2AE35u;
    seed ^= seed >> 16;
    r->s = seed ? seed : 0x6D2B79F5u;
}

static inline uint32_t pg_rng_next(pg_rng_t *r) {
    uint32_t x = r->s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return r->s = x;
}

/* [0, 1) */
static inline float pg_rng_float(pg_rng_t *r) {
    return (float)(pg_rng_next(r) >> 8) * (1.0f / 16777216.0f);
}

/* [lo, hi] inclusive */
static inline int pg_rng_range(pg_rng_t *r, int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + (int)(pg_rng_next(r) % (uint32_t)(hi - lo + 1));
}

static inline int pg_rng_chance(pg_rng_t *r, float p) {
    return pg_rng_float(r) < p;
}

/* Triangular in [-1, 1], peaked at 0 — "a little variation" without libm. */
static inline float pg_rng_tri(pg_rng_t *r) {
    return pg_rng_float(r) - pg_rng_float(r);
}

/* Index drawn with probability proportional to w[i]; -1 if all weights <= 0. */
static inline int pg_rng_weighted(pg_rng_t *r, const float *w, int n) {
    float total = 0.0f;
    for (int i = 0; i < n; i++) if (w[i] > 0.0f) total += w[i];
    if (total <= 0.0f) return -1;
    float x = pg_rng_float(r) * total;
    int last = -1;
    for (int i = 0; i < n; i++) {
        if (w[i] <= 0.0f) continue;
        last = i;
        if (x < w[i]) return i;
        x -= w[i];
    }
    return last;   /* float rounding */
}

#endif /* PG_RNG_H */

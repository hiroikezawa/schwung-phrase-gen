/*
 * Phrase Gen — ScaleQuantizer.
 *
 * A scale is a root pitch class plus a 12-bit mask of allowed pitch classes
 * relative to that root.  Everything that produces a pitch goes through
 * pg_scale_quantize() last, so nothing out of scale can reach the output.
 * Root / scale changes later are just a different pg_scale_t.
 */
#ifndef PG_SCALE_H
#define PG_SCALE_H

#include <stdint.h>

#define PG_MASK_PHRYGIAN  0x5ABu  /* 0 1 3 5 7 8 10  -> C Db Eb F G Ab Bb */

typedef struct {
    int      root;          /* pitch class 0..11 (0 = C) */
    uint16_t mask;          /* bit n set = (root + n) % 12 allowed */
    int      degrees;       /* popcount(mask) */
    int8_t   offsets[12];   /* semitone offset of each degree, ascending */
} pg_scale_t;

void pg_scale_init(pg_scale_t *s, int root, uint16_t mask);

/* 1 if the MIDI note is in the scale. */
int  pg_scale_contains(const pg_scale_t *s, int note);

/* Nearest in-scale note, result always within 0..127 (ties resolve DOWN). */
int  pg_scale_quantize(const pg_scale_t *s, int note);

/* Scale degree (0..degrees-1) of an in-scale note, or -1. */
int  pg_scale_degree(const pg_scale_t *s, int note);

/* Move an in-scale note by `n` scale steps (+ up, - down), clamped to 0..127
 * and always in scale. */
int  pg_scale_shift(const pg_scale_t *s, int note, int n);

/* The in-scale note of `degree` whose value is closest to `near`. */
int  pg_scale_degree_near(const pg_scale_t *s, int degree, int near);

#endif /* PG_SCALE_H */

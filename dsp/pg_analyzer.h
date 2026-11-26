/*
 * Phrase Gen — PatternAnalyzer.
 *
 * Turns one or more source patterns into the statistics the generator draws
 * from.  Run once per source (a "source profile") and once over all of them
 * (the "corpus"); the generator mixes the two, so a variation keeps the
 * chosen source's character while borrowing moves the other sources make.
 *
 * Transitions (interval, repeat, up/down, Markov) are measured between
 * CONSECUTIVE SOUNDING notes, rests skipped, and include the wrap from the
 * last note back to the first, because the pattern loops.
 */
#ifndef PG_ANALYZER_H
#define PG_ANALYZER_H

#include "pg_pattern.h"
#include "pg_scale.h"

#define PG_INTERVAL_MAX   60                       /* clamp, semitones */
#define PG_INTERVAL_BINS  (2 * PG_INTERVAL_MAX + 1)
#define PG_OCTAVES        11                       /* note / 12 */

typedef struct {
    int   n_patterns;
    int   length;                       /* steps analysed (max of inputs) */

    /* rhythm */
    float onset_prob[PG_MAX_STEPS];     /* P(note at step) */
    float count_mean;
    int   count_min, count_max;

    /* pitch */
    float pc_hist[12];                  /* by absolute pitch class, sums to 1 */
    float octave_hist[PG_OCTAVES];      /* sums to 1 */
    int   note_min, note_max;

    /* movement */
    float interval_hist[PG_INTERVAL_BINS];   /* index = interval + MAX */
    float markov[12][12];               /* scale degree -> next degree, rows sum to 1 */
    float repeat_rate;                  /* same pitch as previous note */
    float up_rate, down_rate;           /* shares of all transitions */
    float leap_rate;                    /* |interval| >= 12 */
    int   transitions;

    /* dynamics */
    float vel_mean, vel_dev;            /* mean, mean absolute deviation */
    int   vel_min, vel_max;
    float gate_mean;
    int   gate_min, gate_max;
} pg_stats_t;

void pg_analyze(const pg_pattern_t *pats, int n, const pg_scale_t *scale,
                pg_stats_t *out);

#endif /* PG_ANALYZER_H */

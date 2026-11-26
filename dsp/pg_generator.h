/*
 * Phrase Gen — PatternGenerator.
 *
 * "Same author, new phrase": pick ONE source pattern and mutate it, rather
 * than sampling from corpus averages.  The source supplies the skeleton —
 * rhythm, density, register — and the statistics (source profile mixed with
 * the corpus) decide what each change looks like.
 *
 * `mutation` (0..1) scales every stage at once, so it can sit on one knob:
 *
 *   0.0   the source verbatim (scale-quantized)
 *   0.5   default: ~75% of the rhythm kept, about half the notes changed
 *   1.0   bold: density +-2, most pitches moved, wider velocity / gate swing
 *
 * Stages, each its own function so later features slot in beside them:
 *   rhythm   delete / add / nudge notes, density bounded by the source
 *   pitch    scale-step shift, octave jump, repeat previous, Markov resample
 *   dynamics velocity and gate variation
 *   finish   scale-quantize, fold into the corpus register, dedupe
 *
 * Everything is bounded loops over <= PG_MAX_STEPS steps: safe on the SPI
 * callback.
 */
#ifndef PG_GENERATOR_H
#define PG_GENERATOR_H

#include "pg_pattern.h"
#include "pg_scale.h"
#include "pg_analyzer.h"
#include "pg_rng.h"

typedef struct {
    const pg_pattern_t *sources;
    const pg_stats_t   *source_stats;   /* one per source */
    const pg_stats_t   *corpus;
    int                 n_sources;
    const pg_scale_t   *scale;
} pg_model_t;

typedef struct {
    float mutation;         /* 0..1 */
    int   source;           /* index, or -1 = pick at random */
    int   avoid_source;     /* when picking at random, re-roll this once (-1: none) */
    int   allow_rests;      /* 0 (default): every step plays — rests in the source
                               are filled and no note is ever deleted */
} pg_gen_params_t;

typedef struct {
    int source;             /* index actually used */
    int notes_src, notes_out;
    int added, deleted, moved;
    int pitch_changed;      /* notes on an original step whose pitch changed */
    int rhythm_kept;        /* steps whose on/rest state matches the source */
} pg_gen_report_t;

/* Writes a new pattern to `out`.  `avoid` (may be NULL) is the previous
 * pattern; the result differs from it whenever mutation > 0.  Returns the
 * source index used, or -1 if the model has no sources. */
int pg_generate(const pg_model_t *m, const pg_gen_params_t *p, pg_rng_t *rng,
                const pg_pattern_t *avoid, pg_pattern_t *out,
                pg_gen_report_t *report);

#endif /* PG_GENERATOR_H */

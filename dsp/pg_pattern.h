/*
 * Phrase Gen — shared pattern types.
 *
 * Every module in this directory speaks in pg_pattern_t: a monophonic step
 * grid, one 16th note per step.  Nothing here allocates; every array is fixed
 * size so the whole engine can run on the SPI callback (see the threading
 * contract at the top of src/host/plugin_api_v1.h).
 */
#ifndef PG_PATTERN_H
#define PG_PATTERN_H

#include <stdint.h>

/* Longest pattern any stage can hold.  The sequencer plays pattern.length
 * steps, so 8 / 16 / 32-step modes only need a length change, not a resize. */
#define PG_MAX_STEPS        32
#define PG_DEFAULT_STEPS    8
#define PG_STEPS_PER_BEAT   4       /* 1 step = one 16th note */
#define PG_MAX_SOURCES      16
#define PG_NAME_LEN         32

#define PG_REST             (-1)

/* Gate is a percentage of one step: 100 = exactly one 16th, 50 = staccato,
 * 200 = ties over the next step.  Kept integral so patterns compare exactly. */
#define PG_GATE_MIN         5
#define PG_GATE_MAX         400

typedef struct {
    int8_t   note;      /* 0..127, or PG_REST */
    uint8_t  vel;       /* 1..127 when note >= 0 */
    uint16_t gate;      /* PG_GATE_MIN..PG_GATE_MAX when note >= 0 */
} pg_step_t;

typedef struct {
    pg_step_t steps[PG_MAX_STEPS];
    int       length;   /* 1..PG_MAX_STEPS */
} pg_pattern_t;

static inline int pg_step_is_note(const pg_step_t *s) { return s->note >= 0; }

static inline int pg_pattern_note_count(const pg_pattern_t *p) {
    int n = 0;
    for (int i = 0; i < p->length; i++) n += pg_step_is_note(&p->steps[i]);
    return n;
}

static inline void pg_pattern_clear(pg_pattern_t *p, int length) {
    if (length < 1) length = 1;
    if (length > PG_MAX_STEPS) length = PG_MAX_STEPS;
    for (int i = 0; i < PG_MAX_STEPS; i++) {
        p->steps[i].note = PG_REST;
        p->steps[i].vel = 0;
        p->steps[i].gate = 0;
    }
    p->length = length;
}

static inline int pg_pattern_equal(const pg_pattern_t *a, const pg_pattern_t *b) {
    if (a->length != b->length) return 0;
    for (int i = 0; i < a->length; i++) {
        const pg_step_t *x = &a->steps[i], *y = &b->steps[i];
        if (x->note != y->note) return 0;
        if (x->note >= 0 && (x->vel != y->vel || x->gate != y->gate)) return 0;
    }
    return 1;
}

#endif /* PG_PATTERN_H */

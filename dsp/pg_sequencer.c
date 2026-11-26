/* Phrase Gen — Sequencer.  See pg_sequencer.h. */
#include "pg_sequencer.h"

#include <string.h>

/* Transport position -> absolute 16th index.  beat >= 0 here, so truncation
 * is floor; the epsilon keeps an exact boundary from reading as the step
 * before it through float noise. */
static long long abs_step_of(double beat) {
    return (long long)(beat * PG_STEPS_PER_BEAT + 1e-9);
}

static void emit(uint8_t out[][3], int out_lens[], int *count,
                 uint8_t status, uint8_t d1, uint8_t d2) {
    out[*count][0] = status;
    out[*count][1] = d1;
    out[*count][2] = d2;
    out_lens[*count] = 3;
    (*count)++;
}

static int release_voice(pg_voice_t *v, uint8_t out[][3], int out_lens[],
                         int *count, int max_out) {
    if (!v->active) return 1;
    if (*count >= max_out) return 0;            /* stays owed until next tick */
    emit(out, out_lens, count, (uint8_t)(0x80 | (v->channel & 0x0F)), v->note, 0);
    v->active = 0;
    return 1;
}

static void release_all(pg_seq_t *q, uint8_t out[][3], int out_lens[],
                        int *count, int max_out) {
    int done = 1;
    for (int i = 0; i < PG_MAX_VOICES; i++)
        done &= release_voice(&q->voices[i], out, out_lens, count, max_out);
    if (done) q->flush_requested = 0;
    else q->flush_requested = 1;
}

void pg_seq_init(pg_seq_t *q, const pg_pattern_t *initial) {
    memset(q, 0, sizeof(*q));
    if (initial) q->current = *initial;
    else pg_pattern_clear(&q->current, PG_DEFAULT_STEPS);
    q->last_abs_step = -1;
    q->play_step = -1;
}

void pg_seq_queue(pg_seq_t *q, const pg_pattern_t *next) {
    if (!next || next->length < 1 || next->length > PG_MAX_STEPS) return;
    if (!q->running) {
        q->current = *next;
        q->has_pending = 0;
        return;
    }
    q->pending = *next;
    q->has_pending = 1;
}

const pg_pattern_t *pg_seq_latest(const pg_seq_t *q) {
    return q->has_pending ? &q->pending : &q->current;
}

void pg_seq_flush(pg_seq_t *q) {
    q->flush_requested = 1;
}

int pg_seq_active_voices(const pg_seq_t *q) {
    int n = 0;
    for (int i = 0; i < PG_MAX_VOICES; i++) n += q->voices[i].active;
    return n;
}

/* Fire one step.  Needs up to two output slots (retrigger off + on). */
static void fire_step(pg_seq_t *q, long long abs_step, uint8_t out[][3],
                      int out_lens[], int *count, int max_out) {
    int step = (int)(abs_step % q->current.length);
    q->play_step = step;
    const pg_step_t *st = &q->current.steps[step];
    if (!pg_step_is_note(st)) return;

    /* Same pitch still sounding (a long gate): end it first so the synth
     * sees off-then-on, never two ons for one key. */
    pg_voice_t *slot = NULL;
    for (int i = 0; i < PG_MAX_VOICES; i++) {
        pg_voice_t *v = &q->voices[i];
        if (v->active && v->note == (uint8_t)st->note) {
            if (!release_voice(v, out, out_lens, count, max_out)) return;
            slot = v;
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < PG_MAX_VOICES && !slot; i++)
            if (!q->voices[i].active) slot = &q->voices[i];
    }
    if (!slot) {                                /* all busy: steal the oldest */
        slot = &q->voices[0];
        for (int i = 1; i < PG_MAX_VOICES; i++)
            if (q->voices[i].start_step < slot->start_step) slot = &q->voices[i];
        if (!release_voice(slot, out, out_lens, count, max_out)) return;
    }
    if (*count >= max_out) return;              /* no room: skip the note, never half-send */

    emit(out, out_lens, count, (uint8_t)(0x90 | (q->channel & 0x0F)),
         (uint8_t)st->note, st->vel ? st->vel : 1);
    slot->active = 1;
    slot->note = (uint8_t)st->note;
    slot->channel = q->channel;
    slot->start_step = abs_step;
    slot->off_beat = ((double)abs_step + (double)st->gate / 100.0) / PG_STEPS_PER_BEAT;
}

int pg_seq_tick(pg_seq_t *q, double beat, uint8_t out[][3], int out_lens[], int max_out) {
    int count = 0;
    if (max_out <= 0) return 0;

    if (q->flush_requested) release_all(q, out, out_lens, &count, max_out);

    if (beat < 0.0) {                           /* transport stopped */
        if (q->running) {
            q->running = 0;
            q->last_abs_step = -1;
            q->play_step = -1;
        }
        release_all(q, out, out_lens, &count, max_out);
        if (q->has_pending) {                   /* nothing playing: take it now */
            q->current = q->pending;
            q->has_pending = 0;
        }
        return count;
    }

    long long abs_step = abs_step_of(beat);

    /* Transport jumped backwards (restart without a Stop, loop, relocate).
     * The voices' off-beats belong to the old timeline — release them now
     * or they would ring until the transport caught up again. */
    if (q->running && q->last_abs_step >= 0 && abs_step < q->last_abs_step) {
        release_all(q, out, out_lens, &count, max_out);
        q->last_abs_step = -1;
    }
    q->running = 1;

    /* 1) Fire a new step first: it is the timing-critical message. */
    int fired = 0;
    if (abs_step != q->last_abs_step) {
        /* A queued pattern swaps on a loop boundary.  If ticks were late
         * and skipped steps, the boundary may lie among the skipped ones. */
        if (q->has_pending) {
            long long from = q->last_abs_step < 0 ? abs_step : q->last_abs_step + 1;
            if (abs_step - from > 64) from = abs_step - 64;
            for (long long s = from; s <= abs_step; s++) {
                if (s % q->current.length == 0) {
                    q->current = q->pending;
                    q->has_pending = 0;
                    break;
                }
            }
        }
        q->last_abs_step = abs_step;
        fire_step(q, abs_step, out, out_lens, &count, max_out);
        fired = 1;
    }

    /* 2) Note-offs that are due.  A voice started in THIS call waits for the
     * next one even if its gate is shorter than a block, so the synth never
     * sees a zero-length note. */
    for (int i = 0; i < PG_MAX_VOICES; i++) {
        pg_voice_t *v = &q->voices[i];
        if (v->active && v->off_beat <= beat && !(fired && v->start_step == abs_step))
            release_voice(v, out, out_lens, &count, max_out);
    }
    return count;
}

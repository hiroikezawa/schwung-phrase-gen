/*
 * Phrase Gen — Sequencer.
 *
 * Plays a pg_pattern_t locked to the host transport.  It owns no clock: every
 * call is handed the transport position in beats (host->get_beat_position),
 * and a step fires when that position crosses a 16th-note boundary.  Step
 * numbering is anchored to the transport's beat 0, so an 8-step pattern
 * always restarts on a half-bar and a queued pattern always lands on its
 * first step.
 *
 * Stuck-note safety is structural: every note-on takes a voice slot that
 * remembers pitch, channel and the beat it must end at, and every path that
 * can strand a note (stop, a transport jump backwards, a flush) walks the
 * voice table.  Messages that do not fit in one call's output buffer stay
 * owed and go out on the next call.
 */
#ifndef PG_SEQUENCER_H
#define PG_SEQUENCER_H

#include <stdint.h>
#include "pg_pattern.h"

#define PG_MAX_VOICES 16

typedef struct {
    uint8_t active;
    uint8_t note;
    uint8_t channel;
    double  off_beat;       /* transport beat at which the note-off is due */
    long long start_step;   /* for stealing the oldest */
} pg_voice_t;

typedef struct {
    pg_pattern_t current;
    pg_pattern_t pending;
    int          has_pending;

    int          running;
    long long    last_abs_step;     /* last fired absolute 16th, -1 = none */
    int          play_step;         /* step index last fired, -1 = none */
    int          flush_requested;   /* release every voice ASAP */
    uint8_t      channel;           /* output channel for new notes */

    pg_voice_t   voices[PG_MAX_VOICES];
} pg_seq_t;

void pg_seq_init(pg_seq_t *q, const pg_pattern_t *initial);

/* Replace the playing pattern at the start of the next loop, or immediately
 * if the transport is not running.  Queuing again before that replaces the
 * queued one (the newest press wins). */
void pg_seq_queue(pg_seq_t *q, const pg_pattern_t *next);

/* The pattern that will be heard next: the queued one if any, else current. */
const pg_pattern_t *pg_seq_latest(const pg_seq_t *q);

/* Ask for every sounding note to be released on the next tick. */
void pg_seq_flush(pg_seq_t *q);

/* Advance to transport position `beat` (< 0 = transport stopped) and write
 * due MIDI into out.  Returns the number of messages written. */
int  pg_seq_tick(pg_seq_t *q, double beat, uint8_t out[][3], int out_lens[], int max_out);

int  pg_seq_active_voices(const pg_seq_t *q);

#endif /* PG_SEQUENCER_H */

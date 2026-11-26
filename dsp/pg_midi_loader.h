/*
 * Phrase Gen — MidiPatternLoader.
 *
 * Two separate stages, so a later version can pick any window of a long file:
 *
 *   pg_midi_parse()    Standard MIDI File bytes -> flat list of note events
 *                      (absolute ticks, length, pitch, velocity).  All tracks
 *                      and channels are merged.
 *   pg_midi_extract()  note events -> pg_pattern_t, reading `n_steps` 16th
 *                      steps starting at `start_step`.
 *
 * Both work on caller-owned fixed-size memory: no allocation, no file I/O.
 * The bytes come from patterns embedded at build time (see
 * scripts/embed_patterns.py), or from a file the host CLI read for us.
 * Malformed input is rejected with a negative return, never a crash.
 */
#ifndef PG_MIDI_LOADER_H
#define PG_MIDI_LOADER_H

#include <stddef.h>
#include <stdint.h>
#include "pg_pattern.h"

#define PG_MIDI_MAX_EVENTS  256

typedef struct {
    uint32_t start;     /* absolute tick */
    uint32_t length;    /* ticks */
    uint8_t  note;
    uint8_t  vel;
} pg_note_event_t;

typedef struct {
    int             ppq;            /* ticks per quarter note */
    int             count;
    int             truncated;      /* 1 if more than PG_MIDI_MAX_EVENTS notes */
    pg_note_event_t events[PG_MIDI_MAX_EVENTS];
} pg_midi_file_t;

enum {
    PG_MIDI_OK          = 0,
    PG_MIDI_E_HEADER    = -1,   /* not an SMF / bad MThd */
    PG_MIDI_E_SMPTE     = -2,   /* SMPTE time division is not supported */
    PG_MIDI_E_TRACK     = -3,   /* malformed track chunk */
};

int pg_midi_parse(const uint8_t *data, size_t len, pg_midi_file_t *out);

/* Place notes onto the step grid.  A note belongs to the step whose start is
 * nearest its start tick; if several land on one step the loudest (then the
 * earliest) wins, because the generator is monophonic.  Returns the number of
 * notes placed. */
int pg_midi_extract(const pg_midi_file_t *mf, int start_step, int n_steps,
                    pg_pattern_t *out);

#endif /* PG_MIDI_LOADER_H */

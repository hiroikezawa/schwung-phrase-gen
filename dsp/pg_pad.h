/*
 * Phrase Gen — PadController.
 *
 * A MIDI FX never sees Move's pads directly: Move turns a press into a note
 * on the track's channel (its own pad->note layout) and the chain hands that
 * note to us.  So "the GENERATE pad" is a NOTE, and this decides what each
 * incoming note means:
 *
 *   any    every pad generates; no pad note reaches the synth (default)
 *   note   only `trig_note` generates; every other pad plays through
 *   learn  the next pad pressed becomes `trig_note` (and generates); the
 *          mode then switches itself to `note`
 *
 * Note-offs follow their note-on, not the CURRENT mode: a key that was
 * passed through always gets its note-off passed through, even if the mode
 * or trigger note changed while it was held.  That is what keeps a mode
 * change from stranding a note in the synth.
 */
#ifndef PG_PAD_H
#define PG_PAD_H

#include <stdint.h>

typedef enum { PG_TRIG_ANY = 0, PG_TRIG_NOTE, PG_TRIG_LEARN, PG_TRIG_COUNT } pg_trig_mode_t;

typedef enum {
    PG_PAD_PASS = 0,        /* forward the message unchanged */
    PG_PAD_SWALLOW,         /* drop it */
    PG_PAD_GENERATE,        /* drop it and generate a new pattern */
} pg_pad_action_t;

typedef struct {
    pg_trig_mode_t mode;
    int            trig_note;           /* 0..127 */
    uint8_t        passed[128];         /* note-on forwarded, note-off owed */
} pg_pad_t;

void            pg_pad_init(pg_pad_t *p);
pg_pad_action_t pg_pad_on_note(pg_pad_t *p, int is_note_on, int note);

const char     *pg_trig_mode_name(pg_trig_mode_t m);
int             pg_trig_mode_parse(const char *s);    /* -1 if unknown */

#endif /* PG_PAD_H */

/* Phrase Gen — PadController.  See pg_pad.h. */
#include "pg_pad.h"

#include <string.h>

static const char *const k_mode_names[PG_TRIG_COUNT] = { "any", "note", "learn" };

void pg_pad_init(pg_pad_t *p) {
    memset(p, 0, sizeof(*p));
    p->mode = PG_TRIG_ANY;
    p->trig_note = 36;
}

pg_pad_action_t pg_pad_on_note(pg_pad_t *p, int is_note_on, int note) {
    note &= 0x7F;

    if (!is_note_on) {
        if (p->passed[note]) {
            p->passed[note] = 0;
            return PG_PAD_PASS;
        }
        return PG_PAD_SWALLOW;
    }

    switch (p->mode) {
        case PG_TRIG_LEARN:
            p->trig_note = note;
            p->mode = PG_TRIG_NOTE;
            return PG_PAD_GENERATE;
        case PG_TRIG_NOTE:
            if (note == p->trig_note) return PG_PAD_GENERATE;
            p->passed[note] = 1;
            return PG_PAD_PASS;
        case PG_TRIG_ANY:
        default:
            return PG_PAD_GENERATE;
    }
}

const char *pg_trig_mode_name(pg_trig_mode_t m) {
    return (m >= 0 && m < PG_TRIG_COUNT) ? k_mode_names[m] : "any";
}

int pg_trig_mode_parse(const char *s) {
    if (!s) return -1;
    for (int i = 0; i < PG_TRIG_COUNT; i++)
        if (strcmp(s, k_mode_names[i]) == 0) return i;
    return -1;
}

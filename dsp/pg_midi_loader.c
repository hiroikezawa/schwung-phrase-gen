/* Phrase Gen — MidiPatternLoader.  See pg_midi_loader.h. */
#include "pg_midi_loader.h"

#include <string.h>

#define PG_MAX_OPEN_NOTES 128   /* one per pitch; overlapping same-pitch ons
                                   close the previous one */

static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t rd_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* Variable-length quantity; returns 0 on overrun or a >4-byte value. */
static int rd_vlq(const uint8_t *p, size_t end, size_t *pos, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (*pos >= end) return 0;
        uint8_t b = p[(*pos)++];
        v = (v << 7) | (b & 0x7Fu);
        if (!(b & 0x80u)) { *out = v; return 1; }
    }
    return 0;
}

typedef struct {
    int      open;
    uint32_t start;
    uint8_t  vel;
} open_note_t;

static void close_note(pg_midi_file_t *out, open_note_t *on, uint8_t note,
                       uint32_t tick) {
    if (!on->open) return;
    on->open = 0;
    if (out->count >= PG_MIDI_MAX_EVENTS) { out->truncated = 1; return; }
    pg_note_event_t *e = &out->events[out->count++];
    e->start = on->start;
    e->length = tick > on->start ? tick - on->start : 1;
    e->note = note;
    e->vel = on->vel;
}

static int parse_track(const uint8_t *d, size_t start, size_t end,
                       pg_midi_file_t *out) {
    open_note_t open[PG_MAX_OPEN_NOTES];
    memset(open, 0, sizeof(open));
    size_t pos = start;
    uint32_t tick = 0;
    uint8_t running = 0;

    while (pos < end) {
        uint32_t delta;
        if (!rd_vlq(d, end, &pos, &delta)) return PG_MIDI_E_TRACK;
        tick += delta;
        if (pos >= end) return PG_MIDI_E_TRACK;

        uint8_t status = d[pos];
        if (status & 0x80u) {
            pos++;
        } else {
            if (!running) return PG_MIDI_E_TRACK;   /* data byte, no status */
            status = running;
        }

        if (status == 0xFF) {                       /* meta */
            if (pos >= end) return PG_MIDI_E_TRACK;
            uint8_t type = d[pos++];
            uint32_t len;
            if (!rd_vlq(d, end, &pos, &len) || len > end - pos) return PG_MIDI_E_TRACK;
            pos += len;
            if (type == 0x2F) break;                /* end of track */
            continue;
        }
        if (status == 0xF0 || status == 0xF7) {     /* sysex */
            uint32_t len;
            if (!rd_vlq(d, end, &pos, &len) || len > end - pos) return PG_MIDI_E_TRACK;
            pos += len;
            running = 0;
            continue;
        }
        if (status >= 0xF0) return PG_MIDI_E_TRACK; /* realtime/common: not in SMF */

        running = status;
        uint8_t type = status & 0xF0u;
        int nbytes = (type == 0xC0 || type == 0xD0) ? 1 : 2;
        if (end - pos < (size_t)nbytes) return PG_MIDI_E_TRACK;
        uint8_t a = d[pos] & 0x7Fu;
        uint8_t b = nbytes > 1 ? (d[pos + 1] & 0x7Fu) : 0;
        pos += (size_t)nbytes;

        if (type == 0x90 && b > 0) {
            close_note(out, &open[a], a, tick);     /* retrigger ends the old one */
            open[a].open = 1;
            open[a].start = tick;
            open[a].vel = b;
        } else if (type == 0x80 || type == 0x90) {
            close_note(out, &open[a], a, tick);
        }
    }

    /* Notes still open at the end of the track end there. */
    for (int n = 0; n < PG_MAX_OPEN_NOTES; n++) close_note(out, &open[n], (uint8_t)n, tick);
    return PG_MIDI_OK;
}

int pg_midi_parse(const uint8_t *data, size_t len, pg_midi_file_t *out) {
    if (!out) return PG_MIDI_E_HEADER;
    out->ppq = 0;
    out->count = 0;
    out->truncated = 0;
    if (!data || len < 14 || memcmp(data, "MThd", 4) != 0) return PG_MIDI_E_HEADER;

    uint32_t hlen = rd_be32(data + 4);
    if (hlen < 6 || hlen > len - 8) return PG_MIDI_E_HEADER;
    uint16_t ntracks = rd_be16(data + 10);
    uint16_t division = rd_be16(data + 12);
    if (division & 0x8000u) return PG_MIDI_E_SMPTE;
    if (division == 0) return PG_MIDI_E_HEADER;
    out->ppq = division;

    size_t pos = 8 + (size_t)hlen;
    int tracks_seen = 0;
    while (pos + 8 <= len && tracks_seen < ntracks) {
        uint32_t clen = rd_be32(data + pos + 4);
        size_t cstart = pos + 8;
        if (clen > len - cstart) return PG_MIDI_E_TRACK;
        if (memcmp(data + pos, "MTrk", 4) == 0) {
            int rc = parse_track(data, cstart, cstart + clen, out);
            if (rc != PG_MIDI_OK) return rc;
            tracks_seen++;
        }
        pos = cstart + clen;                        /* skip unknown chunks */
    }
    if (tracks_seen == 0) return PG_MIDI_E_TRACK;

    /* Tracks are appended one after another; order by start for extract(). */
    for (int i = 1; i < out->count; i++) {
        pg_note_event_t e = out->events[i];
        int j = i - 1;
        while (j >= 0 && out->events[j].start > e.start) {
            out->events[j + 1] = out->events[j];
            j--;
        }
        out->events[j + 1] = e;
    }
    return PG_MIDI_OK;
}

int pg_midi_extract(const pg_midi_file_t *mf, int start_step, int n_steps,
                    pg_pattern_t *out) {
    pg_pattern_clear(out, n_steps);
    if (!mf || mf->ppq <= 0) return 0;
    if (start_step < 0) start_step = 0;

    const uint32_t step_ticks = (uint32_t)(mf->ppq / PG_STEPS_PER_BEAT) > 0
                              ? (uint32_t)(mf->ppq / PG_STEPS_PER_BEAT) : 1;
    int placed = 0;
    for (int i = 0; i < mf->count; i++) {
        const pg_note_event_t *e = &mf->events[i];
        /* nearest step: floor((start + half) / step) */
        int abs_step = (int)((e->start + step_ticks / 2) / step_ticks);
        int s = abs_step - start_step;
        if (s < 0 || s >= out->length) continue;

        pg_step_t *st = &out->steps[s];
        if (st->note >= 0 && st->vel >= e->vel) continue;   /* keep loudest */
        if (st->note < 0) placed++;

        uint32_t gate = (e->length * 100u + step_ticks / 2) / step_ticks;
        if (gate < PG_GATE_MIN) gate = PG_GATE_MIN;
        if (gate > PG_GATE_MAX) gate = PG_GATE_MAX;
        st->note = (int8_t)e->note;
        st->vel = e->vel ? e->vel : 1;
        st->gate = (uint16_t)gate;
    }
    return placed;
}

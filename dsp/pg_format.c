/* Phrase Gen — text forms of a pattern.  See pg_format.h. */
#include "pg_format.h"

#include <stdio.h>

static const char *const k_names[12] = {
    "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"
};

void pg_note_name(int note, char *buf, int buf_len) {
    if (note < 0 || note > 127) { snprintf(buf, (size_t)buf_len, "REST"); return; }
    snprintf(buf, (size_t)buf_len, "%s%d", k_names[note % 12], note / 12 - 2);
}

static int clip(int n, int buf_len) {
    if (n < 0) return 0;
    return n >= buf_len ? buf_len - 1 : n;
}

int pg_format_pattern(const pg_pattern_t *p, const char *source_name,
                      char *buf, int buf_len) {
    if (!buf || buf_len < 1) return 0;
    int n = clip(snprintf(buf, (size_t)buf_len, "Source: %s\nGenerated:\n",
                          source_name ? source_name : "?"), buf_len);
    for (int s = 0; s < p->length && n < buf_len - 1; s++) {
        const pg_step_t *st = &p->steps[s];
        if (pg_step_is_note(st)) {
            char name[8];
            pg_note_name(st->note, name, sizeof(name));
            n += clip(snprintf(buf + n, (size_t)(buf_len - n),
                               "Step %d: %s vel=%d len=%d%%\n",
                               s + 1, name, st->vel, st->gate), buf_len - n);
        } else {
            n += clip(snprintf(buf + n, (size_t)(buf_len - n), "Step %d: REST\n", s + 1),
                      buf_len - n);
        }
    }
    return n;
}

int pg_encode_pattern(const pg_pattern_t *p, char *buf, int buf_len) {
    if (!buf || buf_len < 1) return 0;
    int n = clip(snprintf(buf, (size_t)buf_len, "%d|", p->length), buf_len);
    for (int s = 0; s < p->length && n < buf_len - 1; s++) {
        const pg_step_t *st = &p->steps[s];
        const char *sep = s + 1 < p->length ? "," : "";
        if (pg_step_is_note(st))
            n += clip(snprintf(buf + n, (size_t)(buf_len - n), "%d.%d.%d%s",
                               st->note, st->vel, st->gate, sep), buf_len - n);
        else
            n += clip(snprintf(buf + n, (size_t)(buf_len - n), "-%s", sep), buf_len - n);
    }
    return n;
}

/* Non-negative decimal; returns chars consumed, 0 if none. */
static int read_uint(const char *s, int *out) {
    int i = 0, v = 0;
    while (s[i] >= '0' && s[i] <= '9' && i < 6) v = v * 10 + (s[i++] - '0');
    *out = v;
    return i;
}

int pg_decode_pattern(const char *s, pg_pattern_t *out) {
    if (!s) return 0;
    int len, k = read_uint(s, &len);
    if (!k || s[k] != '|' || len < 1 || len > PG_MAX_STEPS) return 0;
    s += k + 1;

    pg_pattern_t p;
    pg_pattern_clear(&p, len);
    for (int step = 0; step < len; step++) {
        if (*s == '-') {
            s++;
        } else {
            int note, vel, gate;
            if (!(k = read_uint(s, &note)) || s[k] != '.') return 0;
            s += k + 1;
            if (!(k = read_uint(s, &vel)) || s[k] != '.') return 0;
            s += k + 1;
            if (!(k = read_uint(s, &gate))) return 0;
            s += k;
            if (note > 127 || vel < 1 || vel > 127 || gate < PG_GATE_MIN || gate > PG_GATE_MAX) return 0;
            p.steps[step].note = (int8_t)note;
            p.steps[step].vel = (uint8_t)vel;
            p.steps[step].gate = (uint16_t)gate;
        }
        if (step + 1 < len) {
            if (*s != ',') return 0;
            s++;
        }
    }
    *out = p;
    return 1;
}

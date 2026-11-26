/* Phrase Gen — PatternAnalyzer.  See pg_analyzer.h. */
#include "pg_analyzer.h"

#include <string.h>

static void normalize(float *v, int n) {
    float t = 0.0f;
    for (int i = 0; i < n; i++) t += v[i];
    if (t <= 0.0f) return;
    for (int i = 0; i < n; i++) v[i] /= t;
}

void pg_analyze(const pg_pattern_t *pats, int n, const pg_scale_t *scale,
                pg_stats_t *out) {
    memset(out, 0, sizeof(*out));
    out->n_patterns = n;
    out->count_min = PG_MAX_STEPS;
    out->note_min = 127;
    out->vel_min = 127;
    out->gate_min = PG_GATE_MAX;

    int notes_total = 0;
    long vel_sum = 0, gate_sum = 0;
    int onset_n[PG_MAX_STEPS] = {0};
    float markov_raw[12][12];
    memset(markov_raw, 0, sizeof(markov_raw));
    int ups = 0, downs = 0, repeats = 0, leaps = 0;

    for (int p = 0; p < n; p++) {
        const pg_pattern_t *pat = &pats[p];
        if (pat->length > out->length) out->length = pat->length;

        int count = 0;
        int seq[PG_MAX_STEPS];          /* sounding notes in time order */
        for (int s = 0; s < pat->length; s++) {
            const pg_step_t *st = &pat->steps[s];
            if (!pg_step_is_note(st)) continue;
            seq[count++] = st->note;
            onset_n[s]++;

            out->pc_hist[st->note % 12] += 1.0f;
            out->octave_hist[st->note / 12] += 1.0f;
            if (st->note < out->note_min) out->note_min = st->note;
            if (st->note > out->note_max) out->note_max = st->note;
            vel_sum += st->vel;
            gate_sum += st->gate;
            if (st->vel < out->vel_min) out->vel_min = st->vel;
            if (st->vel > out->vel_max) out->vel_max = st->vel;
            if (st->gate < out->gate_min) out->gate_min = st->gate;
            if (st->gate > out->gate_max) out->gate_max = st->gate;
        }
        notes_total += count;
        if (count < out->count_min) out->count_min = count;
        if (count > out->count_max) out->count_max = count;

        /* transitions, including the loop wrap (only if >1 note) */
        for (int i = 0; count > 1 && i < count; i++) {
            int a = seq[i], b = seq[(i + 1) % count];
            int iv = b - a;
            if (iv > PG_INTERVAL_MAX) iv = PG_INTERVAL_MAX;
            if (iv < -PG_INTERVAL_MAX) iv = -PG_INTERVAL_MAX;
            out->interval_hist[iv + PG_INTERVAL_MAX] += 1.0f;
            if (iv == 0) repeats++;
            else if (iv > 0) ups++;
            else downs++;
            if (iv >= 12 || iv <= -12) leaps++;
            out->transitions++;

            int da = pg_scale_degree(scale, a), db = pg_scale_degree(scale, b);
            if (da >= 0 && db >= 0) markov_raw[da][db] += 1.0f;
        }
    }

    if (n > 0) out->count_mean = (float)notes_total / (float)n;
    for (int s = 0; s < PG_MAX_STEPS; s++)
        out->onset_prob[s] = n > 0 ? (float)onset_n[s] / (float)n : 0.0f;

    if (notes_total == 0) {                     /* degenerate: keep it usable */
        out->count_min = 0;
        out->note_min = 48; out->note_max = 72;
        out->vel_min = out->vel_max = 100; out->vel_mean = 100.0f;
        out->gate_min = out->gate_max = 90; out->gate_mean = 90.0f;
        return;
    }

    normalize(out->pc_hist, 12);
    normalize(out->octave_hist, PG_OCTAVES);
    normalize(out->interval_hist, PG_INTERVAL_BINS);

    /* Markov rows: observed counts plus a small smoothing mass, so a degree
     * never seen as a "from" still has somewhere to go.  Smoothing favours
     * stepwise neighbours over random leaps. */
    int deg = scale->degrees;
    for (int a = 0; a < deg; a++) {
        for (int b = 0; b < deg; b++) {
            int dist = a > b ? a - b : b - a;
            if (dist > deg / 2) dist = deg - dist;
            float smooth = dist <= 1 ? 0.15f : 0.05f;
            out->markov[a][b] = markov_raw[a][b] + smooth;
        }
        normalize(out->markov[a], deg);
    }

    if (out->transitions > 0) {
        float t = (float)out->transitions;
        out->repeat_rate = (float)repeats / t;
        out->up_rate = (float)ups / t;
        out->down_rate = (float)downs / t;
        out->leap_rate = (float)leaps / t;
    }

    out->vel_mean = (float)vel_sum / (float)notes_total;
    out->gate_mean = (float)gate_sum / (float)notes_total;
    float dev = 0.0f;
    for (int p = 0; p < n; p++) {
        for (int s = 0; s < pats[p].length; s++) {
            const pg_step_t *st = &pats[p].steps[s];
            if (!pg_step_is_note(st)) continue;
            float d = (float)st->vel - out->vel_mean;
            dev += d < 0 ? -d : d;
        }
    }
    out->vel_dev = dev / (float)notes_total;
}

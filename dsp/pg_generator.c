/* Phrase Gen — PatternGenerator.  See pg_generator.h. */
#include "pg_generator.h"

#include <string.h>

#define GEN_ATTEMPTS 4

/* Weight of the chosen source's own profile vs. the whole corpus. */
#define W_SOURCE 0.6f
#define W_CORPUS (1.0f - W_SOURCE)

typedef struct {
    const pg_model_t *m;
    const pg_stats_t *S;        /* chosen source's profile */
    const pg_stats_t *C;        /* corpus */
    const pg_scale_t *scale;
    pg_rng_t         *rng;
    float             mut;
    int               lo, hi;   /* register */
    int               no_rests;
} gen_ctx_t;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static int   clampi(int v, int lo, int hi)       { return v < lo ? lo : v > hi ? hi : v; }

/* Fold into the register by octaves, then snap to the scale. */
static int fit_note(const gen_ctx_t *g, int note) {
    if (g->hi - g->lo >= 11) {
        while (note > g->hi) note -= 12;
        while (note < g->lo) note += 12;
    }
    return pg_scale_quantize(g->scale, clampi(note, 0, 127));
}

/* Previous sounding note before `step`, wrapping (the pattern loops). */
static int prev_note(const pg_pattern_t *p, int step) {
    for (int k = 1; k <= p->length; k++) {
        int s = ((step - k) % p->length + p->length) % p->length;
        if (s != step && pg_step_is_note(&p->steps[s])) return p->steps[s].note;
    }
    return -1;
}

static int sample_fresh_note(gen_ctx_t *g) {
    float pc[12], oct[PG_OCTAVES];
    for (int i = 0; i < 12; i++) pc[i] = W_SOURCE * g->S->pc_hist[i] + W_CORPUS * g->C->pc_hist[i];
    for (int i = 0; i < PG_OCTAVES; i++) oct[i] = W_SOURCE * g->S->octave_hist[i] + W_CORPUS * g->C->octave_hist[i];
    int c = pg_rng_weighted(g->rng, pc, 12);
    int o = pg_rng_weighted(g->rng, oct, PG_OCTAVES);
    if (c < 0) c = g->scale->root;
    if (o < 0) o = 5;
    return fit_note(g, o * 12 + c);
}

/* Markov step: the next pitch CLASS from the degree transition table, its
 * REGISTER from the interval distribution. */
static int markov_next(gen_ctx_t *g, int prev) {
    if (prev < 0) return sample_fresh_note(g);
    int deg = pg_scale_degree(g->scale, pg_scale_quantize(g->scale, prev));
    int n = g->scale->degrees;
    float row[12];
    for (int b = 0; b < n; b++)
        row[b] = W_SOURCE * g->S->markov[deg][b] + W_CORPUS * g->C->markov[deg][b];
    int next_deg = pg_rng_weighted(g->rng, row, n);

    float iv[PG_INTERVAL_BINS];
    for (int i = 0; i < PG_INTERVAL_BINS; i++)
        iv[i] = W_SOURCE * g->S->interval_hist[i] + W_CORPUS * g->C->interval_hist[i];
    int bin = pg_rng_weighted(g->rng, iv, PG_INTERVAL_BINS);
    int target = prev + (bin >= 0 ? bin - PG_INTERVAL_MAX : 0);

    return fit_note(g, pg_scale_degree_near(g->scale, next_deg, target));
}

static int sample_velocity(gen_ctx_t *g) {
    float v = g->S->vel_mean + pg_rng_tri(g->rng) * (g->S->vel_dev + 4.0f + 8.0f * g->mut);
    return clampi((int)(v + 0.5f), 1, 127);
}

static int sample_gate(gen_ctx_t *g) {
    int gate = (int)(g->S->gate_mean + 0.5f);
    return clampi(gate, PG_GATE_MIN, PG_GATE_MAX);
}

/* ---- stage 1: rhythm ---------------------------------------------------- */

static void stage_rhythm(gen_ctx_t *g, pg_pattern_t *out, int8_t *orig,
                         pg_gen_report_t *rep) {
    const int L = out->length;
    float onset[PG_MAX_STEPS];
    for (int s = 0; s < L; s++)
        onset[s] = 0.5f * g->S->onset_prob[s] + 0.5f * g->C->onset_prob[s];

    /* Density stays near the SOURCE's count, never the corpus average: a
     * sparse source stays sparse, a busy one may get busier. */
    int n_src = pg_pattern_note_count(out);
    int dmax = (int)(g->mut * 2.0f + 0.5f);
    int lo = n_src - dmax < 1 ? 1 : n_src - dmax;
    int hi = n_src + dmax > L ? L : n_src + dmax;
    int densest = g->C->count_max + 1;
    if (hi > densest && densest >= n_src) hi = densest;
    int target = clampi(n_src + pg_rng_range(g->rng, -dmax, dmax), lo, hi);
    if (g->no_rests) target = L;               /* every step plays */

    float w[PG_MAX_STEPS];
    int count = n_src;
    while (count > target) {                   /* delete: unlikely steps first */
        for (int s = 0; s < L; s++)
            w[s] = pg_step_is_note(&out->steps[s]) ? (1.05f - onset[s]) : 0.0f;
        int s = pg_rng_weighted(g->rng, w, L);
        if (s < 0) break;
        out->steps[s].note = PG_REST;
        orig[s] = -1;
        count--;
        rep->deleted++;
    }
    while (count < target) {                   /* add: likely steps first */
        for (int s = 0; s < L; s++)
            w[s] = pg_step_is_note(&out->steps[s]) ? 0.0f : (onset[s] + 0.05f);
        int s = pg_rng_weighted(g->rng, w, L);
        if (s < 0) break;
        out->steps[s].note = (int8_t)markov_next(g, prev_note(out, s));
        out->steps[s].vel = (uint8_t)sample_velocity(g);
        out->steps[s].gate = (uint16_t)sample_gate(g);
        orig[s] = -2;                           /* -2: added this pass */
        count++;
        rep->added++;
    }

    /* Nudge: slide an original note one step into an adjacent rest.  Steps
     * every source plays on (the downbeat, usually) almost never move. */
    for (int s = 0; s < L; s++) {
        if (orig[s] < 0 || !pg_step_is_note(&out->steps[s])) continue;
        if (!pg_rng_chance(g->rng, g->mut * 0.15f * (1.1f - g->C->onset_prob[s]))) continue;
        int left = (s + L - 1) % L, right = (s + 1) % L;
        float dw[2] = {
            pg_step_is_note(&out->steps[left])  ? 0.0f : onset[left] + 0.05f,
            pg_step_is_note(&out->steps[right]) ? 0.0f : onset[right] + 0.05f,
        };
        int pick = pg_rng_weighted(g->rng, dw, 2);
        if (pick < 0) continue;
        int d = pick == 0 ? left : right;
        out->steps[d] = out->steps[s];
        orig[d] = orig[s];
        out->steps[s].note = PG_REST;
        orig[s] = -1;
        rep->moved++;
        if (d == right) s++;                    /* don't move it twice */
    }
}

/* Fill every rest with a Markov note from the note before it.  Runs on the
 * source copy before any stage, so with rests disallowed nothing downstream
 * ever sees a rest (the rhythm stage then has nothing to delete or nudge). */
static void fill_rests(gen_ctx_t *g, pg_pattern_t *p) {
    for (int s = 0; s < p->length; s++) {
        pg_step_t *st = &p->steps[s];
        if (pg_step_is_note(st)) continue;
        st->note = (int8_t)markov_next(g, prev_note(p, s));
        st->vel = (uint8_t)sample_velocity(g);
        st->gate = (uint16_t)sample_gate(g);
    }
}

/* ---- stage 2: pitch ----------------------------------------------------- */

enum { OP_SHIFT, OP_OCTAVE, OP_REPEAT, OP_MARKOV, OP_COUNT };

static int op_shift(gen_ctx_t *g, int note) {
    static const float span_w[3] = { 0.55f, 0.30f, 0.15f };     /* 1, 2, 3 steps */
    int n = pg_rng_weighted(g->rng, span_w, 3) + 1;
    float up = W_SOURCE * g->S->up_rate + W_CORPUS * g->C->up_rate;
    float dn = W_SOURCE * g->S->down_rate + W_CORPUS * g->C->down_rate;
    float p_up = (up + dn) > 0.0f ? up / (up + dn) : 0.5f;
    int shifted = pg_scale_shift(g->scale, note, pg_rng_chance(g->rng, p_up) ? n : -n);
    return fit_note(g, shifted);
}

static int op_octave(gen_ctx_t *g, int note) {
    int cur = note / 12;
    float w[PG_OCTAVES];
    for (int o = 0; o < PG_OCTAVES; o++) {
        int d = o > cur ? o - cur : cur - o;
        float near = d == 1 ? 1.0f : d == 2 ? 0.5f : 0.15f;
        int cand = note + 12 * (o - cur);
        w[o] = (o == cur || cand < g->lo || cand > g->hi) ? 0.0f
             : near * (0.05f + W_SOURCE * g->S->octave_hist[o] + W_CORPUS * g->C->octave_hist[o]);
    }
    int o = pg_rng_weighted(g->rng, w, PG_OCTAVES);
    if (o < 0) o = (note + 12 <= 127) ? cur + 1 : cur - 1;
    return fit_note(g, note + 12 * (o - cur));
}

static void stage_pitch(gen_ctx_t *g, pg_pattern_t *out, const int8_t *orig) {
    const float p_pitch = 0.20f + 0.60f * g->mut;
    const float leap = W_SOURCE * g->S->leap_rate + W_CORPUS * g->C->leap_rate;
    const float rep  = W_SOURCE * g->S->repeat_rate + W_CORPUS * g->C->repeat_rate;

    for (int s = 0; s < out->length; s++) {
        pg_step_t *st = &out->steps[s];
        if (!pg_step_is_note(st) || orig[s] < 0) continue;   /* added notes are fresh */
        if (!pg_rng_chance(g->rng, p_pitch)) continue;

        int prev = prev_note(out, s);
        float w[OP_COUNT] = {
            [OP_SHIFT]  = 0.40f,
            [OP_OCTAVE] = 0.10f + 0.35f * leap,
            [OP_REPEAT] = (prev >= 0 && prev != st->note) ? 0.04f + 0.60f * rep : 0.0f,
            [OP_MARKOV] = 0.25f,
        };
        int op = pg_rng_weighted(g->rng, w, OP_COUNT);
        int note = st->note;
        switch (op) {
            case OP_SHIFT:  note = op_shift(g, note); break;
            case OP_OCTAVE: note = op_octave(g, note); break;
            case OP_REPEAT: note = prev; break;
            case OP_MARKOV: note = markov_next(g, prev); break;
            default: break;
        }
        if (note == st->note) note = fit_note(g, pg_scale_shift(g->scale, note, pg_rng_chance(g->rng, 0.5f) ? 1 : -1));
        st->note = (int8_t)note;
    }
}

/* ---- stage 3: dynamics -------------------------------------------------- */

static void stage_dynamics(gen_ctx_t *g, pg_pattern_t *out, const int8_t *orig) {
    const float p_vel = 0.30f + 0.50f * g->mut;
    const float span = 3.0f + 14.0f * g->mut;
    int vmin = clampi(g->C->vel_min - 24, 1, 127);
    int vmax = clampi(g->C->vel_max + 20, vmin, 127);

    static const int   gate_pct[5] = { 50, 75, 125, 150, 200 };
    static const float gate_w[5]   = { 0.25f, 0.30f, 0.20f, 0.15f, 0.10f };
    const float p_gate = 0.35f * g->mut;
    int gmin = clampi(g->C->gate_min / 2, PG_GATE_MIN, PG_GATE_MAX);
    int gmax = clampi(g->C->gate_max * 2, gmin, PG_GATE_MAX);

    for (int s = 0; s < out->length; s++) {
        pg_step_t *st = &out->steps[s];
        if (!pg_step_is_note(st)) continue;
        if (orig[s] >= 0 && pg_rng_chance(g->rng, p_vel)) {
            float v = (float)st->vel + pg_rng_tri(g->rng) * span;
            st->vel = (uint8_t)clampi((int)(v + (v >= 0 ? 0.5f : -0.5f)), vmin, vmax);
        } else {
            st->vel = (uint8_t)clampi(st->vel, vmin, vmax);
        }
        if (pg_rng_chance(g->rng, p_gate)) {
            int k = pg_rng_weighted(g->rng, gate_w, 5);
            st->gate = (uint16_t)clampi(st->gate * gate_pct[k] / 100, gmin, gmax);
        }
    }
}

/* ---- driver ------------------------------------------------------------- */

static void quantize_all(const gen_ctx_t *g, pg_pattern_t *p) {
    for (int s = 0; s < p->length; s++) {
        pg_step_t *st = &p->steps[s];
        if (!pg_step_is_note(st)) continue;
        st->note = (int8_t)pg_scale_quantize(g->scale, st->note);
        if (st->vel < 1) st->vel = 1;
        st->gate = (uint16_t)clampi(st->gate, PG_GATE_MIN, PG_GATE_MAX);
    }
}

static int choose_source(const pg_model_t *m, const pg_gen_params_t *p, pg_rng_t *rng) {
    if (p->source >= 0 && p->source < m->n_sources) return p->source;
    int s = pg_rng_range(rng, 0, m->n_sources - 1);
    if (s == p->avoid_source && m->n_sources > 1) s = pg_rng_range(rng, 0, m->n_sources - 1);
    return s;
}

static void fill_report(const pg_pattern_t *src, const pg_pattern_t *out,
                        const int8_t *orig, pg_gen_report_t *rep) {
    rep->notes_src = pg_pattern_note_count(src);
    rep->notes_out = pg_pattern_note_count(out);
    rep->rhythm_kept = 0;
    rep->pitch_changed = 0;
    for (int s = 0; s < out->length; s++) {
        int a = pg_step_is_note(&src->steps[s]), b = pg_step_is_note(&out->steps[s]);
        rep->rhythm_kept += (a == b);
        if (b && orig[s] >= 0 && out->steps[s].note != src->steps[orig[s]].note)
            rep->pitch_changed++;
    }
}

int pg_generate(const pg_model_t *m, const pg_gen_params_t *p, pg_rng_t *rng,
                const pg_pattern_t *avoid, pg_pattern_t *out,
                pg_gen_report_t *report) {
    pg_gen_report_t local;
    pg_gen_report_t *rep = report ? report : &local;
    memset(rep, 0, sizeof(*rep));
    if (!m || m->n_sources <= 0) { pg_pattern_clear(out, PG_DEFAULT_STEPS); return -1; }

    const int src_idx = choose_source(m, p, rng);
    gen_ctx_t g = {
        .m = m, .S = &m->source_stats[src_idx], .C = m->corpus, .scale = m->scale,
        .rng = rng, .mut = clampf(p->mutation, 0.0f, 1.0f),
        .lo = m->corpus->note_min, .hi = m->corpus->note_max,
        .no_rests = !p->allow_rests,
    };

    pg_pattern_t src = m->sources[src_idx];
    if (g.no_rests) fill_rests(&g, &src);
    quantize_all(&g, &src);
    int8_t orig[PG_MAX_STEPS];

    for (int attempt = 0; attempt < GEN_ATTEMPTS; attempt++) {
        memset(rep, 0, sizeof(*rep));
        *out = src;
        for (int s = 0; s < PG_MAX_STEPS; s++) orig[s] = (int8_t)(s < src.length && pg_step_is_note(&src.steps[s]) ? s : -1);
        if (g.mut <= 0.0f) break;               /* 0% = the source itself */

        stage_rhythm(&g, out, orig, rep);
        stage_pitch(&g, out, orig);
        stage_dynamics(&g, out, orig);
        quantize_all(&g, out);

        if (pg_pattern_note_count(out) == 0) continue;
        if (pg_pattern_equal(out, &src)) continue;
        if (avoid && pg_pattern_equal(out, avoid)) continue;
        break;
    }

    if (g.mut > 0.0f && (pg_pattern_equal(out, &src) || (avoid && pg_pattern_equal(out, avoid)) ||
                         pg_pattern_note_count(out) == 0)) {
        /* Every attempt collided: take the source and move one note. */
        *out = src;
        for (int s = 0; s < PG_MAX_STEPS; s++) orig[s] = (int8_t)(s < src.length && pg_step_is_note(&src.steps[s]) ? s : -1);
        float w[PG_MAX_STEPS];
        for (int s = 0; s < out->length; s++) w[s] = pg_step_is_note(&out->steps[s]) ? 1.0f : 0.0f;
        int s = pg_rng_weighted(rng, w, out->length);
        if (s >= 0) out->steps[s].note = (int8_t)fit_note(&g, pg_scale_shift(g.scale, out->steps[s].note, pg_rng_chance(rng, 0.5f) ? 1 : -1));
    }

    rep->source = src_idx;
    fill_report(&src, out, orig, rep);
    return src_idx;
}

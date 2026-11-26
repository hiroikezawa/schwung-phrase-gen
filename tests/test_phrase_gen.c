/*
 * Phrase Gen tests — run by scripts/test.sh under ASan/UBSan.
 *
 * Units (loader, scale, analyzer, generator, sequencer, pad) and then the
 * plugin through its real midi_fx_api_v1 entry points with a fake host
 * transport, including randomized stuck-note fuzzing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"
#include "pg_pattern.h"
#include "pg_scale.h"
#include "pg_midi_loader.h"
#include "pg_analyzer.h"
#include "pg_generator.h"
#include "pg_sequencer.h"
#include "pg_pad.h"
#include "pg_format.h"
#include "pg_library.h"
#include "pg_rng.h"
#include "pg_patterns_data.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); } } while (0)

static pg_library_t g_lib;

static void build_embedded_library(pg_library_t *lib) {
    pg_library_init(lib, 0, PG_MASK_PHRYGIAN, PG_DEFAULT_STEPS);
    for (int i = 0; i < PG_EMBEDDED_COUNT; i++)
        pg_library_add(lib, PG_EMBEDDED_PATTERNS[i].name, PG_EMBEDDED_PATTERNS[i].data,
                       PG_EMBEDDED_PATTERNS[i].len, PG_EMBEDDED_PATTERNS[i].start_step);
    pg_library_finalize(lib);
}

/* ---- loader ------------------------------------------------------------- */

static void test_loader_embedded(void) {
    CHECK(PG_EMBEDDED_COUNT == 6, "expected 6 embedded patterns, got %d", PG_EMBEDDED_COUNT);
    CHECK(g_lib.n == PG_EMBEDDED_COUNT, "library loaded %d of %d", g_lib.n, PG_EMBEDDED_COUNT);

    /* pattern01 = midi1.mid: F2 F3 F2 Eb2 Bb5 Bb6 Ab1 Eb2, vel 100, 22/24 tick gates */
    static const int p1[8] = { 53, 65, 53, 51, 94, 106, 44, 51 };
    for (int s = 0; s < 8; s++) {
        CHECK(g_lib.pats[0].steps[s].note == p1[s], "pattern01 step %d note %d", s, g_lib.pats[0].steps[s].note);
        CHECK(g_lib.pats[0].steps[s].vel == 100, "pattern01 vel");
        CHECK(g_lib.pats[0].steps[s].gate == 92, "pattern01 gate %d", g_lib.pats[0].steps[s].gate);
    }
    /* pattern02 = midi2.mid has a rest on step 6 */
    CHECK(g_lib.pats[1].steps[5].note == PG_REST, "pattern02 step 6 should be a rest");
    CHECK(pg_pattern_note_count(&g_lib.pats[1]) == 7, "pattern02 has 7 notes");
    for (int i = 0; i < g_lib.n; i++) CHECK(g_lib.pats[i].length == 8, "length 8");
}

/* Hand-built SMF: format 0, ppq 96, 16 steps of notes 48..63 (vel 60..75,
 * half-step gates) on channel 2.  Exercises running status, note-on vel 0 as
 * note-off, a leading sysex, and text metas mid-track. */
static size_t build_smf(uint8_t *buf) {
    uint8_t trk[512];
    size_t t = 0;
    trk[t++] = 0x00; trk[t++] = 0xF0; trk[t++] = 0x02; trk[t++] = 0x7E; trk[t++] = 0xF7;
    for (int s = 0; s < 16; s++) {
        trk[t++] = s == 0 ? 0x00 : 12;                /* rest of the previous step */
        if (s % 4 == 0) trk[t++] = 0x91;              /* explicit status after sysex/meta */
        trk[t++] = (uint8_t)(48 + s); trk[t++] = (uint8_t)(60 + s);
        trk[t++] = 12;                                /* half a step later: */
        trk[t++] = (uint8_t)(48 + s); trk[t++] = 0x00; /* running-status vel-0 note-off */
        if (s % 4 == 3 && s < 15) { trk[t++] = 0x00; trk[t++] = 0xFF; trk[t++] = 0x01; trk[t++] = 0x00; }
    }
    trk[t++] = 12; trk[t++] = 0xFF; trk[t++] = 0x2F; trk[t++] = 0x00;

    size_t n = 0;
    memcpy(buf, "MThd\0\0\0\x06\0\0\0\x01\0\x60", 14); n = 14;
    memcpy(buf + n, "MTrk", 4); n += 4;
    buf[n++] = 0; buf[n++] = 0; buf[n++] = (uint8_t)(t >> 8); buf[n++] = (uint8_t)t;
    memcpy(buf + n, trk, t);
    return n + t;
}

static void test_loader_handbuilt(void) {
    uint8_t smf[1024];
    size_t n = build_smf(smf);
    static pg_midi_file_t mf;
    int rc = pg_midi_parse(smf, n, &mf);
    CHECK(rc == PG_MIDI_OK, "hand-built SMF parse rc=%d", rc);
    CHECK(mf.ppq == 96, "ppq");
    CHECK(mf.count == 16, "16 notes parsed, got %d", mf.count);

    pg_pattern_t p;
    int placed = pg_midi_extract(&mf, 8, 8, &p);         /* window: steps 9..16 */
    CHECK(placed == 8, "window placed %d", placed);
    CHECK(p.steps[0].note == 56 && p.steps[7].note == 63, "window starts at step 9: %d..%d",
          p.steps[0].note, p.steps[7].note);
    CHECK(p.steps[0].vel == 68 && p.steps[0].gate == 50, "vel/gate from window: %d %d",
          p.steps[0].vel, p.steps[0].gate);

    placed = pg_midi_extract(&mf, 12, 8, &p);            /* runs past the end */
    CHECK(placed == 4 && p.steps[4].note == PG_REST, "partial window pads with rests");
}

static void test_loader_robustness(void) {
    static pg_midi_file_t mf;
    uint8_t buf[256];
    pg_rng_t r; pg_rng_seed(&r, 99);
    for (int i = 0; i < PG_EMBEDDED_COUNT; i++) {
        const pg_embedded_pattern_t *e = &PG_EMBEDDED_PATTERNS[i];
        for (unsigned cut = 0; cut <= e->len; cut++) (void)pg_midi_parse(e->data, cut, &mf);
        for (int k = 0; k < 2000; k++) {
            memcpy(buf, e->data, e->len);
            int flips = pg_rng_range(&r, 1, 4);
            for (int f = 0; f < flips; f++) buf[pg_rng_range(&r, 0, (int)e->len - 1)] = (uint8_t)pg_rng_next(&r);
            if (pg_midi_parse(buf, e->len, &mf) == PG_MIDI_OK) {
                CHECK(mf.count <= PG_MIDI_MAX_EVENTS, "event bound");
                pg_pattern_t p; (void)pg_midi_extract(&mf, 0, 8, &p);
            }
        }
    }
    for (int k = 0; k < 5000; k++) {
        int len = pg_rng_range(&r, 0, (int)sizeof(buf));
        for (int j = 0; j < len; j++) buf[j] = (uint8_t)pg_rng_next(&r);
        if (len >= 4 && (k & 1)) memcpy(buf, "MThd", 4);
        (void)pg_midi_parse(buf, (size_t)len, &mf);
    }
    CHECK(pg_midi_parse(NULL, 0, &mf) != PG_MIDI_OK, "NULL rejected");
    CHECK(pg_midi_parse((const uint8_t *)"MThd\0\0\0\x06\0\0\0\x01\xE7\x28", 14, &mf) == PG_MIDI_E_SMPTE, "SMPTE rejected");
}

/* ---- scale -------------------------------------------------------------- */

static void test_scale(void) {
    pg_scale_t s;
    pg_scale_init(&s, 0, PG_MASK_PHRYGIAN);
    static const int pcs[7] = { 0, 1, 3, 5, 7, 8, 10 };   /* C Db Eb F G Ab Bb */
    CHECK(s.degrees == 7, "7 degrees");
    for (int pc = 0; pc < 12; pc++) {
        int in = 0;
        for (int i = 0; i < 7; i++) in |= pcs[i] == pc;
        CHECK(pg_scale_contains(&s, 60 + pc) == in, "pc %d membership", pc);
    }
    for (int n = -5; n < 133; n++) {
        int q = pg_scale_quantize(&s, n);
        CHECK(pg_scale_contains(&s, q), "quantize(%d)=%d in scale", n, q);
        int c = n < 0 ? 0 : n > 127 ? 127 : n;
        CHECK(abs(q - c) <= 1, "quantize(%d)=%d is nearest", n, q);
        for (int k = -9; k <= 9; k++)
            CHECK(pg_scale_contains(&s, pg_scale_shift(&s, q, k)), "shift in scale");
    }
    CHECK(pg_scale_shift(&s, 60, 1) == 61 && pg_scale_shift(&s, 60, -1) == 58, "C +-1 = Db / Bb");
    CHECK(pg_scale_shift(&s, 60, 7) == 72, "7 steps = an octave");
    CHECK(pg_scale_degree_near(&s, 4, 70) == 67, "G nearest 70");
}

/* ---- generator ---------------------------------------------------------- */

static void test_generator(void) {
    pg_rng_t r; pg_rng_seed(&r, 1234);
    pg_pattern_t prev, out;
    pg_pattern_clear(&prev, 8);

    /* 0% = the source itself (with rests allowed) */
    for (int i = 0; i < g_lib.n; i++) {
        pg_gen_params_t p = { .mutation = 0.0f, .source = i, .avoid_source = -1, .allow_rests = 1 };
        pg_generate(&g_lib.model, &p, &r, NULL, &out, NULL);
        CHECK(pg_pattern_equal(&out, &g_lib.pats[i]), "mutation 0 returns source %d verbatim", i + 1);
    }
    /* 0% with rests disallowed (the default): the source's notes, rests filled */
    for (int i = 0; i < g_lib.n; i++) {
        pg_gen_params_t p = { .mutation = 0.0f, .source = i, .avoid_source = -1 };
        pg_generate(&g_lib.model, &p, &r, NULL, &out, NULL);
        CHECK(pg_pattern_note_count(&out) == 8, "mutation 0, source %d: no rests", i + 1);
        for (int s = 0; s < 8; s++)
            if (pg_step_is_note(&g_lib.pats[i].steps[s]))
                CHECK(out.steps[s] .note == g_lib.pats[i].steps[s].note, "mutation 0 keeps source notes");
    }

    static const float muts[] = { 0.05f, 0.25f, 0.5f, 0.75f, 1.0f };
    for (unsigned mi = 0; mi < sizeof(muts) / sizeof(muts[0]); mi++) {
        double verbatim = 0, rhythm = 0;
        int n_gen = 4000, last = -1, src_hist[PG_MAX_SOURCES] = {0};
        for (int k = 0; k < n_gen; k++) {
            pg_gen_params_t p = { .mutation = muts[mi], .source = -1, .avoid_source = last };
            pg_gen_report_t rep;
            int src = pg_generate(&g_lib.model, &p, &r, &prev, &out, &rep);
            CHECK(src >= 0 && src < g_lib.n, "source index");
            src_hist[src]++;
            last = src;
            CHECK(!pg_pattern_equal(&out, &prev), "m=%.2f: consecutive patterns differ", muts[mi]);
            CHECK(!pg_pattern_equal(&out, &g_lib.pats[src]), "m=%.2f: differs from its source", muts[mi]);
            CHECK(out.length == 8, "length 8");

            int n_out = pg_pattern_note_count(&out), n_src = pg_pattern_note_count(&g_lib.pats[src]);
            CHECK(n_out == 8, "m=%.2f: no rests (%d notes from source with %d)", muts[mi], n_out, n_src);
            for (int s = 0; s < 8; s++) {
                const pg_step_t *st = &out.steps[s];
                if (!pg_step_is_note(st)) continue;
                CHECK(pg_scale_contains(&g_lib.scale, st->note), "note %d out of C Phrygian", st->note);
                CHECK(st->note >= g_lib.corpus.note_min && st->note <= g_lib.corpus.note_max,
                      "note %d outside source register", st->note);
                CHECK(st->vel >= 1 && st->vel <= 127, "velocity");
                CHECK(st->gate >= PG_GATE_MIN && st->gate <= PG_GATE_MAX, "gate");
                if (st->note == g_lib.pats[src].steps[s].note) verbatim += 1.0 / n_src;
            }
            rhythm += rep.rhythm_kept / 8.0;
            prev = out;
        }
        verbatim /= n_gen; rhythm /= n_gen;
        printf("  mutation %3.0f%%: notes kept verbatim %4.1f%%, rhythm kept %4.1f%%\n",
               muts[mi] * 100, verbatim * 100, rhythm * 100);
        CHECK(rhythm >= 0.70, "rhythm retention %.2f", rhythm);
        if (muts[mi] == 0.5f)
            CHECK(verbatim >= 0.30 && verbatim <= 0.55, "default mutation keeps 30-55%% (%.2f)", verbatim);
        for (int i = 0; i < g_lib.n; i++) CHECK(src_hist[i] > n_gen / 20, "source %d is picked", i + 1);
    }

    /* A sparse source stays sparse: X---X-X- never becomes eight notes. */
    static pg_library_t sparse;
    pg_library_init(&sparse, 0, PG_MASK_PHRYGIAN, 8);
    sparse.pats[0] = g_lib.pats[0];
    for (int s = 0; s < 8; s++) if (s != 0 && s != 4 && s != 6) sparse.pats[0].steps[s].note = PG_REST;
    sparse.n = 1;
    snprintf(sparse.names[0], PG_NAME_LEN, "sparse");
    pg_library_finalize(&sparse);
    int max_seen = 0;
    for (int k = 0; k < 3000; k++) {
        pg_gen_params_t p = { .mutation = 1.0f, .source = 0, .avoid_source = -1, .allow_rests = 1 };
        pg_generate(&sparse.model, &p, &r, NULL, &out, NULL);
        int n = pg_pattern_note_count(&out);
        if (n > max_seen) max_seen = n;
    }
    CHECK(max_seen <= 5, "sparse (3-note) source grew to %d notes", max_seen);
}

/* ---- sequencer ---------------------------------------------------------- */

typedef struct { int on[16][128]; int ons, offs, errors; } note_ledger_t;

static void ledger_apply(note_ledger_t *l, uint8_t msgs[][3], int n) {
    for (int i = 0; i < n; i++) {
        int type = msgs[i][0] & 0xF0, ch = msgs[i][0] & 0x0F, note = msgs[i][1];
        if (type == 0x90 && msgs[i][2] > 0) {
            if (l->on[ch][note]) l->errors++;           /* double note-on */
            l->on[ch][note] = 1; l->ons++;
        } else if (type == 0x80 || type == 0x90) {
            if (!l->on[ch][note]) l->errors++;          /* off without on */
            l->on[ch][note] = 0; l->offs++;
        }
    }
}

static int ledger_sounding(const note_ledger_t *l) {
    int n = 0;
    for (int c = 0; c < 16; c++) for (int k = 0; k < 128; k++) n += l->on[c][k];
    return n;
}

static void test_sequencer_timing(void) {
    pg_seq_t q;
    pg_pattern_t a = g_lib.pats[0], b = g_lib.pats[2];
    pg_seq_init(&q, &a);
    note_ledger_t led; memset(&led, 0, sizeof(led));
    uint8_t out[16][3]; int lens[16];

    const double spb = 44100.0 * 60.0 / 123.0;          /* odd tempo on purpose */
    long long frames = 0;
    int queued = 0, swap_seen_at = -1;
    double worst_late = 0;
    for (int blk = 0; blk < 20000; blk++, frames += 128) {
        double beat = (double)frames / spb;
        if (!queued && beat > 2.3) { pg_seq_queue(&q, &b); queued = 1; }  /* mid-loop */
        int n = pg_seq_tick(&q, beat, out, lens, 16);
        ledger_apply(&led, out, n);
        for (int i = 0; i < n; i++) {
            if ((out[i][0] & 0xF0) != 0x90) continue;
            long long abs_step = (long long)(beat * 4 + 1e-9);
            double late = (beat - abs_step / 4.0) * spb;          /* frames after the grid */
            if (late > worst_late) worst_late = late;
            /* queued at beat 2.3 (step 9): pattern a until step 16, b after */
            const pg_pattern_t *exp = abs_step >= 16 ? &b : &a;
            if (abs_step >= 16 && swap_seen_at < 0) swap_seen_at = (int)abs_step;
            CHECK(out[i][1] == exp->steps[abs_step % 8].note, "step %lld note %d", abs_step, out[i][1]);
        }
        if (beat > 12) break;
    }
    CHECK(worst_late < 128.0, "note-on is at most one block late (%.1f frames)", worst_late);
    CHECK(swap_seen_at == 16, "played through the swap at step 16 (%d)", swap_seen_at);
    CHECK(led.errors == 0, "ledger errors %d", led.errors);

    /* stop: everything off, and the transport clock is the only clock */
    int n = pg_seq_tick(&q, -1.0, out, lens, 16);
    ledger_apply(&led, out, n);
    CHECK(ledger_sounding(&led) == 0 && pg_seq_active_voices(&q) == 0, "stop releases all");
    n = pg_seq_tick(&q, -1.0, out, lens, 16);
    CHECK(n == 0, "stopped transport is silent (no free-running)");
    pg_seq_queue(&q, &a);                                  /* stopped: applies at once */
    n = pg_seq_tick(&q, 0.0, out, lens, 16);
    ledger_apply(&led, out, n);
    CHECK(n == 1 && out[0][1] == a.steps[0].note, "restart plays step 1 at beat 0");
}

static void test_sequencer_fuzz(void) {
    pg_rng_t r; pg_rng_seed(&r, 77);
    uint8_t out[16][3]; int lens[16];
    for (int round = 0; round < 60; round++) {
        pg_seq_t q;
        pg_seq_init(&q, &g_lib.pats[round % g_lib.n]);
        note_ledger_t led; memset(&led, 0, sizeof(led));
        double beat = 0, bpm = 60 + pg_rng_range(&r, 0, 240);
        int stopped = 0;
        int max_out = 1 + (round % 16);                    /* starve the output buffer too */
        for (int blk = 0; blk < 6000; blk++) {
            int ev = pg_rng_range(&r, 0, 999);
            if (ev < 8) {                                   /* new pattern, wild gates */
                pg_pattern_t p = g_lib.pats[pg_rng_range(&r, 0, g_lib.n - 1)];
                for (int s = 0; s < 8; s++) p.steps[s].gate = (uint16_t)pg_rng_range(&r, PG_GATE_MIN, PG_GATE_MAX);
                q.channel = (uint8_t)pg_rng_range(&r, 0, 15);
                pg_seq_queue(&q, &p);
            } else if (ev < 10) { stopped = !stopped; if (!stopped) beat = 0; }
            else if (ev < 11) beat = pg_rng_float(&r) * beat;            /* jump back */
            else if (ev < 12) pg_seq_flush(&q);
            else if (ev < 14) bpm = 60 + pg_rng_range(&r, 0, 240);
            if (!stopped) beat += 128.0 / (44100.0 * 60.0 / bpm);
            int n = pg_seq_tick(&q, stopped ? -1.0 : beat, out, lens, max_out);
            CHECK(n <= max_out, "respects max_out");
            ledger_apply(&led, out, n);
            CHECK(ledger_sounding(&led) == pg_seq_active_voices(&q), "ledger == voices");
        }
        for (int k = 0; k < 40; k++) ledger_apply(&led, out, pg_seq_tick(&q, -1.0, out, lens, max_out));
        CHECK(led.errors == 0, "round %d: %d on/off errors", round, led.errors);
        CHECK(ledger_sounding(&led) == 0, "round %d: %d stuck notes after stop", round, ledger_sounding(&led));
        CHECK(led.ons == led.offs, "round %d: %d ons vs %d offs", round, led.ons, led.offs);
    }
}

/* ---- pad ---------------------------------------------------------------- */

static void test_pad(void) {
    pg_pad_t p;
    pg_pad_init(&p);
    CHECK(pg_pad_on_note(&p, 1, 60) == PG_PAD_GENERATE, "any: generate");
    CHECK(pg_pad_on_note(&p, 0, 60) == PG_PAD_SWALLOW, "any: off swallowed");

    p.mode = PG_TRIG_NOTE; p.trig_note = 51;
    CHECK(pg_pad_on_note(&p, 1, 51) == PG_PAD_GENERATE, "note: trigger");
    CHECK(pg_pad_on_note(&p, 1, 60) == PG_PAD_PASS, "note: other pad plays");
    p.mode = PG_TRIG_ANY;                                  /* mode flips while 60 is held */
    CHECK(pg_pad_on_note(&p, 0, 60) == PG_PAD_PASS, "a passed note-on always gets its off");
    CHECK(pg_pad_on_note(&p, 0, 51) == PG_PAD_SWALLOW, "trigger off swallowed");

    p.mode = PG_TRIG_LEARN;
    CHECK(pg_pad_on_note(&p, 1, 70) == PG_PAD_GENERATE && p.trig_note == 70 && p.mode == PG_TRIG_NOTE,
          "learn takes the next pad");
    CHECK(pg_trig_mode_parse("learn") == PG_TRIG_LEARN && pg_trig_mode_parse("x") == -1, "parse");
}

/* ---- format ------------------------------------------------------------- */

static void test_format(void) {
    char name[8];
    pg_note_name(60, name, sizeof(name)); CHECK(!strcmp(name, "C3"), "60 = C3 (%s)", name);
    pg_note_name(61, name, sizeof(name)); CHECK(!strcmp(name, "Db3"), "61 = Db3");
    pg_note_name(36, name, sizeof(name)); CHECK(!strcmp(name, "C1"), "36 = C1");

    char buf[512];
    pg_format_pattern(&g_lib.pats[1], "pattern02.mid", buf, sizeof(buf));
    CHECK(strstr(buf, "Source: pattern02.mid\nGenerated:\nStep 1: C3 vel=100") != NULL, "debug header: %s", buf);
    CHECK(strstr(buf, "Step 6: REST\n") != NULL, "rest line");

    pg_pattern_t back;
    for (int i = 0; i < g_lib.n; i++) {
        pg_encode_pattern(&g_lib.pats[i], buf, sizeof(buf));
        CHECK(pg_decode_pattern(buf, &back) && pg_pattern_equal(&back, &g_lib.pats[i]), "roundtrip %s", buf);
    }
    CHECK(!pg_decode_pattern("8|1.2", &back), "truncated rejected");
    CHECK(!pg_decode_pattern("0|", &back), "zero length rejected");
    CHECK(!pg_decode_pattern("2|60.0.50,-", &back), "vel 0 rejected");
    char tiny[5];
    pg_format_pattern(&g_lib.pats[0], "x", tiny, sizeof(tiny));
    CHECK(strlen(tiny) == 4, "clipped to buffer");
}

/* ---- plugin through the real API ---------------------------------------- */

static double g_beat = -1.0;
static double fake_beat(void) { return g_beat; }

static midi_fx_api_v1_t *load_api(int with_beat) {
    static host_api_v1_t host;
    memset(&host, 0, sizeof(host));
    host.api_version = 1;
    host.sample_rate = 44100;
    host.frames_per_block = 128;
    host.get_beat_position = with_beat ? fake_beat : NULL;
    midi_fx_api_v1_t *move_midi_fx_init(const host_api_v1_t *h);
    return move_midi_fx_init(&host);
}

static int midi(midi_fx_api_v1_t *api, void *inst, uint8_t a, uint8_t b, uint8_t c,
                uint8_t out[][3], int lens[]) {
    uint8_t m[3] = { a, b, c };
    int len = a >= 0xF0 ? 1 : (a & 0xF0) == 0xC0 || (a & 0xF0) == 0xD0 ? 2 : 3;
    return api->process_midi(inst, m, len, out, lens, 16);
}

static void test_plugin(void) {
    midi_fx_api_v1_t *api = load_api(1);
    CHECK(api && api->api_version == MIDI_FX_API_VERSION, "api");
    void *inst = api->create_instance("/nonexistent", NULL);
    CHECK(inst != NULL, "create");

    char buf[2048], before[2048];
    uint8_t out[16][3]; int lens[16];
    note_ledger_t led; memset(&led, 0, sizeof(led));

    CHECK(api->get_param(inst, "debug", buf, sizeof(buf)) > 0 && strstr(buf, "Step 8:"), "debug text");
    CHECK(strstr(buf, "vel=") != NULL, "initial pattern is not silent:\n%s", buf);
    CHECK(api->get_param(inst, "chain_params", buf, sizeof(buf)) > 0 &&
          strstr(buf, "[\"random\",\"1\",\"2\",\"3\",\"4\",\"5\",\"6\"]"), "chain_params sources: %s", buf);
    CHECK(api->get_param(inst, "nope", buf, sizeof(buf)) == -1, "unknown key -> -1");
    CHECK(api->get_param(inst, "ui_hierarchy", buf, sizeof(buf)) == 0 && buf[0] == '\0',
          "ui_hierarchy served empty, so the shadow UI opens ui_chain.js");
    {
        int n = api->get_param(inst, "ui_state", buf, sizeof(buf));
        int commas = 0;
        for (int i = 0; i < n; i++) commas += buf[i] == ',';
        CHECK(n > 0 && !strncmp(buf, "1,", 2) && commas == 26, "ui_state has 27 fields: %s", buf);
        char tiny[10];
        CHECK(api->get_param(inst, "ui_state", tiny, sizeof(tiny)) == -1, "ui_state refuses a short buffer");
    }

    /* transport stopped: silent */
    g_beat = -1.0;
    CHECK(api->tick(inst, 128, 44100, out, lens, 16) == 0, "stopped = silent");

    /* play: notes come out */
    const double spb = 44100.0 * 60.0 / 120.0;
    long long frames = 0;
    for (int blk = 0; blk < 1400; blk++, frames += 128) {   /* ~4 s = 8 loops */
        g_beat = frames / spb;
        ledger_apply(&led, out, api->tick(inst, 128, 44100, out, lens, 16));
    }
    CHECK(led.ons > 20, "plays while the transport runs (%d ons)", led.ons);

    /* no rests: 500 presses, every resulting phrase has 8 notes */
    for (int k = 0; k < 500; k++) {
        midi(api, inst, 0x90, 60, 100, out, lens);
        api->get_param(inst, "debug", buf, sizeof(buf));
        CHECK(strstr(buf, "REST") == NULL, "no rest in generated phrase:\n%s", buf);
    }

    /* a pad press generates, swallows the pad, and swaps on the boundary */
    api->get_param(inst, "debug", before, sizeof(before));
    CHECK(midi(api, inst, 0x90, 60, 100, out, lens) == 0, "pad note swallowed");
    CHECK(midi(api, inst, 0x80, 60, 0, out, lens) == 0, "pad off swallowed");
    api->get_param(inst, "gen_count", buf, sizeof(buf));
    CHECK(!strcmp(buf, "502"), "generated (count %s)", buf);
    api->get_param(inst, "debug", buf, sizeof(buf));
    CHECK(strcmp(buf, before) != 0, "new pattern differs");

    /* pad spam inside one block, then keep playing: no crash, no stuck notes */
    for (int k = 0; k < 2000; k++) {
        midi(api, inst, 0x90, (uint8_t)(36 + k % 16), 100, out, lens);
        midi(api, inst, 0x80, (uint8_t)(36 + k % 16), 0, out, lens);
    }
    api->set_param(inst, "mutation", "100");
    for (int blk = 0; blk < 3000; blk++, frames += 128) {
        g_beat = frames / spb;
        if (blk % 37 == 0) midi(api, inst, 0x90, 40, 90, out, lens);
        if (blk % 37 == 1) midi(api, inst, 0x80, 40, 0, out, lens);
        ledger_apply(&led, out, api->tick(inst, 128, 44100, out, lens, 16));
    }

    /* note mode: other pads play through, including their offs across a
     * mode change */
    api->set_param(inst, "trigger", "note");
    api->set_param(inst, "trig_note", "51");
    CHECK(midi(api, inst, 0x92, 64, 90, out, lens) == 1 && out[0][1] == 64, "note mode passes other pads");
    api->set_param(inst, "trigger", "any");
    CHECK(midi(api, inst, 0x82, 64, 0, out, lens) == 1, "its note-off still passes");
    api->set_param(inst, "trigger", "learn");
    midi(api, inst, 0x90, 45, 100, out, lens);
    api->get_param(inst, "trig_note", buf, sizeof(buf));
    CHECK(!strcmp(buf, "45"), "learned trig note %s", buf);
    api->get_param(inst, "trigger", buf, sizeof(buf));
    CHECK(!strcmp(buf, "note"), "learn -> note (%s)", buf);

    /* non-note MIDI passes; CC123 and Stop flush */
    CHECK(midi(api, inst, 0xB0, 1, 64, out, lens) == 1, "CC passes");
    CHECK(midi(api, inst, 0xF8, 0, 0, out, lens) == 1, "clock passes");
    midi(api, inst, 0xB0, 123, 0, out, lens);
    ledger_apply(&led, out, api->tick(inst, 128, 44100, out, lens, 16));
    CHECK(ledger_sounding(&led) == 0, "CC123 releases everything");

    /* stop: nothing left sounding */
    g_beat = -1.0;
    ledger_apply(&led, out, api->tick(inst, 128, 44100, out, lens, 16));
    CHECK(led.errors == 0 && ledger_sounding(&led) == 0 && led.ons == led.offs,
          "plugin: errors %d sounding %d ons %d offs %d", led.errors, ledger_sounding(&led), led.ons, led.offs);

    /* state round trip into a fresh instance */
    char state[1024];
    api->set_param(inst, "source", "3");
    CHECK(api->get_param(inst, "state", state, sizeof(state)) > 0, "state");
    void *inst2 = api->create_instance("/nonexistent", NULL);
    api->set_param(inst2, "state", state);
    char a[2048], b[2048];
    api->get_param(inst, "debug", a, sizeof(a));
    api->get_param(inst2, "debug", b, sizeof(b));
    CHECK(!strcmp(a, b), "state restores the pattern\n%s\nvs\n%s", a, b);
    char state2[1024];
    api->get_param(inst2, "state", state2, sizeof(state2));
    CHECK(!strcmp(state, state2), "state roundtrip\n%s\n%s", state, state2);
    api->set_param(inst2, "state", "{\"pattern\":\"garbage\",\"mutation\":999}");
    api->get_param(inst2, "mutation", buf, sizeof(buf));
    CHECK(!strcmp(buf, "100"), "mutation clamped (%s)", buf);
    api->set_param(inst2, "source", "99");
    api->get_param(inst2, "source", buf, sizeof(buf));
    CHECK(!strcmp(buf, "random"), "bad source -> random");
    char small[8];
    CHECK(api->get_param(inst2, "chain_params", small, sizeof(small)) == -1 || strlen(small) < 8, "small buffer safe");
    api->destroy_instance(inst2);
    api->destroy_instance(inst);
}

static void test_plugin_fallback_clock(void) {
    midi_fx_api_v1_t *api = load_api(0);                   /* host without get_beat_position */
    void *inst = api->create_instance("/x", NULL);
    uint8_t out[16][3]; int lens[16];
    note_ledger_t led; memset(&led, 0, sizeof(led));
    CHECK(api->tick(inst, 128, 44100, out, lens, 16) == 0, "no clock = silent");
    midi(api, inst, 0xFA, 0, 0, out, lens);
    for (int t = 0; t < 24 * 8; t++) {                     /* two bars of MIDI clock */
        midi(api, inst, 0xF8, 0, 0, out, lens);
        ledger_apply(&led, out, api->tick(inst, 128, 44100, out, lens, 16));
    }
    CHECK(led.ons >= 28, "fallback clock plays 16ths (%d)", led.ons);
    midi(api, inst, 0xFC, 0, 0, out, lens);
    ledger_apply(&led, out, api->tick(inst, 128, 44100, out, lens, 16));
    CHECK(ledger_sounding(&led) == 0 && led.errors == 0, "fallback stop releases");
    api->destroy_instance(inst);
}

int main(void) {
    build_embedded_library(&g_lib);
    test_loader_embedded();
    test_loader_handbuilt();
    test_loader_robustness();
    test_scale();
    test_generator();
    test_sequencer_timing();
    test_sequencer_fuzz();
    test_pad();
    test_format();
    test_plugin();
    test_plugin_fallback_clock();
    printf("%s: %d checks, %d failures\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

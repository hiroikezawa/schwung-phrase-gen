/*
 * Phrase Gen — Schwung MIDI FX (midi_fx_api_v1).
 *
 * Plays an 8-step, 16th-note phrase in C Phrygian, locked to Move's
 * transport.  Each pad press generates a new phrase "in the style of" the
 * source MIDI files embedded at build time, and the new phrase takes over at
 * the next 8-step boundary.
 *
 * This file is only glue.  The work lives in:
 *   pg_midi_loader  MidiPatternLoader   SMF bytes -> step pattern
 *   pg_analyzer     PatternAnalyzer     patterns  -> statistics
 *   pg_generator    PatternGenerator    statistics + a source -> new pattern
 *   pg_scale        ScaleQuantizer
 *   pg_sequencer    Sequencer           pattern + transport -> MIDI
 *   pg_pad          PadController       incoming notes -> generate / pass
 *   pg_library      loader + analyzer wired together (shared with the CLI)
 *
 * REALTIME: every entry point here runs on the SPI callback (see the contract
 * at the top of src/host/plugin_api_v1.h).  So the MIDI files are NOT read
 * from disk — scripts/embed_patterns.py compiles them in — and nothing below
 * allocates (beyond the one instance calloc every module makes), logs, or
 * does I/O.  The "debug log" of a generated phrase is served as the `debug`
 * parameter instead, and the host CLI (tools/phrase_gen_cli.c) prints the
 * same listing offline.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

#include "pg_pattern.h"
#include "pg_scale.h"
#include "pg_library.h"
#include "pg_generator.h"
#include "pg_sequencer.h"
#include "pg_pad.h"
#include "pg_format.h"
#include "pg_rng.h"
#include "pg_patterns_data.h"   /* generated: PG_EMBEDDED_PATTERNS[] */

/* Fixed for v1; the library takes them as arguments so a Root / Scale /
 * Length parameter only has to rebuild it. */
#define PG_ROOT             0                   /* C */
#define PG_SCALE_MASK       PG_MASK_PHRYGIAN
#define PG_LENGTH           PG_DEFAULT_STEPS    /* 8 */
#define PG_DEFAULT_MUTATION 50                  /* percent */
#define PG_CLOCKS_PER_BEAT  24

typedef struct {
    pg_library_t lib;
    pg_seq_t     seq;
    pg_pad_t     pad;
    pg_rng_t     rng;

    int          mutation;      /* 0..100 */
    int          source_sel;    /* -1 = random, else source index */
    int          cur_source;    /* source of the most recent pattern */
    uint32_t     gen_count;
    uint32_t     entropy;       /* frames seen; mixes press timing into the RNG */

    /* Fallback transport, only when the host has no get_beat_position:
     * counts 24-PPQN clock arriving through process_midi. */
    int          clk_running;
    int          clk_awaiting_first;
    long long    clk_ticks;
} pg_instance_t;

static const host_api_v1_t *g_host = NULL;

/* ---- small parsers (no strtol: keeps the .so on Move's glibc) ----------- */

static int parse_int(const char *s, int *out) {
    if (!s) return 0;
    while (*s == ' ' || *s == '"') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    if (*s < '0' || *s > '9') return 0;
    long v = 0;
    while (*s >= '0' && *s <= '9' && v < 1000000) v = v * 10 + (*s++ - '0');
    *out = (int)(neg ? -v : v);
    return 1;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static const char *json_find_value(const char *json, const char *key) {
    char search[48];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return NULL;
    pos = strchr(pos + strlen(search), ':');
    if (!pos) return NULL;
    pos++;
    while (*pos == ' ' || *pos == '\t') pos++;
    return pos;
}

static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    const char *v = json_find_value(json, key);
    if (!v || *v != '"') return 0;
    v++;
    const char *end = strchr(v, '"');
    if (!end) return 0;
    int len = (int)(end - v);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, v, (size_t)len);
    out[len] = '\0';
    return 1;
}

static int json_get_int(const char *json, const char *key, int *out) {
    const char *v = json_find_value(json, key);
    return v ? parse_int(v, out) : 0;
}

/* ---- parameters --------------------------------------------------------- */

static void set_trigger(pg_instance_t *inst, const char *val) {
    int m = pg_trig_mode_parse(val);
    if (m < 0 && parse_int(val, &m) && (m < 0 || m >= PG_TRIG_COUNT)) m = -1;
    if (m >= 0) inst->pad.mode = (pg_trig_mode_t)m;
}

/* "random" / "0" -> random; "N" -> source N (1-based).  An enum index and
 * its option string agree by construction: option k is the string "k". */
static void set_source(pg_instance_t *inst, const char *val) {
    int n;
    if (!val || strcmp(val, "random") == 0) { inst->source_sel = -1; return; }
    if (!parse_int(val, &n)) return;
    inst->source_sel = (n <= 0 || n > inst->lib.n) ? -1 : n - 1;
}

static int source_label(const pg_instance_t *inst, char *buf, int len) {
    if (inst->source_sel < 0) return snprintf(buf, (size_t)len, "random");
    return snprintf(buf, (size_t)len, "%d", inst->source_sel + 1);
}

/* ---- generation --------------------------------------------------------- */

static void generate(pg_instance_t *inst) {
    if (inst->lib.n <= 0) return;

    /* Mix in when the press happened (frames since load) so two sessions
     * don't produce the same sequence of phrases. */
    inst->rng.s ^= inst->entropy * 0x9E3779B1u + inst->gen_count * 0x85EBCA6Bu;
    if (inst->rng.s == 0) inst->rng.s = 0x6D2B79F5u;
    pg_rng_next(&inst->rng);

    pg_gen_params_t params = {
        .mutation = (float)inst->mutation / 100.0f,
        .source = inst->source_sel,
        .avoid_source = inst->cur_source,
    };
    pg_pattern_t next;
    int src = pg_generate(&inst->lib.model, &params, &inst->rng,
                          pg_seq_latest(&inst->seq), &next, NULL);
    if (src < 0) return;
    inst->cur_source = src;
    inst->gen_count++;
    pg_seq_queue(&inst->seq, &next);
}

/* ---- transport ---------------------------------------------------------- */

static double transport_beat(pg_instance_t *inst) {
    if (g_host && g_host->get_beat_position) return g_host->get_beat_position();
    if (!inst->clk_running || inst->clk_awaiting_first) return -1.0;
    return (double)inst->clk_ticks / PG_CLOCKS_PER_BEAT;
}

static void fallback_clock(pg_instance_t *inst, uint8_t status) {
    switch (status) {
        case 0xFA: inst->clk_running = 1; inst->clk_awaiting_first = 1; inst->clk_ticks = 0; break;
        case 0xFB: inst->clk_running = 1; break;
        case 0xFC: inst->clk_running = 0; break;
        case 0xF8:
            if (!inst->clk_running) { inst->clk_running = 1; inst->clk_awaiting_first = 1; }
            if (inst->clk_awaiting_first) { inst->clk_awaiting_first = 0; inst->clk_ticks = 0; }
            else inst->clk_ticks++;
            break;
        default: break;
    }
}

/* ---- API ---------------------------------------------------------------- */

static void *pg_create_instance(const char *module_dir, const char *config_json) {
    (void)module_dir;
    (void)config_json;

    pg_instance_t *inst = calloc(1, sizeof(pg_instance_t));
    if (!inst) return NULL;

    pg_library_init(&inst->lib, PG_ROOT, PG_SCALE_MASK, PG_LENGTH);
    for (int i = 0; i < PG_EMBEDDED_COUNT; i++) {
        const pg_embedded_pattern_t *e = &PG_EMBEDDED_PATTERNS[i];
        pg_library_add(&inst->lib, e->name, e->data, e->len, e->start_step);
    }
    pg_library_finalize(&inst->lib);

    pg_pad_init(&inst->pad);
    inst->mutation = PG_DEFAULT_MUTATION;
    inst->source_sel = -1;
    inst->cur_source = -1;
    /* ASLR makes the instance address differ per load: a cheap, RT-safe seed. */
    pg_rng_seed(&inst->rng, (uint32_t)(uintptr_t)inst ^ 0x5A17C0DEu);

    /* Never start silent: open on a variation of one source. */
    pg_seq_init(&inst->seq, NULL);
    generate(inst);
    return inst;
}

static void pg_destroy_instance(void *instance) {
    free(instance);
}

static int pass_through(const uint8_t *in, int in_len, uint8_t out[][3], int out_lens[]) {
    out[0][0] = in[0];
    out[0][1] = in_len > 1 ? in[1] : 0;
    out[0][2] = in_len > 2 ? in[2] : 0;
    out_lens[0] = in_len;
    return 1;
}

static int pg_process_midi(void *instance, const uint8_t *in, int in_len,
                           uint8_t out[][3], int out_lens[], int max_out) {
    pg_instance_t *inst = (pg_instance_t *)instance;
    if (!inst || !in || in_len < 1 || max_out < 1) return 0;

    const uint8_t status = in[0];
    if (status >= 0xF8) {                               /* realtime */
        fallback_clock(inst, status);
        if (status == 0xFC) pg_seq_flush(&inst->seq);
        return pass_through(in, in_len, out, out_lens);
    }

    const uint8_t type = status & 0xF0;
    if ((type == 0x90 || type == 0x80) && in_len >= 3) {
        const int on = type == 0x90 && in[2] > 0;
        if (on) inst->seq.channel = status & 0x0F;      /* play on the slot's channel */
        switch (pg_pad_on_note(&inst->pad, on, in[1])) {
            case PG_PAD_GENERATE: generate(inst); return 0;
            case PG_PAD_SWALLOW:  return 0;
            case PG_PAD_PASS:
            default:              return pass_through(in, in_len, out, out_lens);
        }
    }

    if (type == 0xB0 && in_len >= 3 && (in[1] == 120 || in[1] == 123))
        pg_seq_flush(&inst->seq);                       /* all sound / notes off */
    return pass_through(in, in_len, out, out_lens);
}

static int pg_tick(void *instance, int frames, int sample_rate,
                   uint8_t out[][3], int out_lens[], int max_out) {
    (void)sample_rate;
    pg_instance_t *inst = (pg_instance_t *)instance;
    if (!inst) return 0;
    inst->entropy += (uint32_t)(frames > 0 ? frames : 1);
    return pg_seq_tick(&inst->seq, transport_beat(inst), out, out_lens, max_out);
}

static void pg_set_param(void *instance, const char *key, const char *val) {
    pg_instance_t *inst = (pg_instance_t *)instance;
    if (!inst || !key || !val) return;
    int v;

    if (strcmp(key, "mutation") == 0) {
        if (parse_int(val, &v)) inst->mutation = clampi(v, 0, 100);
    } else if (strcmp(key, "trigger") == 0) {
        set_trigger(inst, val);
    } else if (strcmp(key, "trig_note") == 0) {
        if (parse_int(val, &v)) inst->pad.trig_note = clampi(v, 0, 127);
    } else if (strcmp(key, "source") == 0) {
        set_source(inst, val);
    } else if (strcmp(key, "generate") == 0) {
        generate(inst);                                 /* write-only trigger */
    } else if (strcmp(key, "state") == 0) {
        char s[16];
        pg_pattern_t pat;
        if (json_get_int(val, "mutation", &v)) inst->mutation = clampi(v, 0, 100);
        if (json_get_string(val, "trigger", s, sizeof(s))) set_trigger(inst, s);
        if (json_get_int(val, "trig_note", &v)) inst->pad.trig_note = clampi(v, 0, 127);
        if (json_get_string(val, "source", s, sizeof(s))) set_source(inst, s);
        if (json_get_int(val, "cur_source", &v) && v >= 0 && v < inst->lib.n) inst->cur_source = v;
        /* The phrase itself comes back too, so a reloaded set plays what
         * was playing — not a fresh roll. */
        char enc[PG_MAX_STEPS * 14 + 8];
        /* A pattern saved by v0.1.0 may contain rests; this version never
         * plays one, so such a pattern is dropped for the freshly generated. */
        if (json_get_string(val, "pattern", enc, sizeof(enc)) && pg_decode_pattern(enc, &pat) &&
            pg_pattern_note_count(&pat) == pat.length) {
            for (int s2 = 0; s2 < pat.length; s2++)     /* never trust saved pitches */
                if (pg_step_is_note(&pat.steps[s2]))
                    pat.steps[s2].note = (int8_t)pg_scale_quantize(&inst->lib.scale, pat.steps[s2].note);
            pg_seq_queue(&inst->seq, &pat);
        }
    }
}

static int pg_get_param(void *instance, const char *key, char *buf, int buf_len) {
    pg_instance_t *inst = (pg_instance_t *)instance;
    if (!inst || !key || !buf || buf_len < 1) return -1;

    if (strcmp(key, "mutation") == 0)
        return snprintf(buf, (size_t)buf_len, "%d", inst->mutation);
    if (strcmp(key, "trigger") == 0)
        return snprintf(buf, (size_t)buf_len, "%s", pg_trig_mode_name(inst->pad.mode));
    if (strcmp(key, "trig_note") == 0)
        return snprintf(buf, (size_t)buf_len, "%d", inst->pad.trig_note);
    if (strcmp(key, "source") == 0)
        return source_label(inst, buf, buf_len);
    if (strcmp(key, "source_name") == 0)
        return snprintf(buf, (size_t)buf_len, "%s",
                        inst->cur_source >= 0 ? inst->lib.names[inst->cur_source] : "");
    if (strcmp(key, "step") == 0)                       /* 1-based, 0 = stopped */
        return snprintf(buf, (size_t)buf_len, "%d", inst->seq.play_step + 1);
    if (strcmp(key, "gen_count") == 0)
        return snprintf(buf, (size_t)buf_len, "%u", inst->gen_count);
    /* No host hierarchy: the shadow UI then opens ui_chain.js, which draws
     * its own screen and takes the pads.  Served-and-EMPTY (0), never -1: a
     * failed read is "not known yet" to the shadow UI, an empty one is "none". */
    if (strcmp(key, "ui_hierarchy") == 0) {
        buf[0] = '\0';
        return 0;
    }
    /* Everything ui_chain.js draws, in one read (a read costs ~2.8 ms):
     *   1,running,step,mutation,trigger,trig_note,source_sel,cur_source,
     *   gen_count,pending,n_sources, current[8 notes], next[8 notes]
     * A note is 0..127, or -1 for a rest. */
    if (strcmp(key, "ui_state") == 0) {
        const pg_pattern_t *cur = &inst->seq.current, *nxt = pg_seq_latest(&inst->seq);
        int n = snprintf(buf, (size_t)buf_len, "1,%d,%d,%d,%d,%d,%d,%d,%u,%d,%d",
                         inst->seq.running, inst->seq.play_step, inst->mutation,
                         (int)inst->pad.mode, inst->pad.trig_note, inst->source_sel,
                         inst->cur_source, inst->gen_count, inst->seq.has_pending, inst->lib.n);
        for (int k = 0; k < 2 * PG_LENGTH && n > 0 && n < buf_len; k++) {
            const pg_pattern_t *p = k < PG_LENGTH ? cur : nxt;
            int st = k % PG_LENGTH;
            int note = st < p->length && pg_step_is_note(&p->steps[st]) ? p->steps[st].note : -1;
            n += snprintf(buf + n, (size_t)(buf_len - n), ",%d", note);
        }
        return n < buf_len ? n : -1;
    }
    if (strcmp(key, "debug") == 0)
        return pg_format_pattern(pg_seq_latest(&inst->seq),
                                 inst->cur_source >= 0 ? inst->lib.names[inst->cur_source] : "?",
                                 buf, buf_len);

    if (strcmp(key, "state") == 0) {
        char enc[PG_MAX_STEPS * 14 + 8], src[8];
        pg_encode_pattern(pg_seq_latest(&inst->seq), enc, sizeof(enc));
        source_label(inst, src, sizeof(src));
        return snprintf(buf, (size_t)buf_len,
                        "{\"mutation\":%d,\"trigger\":\"%s\",\"trig_note\":%d,"
                        "\"source\":\"%s\",\"cur_source\":%d,\"pattern\":\"%s\"}",
                        inst->mutation, pg_trig_mode_name(inst->pad.mode),
                        inst->pad.trig_note, src, inst->cur_source, enc);
    }

    if (strcmp(key, "chain_params") == 0) {
        int n = snprintf(buf, (size_t)buf_len,
            "["
            "{\"key\":\"mutation\",\"name\":\"Mutation\",\"type\":\"int\",\"min\":0,\"max\":100,\"step\":1},"
            "{\"key\":\"trigger\",\"name\":\"Trigger\",\"type\":\"enum\",\"options\":[\"any\",\"note\",\"learn\"]},"
            "{\"key\":\"trig_note\",\"name\":\"Trig Note\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
            "{\"key\":\"source\",\"name\":\"Source\",\"type\":\"enum\",\"options\":[\"random\"");
        for (int i = 0; i < inst->lib.n && n > 0 && n < buf_len; i++)
            n += snprintf(buf + n, (size_t)(buf_len - n), ",\"%d\"", i + 1);
        if (n > 0 && n < buf_len) n += snprintf(buf + n, (size_t)(buf_len - n), "]}]");
        return n < buf_len ? n : -1;
    }
    return -1;
}

static midi_fx_api_v1_t g_api = {
    .api_version = MIDI_FX_API_VERSION,
    .create_instance = pg_create_instance,
    .destroy_instance = pg_destroy_instance,
    .process_midi = pg_process_midi,
    .tick = pg_tick,
    .set_param = pg_set_param,
    .get_param = pg_get_param,
};

midi_fx_api_v1_t *move_midi_fx_init(const host_api_v1_t *host) {
    g_host = host;
    return &g_api;
}

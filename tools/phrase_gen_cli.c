/*
 * phrase-gen-cli — run the Phrase Gen engine on the desktop.
 *
 * The device cannot log from the audio thread, so this is where the
 * generator is tuned: it builds the same model the plugin builds, from MIDI
 * files on disk (or the embedded set), and prints the analysis and N
 * generated phrases in the debug format.
 *
 *   phrase-gen-cli [-n count] [-m mutation 0..100] [-s seed] [-S source 1..N]
 *                  [-q] [file.mid[:start_step] ...]
 *
 *   -q   summary only: per-phrase one-liners plus retention averages
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pg_library.h"
#include "pg_generator.h"
#include "pg_format.h"
#include "pg_patterns_data.h"

static int load_file(pg_library_t *lib, const char *arg) {
    char path[512];
    snprintf(path, sizeof(path), "%s", arg);
    int start = 0;
    char *colon = strrchr(path, ':');
    if (colon && colon[1] >= '0' && colon[1] <= '9') { start = atoi(colon + 1); *colon = '\0'; }

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    static uint8_t buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    int rc = pg_library_add(lib, base, buf, n, start);
    if (rc < 0) fprintf(stderr, "%s: rejected (error %d)\n", path, rc);
    return rc;
}

static void print_pattern_row(const char *label, const pg_pattern_t *p) {
    printf("  %-14s", label);
    for (int s = 0; s < p->length; s++) {
        char nm[8];
        if (pg_step_is_note(&p->steps[s])) { pg_note_name(p->steps[s].note, nm, sizeof(nm)); printf(" %-4s", nm); }
        else printf(" %-4s", "-");
    }
    printf("\n");
}

static void print_stats(const pg_library_t *lib) {
    const pg_stats_t *c = &lib->corpus;
    printf("=== Analysis (%d sources, %d steps, C Phrygian) ===\n", lib->n, lib->length);
    for (int i = 0; i < lib->n; i++) {
        char label[40];
        snprintf(label, sizeof(label), "%d %s", i + 1, lib->names[i]);
        print_pattern_row(label, &lib->pats[i]);
    }
    printf("  onset prob   ");
    for (int s = 0; s < lib->length; s++) printf(" %4.2f", c->onset_prob[s]);
    printf("\n  density      mean %.2f  min %d  max %d notes / %d steps\n",
           c->count_mean, c->count_min, c->count_max, lib->length);
    printf("  register     %d..%d", c->note_min, c->note_max);
    printf("   octaves:");
    for (int o = 0; o < PG_OCTAVES; o++) if (c->octave_hist[o] > 0) printf(" %d:%.2f", o - 2, c->octave_hist[o]);
    static const char *pcn[12] = {"C","Db","D","Eb","E","F","Gb","G","Ab","A","Bb","B"};
    printf("\n  pitch class ");
    for (int i = 0; i < 12; i++) if (c->pc_hist[i] > 0) printf(" %s:%.2f", pcn[i], c->pc_hist[i]);
    printf("\n  movement     up %.2f  down %.2f  repeat %.2f  leap(>=oct) %.2f  (%d transitions)\n",
           c->up_rate, c->down_rate, c->repeat_rate, c->leap_rate, c->transitions);
    printf("  velocity     mean %.1f  dev %.1f  range %d..%d\n", c->vel_mean, c->vel_dev, c->vel_min, c->vel_max);
    printf("  gate         mean %.1f%%  range %d..%d%%\n\n", c->gate_mean, c->gate_min, c->gate_max);
}

int main(int argc, char **argv) {
    int count = 8, mutation = 50, source = -1, quiet = 0;
    unsigned seed = 1;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-q")) { quiet = 1; continue; }
        if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", argv[i]); return 2; }
        if (!strcmp(argv[i], "-n")) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-m")) mutation = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s")) seed = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-S")) source = atoi(argv[++i]) - 1;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }

    static pg_library_t lib;
    pg_library_init(&lib, 0, PG_MASK_PHRYGIAN, PG_DEFAULT_STEPS);
    if (i < argc) {
        for (; i < argc; i++) load_file(&lib, argv[i]);
    } else {
        for (int k = 0; k < PG_EMBEDDED_COUNT; k++)
            pg_library_add(&lib, PG_EMBEDDED_PATTERNS[k].name, PG_EMBEDDED_PATTERNS[k].data,
                           PG_EMBEDDED_PATTERNS[k].len, PG_EMBEDDED_PATTERNS[k].start_step);
    }
    if (lib.n == 0) { fprintf(stderr, "no usable patterns\n"); return 1; }
    pg_library_finalize(&lib);
    print_stats(&lib);

    pg_rng_t rng;
    pg_rng_seed(&rng, seed);
    pg_pattern_t prev, out;
    pg_pattern_clear(&prev, lib.length);
    int last_src = -1;
    double kept_rhythm = 0, kept_pitch = 0, notes_ratio = 0;

    printf("=== Generated (mutation %d%%, seed %u) ===\n", mutation, seed);
    for (int k = 0; k < count; k++) {
        pg_gen_params_t p = { .mutation = mutation / 100.0f, .source = source, .avoid_source = last_src };
        pg_gen_report_t rep;
        int src = pg_generate(&lib.model, &p, &rng, &prev, &out, &rep);
        last_src = src;
        prev = out;
        int orig_notes = rep.notes_src;
        int unchanged = 0;                        /* same pitch on the same step */
        for (int s = 0; s < out.length; s++)
            if (pg_step_is_note(&out.steps[s]) && out.steps[s].note == lib.pats[src].steps[s].note) unchanged++;
        kept_rhythm += (double)rep.rhythm_kept / out.length;
        kept_pitch += orig_notes ? (double)unchanged / orig_notes : 0;
        notes_ratio += orig_notes ? (double)rep.notes_out / orig_notes : 0;

        if (quiet) {
            char label[40];
            snprintf(label, sizeof(label), "#%d <- %d", k + 1, src + 1);
            print_pattern_row(label, &out);
        } else {
            char text[1024];
            printf("--- #%d ---\n", k + 1);
            pg_format_pattern(&out, lib.names[src], text, sizeof(text));
            fputs(text, stdout);
            printf("(notes %d->%d, +%d -%d moved %d, pitch changed %d, rhythm kept %d/%d)\n",
                   rep.notes_src, rep.notes_out, rep.added, rep.deleted, rep.moved,
                   rep.pitch_changed, rep.rhythm_kept, out.length);
        }
    }
    if (count > 0)
        printf("\nAverages over %d: rhythm kept %.0f%%, notes kept verbatim %.0f%%, density x%.2f\n",
               count, 100 * kept_rhythm / count, 100 * kept_pitch / count, notes_ratio / count);
    return 0;
}

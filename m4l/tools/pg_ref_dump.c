/*
 * Phrase Gen M4L — reference dumper.
 *
 * Builds the library exactly as the Move plugin does (embedded MIDI bytes,
 * C Phrygian, 8 steps) and prints JSON:
 *   {"sources": [[[note,vel,gate] x8] ...], "names": [...],
 *    "runs": [[source, [[note,vel,gate] x8]] ...]}
 * `runs` is `n` successive generations from one seed, each avoiding the last,
 * which is what tests/test_engine.js compares the JavaScript port against.
 *
 * usage: pg_ref_dump <seed> <mutation 0..100> <n> <source -1..N-1>
 */
#include <stdio.h>
#include <stdlib.h>
#include "pg_library.h"
#include "pg_patterns_data.h"

static pg_library_t lib;

static void print_pat(const pg_pattern_t *p) {
    printf("[");
    for (int s = 0; s < p->length; s++)
        printf("%s[%d,%d,%d]", s ? "," : "", p->steps[s].note, p->steps[s].vel, p->steps[s].gate);
    printf("]");
}

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s seed mutation n source\n", argv[0]); return 2; }
    unsigned seed = (unsigned)strtoul(argv[1], NULL, 10);
    int mut = atoi(argv[2]), n = atoi(argv[3]), source = atoi(argv[4]);

    pg_library_init(&lib, 0, PG_MASK_PHRYGIAN, PG_DEFAULT_STEPS);
    for (int i = 0; i < PG_EMBEDDED_COUNT; i++) {
        const pg_embedded_pattern_t *e = &PG_EMBEDDED_PATTERNS[i];
        pg_library_add(&lib, e->name, e->data, e->len, e->start_step);
    }
    pg_library_finalize(&lib);

    printf("{\"sources\":[");
    for (int i = 0; i < lib.n; i++) { if (i) printf(","); print_pat(&lib.pats[i]); }
    printf("],\"names\":[");
    for (int i = 0; i < lib.n; i++) printf("%s\"%s\"", i ? "," : "", lib.names[i]);
    printf("],\"runs\":[");

    pg_rng_t rng;
    pg_rng_seed(&rng, seed);
    pg_pattern_t prev, out;
    int have_prev = 0, cur = -1;
    for (int k = 0; k < n; k++) {
        pg_gen_params_t gp = { .mutation = (float)mut / 100.0f, .source = source,
                               .avoid_source = cur, .allow_rests = 0 };
        cur = pg_generate(&lib.model, &gp, &rng, have_prev ? &prev : NULL, &out, NULL);
        printf("%s[%d,", k ? "," : "", cur);
        print_pat(&out);
        printf("]");
        prev = out;
        have_prev = 1;
    }
    printf("]}\n");
    return 0;
}

/* Phrase Gen — source library.  See pg_library.h. */
#include "pg_library.h"

#include <string.h>
#include "pg_midi_loader.h"

void pg_library_init(pg_library_t *lib, int root, uint16_t mask, int length) {
    memset(lib, 0, sizeof(*lib));
    pg_scale_init(&lib->scale, root, mask);
    if (length < 1) length = PG_DEFAULT_STEPS;
    if (length > PG_MAX_STEPS) length = PG_MAX_STEPS;
    lib->length = length;
}

int pg_library_add(pg_library_t *lib, const char *name,
                   const uint8_t *data, size_t len, int start_step) {
    if (lib->n >= PG_MAX_SOURCES) return -100;

    pg_midi_file_t mf;                      /* ~3 KB, stack only */
    int rc = pg_midi_parse(data, len, &mf);
    if (rc != PG_MIDI_OK) return rc;

    pg_pattern_t *pat = &lib->pats[lib->n];
    if (pg_midi_extract(&mf, start_step, lib->length, pat) <= 0) return -101;

    /* Sources are snapped into the scale here, once, so a stray note in a
     * source can never become the "character" the generator copies. */
    for (int s = 0; s < pat->length; s++)
        if (pg_step_is_note(&pat->steps[s]))
            pat->steps[s].note = (int8_t)pg_scale_quantize(&lib->scale, pat->steps[s].note);

    size_t i = 0;
    for (; name && name[i] && i < PG_NAME_LEN - 1; i++) lib->names[lib->n][i] = name[i];
    lib->names[lib->n][i] = '\0';
    return lib->n++;
}

void pg_library_finalize(pg_library_t *lib) {
    for (int i = 0; i < lib->n; i++)
        pg_analyze(&lib->pats[i], 1, &lib->scale, &lib->stats[i]);
    pg_analyze(lib->pats, lib->n, &lib->scale, &lib->corpus);

    lib->model.sources = lib->pats;
    lib->model.source_stats = lib->stats;
    lib->model.corpus = &lib->corpus;
    lib->model.n_sources = lib->n;
    lib->model.scale = &lib->scale;
}

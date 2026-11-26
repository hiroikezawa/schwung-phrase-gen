/*
 * Phrase Gen — source library: MIDI bytes in, generator model out.
 *
 * The one place that strings MidiPatternLoader -> PatternAnalyzer together,
 * so the plugin (embedded bytes) and the host CLI (files on disk) build
 * exactly the same model from the same input.
 */
#ifndef PG_LIBRARY_H
#define PG_LIBRARY_H

#include <stddef.h>
#include <stdint.h>
#include "pg_pattern.h"
#include "pg_scale.h"
#include "pg_analyzer.h"
#include "pg_generator.h"

typedef struct {
    pg_scale_t   scale;
    int          n;
    int          length;                            /* steps per pattern */
    pg_pattern_t pats[PG_MAX_SOURCES];
    char         names[PG_MAX_SOURCES][PG_NAME_LEN];
    pg_stats_t   stats[PG_MAX_SOURCES];
    pg_stats_t   corpus;
    pg_model_t   model;
} pg_library_t;

void pg_library_init(pg_library_t *lib, int root, uint16_t mask, int length);

/* Parse one SMF and add its window [start_step, start_step + length) as a
 * source.  Returns the source index, or a negative PG_MIDI_E_* / -100 when
 * the library is full / -101 when the window holds no notes. */
int  pg_library_add(pg_library_t *lib, const char *name,
                    const uint8_t *data, size_t len, int start_step);

/* Analyse everything added and wire up lib->model.  Call after the last add. */
void pg_library_finalize(pg_library_t *lib);

#endif /* PG_LIBRARY_H */

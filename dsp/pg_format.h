/*
 * Phrase Gen — text forms of a pattern.
 *
 *   pg_note_name()        60 -> "C3" (Ableton's naming: middle C is C3)
 *   pg_format_pattern()   the debug listing ("Step 1: C3 vel=104 ...")
 *   pg_encode_pattern()   compact form for the saved state
 *   pg_decode_pattern()   its inverse; rejects anything malformed
 *
 * snprintf into caller buffers only: no allocation, no I/O.
 */
#ifndef PG_FORMAT_H
#define PG_FORMAT_H

#include "pg_pattern.h"

void pg_note_name(int note, char *buf, int buf_len);

/* Returns the length written (like snprintf, clipped to buf_len - 1). */
int  pg_format_pattern(const pg_pattern_t *p, const char *source_name,
                       char *buf, int buf_len);

/* "8|53.100.92,-,65.100.92,..." — length, then one entry per step. */
int  pg_encode_pattern(const pg_pattern_t *p, char *buf, int buf_len);
int  pg_decode_pattern(const char *s, pg_pattern_t *out);   /* 1 ok, 0 bad */

#endif /* PG_FORMAT_H */

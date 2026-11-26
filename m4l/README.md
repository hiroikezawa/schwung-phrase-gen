# Phrase Gen — Max for Live version

**English** | [日本語](README.ja.md)

A Max for Live MIDI effect that **writes an 8-step (eight 1/16 notes) C Phrygian
phrase straight into an Ableton Live clip**. It neither plays nor records.
Pressing GENERATE replaces the clip's notes, and Ctrl+Z / Cmd+Z undoes it like
any normal edit in Live.

The generation engine is the same as the Ableton Move MIDI FX "Phrase Gen": the
C code is ported to JavaScript line by line. The six source MIDI files are built
in. With the same random seed it generates exactly the same phrases as the Move
version (verified in `tests/test_engine.js`).

## Requirements

- Ableton Live 11 / 12 with Max for Live (Suite, or Standard + Max for Live)
- Live 10 is not supported (it uses the note editing API added in Live 11).

## Installing

Place these two files **together in the same folder**:

```
Phrase Gen/
  Phrase Gen.amxd
  phrase_gen_m4l.js
```

Copy the whole folder into your User Library, for example
`Documents/Ableton/User Library/Presets/MIDI Effects/Max MIDI Effect/`,
then drag `Phrase Gen.amxd` onto a MIDI track.

(Optional) To make it a single file: open Max with the device's Edit button,
*Freeze Device*, then save. A frozen .amxd carries the .js inside it.

## Using it

1. Put the device on a MIDI track. Incoming MIDI passes straight through.
2. **Session View**: click a clip slot on any MIDI track.
   - An empty slot → a new clip is created.
   - A slot with a clip → all of that clip's notes are replaced, the loop length
     is set to Length, and the clip is named `PG <source number>`.
3. **Arrangement View**: select a MIDI clip. Its length and position stay as they
   are, and the phrase is written repeatedly across the whole loop range.
4. Press **GENERATE**. Press again for a different phrase.

| Control | What it does |
|------|------|
| Mutation | 0 % = the source phrase as it is, 50 % = about half the notes changed, 100 % = bold changes |
| Source | `random` (never the same source twice in a row), or fixed to 1–6 |
| Length | Session clips only: 8 steps (2 beats) / 1 bar / 2 bars / 4 bars. Filled by repeating the 8-step phrase |
| GENERATE | Writes a new phrase into the target clip |

Every step sounds (no rests). Notes are 1/16 notes quantised to C Phrygian
(C Db Eb F G Ab Bb). The status line under the button says what was written, or
why nothing could be written.

## Building (for developers)

```
python3 build_m4l.py        # embeds the MIDI files in ../patterns and makes dist/Phrase Gen/
node tests/test_engine.js <reference file>
node tests/test_live_glue.js
```

`build_m4l.py` compiles `tools/pg_ref_dump.c` together with the C engine in
`../dsp` and runs it, so the phrases built into the device are exactly the ones
the Move version makes from the same MIDI files.

Copyright (c) 2026 Hiro Ikezawa. MIT License.

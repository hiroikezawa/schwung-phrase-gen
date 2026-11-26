# Phrase Gen v0.4.0 — Schwung MIDI FX

**English** | [日本語](README.ja.md)

A Schwung MIDI FX that analyses six source MIDI files and, every time a pad is
pressed, generates an 8-step (eight 1/16 notes) C Phrygian phrase that sounds
"as if the same person wrote it", looping it in sync with Move's tempo. It has
no sound of its own; it sends Note On/Off to the Sound Generator after it.

## 1. Schwung APIs used, and existing code consulted

| Item | What | Existing code consulted |
|---|---|---|
| Plugin API | `midi_fx_api_v1_t` (`create_instance` / `process_midi` / `tick` / `set_param` / `get_param`), exported as `move_midi_fx_init` | `src/host/midi_fx_api_v1.h`, the built-in `arp` / `chord` |
| Loading | The chain `dlopen`s `modules/midi_fx/<id>/dsp.so` | `src/modules/chain/dsp/chain_midi.c` `v2_load_midi_fx_slot` |
| Output | `tick()` is called every audio block (128 frames ≈ 2.9 ms); the messages it returns flow on to the next MIDI FX → Synth | `chain_midi.c` `v2_tick_midi_fx` |
| Tempo sync | `host->get_beat_position()` (the beat position interpolated at 24 PPQN from Move's cable-0 transport; negative while stopped). Does not depend on Move's MIDI Clock Out setting | `src/host/shadow_transport.c`, the chain LFO's sync |
| Pad input | Move turns a pad press into "a note on that track's channel", which reaches the slot's MIDI FX `process_midi` (the same path by which arp receives held keys) | `arp.c` |
| Parameter UI | `ui_hierarchy` in `module.json` (knob assignment) + `get_param("chain_params")` (metadata) | `arp` / `chord` module.json |
| Saved state | JSON through `get_param("state")` / `set_param("state")` (slot autosave / presets) | `chain_patch.c` |
| Distribution | A separate package `dist/phrase-gen-module.tar.gz`, for Schwung Manager's Custom install (tarball) | A package separate from Schwung itself |

**The most important constraint (top of `src/host/plugin_api_v1.h`)**: every
MIDI FX entry point (including `create_instance`) runs on the SPI audio
callback, where file I/O, malloc and logging are forbidden. Therefore:

- The MIDI files are **embedded into `dsp.so` as C arrays at build time**; at run
  time only an in-memory byte array is parsed (`scripts/embed_patterns.py`).
- Analysis, generation and playback all use fixed-length arrays and bounded
  loops. The only allocation is one `calloc` per instance (as in arp).
- A "debug log" cannot be printed on the device, so `get_param("debug")` returns
  the current phrase in the same format, and a PC CLI (`phrase-gen-cli`) shows
  the output of the same engine.

## 2. Design

### Generation algorithm ("variation centred on a source pattern")

1. Pick one of the six (if it repeats the previous source, draw again once).
2. Copy that source and apply the following in proportion to **Mutation** (0–100 %):
   - **Rhythm (from v0.1.1, default: no rests)**: all 8 steps always sound. Rests
     in the source (step 6 of pattern02) are filled with a Markov note before
     generation; notes are never deleted or shifted.
     With `pg_gen_params_t.allow_rests = 1` it returns to the v0.1.0 behaviour
     (note count = original ±(Mutation×2), deletions from the steps least likely
     to sound, additions at the steps most likely to sound, a little nudge).
   - **Pitch** (about 20+60×Mutation % of the original notes): one of these,
     chosen with weights
     - move ±1–3 steps along the scale (the up/down ratio comes from the analysis)
     - octave ± (pulled towards the sources' octave distribution; chosen more
       often the higher the leap rate)
     - repeat the previous note
     - Markov resample (pitch class from the scale-degree transition table,
       register from the distribution of intervals)
   - **Added notes** take a Markov pitch, and velocity/gate from the source's
     distribution.
   - Small **velocity** changes (±3–17) and **note length** changes (×0.5–×2,
     probability 35 %×Mutation).
3. Always **quantise to C Phrygian**, and fold by octaves into the sources'
   overall range (C1–Eb7 here).
4. If the result equals the previous phrase or the source itself, make it again
   (when Mutation > 0).

The source's own statistics (weight 0.6) are mixed with those of all six (0.4),
giving "that source's skeleton plus the habits of the others".

Features analysed (`pg_analyzer`): the probability of each step sounding (= rest
positions), note density (mean/min/max), pitch-class distribution, octave
distribution, range, the distribution of intervals from the previous note, the
repeated-note rate, the up/down ratio, the rate of leaps of an octave or more, a
Markov transition table of scale degrees, the velocity distribution (mean, mean
deviation, range) and the note-length distribution.

**Analysis of these six** (from `phrase-gen-cli` output): almost every step
sounds (7–8 notes per 8 steps), the only rest is step 6 of pattern02, 57 % of
intervals are leaps of an octave or more, 0 % repeated notes, 64 % up / 36 %
down, every velocity 100, every length 92 % of a 1/16.

**Measured per Mutation** (v0.1.1, mean of 4000 each): always 8 notes in 8
steps (no rests).

| Mutation | Original notes kept |
|---|---|
| 0% | 100 % (the original MIDI's notes as they are; only former rest positions are filled) |
| 50% (default) | about 50 % |
| 75% | about 34 % |
| 100% | about 20 % |

### Playback (`pg_sequencer`)

- No timer of its own. Every block reads `get_beat_position()` and plays a step
  when a 1/16 boundary is crossed. Step numbers count from the transport's beat
  0, so the start of the 8 steps is always on a bar or half-bar.
- A new phrase **switches in at the next 8-step boundary** (pressing midway does
  not disturb the timing). Pressing again before the boundary means the latest
  wins. Pressing while stopped replaces it at once.
- Silent while stopped (no free running).
- Only when the host has no `get_beat_position` is the 0xF8 clock arriving at
  `process_midi` used instead.
- Jitter: a note-on can be up to one block (≈2.9 ms) late. That is the limit,
  because MIDI FX output has no sample offset.

### Stuck-note protection

- Each note records its voice (pitch, channel, end beat). Stop, CC120/123, 0xFC
  and a transport rewind (a restart without a stop in between) release every voice.
- Re-sounding a pitch that is still playing always goes Off→On.
- A Note Off that could not be sent because the output buffer was full is "owed"
  and sent in the next block.
- Even if a key is held through a PAD mode change, that key's Note Off is treated
  like its Note On (the Off of an On that was passed is always passed).

## 3. Files

```
schwung-phrase-gen/
  module.json            chain_params (the DSP's four knobs)
  ui_chain.js / ui_core.mjs  dedicated screen (pads = GENERATE, knobs 1–5)
  pad_anim.mjs           pad animations (RIPPLE / SNAKE / INVADE)
  help.json              on-device help
  patterns/
    patterns.txt         list and order of the MIDI files used (edit only this to replace them)
    pattern01..06.mid    sources (six phrases written by Hiro Ikezawa)
  dsp/
    pg_pattern.h         shared types (step/pattern)
    pg_midi_loader.[ch]  MidiPatternLoader: SMF parse → any stretch extracted to 8 steps (two separate stages)
    pg_analyzer.[ch]     PatternAnalyzer: statistics of the features above
    pg_generator.[ch]    PatternGenerator: source choice + variation (rhythm / pitch / dynamics stages)
    pg_scale.[ch]        ScaleQuantizer: root + 12-bit mask (C Phrygian for now)
    pg_sequencer.[ch]    Sequencer: transport-synced playback, swaps at boundaries, voice management
    pg_pad.[ch]          PadController: input note → GENERATE / pass through / drop
    pg_library.[ch]      wiring of Loader + Analyzer (shared by the plugin and the CLI)
    pg_format.[ch]       note names, debug display, encoding for state
    pg_rng.h             deterministic RNG (xorshift32) and weighted choice
    phrase_gen.c         only the glue to the Schwung API (parameters, state)
  scripts/
    build.sh             native | arm64 (default)
    embed_patterns.py    patterns/ → build/generated/pg_patterns_data.h (+ Source options sized to the file count)
    check_abi.py         checks AArch64, exports, GLIBC ≤ 2.34 and the absence of RT-forbidden functions
    test.sh              native build + the whole test suite
    install.py           upload to Schwung Manager's Custom install
  tests/
    test_phrase_gen.c    unit + integration tests through the API (ASan/UBSan)
    test_dlopen.c        dlopens the built dsp.so the same way the chain does and drives it
  tools/
    phrase_gen_cli.c     for the PC: prints analysis and generated results in the debug format
  m4l/                   the Max for Live version (writes phrases into Live clips; see m4l/README.md)
  third_party/schwung/   Schwung's plugin API headers (MIT)
```

## 4. Building

In this repository (Linux/WSL, `gcc`, `aarch64-linux-gnu-gcc`, Python 3, and
Node.js for the UI test). Schwung's plugin API headers (MIT) are bundled in
`third_party/schwung/`:

```bash
bash scripts/test.sh          # native build + all tests
bash scripts/build.sh arm64   # package for Move
```

Output: `dist/phrase-gen-module.tar.gz`. It is not part of Schwung's own
distribution and is provided as a separate package.

## 5. Installing

Install onto a Move with Schwung through the Manager's Custom install (tarball).

```bash
python3 scripts/install.py http://move.local:7700 --check
python3 scripts/install.py http://move.local:7700
```

Choosing `phrase-gen-module.tar.gz` in the browser at
`http://<Move's IP>:7700/modules` → Custom install → tarball does the same. It
unpacks to `modules/midi_fx/phrase-gen/`.

## 6. Using it on Move

1. Set the MIDI Out of the Move track you use (e.g. Track 1) to ch1, and if you do
   not want Move's built-in sound, load an empty Drum Rack / Sampler preset (as in
   Schwung's "Connecting Tracks").
2. Touch the Volume knob with Shift held and press Track 1 to open Schwung's
   Slot 1 (Recv Ch = 1).
3. Choose **Phrase Gen** with **+** at the chain's MIDI FX position, and put any
   instrument in the Synth after it.
4. Press **Play** on Move and the initial phrase (a variation of one of the six)
   loops over 8 steps.
5. **Every press of a Track 1 pad generates a new phrase**, which switches in at
   the start of the next 8 steps.

### The Phrase Gen screen (from v0.2.0)

Choose Phrase Gen in the chain and open it, and a dedicated screen
(`ui_chain.js`) opens instead of the standard knob screen.

- All 32 pads become **GENERATE buttons** and **animate across the whole pad
  grid** (from v0.4.0, `pad_anim.mjs`). A pressed pad is white while held. Knob 5
  chooses the animation.
  - **RIPPLE (radial)**: colourful rings spread out from the centre. Rings also
    spread from a pressed pad.
  - **SNAKE (snake-game style)**: a snake with a white head chases blinking food
    and grows in the colour it ate. Food is placed at a pressed pad. When it
    reaches 14 cells or hits a dead end it flashes red and starts over.
  - **INVADE (invader style)**: enemies come down from the top two rows moving
    sideways, and a white cannon on the bottom row fires on its own. The cannon
    moves to a pressed pad's column and fires. Clear them all for the next wave;
    if they land, everything flashes red and it starts over.
  - **AUTO (default)**: switches between the three every 15 s.
  - It runs at about 12 frames a second (80 ms) and sends LEDs only for pads
    that changed. Knob 5's choice is a UI-only setting and is not saved in the
    DSP (reopening the screen returns to AUTO).
- While this screen is open the pads are taken over with `host_pad_block`, so
  **pad presses do not reach Move itself (its own instruments and sequencer)**.
- The screen shows the 8 playing steps (note names, playback position inverted)
  and `NEXT` at the top right when a next phrase is queued.
- **Limitation (how Schwung works)**: taking over the pads works only while a
  Schwung screen is shown. Going back to Move's screen with Back, Menu, a long
  track press and so on makes the shim release it automatically, and the pad
  colours return to Move's (a design that prevents the pads from being dead for
  good; a module cannot change it). While the screen is closed the pads reach
  Move as before, and their notes reach Phrase Gen, which GENERATEs according to
  the Trigger setting.
- **Operating with Shift+pad only was rejected** (prototyped and dropped on
  2026-10-01): Shift is a button Move uses itself, notes reaching a MIDI FX carry
  no Shift information, and it could not work outside this screen (the
  instrument's screen, Move's screen).

Knobs (while the screen is shown):

| Knob | Parameter | What it does |
|---|---|---|
| 1 | Mutation | 0 = the original MIDI's notes as they are / 50 = default / 100 = bold (1 per detent) |
| 2 | Trigger | Behaviour while the screen is closed: `any` / `note` / `learn` (one step per 4 detents) |
| 3 | Trig Note | The GENERATE note while the screen is closed (default 36 = the bottom-left pad of a Drum Rack) |
| 4 | Source | `rnd` or 1–6 (one step per 4 detents) |
| 5 | Animation | `AUTO` / `RIPL` / `SNAK` / `INVD` (one step per 4 detents; the pad animation) |

Touching a knob shows its name and value large. Because this is a dedicated
screen, the "My Presets / Module" pages at the end of the standard knob screen
are not there (the state is saved by the slot's autosave).

## 7. Testing

**On the PC (automatic)**: `scripts/test.sh` — about 1.45 million checks, with
ASan/UBSan.

- Loader: the contents of the six (pitch, rests, velocity, length), running
  status/SysEx/Meta/vel0 Note Off, extracting any stretch, and no crash on
  truncation, bit flips or random bytes (tens of thousands of cases in all).
- Scale: quantising all 128 notes lands inside the scale and on the nearest
  note; degree moves stay inside the scale.
- Generator: 20,000 runs over 5 Mutation levels — every note in C Phrygian and
  inside the sources' range, always 8 notes (no rests), always different from the
  previous phrase/the source, Mutation 0 keeps the original notes, every source
  gets chosen. With `allow_rests=1`, Mutation 0 is an exact match and a 3-note
  source never becomes 6 notes or more.
- Plugin: 500 pad presses, no REST in any generated phrase.
- Sequencer: note-ons within one block of the grid, a phrase queued midway
  switches at the start of the next 8 steps, stop releases everything and it is
  silent while stopped. A random fuzz (tempo changes, stop/restart, rewinds,
  flushes, gate up to 4×, output buffer 1–16) gives zero double Ons, orphan Offs
  or stuck notes.
- Plugin API: 4000 rapid pad presses, `note`/`learn`, Note Off during a mode
  change, CC123, state round-trip, the MIDI clock fallback on a host without
  `get_beat_position`.
- `test_dlopen`: loads the built `dsp.so` the same way the chain does and plays
  4 s (On count = Off count).

**Seeing the generated results (in place of a debug log)**:

```bash
build/native/phrase-gen-cli -n 8 -m 50 -s 1                  # the embedded six
build/native/phrase-gen-cli -n 8 -m 80 path/to/*.mid          # try any MIDI files
build/native/phrase-gen-cli -q -n 500 -m 50                   # one line each + averages
```

Example output:

```
Source: pattern05.mid
Generated:
Step 1: C4 vel=98 len=92%
Step 2: Eb6 vel=100 len=92%
Step 3: C2 vel=100 len=92%
...
```

On the device the current phrase comes back in the same format from the `debug`
key of `get_param` (through the chain the key is `midi_fx1:debug`; on the PC the
CLI shows the same content).

**On a Move**: following step 6 above, check that (a) it sounds right after
Play, (b) each pad press gives a different phrase, (c) switches land at the start
of 8 steps, (d) tempo changes and stop/restart do not break it, (e) rapid pad
presses and stopping leave no hanging notes.

## 8. Replacing the source MIDI files

Put MIDI files in `patterns/`, change the file names in `patterns/patterns.txt`
and rebuild (1–16 files; without `patterns.txt`, every `*.mid` is used in name
order). A start step in the second column lets you use any 8 steps of a long
MIDI file (e.g. `riff.mid 16`). The options of knob 4 (Source) follow the file
count automatically.

## 9. Extension points (not implemented)

| Feature | Where it goes |
|---|---|
| Density | The target note count in `stage_rhythm` (`dmax` / `target`) |
| Mutation Amount | Done (knob 1). Every stage scales with this one value |
| Octave Range | `gen_ctx_t.lo/hi` (now the sources' overall range) |
| Random Seed | `pg_rng_seed()` (generation is deterministic; same as the CLI's `-s`) |
| Pattern Select | Done (knob 4 Source) |
| Changing Scale / Root | Just rebuild `pg_library_init(root, mask)` (`PG_ROOT` / `PG_SCALE_MASK`) |
| 8/16/32 steps | `PG_LENGTH`. Patterns hold up to 32 and every module works from `length` |
| Saving generated patterns | `pg_encode_pattern()` (already saved and restored in state) |
| A different variation per pad | `pg_pad` returns the note number with `GENERATE`, changing `pg_gen_params_t.source` / mutation |
| Euclidean / Ratchet / Probability | Add fields to `pg_step_t` and interpret them in `stage_rhythm` / `fire_step` |
| MIDI CC generation | Add CCs to the output of `pg_seq_tick` (16 output slots per block) |
| Phrase Generator | Chain generated 8-step phrases into a `pg_pattern_t` (up to 32) |

## 10. Known limitations

- Notes sounding **at the moment Phrase Gen is bypassed (Mute + Jog Click) or
  removed** cannot be sent a Note Off by this module, because the host stops
  calling `tick()` (the built-in arp is the same; this is how Schwung itself
  works). Stop Move before removing it, or Schwung's `v2_unload_midi_fx_slot` /
  bypass would need a change to send a synth panic.
- A Move track sounding its built-in sound on the same pad is Move's own
  behaviour (silence it as in step 6-1).
- Note-on timing is accurate to an audio block (≈2.9 ms).

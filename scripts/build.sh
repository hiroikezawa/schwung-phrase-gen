#!/usr/bin/env bash
# Build Phrase Gen.  Run from anywhere.
#   native  host .so + CLI + tests binary deps, in build/native/
#   arm64   Move .so, packaged as dist/phrase-gen-module.tar.gz (default)
set -euo pipefail
MODULE="$(cd "$(dirname "$0")/.." && pwd)"
API="$MODULE/third_party/schwung"  # Schwung API headers (MIT, see its LICENSE)
GEN="$MODULE/build/generated"
case "${1:-arm64}" in
  native) CC="${CC:-gcc}"; OUT="$MODULE/build/native" ;;
  arm64)  CC="${CROSS_PREFIX:-aarch64-linux-gnu-}gcc"; OUT="$MODULE/dist/phrase-gen" ;;
  *) echo "Usage: $0 [native|arm64]" >&2; exit 2 ;;
esac
rm -rf "$OUT"
mkdir -p "$OUT" "$GEN"

# MIDI files -> C header (RT-safe: nothing is read from disk on the device),
# plus a module.json whose Source options match the number of files.
python3 "$MODULE/scripts/embed_patterns.py" "$MODULE/patterns" "$GEN/pg_patterns_data.h" \
  "$MODULE/module.json" "$OUT/module.json"

SRCS=(pg_scale.c pg_midi_loader.c pg_analyzer.c pg_generator.c pg_sequencer.c
      pg_pad.c pg_format.c pg_library.c phrase_gen.c)
# -std=c11 matters: GCC 15 defaults to C23, whose headers bind string
# conversions to GLIBC_2.38 symbols that Move (glibc 2.34) does not have.
"$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -fPIC -shared -Wl,-z,defs \
  -I"$API" -I"$GEN" -I"$MODULE/dsp" \
  "${SRCS[@]/#/$MODULE/dsp/}" -o "$OUT/dsp.so"

if [[ "${1:-arm64}" == arm64 ]]; then
  python3 "$MODULE/scripts/check_abi.py" "$OUT/dsp.so"
  cp "$MODULE/LICENSE" "$MODULE/THIRD_PARTY_LICENSES.md" "$MODULE/help.json" "$MODULE/README.md" "$MODULE/README.ja.md" "$MODULE/ui_chain.js" "$MODULE/ui_core.mjs" "$MODULE/pad_anim.mjs" "$OUT/"
  mkdir -p "$OUT/patterns"
  cp "$MODULE"/patterns/*.mid "$MODULE/patterns/patterns.txt" "$OUT/patterns/" 2>/dev/null || true
  tar -czf "$MODULE/dist/phrase-gen-module.tar.gz" -C "$MODULE/dist" phrase-gen
  echo "Package: $MODULE/dist/phrase-gen-module.tar.gz"
else
  # Host-only tuning tool: prints the analysis and generated phrases.
  "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror \
    -I"$API" -I"$GEN" -I"$MODULE/dsp" \
    "$MODULE/tools/phrase_gen_cli.c" \
    "$MODULE"/dsp/pg_{scale,midi_loader,analyzer,generator,sequencer,pad,format,library}.c \
    -o "$OUT/phrase-gen-cli"
fi
file "$OUT/dsp.so"

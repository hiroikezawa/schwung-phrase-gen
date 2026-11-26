#!/usr/bin/env bash
# Host tests: native build, unit + integration tests under ASan/UBSan, then
# the CLI against the real pattern files.
set -euo pipefail
MODULE="$(cd "$(dirname "$0")/.." && pwd)"
API="$MODULE/third_party/schwung"  # Schwung API headers (MIT, see its LICENSE)
bash "$MODULE/scripts/build.sh" native
OUT="$MODULE/build/native"
"${CC:-gcc}" -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all \
  -I"$API" -I"$MODULE/build/generated" -I"$MODULE/dsp" \
  "$MODULE/tests/test_phrase_gen.c" \
  "$MODULE"/dsp/pg_{scale,midi_loader,analyzer,generator,sequencer,pad,format,library}.c \
  "$MODULE/dsp/phrase_gen.c" \
  -o "$OUT/test-phrase-gen"
"$OUT/test-phrase-gen"
"$OUT/phrase-gen-cli" -n 2 -s 1 "$MODULE"/patterns/*.mid > /dev/null
echo "CLI OK"
"${CC:-gcc}" -std=c11 -O1 -g -Wall -Wextra -Werror -I"$API" \
  "$MODULE/tests/test_dlopen.c" -o "$OUT/test-dlopen" -ldl
"$OUT/test-dlopen" "$OUT/dsp.so"
if command -v node >/dev/null 2>&1; then
  node "$MODULE/tests/test_ui.mjs"
elif command -v node.exe >/dev/null 2>&1 && command -v wslpath >/dev/null 2>&1; then
  node.exe "$(wslpath -w "$MODULE/tests/test_ui.mjs")"
else
  echo "Node.js is required for the UI test" >&2; exit 1
fi

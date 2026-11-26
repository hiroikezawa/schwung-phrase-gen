#!/usr/bin/env python3
"""Move's glibc is 2.34 (scripts/build.sh); reject newer symbol requirements,
and confirm this is an AArch64 MIDI FX exporting move_midi_fx_init."""
import re
import subprocess
import sys
so = sys.argv[1]
output = subprocess.check_output(["readelf", "--version-info", so], text=True)
versions = {tuple(map(int, s.split("."))) for s in re.findall(r"GLIBC_([0-9.]+)", output)}
assert versions and max(versions) <= (2, 34), f"Move-incompatible GLIBC: {sorted(versions)}"
assert "AArch64" in subprocess.check_output(["readelf", "-h", so], text=True), "not AArch64"
symbols = subprocess.check_output(["readelf", "-Ws", "--dyn-syms", so], text=True)
assert re.search(r"\bmove_midi_fx_init\b", symbols), "move_midi_fx_init not exported"
undefined = sorted({l.split()[7].split("@")[0] for l in symbols.splitlines()
                    if " UND " in l and len(l.split()) >= 8})
forbidden = [s for s in undefined if re.fullmatch(r"fopen|open|read|write|fprintf|fputs|puts|printf|malloc|realloc|pthread_create|usleep|sleep|nanosleep", s)]
assert not forbidden, f"realtime-forbidden calls linked in: {forbidden}"
print("PASS: AArch64 MIDI FX, exports move_midi_fx_init, max GLIBC",
      ".".join(map(str, max(versions))), "| imports:", " ".join(s for s in undefined if not s.startswith(("_ITM", "__gmon", "__cxa"))))

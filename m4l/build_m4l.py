#!/usr/bin/env python3
"""Build the Phrase Gen Max for Live MIDI effect.

  1. Builds tools/pg_ref_dump.c against the Move engine and embeds its
     source phrases into phrase_gen_m4l.js (between /*@SOURCES*/ and /*@END*/),
     so the device ships exactly the patterns the Move plugin builds.
  2. Writes dist/Phrase Gen/Phrase Gen.amxd (+ phrase_gen_m4l.js beside it).

The .amxd container is: b"ampf" u32(4) b"mmmm" (MIDI effect),
b"meta" u32(4) u32(0), b"ptch" u32(len) <patcher JSON, NUL-terminated>.

usage: python3 build_m4l.py
"""
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MODULE = os.path.dirname(HERE)
API = os.path.join(MODULE, "third_party", "schwung")
JS = os.path.join(HERE, "phrase_gen_m4l.js")
OUT_DIR = os.path.join(HERE, "dist", "Phrase Gen")
AMXD = os.path.join(OUT_DIR, "Phrase Gen.amxd")


def embed_sources():
    gen = os.path.join(MODULE, "build", "generated")
    os.makedirs(gen, exist_ok=True)
    subprocess.check_call([sys.executable, os.path.join(MODULE, "scripts", "embed_patterns.py"),
                           os.path.join(MODULE, "patterns"), os.path.join(gen, "pg_patterns_data.h"),
                           os.path.join(MODULE, "module.json"),
                           os.path.join(tempfile.gettempdir(), "pg_m4l_module.json")])
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "pg_ref_dump")
        dsp = os.path.join(MODULE, "dsp")
        subprocess.check_call(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                               "-I" + API, "-I" + gen, "-I" + dsp,
                               os.path.join(HERE, "tools", "pg_ref_dump.c")]
                              + [os.path.join(dsp, f"pg_{n}.c") for n in
                                 ("scale", "midi_loader", "analyzer", "generator", "library")]
                              + ["-o", exe])
        ref = json.loads(subprocess.check_output([exe, "1", "50", "0", "-1"]))
    src = open(JS, encoding="utf-8").read()
    new = re.sub(r"/\*@SOURCES\*/.*?/\*@END\*/",
                 "/*@SOURCES*/" + json.dumps(ref["sources"], separators=(",", ":")) + "/*@END*/",
                 src, count=1, flags=re.S)
    if new != src:
        open(JS, "w", encoding="utf-8", newline="\n").write(new)
    return len(ref["sources"])


# ---- patcher ---------------------------------------------------------------

_boxes = []
_lines = []


def box(id_, maxclass, rect, pres=None, **kw):
    b = {"id": id_, "maxclass": maxclass, "patching_rect": rect,
         "numinlets": kw.pop("numinlets", 1), "numoutlets": kw.pop("numoutlets", 0)}
    if pres is not None:
        b["presentation"] = 1
        b["presentation_rect"] = pres
    b.update(kw)
    _boxes.append({"box": b})


def obj(id_, text, rect, inlets, outlets, outlettype=None):
    box(id_, "newobj", rect, text=text, numinlets=inlets, numoutlets=outlets,
        outlettype=outlettype or [""] * outlets)


def line(src, so, dst, di):
    _lines.append({"patchline": {"source": [src, so], "destination": [dst, di]}})


def param(longname, shortname, ptype, mmax, initial, enum=None, unitstyle=None):
    v = {"parameter_longname": longname, "parameter_shortname": shortname,
         "parameter_type": ptype, "parameter_mmax": mmax,
         "parameter_initial_enable": 1, "parameter_initial": [initial]}
    if enum is not None:
        v["parameter_enum"] = enum
    if unitstyle is not None:
        v["parameter_unitstyle"] = unitstyle
    return {"valueof": v}


def build_patcher(n_sources):
    # MIDI passes straight through: this device only edits clips.
    obj("midiin", "midiin", [20, 20, 45, 22], 1, 1, ["int"])
    obj("midiout", "midiout", [20, 60, 55, 22], 1, 0)
    line("midiin", 0, "midiout", 0)

    obj("thisdev", "live.thisdevice", [200, 20, 100, 22], 1, 3, ["bang", "int", "int"])
    obj("trig", "t b b b", [200, 50, 60, 22], 1, 3, ["bang", "bang", "bang"])
    line("thisdev", 0, "trig", 0)

    box("mut", "live.dial", [200, 100, 44, 48], [6, 4, 44, 48],
        numinlets=1, numoutlets=2, outlettype=["", "float"],
        parameter_enable=1, varname="Mutation",
        saved_attribute_attributes=param("Mutation", "Mutation", 1, 100.0, 50, unitstyle=5))
    box("src", "live.menu", [280, 100, 60, 15], [58, 8, 64, 15],
        numinlets=1, numoutlets=3, outlettype=["", "", "float"],
        parameter_enable=1, varname="Source",
        saved_attribute_attributes=param("Source", "Source", 2, float(n_sources), 0,
                                         enum=["random"] + [str(i + 1) for i in range(n_sources)]))
    box("len", "live.menu", [360, 100, 60, 15], [58, 34, 64, 15],
        numinlets=1, numoutlets=3, outlettype=["", "", "float"],
        parameter_enable=1, varname="Length",
        saved_attribute_attributes=param("Length", "Length", 2, 3.0, 0,
                                         enum=["8 steps", "1 bar", "2 bars", "4 bars"]))
    box("lbl_src", "live.comment", [280, 80, 50, 18], [128, 6, 50, 18],
        numinlets=1, numoutlets=0, text="Source")
    box("lbl_len", "live.comment", [360, 80, 50, 18], [128, 32, 50, 18],
        numinlets=1, numoutlets=0, text="Length")
    box("gen", "live.text", [450, 100, 80, 20], [6, 58, 172, 24],
        numinlets=1, numoutlets=2, outlettype=["", ""],
        parameter_enable=0, mode=0, text="GENERATE", texton="GENERATE",
        varname="Generate")

    line("trig", 2, "mut", 0)
    line("trig", 1, "src", 0)
    line("trig", 0, "len", 0)

    obj("p_mut", "prepend setmutation", [200, 160, 120, 22], 1, 1)
    obj("p_src", "prepend setsource", [280, 190, 110, 22], 1, 1)
    obj("p_len", "prepend setlength", [360, 220, 110, 22], 1, 1)
    line("mut", 0, "p_mut", 0)
    line("src", 0, "p_src", 0)
    line("len", 0, "p_len", 0)

    obj("js", "js phrase_gen_m4l.js", [200, 270, 160, 22], 1, 1)
    line("p_mut", 0, "js", 0)
    line("p_src", 0, "js", 0)
    line("p_len", 0, "js", 0)
    line("gen", 0, "js", 0)

    obj("p_set", "prepend set", [200, 310, 75, 22], 1, 1)
    line("js", 0, "p_set", 0)
    box("status", "comment", [200, 340, 300, 20], [6, 88, 172, 32],
        numinlets=1, numoutlets=0, text="Select a clip slot, then GENERATE",
        fontsize=9.0, linecount=2, textcolor=[0.85, 0.85, 0.85, 1.0])

    return {
        "patcher": {
            "fileversion": 1,
            "appversion": {"major": 8, "minor": 6, "revision": 0,
                           "architecture": "x64", "modernui": 1},
            "classnamespace": "box",
            "rect": [100.0, 100.0, 700.0, 450.0],
            "openrect": [0.0, 0.0, 184.0, 169.0],
            "openinpresentation": 1,
            "default_fontsize": 10.0,
            "default_fontname": "Arial Bold",
            "gridsize": [8.0, 8.0],
            "boxes": _boxes,
            "lines": _lines,
            "dependency_cache": [
                {"name": "phrase_gen_m4l.js", "bootpath": ".", "type": "TEXT", "implicit": 1}
            ],
            "latency": 0,
            "is_mpe": 0,
            "project": {"version": 1, "creationdate": 3590052493, "modificationdate": 3590052493,
                        "viewrect": [0, 0, 300, 500], "autoorganize": 1, "hideprojectwindow": 1,
                        "showdependencies": 1, "autolocalize": 0,
                        "contents": {"patchers": {}, "code": {}}, "layout": {},
                        "searchpath": {}, "detailsvisible": 0, "amxdtype": 1835887981,
                        "readonly": 0, "devpathtype": 0, "devpath": ".", "sortmode": 0,
                        "viewmode": 0, "includepackages": 0},
            "autosave": 0,
            "description": "Phrase Gen: writes a C Phrygian phrase into the selected clip",
            "digest": "", "tags": "",
        }
    }


def write_amxd(patcher, path):
    body = json.dumps(patcher, indent=1).encode("utf-8") + b"\n\x00"
    data = (b"ampf" + struct.pack("<I", 4) + b"mmmm"
            + b"meta" + struct.pack("<I", 4) + struct.pack("<I", 0)
            + b"ptch" + struct.pack("<I", len(body)) + body)
    with open(path, "wb") as f:
        f.write(data)


def main():
    n = embed_sources()
    os.makedirs(OUT_DIR, exist_ok=True)
    write_amxd(build_patcher(n), AMXD)
    shutil.copy2(JS, os.path.join(OUT_DIR, "phrase_gen_m4l.js"))
    for doc in ("README.md", "README.ja.md"):
        shutil.copy2(os.path.join(HERE, doc), os.path.join(OUT_DIR, doc))
    print("Built:", AMXD)


if __name__ == "__main__":
    main()

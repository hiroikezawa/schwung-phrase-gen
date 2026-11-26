/*
 * The JavaScript port must produce the SAME phrases as the C engine on the
 * Move.  Reads reference runs (tools/pg_ref_dump.c output, one JSON per line
 * as "seed mutation source <json>") from the file given as argv[2].
 */
"use strict";
var fs = require("fs");
var pg = require("../phrase_gen_m4l.js");

var lines = fs.readFileSync(process.argv[2], "utf8").split(/\r?\n/).filter(Boolean);
var model = pg.buildModel(pg.PG_SOURCES);
var fails = 0, runs = 0;

function arr(p) { return p.steps.slice(0, p.length).map(function (s) { return [s.note, s.vel, s.gate]; }); }

lines.forEach(function (line) {
    var parts = line.split(" ");
    var seed = +parts[0], mut = +parts[1], source = +parts[2];
    var ref = JSON.parse(parts.slice(3).join(" "));
    if (JSON.stringify(ref.sources) !== JSON.stringify(pg.PG_SOURCES)) {
        console.error("embedded sources differ from the C library"); process.exit(1);
    }
    var rng = new pg.Rng(seed), prev = null, cur = -1;
    ref.runs.forEach(function (r, k) {
        var res = pg.generate(model, { mutation: mut / 100, source: source, avoidSource: cur, allowRests: false }, rng, prev);
        runs++;
        var got = JSON.stringify([res.source, arr(res.pattern)]), want = JSON.stringify(r);
        if (got !== want) {
            if (fails < 5) console.error("MISMATCH seed " + seed + " mut " + mut + " src " + source + " gen " + k + "\n  C:  " + want + "\n  JS: " + got);
            fails++;
        }
        prev = res.pattern; cur = res.source;
    });
});

/* phraseNotes: 8 steps repeated, no rests, durations from gate. */
var p = pg.generate(model, { mutation: 0.5, source: -1, avoidSource: -1, allowRests: false }, new pg.Rng(7), null).pattern;
var n2 = pg.phraseNotes(p, 2), n16 = pg.phraseNotes(p, 16);
if (n2.length !== 8 || n16.length !== 64 || n16[8].start_time !== 2 || n16[63].start_time !== 15.75) {
    console.error("phraseNotes layout wrong", n2.length, n16.length); fails++;
}

console.log(runs + " generations compared, " + fails + " mismatches");
process.exit(fails ? 1 : 0);

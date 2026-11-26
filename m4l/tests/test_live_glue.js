/*
 * Drives generateclip() against a mock LiveAPI: session empty slot (creates a
 * clip), session existing clip (replaces notes, sets loop), arrangement clip
 * (keeps length), audio track (refuses).
 */
"use strict";
var vm = require("vm"), fs = require("fs"), path = require("path");
var code = fs.readFileSync(path.join(__dirname, "..", "phrase_gen_m4l.js"), "utf8");

function makeWorld(o) {
    var log = [], status = [];
    var clip = o.clip ? Object.assign({ is_midi_clip: 1, loop_start: 0, loop_end: 4, end_marker: 4, notes: [] }, o.clip) : null;
    function LiveAPI(p) {
        this.unquotedpath = p;
        var self = this;
        var obj = null;
        if (p === "live_app view") obj = { _call: { is_view_visible: function () { return [o.arranger ? 1 : 0]; } } };
        else if (p === "live_set view highlighted_clip_slot") { this.unquotedpath = "live_set tracks 1 clip_slots 3"; obj = { has_clip: clip ? 1 : 0, _call: { create_clip: function (len) { clip = { is_midi_clip: 1, loop_start: 0, loop_end: len, end_marker: len, notes: [] }; log.push("create_clip " + len); } } }; }
        else if (p === "live_set tracks 1") obj = { has_midi_input: o.midiTrack ? 1 : 0 };
        else if (p === "live_set tracks 1 clip_slots 3 clip" || p === "live_set view detail_clip") obj = clip ? clip : null;
        this.id = obj ? "5" : "0";
        this.get = function (k) { return [obj[k]]; };
        this.set = function (k, v) { obj[k] = v; log.push("set " + k + " " + v); };
        this.call = function (name) {
            var a = Array.prototype.slice.call(arguments, 1);
            if (obj === clip && name === "remove_notes_extended") { clip.notes = []; return; }
            if (obj === clip && name === "add_new_notes") { clip.notes = a[0].notes; return; }
            return obj._call[name].apply(null, a);
        };
        void self;
    }
    var ctx = { LiveAPI: LiveAPI, Float32Array: Float32Array, Math: Math, Date: Date, outlet: function (i, t) { status.push(t); } };
    vm.createContext(ctx);
    vm.runInContext(code, ctx);
    return { ctx: ctx, log: log, status: status, clip: function () { return clip; } };
}
var fails = 0;
function check(c, m) { if (!c) { console.error("FAIL: " + m); fails++; } }

var w = makeWorld({ midiTrack: true });
w.ctx.setlength(1); w.ctx.bang();
check(w.log[0] === "create_clip 4", "empty slot creates a 1-bar clip: " + w.log);
check(w.clip().notes.length === 16, "1 bar = 16 notes, got " + w.clip().notes.length);
check(/^New clip/.test(w.status[0]), w.status[0]);

w = makeWorld({ midiTrack: true, clip: { loop_start: 4, loop_end: 12, end_marker: 12, notes: [{ pitch: 1 }] } });
w.ctx.setlength(0); w.ctx.setmutation(100); w.ctx.setsource(3); w.ctx.msg_int(1); w.ctx.msg_int(0);
check(w.clip().notes.length === 8 && w.clip().notes[0].start_time === 4, "existing clip replaced from loop_start");
check(w.clip().loop_end === 6 && w.clip().name === "PG 3", "loop set to 8 steps, named by source");

w = makeWorld({ arranger: true, clip: { loop_start: 0, loop_end: 7 } });
w.ctx.bang();
check(w.clip().notes.length === 28 && w.clip().loop_end === 7 && !w.clip().name, "arrangement keeps length: " + w.clip().notes.length);

w = makeWorld({ midiTrack: false });
w.ctx.bang();
check(/not on a MIDI track/.test(w.status[0]), "audio track refused: " + w.status[0]);

w = makeWorld({ midiTrack: true, clip: { is_midi_clip: 0 } });
w.ctx.bang();
check(/audio clip/.test(w.status[0]) && w.clip().notes.length === 0, "audio clip refused");

console.log(fails ? fails + " failures" : "Live glue OK");
process.exit(fails ? 1 : 0);

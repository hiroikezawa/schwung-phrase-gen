/*
 * Phrase Gen - its own screen, and the pads as GENERATE buttons.
 *
 * Pure logic: every host service (param channel, LEDs, pad block, display)
 * is injected, so tests/test_ui.mjs drives exactly this file under node.
 * The wiring to Schwung's real globals is ui_chain.js.
 *
 * While this screen is up it holds host_pad_block, so a pad press never
 * reaches Move's own instrument: it arrives here instead, and becomes a
 * `generate` write. The 32 pads play an animation (pad_anim.mjs: ripple,
 * snake, invaders, or all three in turn), and a press pokes it. The shim only honours pad_block while the Schwung screen
 * is shown and drops it on its own when the screen goes away, so on a Move
 * track the pads are Move's again (and the DSP's Trigger setting applies).
 */

import { ANIMS, ANIM_LABELS, SCENES, makeScene, W } from './pad_anim.mjs';

/* Move's pads are notes 68-99, rows ascending from the BOTTOM. */
export const PAD_FIRST = 68;
export const PAD_LAST = 99;
const PAD_COUNT = PAD_LAST - PAD_FIRST + 1;

/* Bright, saturated entries of Move's palette (src/shared/constants.mjs). */
export const COLORFUL = [
    1,   /* BrightRed */     3,  /* BrightOrange */  7,  /* VividYellow */
    9,   /* BrightLime */    11, /* NeonGreen */     15, /* TealCyan */
    16,  /* AzureBlue */     17, /* RoyalBlue */     21, /* HotMagenta */
    22,  /* Purple */        23, /* NeonPink */      25, /* BrightPink */
    26,  /* LightMagenta */  8,  /* BrightYellow */  126 /* Green */,
];
export const WHITE = 120;

const KNOB_CC_FIRST = 71;
export const TRIGGERS = ["any", "note", "learn"];

/* Knobs 1-4. `detents` = encoder detents per value step: a flick of a Move
 * encoder is a dozen detents, so a list steps slower than a number. */
export const KNOBS = [
    { key: "mutation",  label: "MUT",  min: 0, max: 100, detents: 1 },
    { key: "trigger",   label: "TRIG", min: 0, max: 2,   detents: 4 },
    { key: "trig_note", label: "NOTE", min: 0, max: 127, detents: 1 },
    { key: "source",    label: "SRC",  min: 0, max: 6,   detents: 4 },  /* 0 = random */
    { key: "anim",      label: "ANIM", min: 0, max: 3,   detents: 4, local: true },
];

const NAMES = ["C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"];
/* Ableton naming: 60 = C3. */
export function noteName(n) {
    if (!Number.isInteger(n) || n < 0 || n > 127) return "-";
    return NAMES[n % 12] + (Math.floor(n / 12) - 2);
}

export function decodeDelta(v) {
    if (v >= 1 && v <= 63) return v;
    if (v >= 65 && v <= 127) return v - 128;
    return 0;
}

/* The DSP's one-line state; see "ui_state" in dsp/phrase_gen.c. */
export function parseUiState(text) {
    if (typeof text !== "string" || text.length === 0) return null;
    const f = text.split(",").map(Number);
    if (f.length !== 11 + 16 || f[0] !== 1 || f.some((x) => !Number.isInteger(x))) return null;
    return {
        running: f[1] === 1, step: f[2], mutation: f[3], trigger: f[4],
        trigNote: f[5], sourceSel: f[6], curSource: f[7], genCount: f[8],
        pending: f[9] === 1, nSources: f[10],
        current: f.slice(11, 19), next: f.slice(19, 27),
    };
}

export function defaultState() {
    return parseUiState("1,0,-1,50,0,36,-1,-1,0,0,6," + new Array(16).fill(-1).join(","));
}

const ANIM_FRAME_MS = 80;          /* ~12 frames a second */
const AUTO_SCENE_MS = 15000;       /* auto: next animation every 15 s */

const READ_EVERY_TICKS = 3;        /* ~20 Hz: the playhead */
const REASSERT_EVERY_TICKS = 6;    /* one row of our LEDs re-sent round-robin */
const TOUCH_SHOW_MS = 900;

/**
 * host = {
 *   setParam(key, value) -> bool, getParam(key) -> string|null,
 *   padBlock(on), sendLed(note, color) -> bool, padSnapshot() -> {note: color}|null,
 *   displayOn() -> bool, now() -> ms, random() -> [0,1),
 *   gfx: { clear(), print(x, y, text, color), fillRect(x, y, w, h, c),
 *          drawRect(x, y, w, h, c), textWidth(text) }
 * }
 */
export function createUi(host) {
    const rand = host.random || Math.random;
    let s = defaultState();
    let anim = 0;                        /* ANIMS index; UI-only, not saved */
    let sceneIdx = 0;
    let scene = makeScene(SCENES[0], rand, COLORFUL);
    let colors = scene.colors();
    let nextFrame = 0;
    let sceneSince = 0;
    const held = new Set();
    const acc = [0, 0, 0, 0];            /* detents toward the next list step */
    const pending = new Map();           /* knob writes, flushed once per tick */
    let ledCache = {};
    let moveSnap = null;
    let tickCount = 0;
    let readDue = true;
    let wasOn = false;
    let reassertRow = 0;
    let dirty = true;
    let touched = -1;
    let lastKnob = -1;
    let showUntil = 0;
    let flash = 0;                        /* frames of "NEW" after a press */

    function refresh() {
        const next = parseUiState(host.getParam("ui_state"));
        if (next) {
            /* A knob write still in flight wins over the read it raced. */
            for (const k of pending.keys()) next[stateField(k)] = s[stateField(k)];
            s = next;
            dirty = true;
        }
        readDue = false;
    }

    function flushPending() {
        for (const [key, value] of pending) host.setParam(key, String(value));
        pending.clear();
    }

    /* Hand the pads back to Move with Move's own colours. */
    function restoreLeds() {
        const snap = host.padSnapshot ? host.padSnapshot() : null;
        for (let note = PAD_FIRST; note <= PAD_LAST; note++) {
            const c = snap && snap[String(note)] !== undefined ? snap[String(note)] : 0;
            host.sendLed(note, c);
        }
        ledCache = {};
        moveSnap = null;
    }

    /* ---- pads ---- */
    function onPad(note, down) {
        if (note < PAD_FIRST || note > PAD_LAST) return;
        if (!down) { held.delete(note); return; }
        held.add(note);
        host.setParam("generate", "1");
        const i = note - PAD_FIRST;
        scene.press(i % W, Math.floor(i / W));
        colors = scene.colors();
        flash = 30;
        readDue = true;
        dirty = true;
    }

    /* ---- knobs ---- */
    function knobValue(i) {
        switch (i) {
        case 0: return s.mutation;
        case 1: return s.trigger;
        case 2: return s.trigNote;
        case 4: return anim;
        default: return s.sourceSel + 1;           /* -1 random -> 0 */
        }
    }

    function onKnobTurn(i, raw) {
        const k = KNOBS[i];
        const d = decodeDelta(raw);
        if (!k || d === 0) return;
        let step = 0;
        if (k.detents <= 1) {
            step = d;
        } else {
            if (Math.sign(acc[i]) !== Math.sign(d)) acc[i] = 0;   /* reversal */
            acc[i] += d;
            step = Math.trunc(acc[i] / k.detents);
            acc[i] -= step * k.detents;
        }
        lastKnob = i;
        showUntil = host.now() + TOUCH_SHOW_MS;
        dirty = true;
        if (step === 0) return;
        const max = i === 3 ? Math.max(0, s.nSources) : k.max;
        const v = Math.min(max, Math.max(k.min, knobValue(i) + step));
        switch (i) {
        case 0: s.mutation = v; pending.set("mutation", v); break;
        case 1: s.trigger = v; pending.set("trigger", TRIGGERS[v]); break;
        case 2: s.trigNote = v; pending.set("trig_note", v); break;
        case 4: setAnim(v); break;
        default: s.sourceSel = v - 1; pending.set("source", v === 0 ? "random" : String(v)); break;
        }
    }

    /* ---- animation ---- */
    function startScene(i) {
        sceneIdx = i;
        scene = makeScene(SCENES[i], rand, COLORFUL);
        colors = scene.colors();
        sceneSince = host.now();
    }

    function setAnim(v) {
        anim = v;
        if (v > 0) startScene(v - 1);
        else sceneSince = host.now();
    }

    function stepAnim() {
        const now = host.now();
        if (anim === 0 && now - sceneSince >= AUTO_SCENE_MS)
            startScene((sceneIdx + 1) % SCENES.length);
        if (now < nextFrame) return;
        /* never more than one frame per tick, never a burst after a stall */
        nextFrame = Math.max(nextFrame + ANIM_FRAME_MS, now);
        scene.step();
        colors = scene.colors();
    }

    function onKnobTouch(i, down) {
        if (down) { touched = i; lastKnob = i; }
        else if (touched === i) touched = -1;
        showUntil = host.now() + TOUCH_SHOW_MS;
        dirty = true;
    }

    function onMidi(data) {
        if (!data || data.length < 3) return;
        const status = data[0] & 0xF0, d1 = data[1], d2 = data[2];
        if (status === 0x90 || status === 0x80) {
            const down = status === 0x90 && d2 > 0;
            if (d1 >= PAD_FIRST && d1 <= PAD_LAST) { onPad(d1, down); return; }
            if (d1 <= 7) { if (d1 < KNOBS.length) onKnobTouch(d1, down); return; }
            return;
        }
        if (status === 0xB0 && d1 >= KNOB_CC_FIRST && d1 < KNOB_CC_FIRST + KNOBS.length) {
            onKnobTurn(d1 - KNOB_CC_FIRST, d2);
        }
    }

    /* ---- LEDs ---- */
    function paintLeds() {
        /* Move repaints its own pads (a playing track flashes them); whatever
         * Move changed since last frame is ours to put back now. */
        const snap = host.padSnapshot ? host.padSnapshot() : null;
        if (snap) {
            if (moveSnap) {
                for (let note = PAD_FIRST; note <= PAD_LAST; note++)
                    if (snap[String(note)] !== moveSnap[String(note)]) delete ledCache[note];
            }
            moveSnap = snap;
        }
        if (tickCount % REASSERT_EVERY_TICKS === 0) {
            for (let col = 0; col < 8; col++) delete ledCache[PAD_FIRST + reassertRow * 8 + col];
            reassertRow = (reassertRow + 1) % 4;
        }
        for (let note = PAD_FIRST; note <= PAD_LAST; note++) {
            const color = held.has(note) ? WHITE : colors[note - PAD_FIRST];
            if (ledCache[note] === color) continue;
            /* Cache only what was queued, so a dropped packet is retried. */
            if (host.sendLed(note, color)) ledCache[note] = color;
        }
    }

    /* ---- screen ---- */
    function valueText(i) {
        switch (i) {
        case 0: return `${s.mutation}%`;
        case 1: return TRIGGERS[s.trigger] || "?";
        case 2: return noteName(s.trigNote);
        case 4: return ANIM_LABELS[anim];
        default: return s.sourceSel < 0 ? "rnd" : String(s.sourceSel + 1);
        }
    }

    function draw() {
        const g = host.gfx;
        const tw = (t) => (g.textWidth ? g.textWidth(t) : t.length * 6);
        g.clear();

        g.fillRect(0, 0, 128, 11, 1);
        g.print(2, 2, "PHRASE GEN", 0);
        let info = s.curSource >= 0 ? `SRC ${s.curSource + 1}` : "";
        if (s.pending) info = (tickCount >> 3) % 2 ? "NEXT" : "";
        else if (flash > 0) info = "NEW";
        if (info) g.print(126 - tw(info), 2, info, 0);

        /* The playing phrase: 2 rows x 4 cells; the playhead cell inverted. */
        for (let i = 0; i < 8; i++) {
            const x = (i % 4) * 32, y = i < 4 ? 13 : 25;
            const name = noteName(s.current[i]);
            const on = s.running && s.step === i;
            if (on) g.fillRect(x, y, 31, 11, 1);
            else g.drawRect(x, y, 31, 11, 1);
            g.print(x + Math.max(1, Math.floor((31 - tw(name)) / 2)), y + 2, name, on ? 0 : 1);
        }

        const now = host.now();
        const big = touched >= 0 ? touched : (now < showUntil ? lastKnob : -1);
        if (big >= 0) {
            const names = ["MUTATION", "TRIGGER", "TRIG NOTE", "SOURCE", "ANIMATION"];
            g.print(0, 40, `${big + 1} ${names[big]}`, 1);
            const v = valueText(big);
            g.print(126 - tw(v), 40, v, 1);
            if (big === 0) {
                g.drawRect(0, 52, 128, 10, 1);
                g.fillRect(2, 54, Math.round(124 * s.mutation / 100), 6, 1);
            } else if (big === 4) {
                g.print(0, 53, anim === 0 ? `auto: ${SCENES[sceneIdx]}` : SCENES[anim - 1], 1);
            } else if (big === 1) {
                g.print(0, 53, s.trigger === 0 ? "screen off: any pad" : "screen off: that pad", 1);
            } else {
                g.print(0, 53, "on screen: all pads", 1);
            }
        } else {
            for (let i = 0; i < KNOBS.length; i++) {
                const x = i * 26;
                g.print(x + 1, 40, KNOBS[i].label, 1);
                /* 26 px columns: "learn" would touch its neighbour */
                g.print(x + 1, 52, i === 1 && s.trigger === 2 ? "lrn" : valueText(i), 1);
            }
        }
        dirty = false;
    }

    /* ---- lifecycle ---- */
    function init() {
        refresh();
        ledCache = {};
        wasOn = false;
        dirty = true;
    }

    function tick() {
        tickCount++;
        const on = host.displayOn();
        if (!on) {
            if (wasOn) {
                /* Screen dismissed with this view still loaded: give the
                 * pads back to Move now. */
                flushPending();
                held.clear();
                restoreLeds();
                host.padBlock(0);
            }
            wasOn = false;
            return;
        }
        /* Restated EVERY frame: the shim drops the flag by itself on the
         * display edge, so remembering it would latch dead pads. */
        host.padBlock(1);
        if (!wasOn) { ledCache = {}; moveSnap = null; readDue = true; }
        wasOn = true;

        flushPending();
        if (readDue || tickCount % READ_EVERY_TICKS === 0) refresh();
        stepAnim();
        paintLeds();
        if (flash > 0) { flash--; dirty = true; }
        const now = host.now();
        if (dirty || s.running || s.pending || now < showUntil + 50) draw();
    }

    /* Back leaves the module: tidy up, then let the host navigate. */
    function handleBack() {
        flushPending();
        held.clear();
        restoreLeds();
        host.padBlock(0);
        return false;
    }

    return {
        init, tick, onMidi, handleBack,
        /* for tests */
        get state() { return s; },
        get colors() { return colors.slice(); },
        get scene() { return scene.name; },
        get anim() { return ANIMS[anim]; },
    };
}

function stateField(key) {
    return { mutation: "mutation", trigger: "trigger", trig_note: "trigNote", source: "sourceSel" }[key];
}

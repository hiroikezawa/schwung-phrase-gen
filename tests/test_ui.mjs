/* Phrase Gen UI (ui_core.mjs) under node with a fake host. */
import { createUi, parseUiState, COLORFUL, WHITE, noteName, decodeDelta } from "../ui_core.mjs";
import { makeScene, SCENES, W, H, OFF } from "../pad_anim.mjs";

let fails = 0, checks = 0;
const check = (cond, msg) => { checks++; if (!cond) { fails++; console.error("FAIL: " + msg); } };

function makeHost() {
    let seed = 7;
    const h = {
        params: { ui_state: "1,1,2,50,0,36,-1,2,1,0,6,53,65,53,51,94,106,44,51,53,65,53,51,94,106,44,51" },
        writes: [], leds: {}, ledSends: 0, block: [], displayOn: true, t: 0,
        snapshot: {},
    };
    for (let n = 68; n <= 99; n++) h.snapshot[String(n)] = 5;   /* Move's own colour */
    h.api = {
        setParam: (k, v) => {
            h.writes.push([k, v]);
            /* like the DSP: a write is visible to the next ui_state read */
            const idx = { mutation: 3, trig_note: 5 }[k];
            if (idx !== undefined && h.params.ui_state) {
                const f = h.params.ui_state.split(",");
                f[idx] = v;
                h.params.ui_state = f.join(",");
            }
            return true;
        },
        getParam: (k) => h.params[k] ?? null,
        padBlock: (on) => h.block.push(on),
        sendLed: (note, c) => { h.leds[note] = c; h.ledSends++; return true; },
        padSnapshot: () => ({ ...h.snapshot }),
        displayOn: () => h.displayOn,
        now: () => h.t,
        random: () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed / 2147483648; },
        gfx: { clear() {}, print() {}, fillRect() {}, drawRect() {}, textWidth: (t) => t.length * 6 },
    };
    return h;
}

/* pure helpers */
check(noteName(60) === "C3" && noteName(36) === "C1" && noteName(106) === "Bb6" && noteName(-1) === "-", "note names");
check(decodeDelta(1) === 1 && decodeDelta(127) === -1 && decodeDelta(64) === 0, "encoder deltas");
check(parseUiState("garbage") === null && parseUiState("") === null && parseUiState(null) === null, "bad ui_state rejected");
const st = parseUiState("1,1,2,50,0,36,-1,2,1,0,6,53,65,53,51,94,106,44,51,53,65,53,51,94,106,44,51");
check(st && st.step === 2 && st.current[4] === 94 && st.nSources === 6 && st.sourceSel === -1, "ui_state parsed");

/* every scene: 32 valid colours, survives a long run and pad spam */
const LEGAL = new Set([...COLORFUL, WHITE, OFF, 1]);
for (const name of SCENES) {
    for (let seed = 1; seed <= 20; seed++) {
        let r = seed;
        const rand = () => { r = (r * 16807) % 2147483647; return r / 2147483647; };
        const sc = makeScene(name, rand, COLORFUL);
        let ok = true, litFrames = 0, changes = 0, prev = sc.colors().join();
        for (let f = 0; f < 3000; f++) {
            if (f % 37 === 0) sc.press(Math.floor(rand() * W), Math.floor(rand() * H));
            sc.step();
            const c = sc.colors();
            if (c.length !== 32 || !c.every((v) => LEGAL.has(v))) ok = false;
            if (c.some((v) => v !== OFF)) litFrames++;
            const j = c.join();
            if (j !== prev) changes++;
            prev = j;
        }
        check(ok, `${name}: 32 legal colours every frame`);
        check(litFrames > 2700, `${name}: something is lit (${litFrames}/3000)`);
        check(changes > 600, `${name}: it moves (${changes} changes)`);
    }
}

/* screen up: pads blocked, every pad lit colourful */
{
    const h = makeHost();
    const ui = createUi(h.api);
    ui.init();
    ui.tick();
    check(h.block[h.block.length - 1] === 1, "pads blocked while the screen is up");
    let sent = 0;
    for (let n = 68; n <= 99; n++) if (h.leds[n] !== undefined) sent++;
    check(sent === 32, `all 32 pads painted (${sent})`);
    check(ui.anim === "auto" && ui.scene === "ripple", "auto starts on ripple");

    /* the animation runs on the clock, not the tick count */
    const f0 = ui.colors.join();
    let moved = false;
    for (let i = 0; i < 40; i++) { h.t += 20; ui.tick(); if (ui.colors.join() !== f0) moved = true; }
    check(moved, "pads animate");
    const frozen = ui.colors.join();
    ui.tick(); ui.tick();
    check(ui.colors.join() === frozen, "no frame without time passing");

    /* a pad press generates, lights white while held, throws a ripple */
    const before = ui.colors;
    ui.onMidi([0x90, 80, 100]);
    check(h.writes.some(([k]) => k === "generate"), "pad press writes generate");
    ui.tick();
    check(h.leds[80] === WHITE, "held pad is white");
    const after = ui.colors;
    check(before.some((c, i) => c !== after[i]), "a press shows at once");
    ui.onMidi([0x80, 80, 0]);
    ui.tick();
    check(h.leds[80] === ui.colors[80 - 68], "released pad back to the animation");
    const gens = h.writes.filter(([k]) => k === "generate").length;
    ui.onMidi([0x80, 81, 0]);
    check(h.writes.filter(([k]) => k === "generate").length === gens, "note-off never generates");

    /* pad spam */
    for (let i = 0; i < 500; i++) { ui.onMidi([0x90, 68 + (i % 32), 90]); ui.onMidi([0x90, 68 + (i % 32), 0]); ui.tick(); }
    check(h.writes.filter(([k]) => k === "generate").length === gens + 500, "every press generates");

    /* Move repaints a pad: it is taken back next frame */
    ui.tick();
    h.snapshot["70"] = 99;
    h.leds[70] = 99;
    ui.tick();
    check(h.leds[70] === ui.colors[2], "Move's repaint is overwritten");

    /* knobs: mutation 1/detent, lists slower, clamped */
    h.writes.length = 0;
    ui.onMidi([0xB0, 71, 3]); ui.tick();
    check(h.writes.some(([k, v]) => k === "mutation" && v === "53"), "knob 1: mutation +3");
    ui.onMidi([0xB0, 71, 65]); ui.onMidi([0xB0, 71, 65]); ui.tick();   /* -63 twice */
    check(ui.state.mutation === 0 && h.writes.some(([k, v]) => k === "mutation" && v === "0"), "mutation clamps at 0");
    h.writes.length = 0;
    ui.onMidi([0xB0, 72, 1]); ui.onMidi([0xB0, 72, 1]); ui.onMidi([0xB0, 72, 1]); ui.tick();
    check(!h.writes.some(([k]) => k === "trigger"), "3 detents do not step a list");
    ui.onMidi([0xB0, 72, 1]); ui.tick();
    check(h.writes.some(([k, v]) => k === "trigger" && v === "note"), "4 detents: trigger -> note");
    h.writes.length = 0;
    ui.onMidi([0xB0, 74, 60]); ui.tick();
    check(h.writes.some(([k, v]) => k === "source" && v === "6"), "source clamps at the last pattern");
    ui.onMidi([0xB0, 74, 128 - 60]); ui.tick();
    check(h.writes.some(([k, v]) => k === "source" && v === "random"), "source 0 = random");
    /* knob 5: animation, UI-only (never written to the DSP) */
    h.writes.length = 0;
    ui.onMidi([0xB0, 75, 4]); ui.tick();
    check(ui.anim === "ripple" && ui.scene === "ripple", "knob 5: auto -> ripple");
    ui.onMidi([0xB0, 75, 4]); ui.tick();
    check(ui.anim === "snake" && ui.scene === "snake", "knob 5: -> snake");
    ui.onMidi([0xB0, 75, 30]); ui.tick();
    check(ui.anim === "invade" && ui.scene === "invade", "knob 5 clamps at invade");
    check(h.writes.length === 0, "knob 5 writes nothing to the DSP");
    h.t += 60000; ui.tick();
    check(ui.scene === "invade", "a fixed choice never rotates");
    ui.onMidi([0xB0, 75, 128 - 40]); ui.tick();
    check(ui.anim === "auto", "knob 5 back to auto");
    h.t += 15000; ui.tick();
    check(ui.scene === "ripple", "auto rotates after 15 s");
    h.t += 15000; ui.tick();
    check(ui.scene === "snake", "auto rotates again");
    ui.onMidi([0x90, 79, 100]); ui.onMidi([0x80, 79, 0]); ui.tick();   /* press in snake: food */
    ui.onMidi([0x90, 6, 127]); ui.onMidi([0xB0, 77, 5]); ui.tick();   /* knob 7: unassigned */

    /* screen dismissed: Move's colours back, pads released */
    h.displayOn = false;
    ui.tick();
    check(h.block[h.block.length - 1] === 0, "pads released when the screen goes");
    let restored = 0;
    for (let n = 68; n <= 99; n++) if (h.leds[n] === h.snapshot[String(n)]) restored++;
    check(restored === 32, `Move's colours restored (${restored})`);
    const sends = h.ledSends;
    ui.tick(); ui.tick();
    check(h.ledSends === sends, "no LED writes while the screen is off");

    /* and back again */
    h.displayOn = true;
    ui.tick();
    check(h.block[h.block.length - 1] === 1, "re-blocked on return");
}

/* Back: restore and release */
{
    const h = makeHost();
    const ui = createUi(h.api);
    ui.init(); ui.tick();
    ui.onMidi([0x90, 90, 100]);
    check(ui.handleBack() === false, "Back lets the host navigate");
    check(h.block[h.block.length - 1] === 0, "Back releases the pads");
    check(h.leds[90] === 5, "Back restores Move's colours");
}

/* a failed read keeps the last good state */
{
    const h = makeHost();
    const ui = createUi(h.api);
    ui.init();
    h.params.ui_state = null;
    for (let i = 0; i < 10; i++) ui.tick();
    check(ui.state.current[0] === 53, "null read keeps last state");
}

console.log(`${fails ? "FAIL" : "PASS"}: ui ${checks} checks, ${fails} failures`);
if (fails) process.exit(1);

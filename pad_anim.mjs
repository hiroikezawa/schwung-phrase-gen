/*
 * Phrase Gen - pad animations.
 *
 * Pure: a scene is stepped by step() and drawn by colors() into 32 palette
 * indices (index = note - 68; Move's pad rows ascend from the BOTTOM, so
 * row 0 is the bottom row, x 0 the left column). Randomness is injected.
 *
 *   ripple  rings spread from the centre; a pad press throws one from there
 *   snake   a snake chases food round the grid; a press drops food there
 *   invade  aliens march down, a cannon shoots them; a press fires there
 */

export const W = 8, H = 4;
export const OFF = 0;
const RED = 1, WHITE = 120;

export const ANIMS = ["auto", "ripple", "snake", "invade"];
export const ANIM_LABELS = ["AUTO", "RIPL", "SNAK", "INVD"];
export const SCENES = ["ripple", "snake", "invade"];

const idx = (x, y) => y * W + x;
const pick = (rand, list) => list[Math.floor(rand() * list.length) % list.length];

/* ---------------- ripple ---------------- */
function makeRipple(rand, palette) {
    const rings = [];
    let t = 0;
    function spawn(cx, cy) {
        if (rings.length >= 6) rings.shift();
        rings.push({ cx, cy, r: 0, color: pick(rand, palette) });
    }
    return {
        name: "ripple",
        step() {
            t++;
            for (const ring of rings) ring.r += 0.5;
            while (rings.length && rings[0].r > 9) rings.shift();
            if (t % 8 === 1) spawn(3.5, 1.5);
        },
        press(x, y) { spawn(x, y); },
        colors() {
            const out = new Array(W * H).fill(OFF);
            for (let y = 0; y < H; y++)
                for (let x = 0; x < W; x++) {
                    /* the newest ring wins where two overlap */
                    for (let i = rings.length - 1; i >= 0; i--) {
                        const g = rings[i];
                        const d = Math.hypot(x - g.cx, y - g.cy);
                        if (Math.abs(d - g.r) < 0.6) { out[idx(x, y)] = g.color; break; }
                    }
                }
            return out;
        },
    };
}

/* ---------------- snake ---------------- */
const DIRS = [[1, 0], [0, 1], [-1, 0], [0, -1]];
const wrap = (v, n) => ((v % n) + n) % n;

function makeSnake(rand, palette) {
    let body, dir, foods, grow, dead, bodyColors, t;
    function reset() {
        body = [[1, 1], [0, 1]];
        dir = 0;
        foods = [];
        grow = 0;
        dead = 0;
        t = 0;
        bodyColors = [pick(rand, palette), pick(rand, palette)];
        addFood();
    }
    const occupied = (x, y) => body.some(([bx, by]) => bx === x && by === y);
    function addFood(x, y) {
        if (x === undefined) {
            const free = [];
            for (let yy = 0; yy < H; yy++)
                for (let xx = 0; xx < W; xx++) if (!occupied(xx, yy)) free.push([xx, yy]);
            if (!free.length) return;
            [x, y] = pick(rand, free);
        }
        if (occupied(x, y) || foods.some((f) => f.x === x && f.y === y)) return;
        if (foods.length >= 4) foods.shift();
        foods.push({ x, y, color: pick(rand, palette) });
    }
    function dist(ax, ay, bx, by) {
        const dx = Math.min(Math.abs(ax - bx), W - Math.abs(ax - bx));
        const dy = Math.min(Math.abs(ay - by), H - Math.abs(ay - by));
        return dx + dy;
    }
    reset();
    return {
        name: "snake",
        step() {
            t++;
            if (dead > 0) { if (--dead === 0) reset(); return; }
            if (t % 2) return;                       /* half speed: readable */
            const [hx, hy] = body[0];
            /* greedy toward the nearest food, never into itself if avoidable */
            let best = -1, bestScore = Infinity;
            for (let d = 0; d < 4; d++) {
                if (d === (dir + 2) % 4) continue;  /* no U-turn */
                const nx = wrap(hx + DIRS[d][0], W), ny = wrap(hy + DIRS[d][1], H);
                const tailFree = body.length > 1 && grow === 0 &&
                    body[body.length - 1][0] === nx && body[body.length - 1][1] === ny;
                if (occupied(nx, ny) && !tailFree) continue;
                let score = foods.length ? Math.min(...foods.map((f) => dist(nx, ny, f.x, f.y))) : 0;
                score += rand() * 0.9;              /* a little wander */
                if (score < bestScore) { bestScore = score; best = d; }
            }
            if (best < 0) { dead = 12; return; }    /* boxed in: game over */
            dir = best;
            const nx = wrap(hx + DIRS[dir][0], W), ny = wrap(hy + DIRS[dir][1], H);
            body.unshift([nx, ny]);
            bodyColors.unshift(bodyColors[0]);
            const fi = foods.findIndex((f) => f.x === nx && f.y === ny);
            if (fi >= 0) {
                bodyColors[0] = foods[fi].color;     /* wears what it ate */
                foods.splice(fi, 1);
                grow += 1;
                if (body.length >= 14) { dead = 12; return; }   /* full: start over */
                addFood();
            }
            if (grow > 0) grow--;
            else { body.pop(); bodyColors.pop(); }
            if (!foods.length) addFood();
        },
        press(x, y) { addFood(x, y); },
        colors() {
            const out = new Array(W * H).fill(OFF);
            if (dead > 0) {
                if (dead % 4 < 2) for (const [x, y] of body) out[idx(x, y)] = RED;
                return out;
            }
            for (const f of foods) out[idx(f.x, f.y)] = (t >> 1) % 2 ? f.color : OFF;
            body.forEach(([x, y], i) => { out[idx(x, y)] = i === 0 ? WHITE : bodyColors[i]; });
            return out;
        },
    };
}

/* ---------------- invaders ---------------- */
function makeInvade(rand, palette) {
    let aliens, march, shots, booms, cannon, t, over, alienColor, wins;
    function wave() {
        /* six wide: room to march two steps before they drop a row */
        const o = wins % 2 ? 2 : 0;
        aliens = [];
        for (let x = 0; x < 6; x += 2) aliens.push({ x: o + x, y: 3 });
        for (let x = 1; x < 6; x += 2) aliens.push({ x: o + x, y: 2 });
        march = o ? -1 : 1;
        shots = [];
        alienColor = pick(rand, palette);
    }
    function reset() { wins = 0; cannon = 3; t = 0; over = 0; booms = []; wave(); }
    function fire(x) {
        if (shots.length >= 3) return;
        shots.push({ x, y: 1 });
        hit();                                       /* point blank */
    }
    function hit() {
        for (const s of shots) {
            const i = aliens.findIndex((a) => a.x === s.x && a.y === s.y);
            if (i >= 0) {
                booms.push({ x: s.x, y: s.y, life: 4, color: pick(rand, palette) });
                aliens.splice(i, 1);
                s.y = H;                             /* spent */
            }
        }
        shots = shots.filter((s) => s.y < H);
    }
    reset();
    return {
        name: "invade",
        step() {
            t++;
            for (const b of booms) b.life--;
            booms = booms.filter((b) => b.life > 0);
            if (over > 0) { if (--over === 0) reset(); return; }
            for (const s of shots) s.y++;
            hit();
            /* the cannon wanders under the nearest alien and shoots */
            if (aliens.length) {
                const target = aliens.reduce((a, b) =>
                    Math.abs(b.x - cannon) < Math.abs(a.x - cannon) ? b : a);
                if (target.x > cannon) cannon++;
                else if (target.x < cannon) cannon--;
                else if (rand() < 0.6) fire(cannon);
            }
            /* aliens march: sideways, down at the wall */
            if (t % 7 === 0 && aliens.length) {
                const edge = aliens.some((a) => a.x + march < 0 || a.x + march >= W);
                if (edge) { march = -march; for (const a of aliens) a.y--; }
                else for (const a of aliens) a.x += march;
                hit();
                if (aliens.some((a) => a.y <= 0)) { over = 16; return; }   /* landed */
            }
            if (!aliens.length) { wins++; wave(); }
        },
        press(x) { cannon = x; fire(x); },
        colors() {
            const out = new Array(W * H).fill(OFF);
            if (over > 0) {
                if (over % 4 < 2) for (let i = 0; i < W * H; i++) out[i] = RED;
                return out;
            }
            for (const a of aliens) if (a.y >= 0 && a.y < H) out[idx(a.x, a.y)] = alienColor;
            for (const s of shots) if (s.y < H) out[idx(s.x, s.y)] = WHITE;
            for (const b of booms) out[idx(b.x, b.y)] = b.color;
            out[idx(cannon, 0)] = WHITE;
            return out;
        },
    };
}

const MAKERS = { ripple: makeRipple, snake: makeSnake, invade: makeInvade };

export function makeScene(name, rand, palette) {
    return (MAKERS[name] || makeRipple)(rand, palette);
}

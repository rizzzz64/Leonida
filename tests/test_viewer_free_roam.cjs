// CPU-only tests of the actual pure helpers in the viewer. No game data or WebGL.
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { join } = require('node:path');
const { test } = require('node:test');
const vm = require('node:vm');
const html = readFileSync(join(__dirname, '..', 'viewer', 'index.html'), 'utf8');
function helper(name, end, exports) {
    const a = html.indexOf(name), b = html.indexOf(end, a + name.length);
    assert(a >= 0 && b > a);
    return vm.runInNewContext(html.slice(a, b) + '\n(' + exports + ')');
}

// Stable ties and bounded storage, with the same result as a full stable sort.
test('nearest candidate selection matches stable sort', () => {
    const retain = helper('function retainNearest(', 'function stream(', 'retainNearest');
    const rows = Array.from({length: 12000}, (_, i) => ({def: i, distance: (i * 7919) % 251}));
    for (const k of [0, 1, 2, 3, 4]) {
        const got = [];
        for (const r of rows) { retain(got, r.def, r.distance, k); assert(got.length <= k); }
        assert.deepEqual(got.map(x => x.def), rows.slice().sort((a,b) => a.distance-b.distance).slice(0,k).map(x=>x.def));
    }
});

test('walking camera uses normalized horizontal input independent of pitch', () => {
    const direction = helper('function walkingDirection(', 'const WALK_SPEED', 'walkingDirection');
    const close = (a,b) => assert(Math.abs(a-b)<1e-9);
    for (const [keys,yaw,x,z] of [
        [[],0,0,0], [['KeyW'],0,0,-1], [['KeyD'],0,1,0], [['KeyW'],Math.PI/2,-1,0],
        [['KeyW','KeyD'],0,Math.SQRT1_2,-Math.SQRT1_2], [['KeyW','KeyS'],0,0,0]]) {
        const d = direction(new Set(keys),yaw); close(d.x,x); close(d.z,z);
    }
});

test('gravity lands on floor, blocks walls and waits for missing geometry', () => {
    const { advanceWalker } = helper('// ---------- walk physics (pure) ----------', '// ---------- end walk physics ----------', '{ advanceWalker }');
    const p = {x:0,y:12,z:0}, state = {vy:0,grounded:false,waiting:false};
    const floor = {ground:()=>0,blocked:()=>false};
    for(let i=0;i<100;i++) advanceWalker(p,state,{x:0,z:0},4.2,0.05,floor);
    assert.equal(p.y,1.7); assert.equal(state.vy,0); assert(state.grounded);
    advanceWalker(p,state,{x:1,z:0},4.2,0.1,{...floor,blocked:()=>true});
    assert.equal(p.x,0);
    advanceWalker(p,state,{x:1,z:0},4.2,0.1,{ground:()=>null,blocked:()=>false});
    assert.equal(p.y,1.7); assert.equal(p.x,0); assert(state.waiting);
    advanceWalker(p,state,{x:1,z:0},4.2,0.1,floor);
    assert(Math.abs(p.x-0.42)<1e-9); assert(!state.waiting);
});

test('jump is edge-triggered, lands and does not repeat while held', () => {
    const { advanceWalker } = helper('// ---------- walk physics (pure) ----------', '// ---------- end walk physics ----------', '{ advanceWalker }');
    const p = {x:0,y:1.7,z:0}, state = {vy:0,grounded:true,waiting:false,jumpHeld:false};
    const floor = {ground:()=>0,blocked:()=>false}, d = {x:0,z:0};
    advanceWalker(p,state,d,4.2,0.1,floor,true);
    assert(p.y>1.7 && !state.grounded);
    for(let i=0;i<30;i++) advanceWalker(p,state,d,4.2,0.1,floor,true);
    assert.equal(p.y,1.7); assert(state.grounded);
    advanceWalker(p,state,d,4.2,0.1,floor,false);
    advanceWalker(p,state,d,4.2,0.1,floor,true);
    assert(p.y>1.7);
});

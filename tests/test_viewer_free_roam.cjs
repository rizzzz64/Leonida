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

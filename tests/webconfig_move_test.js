// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Derek Reynolds
//
// The write side of the Layout (#212): fieldsFromLayout derives the board's
// fields from where the boxes sit, and moveMonitor moves a computer's block.
const fs = require('fs');
const vm = require('vm');
const assert = require('assert/strict');
const html = fs.readFileSync(process.argv[2], 'utf8');
const inert = {addEventListener() {}, style: {}, dataset: {}};
const context = {console, Uint8Array, ArrayBuffer, DataView, Event: function() {},
  navigator: {}, window: {addEventListener() {}}, document: {
    getElementById() { return inert; }, querySelector() { return null; }, querySelectorAll() { return []; },
  }};
vm.createContext(context);
vm.runInContext(html.match(/<script>\s*([\s\S]*?)\s*<\/script>/)[1], context);

// Every key the layout owns. A result must carry all of them: an unused segment is written as zeros.
const owned = [11, 41, 17, 47, 98, 99, ...Array.from({length: 24}, (_, n) => 140 + n)];
// Spread copies a vm-realm object into this realm, which deepEqual insists on.
function filled(fields) {
  return Object.fromEntries(owned.map(key => [key, fields[key] || 0]));
}
// The maintainer's desk: [2][Main] over [Main][2], segments 1 = A2-B1, 2 = A1-B2.
function desk() {
  return filled({11:2, 41:2, 17:5, 47:4, 98:1, 99:2,
    140:2,141:0,142:65535, 143:1,144:0,145:65535,
    152:1,153:0,154:65535, 155:2,156:0,157:65535});
}
function roundTrip(fields, name) {
  const layout = context.layoutFromFields(fields);
  assert.equal(layout.note, '', name);
  const result = context.fieldsFromLayout(layout);
  assert.deepEqual({...result.fields}, fields, name);
}
roundTrip(desk(), 'maintainer desk');
// The user guide's two over two: [Main][2] over [Main][2].
roundTrip({...desk(), 98:2, 140:1, 143:2}, 'guide two over two');
// Three over two, centred: four half-edge segments.
const threeOverTwo = filled({11:3, 41:2, 17:5, 47:4, 98:2, 99:2,
  140:1,141:32768,142:65535, 143:2,144:0,145:32768, 146:2,147:32768,148:65535, 149:3,150:0,151:32768,
  152:1,153:0,154:32768, 155:1,156:32768,157:65535, 158:2,159:0,160:32768, 161:2,162:32768,163:65535});
roundTrip(threeOverTwo, 'three over two centred');
function opposite(direction) { return {1:2, 2:1, 4:5, 5:4}[direction]; }
// The same two-box desk on every side of the seam.
for (const [border, chain] of [[1,5],[2,5],[4,2],[5,2]])
  roundTrip({...desk(), 17:border, 47:opposite(border), 98:chain, 99:chain, 140:1, 143:2}, `border ${border}`);
// A row [Main][2] over a column whose Main is the top box.
roundTrip(filled({11:2, 41:2, 17:5, 47:4, 98:2, 99:5, 140:1,141:0,142:65535, 152:1,153:0,154:65535}), 'row over column');
// One monitor keeps whatever chain direction the field holds.
roundTrip(filled({11:1, 41:2, 17:2, 47:1, 98:5, 99:2, 140:1,141:0,142:65535, 152:1,153:0,154:65535}), 'one monitor');
// A single box has no line to point anywhere, so chain equal to border is not custom.
roundTrip(filled({11:1, 41:2, 17:2, 47:1, 98:2, 99:2, 140:1,141:0,142:65535, 152:1,153:0,154:65535}), 'one monitor, chain equals border');

// Label moves. A gesture is the output letter and the block's shift in boxes.
function move(fields, gesture) {
  return context.moveMonitor(context.layoutFromFields(fields), gesture);
}
function positions(layout) {
  return JSON.parse(JSON.stringify(layout.outputs.map(o => o.monitors.map(m => [m.x, m.y]))));
}
// Slide B half a box right: three segments, two of them half edges.
let result = move(desk(), {output:'B', dx:0.5, dy:0});
assert.equal(result.refused, undefined);
assert.equal(result.layout.note, '');
assert.deepEqual(positions(result.layout), [[[1,0],[0,0]], [[0.5,1],[1.5,1]]]);
assert.deepEqual({...result.fields}, filled({11:2, 41:2, 17:5, 47:4, 98:1, 99:2,
  140:2,141:32768,142:65535, 143:1,144:0,145:32768, 146:1,147:32768,148:65535,
  152:1,153:0,154:32768, 155:1,156:32768,157:65535, 158:2,159:0,160:32768}));
// Jump B to each side. Only Main can face the other computer when the line
// points at it, so the two Mains meet on a side jump and one segment joins them.
const mains = filled({11:2, 41:2, 98:1, 99:2, 17:2, 47:1, 140:1,141:0,142:65535, 152:1,153:0,154:65535});
result = move(desk(), {output:'B', dx:2, dy:-1});
assert.deepEqual({...result.fields}, mains, 'B to the right');
assert.deepEqual(positions(result.layout), [[[1,0],[0,0]], [[2,0],[3,0]]]);
result = move(desk(), {output:'B', dx:0, dy:-2});
assert.deepEqual({...result.fields}, {...desk(), 17:4, 47:5}, 'B on top');
assert.deepEqual(positions(result.layout), [[[1,1],[0,1]], [[0,0],[1,0]]]);
// The mirrored desk, [Main][2] over [2][Main], can take B on the left.
const mirrored = filled({...desk(), 98:2, 99:1, 140:1, 143:2, 152:2, 155:1});
result = move(mirrored, {output:'B', dx:-2, dy:-1});
assert.deepEqual({...result.fields}, {...mirrored, 17:1, 47:2, 143:0,144:0,145:0, 155:0,156:0,157:0, 140:1, 152:1}, 'B on the left');
assert.deepEqual(positions(result.layout), [[[2,0],[3,0]], [[1,0],[0,0]]]);
// Back below A from the right is the desk again.
result = move(mains, {output:'B', dx:-2, dy:1});
assert.deepEqual({...result.fields}, desk(), 'B below again');

// Refusals: one line each, the layout handed back untouched, no fields.
function refuse(fields, gesture, message) {
  const layout = context.layoutFromFields(fields);
  const before = JSON.stringify(layout);
  const result = context.moveMonitor(layout, gesture);
  assert.equal(result.refused, message);
  assert.equal(result.fields, undefined);
  assert.equal(result.layout, layout);
  assert.equal(JSON.stringify(layout), before);
}
refuse(desk(), {output:'B', dx:0, dy:1}, 'There is a gap between the computers. Put them edge to edge.');
refuse(desk(), {output:'B', dx:0, dy:-1}, 'The computers overlap. Put them edge to edge.');
refuse(desk(), {output:'B', dx:2, dy:0}, 'The computers touch only at a corner. Put an edge against an edge.');
refuse(desk(), {output:'B', dx:-2, dy:-1}, 'Put the main monitor next to the other computer.');
// Four over three, centred, needs six segments.
const fourOverThree = filled({11:4, 41:3, 17:5, 47:4, 98:2, 99:2,
  140:1,141:0,142:65535, 143:2,144:0,145:65535, 146:3,147:0,148:65535,
  152:1,153:0,154:65535, 155:2,156:0,157:65535, 158:3,159:0,160:65535});
refuse(fourOverThree, {output:'B', dx:0.5, dy:0}, 'This layout needs 6 segments; the board holds 4.');
console.log('webconfig_move_test: passed');

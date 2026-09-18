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

// Box gestures (#213). Dropping Main onto its own line flips it: the boxes
// stay put, renumbered from Main, and Chain Direction flips with them.
result = move(desk(), {output:'A', monitor:1, dx:-1, dy:0});
assert.equal(result.refused, undefined);
assert.deepEqual({...result.fields}, {...desk(), 98:2, 140:1, 143:2}, 'flip A: [2][Main] becomes [Main][2]');
assert.deepEqual(positions(result.layout), [[[0,0],[1,0]], [[0,1],[1,1]]]);

// Three boxes: [3][2][Main] over [Main][2][3]; Main dropped on 3 gives [Main][2][3].
const threeLine = filled({11:3, 41:3, 17:5, 47:4, 98:1, 99:2,
  140:3,141:0,142:65535, 143:2,144:0,145:65535, 146:1,147:0,148:65535,
  152:1,153:0,154:65535, 155:2,156:0,157:65535, 158:3,159:0,160:65535});
const threeFlipped = {...threeLine, 98:2, 140:1, 146:3};
result = move(threeLine, {output:'A', monitor:1, dx:-2, dy:0});
assert.deepEqual({...result.fields}, threeFlipped, 'flip A on three boxes');
assert.deepEqual(positions(result.layout), [[[0,0],[1,0],[2,0]], [[0,1],[1,1],[2,1]]]);
// Main must reach the far end; an intermediate box is not a flip.
refuse(threeLine, {output:'A', monitor:1, dx:-1, dy:0}, 'Not moved: drag the label to move the whole computer.');
// Another box turns the line to point from Main toward its cell: a row becomes
// a column and back, by box 2 and by box 3.
const rowOverColumn = filled({11:2, 41:2, 17:5, 47:4, 98:2, 99:5, 140:1,141:0,142:65535, 152:1,153:0,154:65535});
result = move(rowOverColumn, {output:'B', monitor:2, dx:1, dy:-1});
assert.deepEqual({...result.fields}, {...desk(), 98:2, 140:1, 143:2}, 'B turns to a row by box 2');
assert.deepEqual(positions(result.layout), [[[0,0],[1,0]], [[0,1],[1,1]]]);
result = move({...desk(), 98:2, 140:1, 143:2}, {output:'B', monitor:2, dx:-1, dy:1});
assert.deepEqual({...result.fields}, rowOverColumn, 'B turns back to a column by box 2');
const threeColumn = filled({11:3, 41:3, 17:5, 47:4, 98:2, 99:5, 140:1,141:0,142:65535, 152:1,153:0,154:65535});
result = move(threeLine, {output:'B', monitor:3, dx:-2, dy:1});
assert.deepEqual({...result.fields}, {...threeColumn, 98:1, 140:3}, 'B turns to a column by box 3 on a far cell');
assert.deepEqual(positions(result.layout), [[[2,0],[1,0],[0,0]], [[0,1],[0,2],[0,3]]]);
result = move({...threeColumn, 98:1, 140:3}, {output:'B', monitor:3, dx:1, dy:-1});
assert.deepEqual({...result.fields}, threeLine, 'B turns back to a row by box 3');
// One arrow-key step on a box off the line turns it the same way: box 2 pressed Down.
result = move({...desk(), 98:2, 140:1, 143:2}, {output:'A', monitor:2, dx:0, dy:-1});
assert.deepEqual({...result.fields}, filled({11:2, 41:2, 17:5, 47:4, 98:4, 99:2, 140:1,141:0,142:65535, 152:1,153:0,154:65535}), 'A turns up by one step');
assert.deepEqual(positions(result.layout), [[[0,1],[0,0]], [[0,2],[1,2]]]);
// "+" appends at the end of the line; "-" removes the last box.
result = move(desk(), {output:'A', add:1});
assert.deepEqual({...result.fields}, {...desk(), 11:3}, 'add to A: box 3 hangs past B');
assert.deepEqual(positions(result.layout), [[[2,0],[1,0],[0,0]], [[1,1],[2,1]]]);
result = context.moveMonitor(result.layout, {output:'A', add:-1});
assert.deepEqual({...result.fields}, desk(), 'remove from A');
let seven = desk();
for (let n = 2; n < 7; n++) seven = {...move(seven, {output:'B', add:1}).fields};
assert.equal(seven[41], 7);
assert.deepEqual(positions(context.layoutFromFields(seven))[1], [[0,1],[1,1],[2,1],[3,1],[4,1],[5,1],[6,1]]);
// One monitor keeps the line direction the field holds when it grows.
result = move(filled({11:1, 41:2, 17:2, 47:1, 98:5, 99:2, 140:1,141:0,142:65535, 152:1,153:0,154:65535}), {output:'A', add:1});
assert.deepEqual({...result.fields}, filled({11:2, 41:2, 17:2, 47:1, 98:5, 99:2, 140:1,141:0,142:65535, 152:1,153:0,154:65535}), 'add to a one-monitor computer');
// A one-monitor computer's only box moves the computer, the same as its label.
const one = filled({11:1, 41:2, 17:2, 47:1, 98:2, 99:2, 140:1,141:0,142:65535, 152:1,153:0,154:65535});
result = move(one, {output:'A', monitor:1, dx:1, dy:1});
assert.deepEqual({...result.fields}, {...one, 17:4, 47:5}, 'one box moves the block under B');
assert.deepEqual(positions(result.layout), [[[0,1]], [[0,0],[1,0]]]);

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
refuse(desk(), {output:'B', dx:0, dy:1}, 'Not moved: that leaves a gap between the computers. Put them edge to edge.');
refuse(desk(), {output:'B', dx:0, dy:-1}, 'Not moved: the computers would overlap. Put them edge to edge.');
refuse(desk(), {output:'B', dx:2, dy:0}, 'Not moved: the computers would touch only at a corner. Put an edge against an edge.');
refuse(desk(), {output:'B', dx:-2, dy:-1}, 'Not moved: put the main monitor next to the other computer.');
// Four over three, centred, needs six segments.
const fourOverThree = filled({11:4, 41:3, 17:5, 47:4, 98:2, 99:2,
  140:1,141:0,142:65535, 143:2,144:0,145:65535, 146:3,147:0,148:65535,
  152:1,153:0,154:65535, 155:2,156:0,157:65535, 158:3,159:0,160:65535});
refuse(fourOverThree, {output:'B', dx:0.5, dy:0}, 'Not moved: that layout needs 6 segments; the board holds 4.');
refuse(desk(), {output:'A', monitor:1, dx:0, dy:-1}, 'Not moved: drag the label to move the whole computer.');
refuse(desk(), {output:'A', monitor:2, dx:1, dy:0}, 'Not moved: drop a monitor beside, above or below the main monitor.');
refuse(desk(), {output:'A', monitor:2, dx:0, dy:1}, 'Not moved: the computers would overlap. Put them edge to edge.');
refuse(seven, {output:'B', add:1}, 'Not added: a computer holds at most 7 monitors.');
refuse(one, {output:'A', add:-1}, 'Not removed: a computer keeps its main monitor.');
console.log('webconfig_move_test: passed');

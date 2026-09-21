const fs = require('fs');
const vm = require('vm');
const assert = require('assert/strict');
const html = fs.readFileSync(process.argv[2], 'utf8');
const inert = {addEventListener() {}, style: {}, dataset: {}};
const context = {console, Uint8Array, ArrayBuffer, DataView, Event: function() {},
  navigator: {}, window: {addEventListener() {}}, MutationObserver: class {observe() {}}, document: {
    getElementById() { return inert; }, querySelector() { return null; }, querySelectorAll() { return []; },
  }};
vm.createContext(context);
vm.runInContext(html.match(/<script>\s*([\s\S]*?)\s*<\/script>/)[1], context);
function desk() {
  return {11:2, 41:2, 16:2, 46:3, 17:5, 47:4, 98:1, 99:2,
    140:2,141:0,142:65535,143:1,144:0,145:65535,
    152:1,153:0,154:65535,155:2,156:0,157:65535};
}
const layout = context.layoutFromFields(desk());
assert.equal(layout.note, '');
assert.deepEqual(JSON.parse(JSON.stringify(layout.outputs.map(o => o.monitors.map(m => [m.x,m.y])))),
  [[[1,0],[0,0]], [[0,1],[1,1]]]);
const markup = context.renderLayout(layout);
for (const label of ['Main', 'Output A', 'MacOS', 'Output B', 'Windows', 'data-segment="1"', 'data-segment="2"'])
  assert.ok(markup.includes(label), label);
assert.ok(markup.startsWith('<svg role="group"'), 'focusable boxes are not hidden inside an image role');
assert.ok(context.renderLayout(null).includes('Connect'));
// Each box is a focusable group that names its output and number, so a drop
// and an arrow key know what moved; a flip shows the renumbering.
const boxLabels = m => [...m.matchAll(/<g class="layout-box[^>]*data-output="(\w)" data-monitor="(\d)" tabindex="0"[^>]*>[\s\S]*?<text[^>]*>([^<]*)</g)].map(x => x.slice(1).join(''));
assert.deepEqual(boxLabels(markup), ['A1Main', 'A22', 'B1Main', 'B22']);
const flipped = context.moveMonitor(layout, {output:'A', monitor:1, dx:-1, dy:0}).layout;
const flippedMarkup = context.renderLayout(flipped);
assert.deepEqual(boxLabels(flippedMarkup), ['A1Main', 'A22', 'B1Main', 'B22']);
const boxX = (m, letter, n) => Number(m.match(new RegExp(`data-output="${letter}" data-monitor="${n}"[^>]*><rect[^>]* x="(\\d+)"`))[1]);
assert.ok(boxX(markup, 'A', 1) > boxX(markup, 'A', 2), 'Main on the right before the flip');
assert.ok(boxX(flippedMarkup, 'A', 1) < boxX(flippedMarkup, 'A', 2), 'Main on the left after the flip');
for (const letter of ['A', 'B']) for (const add of [1, -1])
  assert.ok(markup.includes(`onclick="applyGesture({output:'${letter}',add:${add}})"`), `${letter} ${add}`);

const customNote = 'Custom values are set in Advanced. Moving a monitor here replaces them.';
function expectCustom(fields) {
  const result = context.layoutFromFields(fields);
  assert.equal(result.note, customNote);
  assert.equal(result.outputs.length, 2);
  const markup = context.renderLayout(result);
  assert.ok(markup.includes(customNote));
  assert.ok(markup.includes('Open Advanced'));
}
let fields = desk();
fields[98] = 2; fields[140] = 1; fields[143] = 2;
assert.equal(context.layoutFromFields(fields).note, '', 'guide two-over-two');
assert.deepEqual(JSON.parse(JSON.stringify(context.layoutFromFields(fields).outputs.map(o => o.monitors.map(m => [m.x,m.y])))),
  [[[0,0],[1,0]], [[0,1],[1,1]]]);
fields = {...fields, 11:3};
for (const [index, pair] of [
  [[1,32768,65535],[1,0,32768]], [[2,0,32768],[1,32768,65535]],
  [[2,32768,65535],[2,0,32768]], [[3,0,32768],[2,32768,65535]],
].entries()) pair.forEach((side,i) => side.forEach((v,j) => fields[(i ? 152 : 140)+index*3+j] = v));
let result = context.layoutFromFields(fields);
assert.equal(result.note, '', 'centred half edges');
assert.equal(result.outputs[1].monitors[0].x, 0.5);
assert.equal(result.bands.length, 4);
expectCustom({...desk(), 98:5});
expectCustom({...desk(), 47:2});
expectCustom({...desk(), 141:1234});
expectCustom({...desk(), 140:3});
expectCustom({...desk(), 154:32768});
// Equal partial edges on aligned boxes do not describe their physical overlap.
expectCustom({...desk(), 142:32768, 154:32768});
fields = desk();
for (let key=140; key<164; key++) fields[key] = 0;
result = context.layoutFromFields(fields);
assert.equal(result.note, 'Segments are not set. Move a monitor to set them.');
assert.equal(Math.min(...result.outputs[0].monitors.map(m => m.x)), 0);
assert.equal(Math.min(...result.outputs[1].monitors.map(m => m.x)), 0);
assert.ok(context.renderLayout(result).includes('Open Advanced'));
assert.ok(html.includes('<details id="advanced">'));
assert.equal((html.match(/<summary>Advanced<\/summary>/g) || []).length, 1);
for (const letter of ['A','B']) {
  // One disclosure holds both computers' panels; the page shows the selected one.
  const panel = html.match(new RegExp(`id="advanced-${letter}"[^>]*>([\\s\\S]*?)(?=<div class="computer"|</details>)`));
  assert.ok(panel, `Advanced output ${letter}`);
  const keys = letter === 'A' ? [11,14,15,17,98] : [41,44,45,47,99];
  for (const key of keys) assert.ok(panel[1].includes(`data-key="${key}"`));
  const base = letter === 'A' ? 140 : 152;
  for (let key=base; key<base+12; key++) assert.ok(panel[1].includes(`data-key="${key}"`));
  assert.ok(panel[1].includes('<option value="7">7</option>'));
  assert.equal((panel[1].match(/min="0" max="7"/g) || []).length, 4);
}
// The same geometry must survive rotating the seam to every side.
for (const [border, opposite, chain] of [[1,2,5],[2,1,5],[4,5,2],[5,4,2]]) {
  const rotated = {...desk(),17:border,47:opposite,98:chain,99:chain,140:1,143:2};
  const drawn = context.layoutFromFields(rotated);
  assert.equal(drawn.note, '', `border ${border}`);
  assert.equal(drawn.bands.length, 2);
  assert.ok(!/NaN|Infinity|undefined/.test(context.renderLayout(drawn)));
}
fields = {...desk(),11:7,41:7,140:7,152:1,143:6,155:2};
assert.equal(context.layoutFromFields(fields).outputs[0].monitors.length, 7);
assert.equal(context.layoutFromFields(fields).note, '');
// A chain pointing away from the other computer can join at Main.
fields = {...desk(),11:2,41:2,17:2,47:1,98:1,99:2,140:1,152:1};
for (let key=143; key<152; key++) fields[key]=0;
for (let key=155; key<164; key++) fields[key]=0;
assert.equal(context.layoutFromFields(fields).note, '');
// Joining an interior edge would put another monitor over the other computer.
expectCustom({...fields,140:2});
// Monitor 0 disables a segment even when its previous endpoints remain stored.
fields = desk();
for (let n=0; n<4; n++) {fields[140+n*3]=0; fields[152+n*3]=0;}
assert.equal(context.layoutFromFields(fields).note, 'Segments are not set. Move a monitor to set them.');
fields = {...desk(),146:0,147:1234,148:65535,158:0,159:1234,160:65535};
assert.equal(context.layoutFromFields(fields).note, '');
console.log('webconfig_picture_test: geometry, fields and rendering passed');

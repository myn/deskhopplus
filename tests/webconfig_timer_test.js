const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const html = fs.readFileSync(process.argv[2], 'utf8');
const fields = new Map();
for (const key of [21, 22, 51, 52, 14]) {
  const markup = html.match(new RegExp(`<input[^>]*data-key="${key}"[^>]*>`))?.[0];
  assert.ok(markup, `field ${key} is rendered`);
  const attrs = new Map([...markup.matchAll(/([\w-]+)="([^"]*)"/g)].map(([, k, v]) => [k, v]));
  const error = {textContent: ''};
  fields.set(key, {
    type: attrs.get('type'), _value: '', error,
    get value() { return this._value; },
    set value(value) { this._value = String(value); },
    getAttribute(name) { return attrs.get(name) ?? null; },
    setAttribute(name, value) { attrs.set(name, String(value)); },
    hasAttribute(name) { return attrs.has(name); },
    dispatchEvent() {},
    closest() { return {querySelector(selector) { return selector === 'small' ? error : null; }}; },
    checkValidity() { return this.type === 'number' && this.value !== '' &&
      Number(this.value) >= Number(attrs.get('min')) && Number(this.value) <= Number(attrs.get('max')); },
    reportValidity() { this.reportedInvalid = true; },
  });
  if (key === 14) {
    assert.equal(attrs.has('data-scale'), false, 'non-timer has no unit conversion');
    continue;
  }
  assert.match(html, new RegExp(`<label for="f${key}">(?:Idle|Max) Time \\(seconds\\)</label>`));
  assert.equal(attrs.get('data-scale'), '1000000');
  assert.equal(attrs.get('type'), 'number');
  assert.equal(attrs.get('min'), '0');
  assert.equal(attrs.get('max'), '4294.967295');
  assert.equal(attrs.get('step'), '0.000001');
  assert.equal(markup.includes('onchange='), false, 'timer edit waits for Save');
  assert.match(html, new RegExp(`<small class="timer-error"[^>]*id="timer-error-${key}"`));
  assert.match(html, /0–4294\.967295 seconds/);
}

const context = {console, Uint8Array, ArrayBuffer, DataView, Event: function() {},
  navigator: {}, window: {addEventListener() {}}, MutationObserver: class {observe() {}},
  document: {getElementById() { return {addEventListener() {}}; },
    querySelector(selector) { return fields.get(Number(selector.match(/data-key="(\d+)"/)?.[1])) || null; },
    querySelectorAll(selector) { return selector === '.api' ? [...fields.values()] : []; }}};
vm.createContext(context);
vm.runInContext(html.match(/<script>\s*([\s\S]*?)\s*<\/script>/)[1], context);
const sent = [];
context.redrawLayout = () => {};
context.revealField = field => { field.revealed = true; };
context.sendReport = async (type, payload, both) => sent.push({type, payload, both});
context.saveHotkeys = async () => true;
context.saveKeymaps = async () => true;
context.device = {opened: true};
fields.get(14).setAttribute('fetched-value', '');

function read(key, microseconds) {
  const report = new DataView(new ArrayBuffer(12));
  report.setUint32(4, microseconds, true);
  context.updateElement(key, {data: report});
  const field = fields.get(key);
  assert.equal(Number(field.value), microseconds / 1000000, `field ${key} display`);
  assert.equal(field.getAttribute('fetched-value'), field.value, `field ${key} unchanged`);
  assert.equal(new DataView(context.packValue(field, key, 'uint64').buffer).getUint32(1, true),
    microseconds, `field ${key} packs exact microseconds`);
}

(async () => {
  for (const key of [21, 22, 51, 52]) {
    for (const microseconds of [0, 1, 240000000, 4294967295]) read(key, microseconds);
    await context.valueChangedHandler(fields.get(key));
  }
  assert.equal(sent.length, 0, 'reading unchanged timers sends no writes');
  await context.saveHandler();
  assert.equal(sent.filter(x => x.type === 21).length, 0, 'saving unchanged timers sends no SET');
  sent.length = 0;

  for (const key of [21, 22, 51, 52]) {
    fields.get(key).value = '1.234567';
  }
  assert.equal(sent.length, 0, 'timer edits wait for Save');
  await context.saveHandler();
  const writes = sent.filter(x => x.type === 21);
  assert.equal(writes.length, 4, 'Save writes each changed timer');
  for (const [index, key] of [21, 22, 51, 52].entries()) {
    const write = writes[index];
    assert.equal(write.type, 21);
    assert.equal(write.both, true);
    assert.equal(new DataView(write.payload.buffer).getUint32(1, true), 1234567,
      `field ${key} writes microseconds`);
  }
  sent.length = 0;

  const timer = fields.get(21);
  timer.value = '4294.967296';
  await context.valueChangedHandler(timer);
  assert.equal(sent.length, 0, 'out-of-range timer is refused');
  assert.equal(timer.reportedInvalid, true, 'range error is shown');
  await context.saveHandler();
  assert.equal(sent.length, 0, 'invalid timer prevents Save');
  assert.equal(timer.revealed, true, 'Save reveals the invalid timer');
  assert.match(timer.error.textContent, /0–4294\.967295 seconds/);

  timer.value = '1.234567';
  const normal = fields.get(14);
  const report = new DataView(new ArrayBuffer(12));
  report.setInt32(4, -123, true);
  context.updateElement(14, {data: report});
  assert.equal(normal.value, '-123');
  assert.equal(new DataView(context.packValue(normal, 14, 'int32').buffer).getInt32(1, true), -123);
  console.log('webconfig_timer_test: both outputs, bounds, unchanged Save and non-timer passed');
})().catch(error => { console.error(error); process.exit(1); });

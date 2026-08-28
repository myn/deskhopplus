const fs = require('fs');
const vm = require('vm');

const html = fs.readFileSync(process.argv[2], 'utf8');
const match = html.match(/<script>\s*([\s\S]*?)\s*<\/script>/);
if (!match) throw new Error('rendered config page has no script');

const inert = {addEventListener() {}, style: {}, dataset: {}};
const context = {
  console,
  Uint8Array,
  ArrayBuffer,
  DataView,
  Event: function Event() {},
  navigator: {},
  window: {addEventListener() {}},
  document: {
    getElementById() { return inert; },
    querySelector() { return null; },
    querySelectorAll() { return []; },
  },
};
vm.createContext(context);
vm.runInContext(match[1] + `
  globalThis.testApi = {parseOverrides, parsePassthrough, saveKeymaps};
`, context);

let failures = 0;
function check(condition, message) {
  if (!condition) { ++failures; console.error(`FAIL: ${message}`); }
}
function input(value) {
  const error = {textContent: ''};
  return {
    value,
    attrs: new Map(),
    parentElement: {querySelector() { return error; }},
    getAttribute(name) { return this.attrs.has(name) ? this.attrs.get(name) : null; },
    setAttribute(name, value) { this.attrs.set(name, value); },
    error,
  };
}

const {parseOverrides, parsePassthrough} = context.testApi;
let result = parseOverrides({value: 'capslock=lctrl\nlshift=a'});
check(!result.error && result.parsed.length === 2, 'cross-class overrides did not parse');
check(result.parsed[0][0] === 0x39 && result.parsed[0][1] === 0xe0,
      'override did not use the canonical key vocabulary');
result = parseOverrides({value: 'capslock=nope'});
check(result.error && result.error.line === 1 && result.error.token === 'nope',
      'unknown override key lacks a structured error');
result = parseOverrides({value: Array.from({length: 33}, (_, i) => `a=f${i % 12 + 1}`).join('\n')});
check(result.error && result.error.line === 33, 'oversized overrides were truncated');
result = parsePassthrough({value: 'capslock, lctrl\nf1'});
check(!result.error && result.parsed.join(',') === '57,224,58',
      'passthrough text did not parse canonically');
result = parsePassthrough({value: 'a,,b'});
check(result.error && result.error.line === 1, 'malformed passthrough text was accepted');

const fields = [
  [input('capslock=lctrl'), input('capslock')],
  [input('a=nope'), input('f1')],
];
const sent = [];
context.device = {opened: true};
context.testFields = fields;
context.sentReports = sent;
vm.runInContext(`
  keymapInput = (output, kind) => testFields[output][kind === 'overrides' ? 0 : 1];
  sendReport = async (type, payload, both) => sentReports.push({type, payload, both});
`, context);

(async () => {
  const saved = await context.testApi.saveKeymaps();
  check(!saved, 'malformed profile did not refuse the whole save');
  check(sent.length === 0, 'a prefix was written before malformed profile refusal');
  check(fields[1][0].error.textContent.includes('token “nope”'),
        'structured profile error was not rendered inline');
  fields[1][0].value = 'a=b';
  const validSaved = await context.testApi.saveKeymaps();
  check(validSaved, 'valid profiles were refused');
  check(sent.length === 24, 'valid profiles did not write every fixed-size chunk');
  check(sent[0].payload[0] === 24 && sent[0].payload[1] === 0x39 && sent[0].payload[2] === 0xe0,
        'output A override bytes were not transmitted');
  check(sent[12].payload[0] === 54 && sent[12].payload[1] === 0x04 && sent[12].payload[2] === 0x05,
        'output B override bytes were not transmitted');
  if (failures) process.exit(1);
  console.log('webconfig_keymap_test: all checks passed');
})().catch(error => { console.error(error); process.exit(1); });

const mgmtReportId = 6;
var device;

const packetType = {
  keyboardReportMsg: 1, mouseReportMsg: 2, outputSelectMsg: 3, firmwareUpgradeMsg: 4, switchLockMsg: 7,
  syncBordersMsg: 8, flashLedMsg: 9, wipeConfigMsg: 10, readConfigMsg: 16, writeConfigMsg: 17, saveConfigMsg: 18,
  rebootMsg: 19, getValMsg: 20, setValMsg: 21, getValAllMsg: 22, proxyPacketMsg: 23
};

function calcChecksum(report) {
  let checksum = 0;
  for (let i = 3; i < 11; i++)
    checksum ^= report[i];

  return checksum;
}

async function sendReport(type, payload = [], sendBoth = false) {
  if (!device || !device.opened)
    return;

  /* First send this one, if the first one gets e.g. rebooted */
  if (sendBoth) {
    var reportProxy = makeReport(type, payload, true);
    await device.sendReport(mgmtReportId, reportProxy);
    }

    var report = makeReport(type, payload, false);
    await device.sendReport(mgmtReportId, report);
}

function makeReport(type, payload, proxy=false) {
  var dataOffset = proxy ? 4 : 3;
  report = new Uint8Array([0xaa, 0x55, type, ...new Array(9).fill(0)]);

  if (proxy)
    report = new Uint8Array([0xaa, 0x55, packetType.proxyPacketMsg, type, ...new Array(7).fill(0), type]);

  if (payload) {
    report.set([...payload], dataOffset);
    report[report.length - 1] = calcChecksum(report);
  }
  return report;
}

function packValue(element, key, dataType, buffer) {
  const dataOffset = 1;
  var buffer = new ArrayBuffer(8);
  var view = new DataView(buffer);

  const methods = {
    "uint32": view.setUint32,
    "uint64": view.setUint32, /* Yes, I know. :-| */
    "int32": view.setInt32,
    "uint16": view.setUint16,
    "uint8": view.setUint8,
    "int16": view.setInt16,
    "int8": view.setInt8
  };

  if (dataType in methods) {
    const method = methods[dataType];
    if (element.type === 'checkbox')
      view.setUint8(dataOffset, element.checked ? 1 : 0, true);
    else
      method.call(view, dataOffset, element.value, true);
  }

  view.setUint8(0, key);
  return new Uint8Array(buffer);
}

window.addEventListener('load', function () {
  if (!("hid" in navigator)) {
    document.getElementById('warning').style.display = 'block';
  }

  this.document.getElementById('menu-buttons').addEventListener('click', function (event) {
    window[event.target.dataset.handler]();
  })
});

document.getElementById('submitButton').addEventListener('click', async () => { await saveHandler(); });

async function connectHandler() {
  if (device && device.opened)
    return;

  var devices = await navigator.hid.requestDevice({
    filters: [{ vendorId: 0x2e8a, productId: 0x107c, usagePage: 0xff00, usage: 0x10 }]
  });

  device = devices[0];
  device.open().then(async () => {
    device.addEventListener('inputreport', handleInputReport);
    document.querySelectorAll('.online').forEach(element => { element.style.opacity = 1.0; });
    await readHandler();
  });
}

async function blinkHandler() {
  await sendReport(packetType.flashLedMsg, []);
}

async function blinkBothHandler() {
  await sendReport(packetType.flashLedMsg, [], true);
}

function getValue(element) {
  if (element.type === 'checkbox')
    return element.checked ? 1 : 0;
  else
    return element.value;
}

function setValue(element, value) {
  element.setAttribute('fetched-value', value);

  if (element.type === 'checkbox')
    element.checked = value;
  else
    element.value = value;
    element.dispatchEvent(new Event('input', { bubbles: true }));
}


function updateElement(key, event) {
  var dataOffset = 4;

  if ({% for base in keymap_field_bases %}(key >= {{ base }} && key < {{ base }} + {{ keymap_chunk_count }}){% if not loop.last %} || {% endif %}{% endfor %}) {
    updateKeymapChunk(key, event);
    return;
  }

  var element = document.querySelector(`[data-key="${key}"]`);

  if (key >= {{ hotkey_field_base }} && key < {{ hotkey_last_field }}) {
    updateHotkeyChunk(key, event);
    return;
  }

  if (!element)
    return;

  const methods = {
    "uint32": event.data.getUint32,
    "uint64": event.data.getUint32, /* Yes, I know. :-| */
    "int32": event.data.getInt32,
    "uint16": event.data.getUint16,
    "uint8": event.data.getUint8,
    "int16": event.data.getInt16,
    "int8": event.data.getInt8
  };

  /* u16 version = major * 1000 + minor + 100; */
  const formatVersion = (value) =>
    `v${Math.floor((value - 100) / 1000)}.${(value - 100) % 1000}`;

  dataType = element.getAttribute('data-type');

  if (dataType in methods) {
    var value = methods[dataType].call(event.data, dataOffset, true);
    setValue(element, value);

    if (element.hasAttribute('data-hex'))
      setValue(element, parseInt(value).toString(16));

    if (element.hasAttribute('data-fw-ver'))
      setValue(element, formatVersion(value));

    /* The other board, as heard from its heartbeats. Zero means none has been
       heard from — unplugged, or the inter-board link is down — and that is an
       absence, not a version: formatVersion would render it as v-1.-100. */
    if (element.hasAttribute('data-peer-fw-ver'))
      setValue(element, value ? formatVersion(value) : 'not detected');

    /* The other board's image checksum. Compare it with the running checksum
       above to tell a synced pair from an unsynced one -- since #91 the two
       versions can agree while the images do not. Zero is an absence here for
       the same reason as the version, and the firmware clears both together. */
    if (element.hasAttribute('data-peer-fw-crc'))
      setValue(element, value ? parseInt(value).toString(16) : 'not detected');

    /* The paired helper's key id (#114). It arrives in three pieces and any
       of them can be the last to land, so the whole line is redrawn each
       time rather than assuming the map's order. */
    if (element.hasAttribute('data-helper-paired') || element.hasAttribute('data-helper-key-lo')
        || element.hasAttribute('data-helper-key-hi'))
      renderPairedHelper();

    if (element.hasAttribute('data-dev-build')) {
      setValue(element, value ? 'development — channel authentication is disabled' : 'release');
      if (value) {
        element.classList.add('dev-warning');
        /* Field 83 arrives after 78 (map order), so the version is rendered by now */
        const ver = document.querySelector('[data-fw-ver]');
        if (ver && ver.value && !ver.value.endsWith('-dev'))
          setValue(ver, `${ver.value}-dev`);
      }
    }
  }
}

const hotkeyBytes = Array.from({length: {{ hotkey_count }}}, () => ({bytes: new Uint8Array(9), mask: 0}));
const namedKeyUsages = {{ named_keys_json }};
for (let i=0; i<26; ++i) namedKeyUsages[String.fromCharCode(97+i)] = 0x04+i;
for (let i=1; i<=9; ++i) namedKeyUsages[String(i)] = 0x1d+i;
namedKeyUsages['0'] = 0x27;
for (let i=1; i<=12; ++i) namedKeyUsages[`f${i}`] = 0x39+i;
const usageNames = Object.fromEntries(Object.entries(namedKeyUsages).map(([name, usage]) => [usage, name]));

const keymapProfileBytes = {{ keymap_profile_size }};
const keymapChunkSize = {{ keymap_chunk_size }};
const keymapChunkCount = {{ keymap_chunk_count }};
const keymapProfiles = {{ keymap_field_bases }}.map(base => ({base, bytes: new Uint8Array(keymapProfileBytes), mask: 0}));

function keymapInput(output, kind) {
  return document.querySelector(`[data-keymap-output="${output}"][data-keymap-kind="${kind}"] .keymap-text`);
}

function updateKeymapChunk(key, event) {
  const output = key >= keymapProfiles[1].base ? 1 : 0;
  const entry = keymapProfiles[output];
  const part = key - entry.base;
  const length = Math.min(keymapChunkSize, keymapProfileBytes - part*keymapChunkSize);
  for (let i=0; i<length; ++i) entry.bytes[part*keymapChunkSize+i] = event.data.getUint8(4+i);
  entry.mask |= 1 << part;
  if (entry.mask !== (1 << keymapChunkCount) - 1) return;

  const overrideCount = entry.bytes[{{ keymap_override_count_offset }}];
  const passthroughCount = entry.bytes[{{ keymap_passthrough_count_offset }}];
  const overrides = keymapInput(output, 'overrides');
  const passthrough = keymapInput(output, 'passthrough');
  const error = overrides.parentElement.querySelector('.keymap-error');
  if (overrideCount > {{ override_capacity }} || passthroughCount > {{ passthrough_capacity }}) {
    error.textContent = 'Stored profile is invalid; Save will replace it.';
    overrides.value = 'invalid';
    passthrough.value = 'invalid';
    return;
  }
  const shownOverrides = [];
  for (let i=0; i<overrideCount; ++i)
    shownOverrides.push(`${usageNames[entry.bytes[2*i]] || `unknown_${entry.bytes[2*i]}`}=${usageNames[entry.bytes[2*i+1]] || `unknown_${entry.bytes[2*i+1]}`}`);
  const shownPassthrough = [];
  for (let i=0; i<passthroughCount; ++i)
    shownPassthrough.push(usageNames[entry.bytes[{{ keymap_passthrough_offset }}+i]] || `unknown_${entry.bytes[{{ keymap_passthrough_offset }}+i]}`);
  overrides.value = shownOverrides.join('\n');
  passthrough.value = shownPassthrough.join(', ');
  overrides.setAttribute('fetched-value', overrides.value);
  passthrough.setAttribute('fetched-value', passthrough.value);
}

function keymapError(line, column, token, message) {
  return {error: {line, column, token, message}};
}

function parseOverrides(input) {
  const parsed = [];
  const lines = input.value.split('\n');
  for (let lineIndex=0; lineIndex<lines.length; ++lineIndex) {
    const line = lines[lineIndex];
    if (!line.trim()) continue;
    const equals = line.indexOf('=');
    if (equals < 0)
      return keymapError(lineIndex+1, line.length+1, line.trim(), 'expected “=”');
    const from = line.slice(0, equals).trim().toLowerCase();
    const to = line.slice(equals+1).trim().toLowerCase();
    if (!from || !to)
      return keymapError(lineIndex+1, equals+2, '', 'expected a key name');
    if (to.includes('='))
      return keymapError(lineIndex+1, equals+2+to.indexOf('='), '=', 'expected one mapping per line');
    if (!(from in namedKeyUsages))
      return keymapError(lineIndex+1, line.toLowerCase().indexOf(from)+1, from, 'unknown key name');
    if (!(to in namedKeyUsages))
      return keymapError(lineIndex+1, line.toLowerCase().indexOf(to, equals+1)+1, to, 'unknown key name');
    if (parsed.length >= {{ override_capacity }})
      return keymapError(lineIndex+1, 1, from, 'profile exceeds the {{ override_capacity }}-override capacity');
    parsed.push([namedKeyUsages[from], namedKeyUsages[to]]);
  }
  return {parsed};
}

function parsePassthrough(input) {
  const parsed = [];
  const lines = input.value.split('\n');
  for (let lineIndex=0; lineIndex<lines.length; ++lineIndex) {
    if (!lines[lineIndex].trim()) continue;
    const rawTokens = lines[lineIndex].split(',');
    for (const raw of rawTokens) {
      const token = raw.trim().toLowerCase();
      if (!token)
        return keymapError(lineIndex+1, Math.max(1, lines[lineIndex].indexOf(raw)+1), '', 'expected a key name');
      if (!(token in namedKeyUsages))
        return keymapError(lineIndex+1, lines[lineIndex].toLowerCase().indexOf(token)+1, token, 'unknown key name');
      if (parsed.length >= {{ passthrough_capacity }})
        return keymapError(lineIndex+1, lines[lineIndex].toLowerCase().indexOf(token)+1, token, 'profile exceeds the {{ passthrough_capacity }}-key capacity');
      parsed.push(namedKeyUsages[token]);
    }
  }
  return {parsed};
}

function showKeymapError(input, result) {
  const shown = input.parentElement.querySelector('.keymap-error');
  shown.textContent = result.error ? `Line ${result.error.line}, column ${result.error.column}, token “${result.error.token}”: ${result.error.message}` : '';
}

async function saveKeymaps() {
  const parsed = [];
  let valid = true;
  for (let output=0; output<2; ++output) {
    const overrides = keymapInput(output, 'overrides');
    const passthrough = keymapInput(output, 'passthrough');
    const overrideResult = parseOverrides(overrides);
    const passthroughResult = parsePassthrough(passthrough);
    showKeymapError(overrides, overrideResult);
    showKeymapError(passthrough, passthroughResult);
    valid = valid && !overrideResult.error && !passthroughResult.error;
    parsed.push({overrides, passthrough, overrideResult, passthroughResult});
  }
  if (!valid) return false;

  for (let output=0; output<2; ++output) {
    const item = parsed[output];
    if (item.overrides.getAttribute('fetched-value') === item.overrides.value &&
        item.passthrough.getAttribute('fetched-value') === item.passthrough.value) continue;
    const bytes = new Uint8Array(keymapProfileBytes);
    item.overrideResult.parsed.forEach((mapping, i) => { bytes[2*i] = mapping[0]; bytes[2*i+1] = mapping[1]; });
    bytes[{{ keymap_override_count_offset }}] = item.overrideResult.parsed.length;
    item.passthroughResult.parsed.forEach((usage, i) => { bytes[{{ keymap_passthrough_offset }}+i] = usage; });
    bytes[{{ keymap_passthrough_count_offset }}] = item.passthroughResult.parsed.length;
    for (let part=0; part<keymapChunkCount; ++part)
      await sendReport(packetType.setValMsg, [keymapProfiles[output].base+part, ...bytes.slice(part*keymapChunkSize, Math.min((part+1)*keymapChunkSize, bytes.length))], true);
    item.overrides.setAttribute('fetched-value', item.overrides.value);
    item.passthrough.setAttribute('fetched-value', item.passthrough.value);
  }
  return true;
}

function updateHotkeyChunk(key, event) {
  const action = Math.floor((key - {{ hotkey_field_base }}) / 2);
  const part = (key - {{ hotkey_field_base }}) % 2;
  const entry = hotkeyBytes[action];
  const length = part ? 3 : 6;
  const offset = part ? 6 : 0;
  for (let i=0; i<length; ++i) entry.bytes[offset+i] = event.data.getUint8(4+i);
  entry.mask |= 1 << part;
  if (entry.mask !== 3) return;
  const input = document.querySelector(`[data-hotkey-action="${action}"] .hotkey-text`);
  const error = input.parentElement.querySelector('.hotkey-error');
  if (entry.bytes[8] !== action || entry.bytes[7] > {{ chord_capacity }}) {
    error.textContent = 'Stored binding is invalid; Save will replace it.';
    input.value = 'invalid';
    return;
  }
  const names = [];
  for (let bit=0; bit<8; ++bit) if (entry.bytes[0] & (1 << bit)) names.push(usageNames[0xe0+bit]);
  for (let i=0; i<entry.bytes[7]; ++i) names.push(usageNames[entry.bytes[1+i]] || `unknown_${entry.bytes[1+i]}`);
  input.value = names.join('+');
  input.setAttribute('fetched-value', input.value);
}

function parseHotkey(input, action) {
  const rawTokens = input.value.split('+');
  const tokens = rawTokens.map(token => token.trim().toLowerCase());
  if (!input.value.trim() || tokens.some(token => !token))
    return {error: {line:1, column:1, token: tokens.find(token => !token) || '', message:'expected a key name'}};
  if (tokens.length > {{ chord_capacity }})
    return {error: {line:1, column:input.value.indexOf(rawTokens[{{ chord_capacity }}])+1, token:tokens[{{ chord_capacity }}], message:'chord exceeds the {{ chord_capacity }}-key capacity'}};
  const usages = [];
  for (const token of tokens) {
    if (!(token in namedKeyUsages))
      return {error:{line:1, column:input.value.toLowerCase().indexOf(token)+1, token, message:'unknown key name'}};
    usages.push(namedKeyUsages[token]);
  }
  const bytes = new Uint8Array(9);
  let keyCount = 0;
  for (const usage of usages) {
    if (usage >= 0xe0 && usage <= 0xe7) bytes[0] |= 1 << (usage - 0xe0);
    else bytes[1 + keyCount++] = usage;
  }
  bytes[7] = keyCount;
  bytes[8] = action;
  return {bytes};
}

async function saveHotkeys() {
  const inputs = [...document.querySelectorAll('.hotkey-text')];
  const parsed = inputs.map((input, action) => parseHotkey(input, action));
  let valid = true;
  parsed.forEach((result, action) => {
    const error = inputs[action].parentElement.querySelector('.hotkey-error');
    error.textContent = result.error ? `Line ${result.error.line}, column ${result.error.column}, token “${result.error.token}”: ${result.error.message}` : '';
    valid = valid && !result.error;
  });
  if (!valid) return false;
  for (let action=0; action<parsed.length; ++action) {
    const input = inputs[action];
    if (input.getAttribute('fetched-value') === input.value) continue;
    const bytes = parsed[action].bytes;
    await sendReport(packetType.setValMsg, [{{ hotkey_field_base }}+2*action, ...bytes.slice(0,6)], true);
    await sendReport(packetType.setValMsg, [{{ hotkey_field_base + 1 }}+2*action, ...bytes.slice(6)], true);
    input.setAttribute('fetched-value', input.value);
  }
  return true;
}

/* The key id the paired helper's hellos carry, as eight bytes of hex — the
   same order and spelling the helper writes into its own log, so the two can
   be compared by eye. Each half arrives as a little-endian u32, so the bytes
   come out lowest first. */
function renderPairedHelper() {
  const shown = document.querySelector('[data-helper-paired]');
  const lo = document.querySelector('[data-helper-key-lo]');
  const hi = document.querySelector('[data-helper-key-hi]');

  if (!shown || !lo || !hi)
    return;

  const paired = shown.getAttribute('fetched-value');

  /* Nothing registered: the halves are zeros and mean nothing, so say so
     rather than showing sixteen noughts as if they were a key. */
  if (paired === null || parseInt(paired) === 0) {
    if (paired !== null)
      shown.value = 'none — press the config chord to pair a helper';
    return;
  }

  if (lo.value === '' || hi.value === '')
    return;

  const bytes = (word) => {
    const n = parseInt(word) >>> 0;
    return [n & 0xff, (n >>> 8) & 0xff, (n >>> 16) & 0xff, (n >>> 24) & 0xff]
      .map(b => b.toString(16).padStart(2, '0')).join('');
  };

  shown.value = bytes(lo.value) + bytes(hi.value);
}

async function readHandler() {
  if (!device || !device.opened)
    await connectHandler();

  await sendReport(packetType.getValAllMsg);
}

async function handleInputReport(event) {
  var data = new Uint8Array(event.data.buffer);
  var key = data[3];

  updateElement(key, event);
}

async function rebootHandler() {
  await sendReport(packetType.rebootMsg);
}

async function enterBootloaderHandler() {
  await sendReport(packetType.firmwareUpgradeMsg, true, true);
}

async function valueChangedHandler(element) {
  var key = element.getAttribute('data-key');
  var dataType = element.getAttribute('data-type');

  var origValue = element.getAttribute('fetched-value');
  var newValue = getValue(element);

  if (origValue != newValue) {
    uintBuffer = packValue(element, key, dataType);

    /* Send to both devices */
    await sendReport(packetType.setValMsg, uintBuffer, true);

    /* Set this as the current value */
    element.setAttribute('fetched-value', newValue);
  }
}

async function saveHandler() {
  const elements = document.querySelectorAll('.api');

  if (!device || !device.opened)
    return;

  if (!await saveHotkeys())
    return;

  if (!await saveKeymaps())
    return;

  for (const element of elements) {
    var origValue = element.getAttribute('fetched-value')

    if (element.hasAttribute('readonly'))
      continue;

    if (origValue != getValue(element))
      await valueChangedHandler(element);
  }
  await sendReport(packetType.saveConfigMsg, [], true);
}

async function wipeConfigHandler() {
  await sendReport(packetType.wipeConfigMsg, [], true);
}

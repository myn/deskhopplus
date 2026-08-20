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
  var element = document.querySelector(`[data-key="${key}"]`);

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

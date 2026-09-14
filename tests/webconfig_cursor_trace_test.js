const fs = require("fs");

const page = fs.readFileSync(process.argv[2], "utf8");
const extract = (pattern, name) => {
  const match = page.match(pattern);
  if (!match) throw new Error(`missing ${name}`);
  return match[0];
};

const events = extract(/const cursorTraceEvents = .*?;/s, "cursor trace events");
const state = extract(/const cursorTraceState = .*?;/s, "cursor trace state layout");
const signed = extract(/function signed16\([\s\S]*?\n}/, "signed16 decoder");
const render = extract(/function renderCursorTraceRecord\([\s\S]*?\n}/, "trace renderer");
const decode = new Function(`${events}\n${state}\n${signed}\n${render}\nreturn renderCursorTraceRecord;`)();

/* pending input, query 9, dx=-2, dy=300, x=1234, y=-50, output B,
   screen 2, TOP, WAITING, relative, OUTPUT transition. */
const packed = 1 | (2 << 1) | (4 << 4) | (1 << 7) | (1 << 10) | (1 << 11);
const bytes = [1, 9, 0xfe, 0xff, 0x2c, 0x01, 0xd2, 0x04, 0xce, 0xff,
               packed & 0xff, packed >> 8];
const actual = decode(3, bytes);
const expected = "3: input q=9 d=(-2,300) p=(1234,-50) out=B screen=2 dir=4 " +
                 "transition=1 phase=1 relative=1";
if (actual !== expected) {
  console.error(`FAIL webconfig_cursor_trace: expected ${expected}; got ${actual}`);
  process.exit(1);
}

/* A USB host mount (#102): dev_addr 1, instance 0, keyboard protocol,
   keyboard seen, polling started. The page names the event, and the number
   must match DH_CURSOR_TRACE_HID_MOUNT in src/core/dh_cursor_trace.h. */
const mountPacked = (1 << 4) | (1 << 11);
const mount = [10, 1, 1, 0, 1, 0, 0, 0, 0, 0, mountPacked & 0xff, mountPacked >> 8];
const mountExpected = "0: hid-mount q=1 d=(1,1) p=(0,0) out=A screen=0 dir=1 " +
                      "transition=1 phase=0 relative=0";
const mountActual = decode(0, mount);
if (mountActual !== mountExpected) {
  console.error(`FAIL webconfig_cursor_trace: expected ${mountExpected}; got ${mountActual}`);
  process.exit(1);
}

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

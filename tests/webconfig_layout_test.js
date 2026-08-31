const fs = require("fs");

const page = fs.readFileSync(process.argv[2], "utf8");
let failures = 0;

function check(condition, message) {
  if (!condition) {
    console.error(`FAIL webconfig_layout: ${message}`);
    failures++;
  }
}

for (const key of [17, 47]) {
  const field = page.match(new RegExp(`<select class="api" data-type="uint8" data-key="${key}"[\\s\\S]*?</select>`));
  check(field, `missing border-direction field ${key}`);
  if (field) {
    for (const value of [1, 2, 4, 5])
      check(field[0].includes(`<option value="${value}">`), `border field ${key} lacks direction ${value}`);
  }
}

for (const key of [98, 99]) {
  const field = page.match(new RegExp(`<select class="api" data-type="uint8" data-key="${key}"[\\s\\S]*?</select>`));
  check(field, `missing chain-direction field ${key}`);
  if (field) {
    for (const value of [1, 2, 4, 5])
      check(field[0].includes(`<option value="${value}">`), `chain field ${key} lacks direction ${value}`);
  }
}

for (const [base, output] of [[140, "A"], [152, "B"]]) {
  for (let segment = 0; segment < 4; segment++) {
    for (let offset = 0; offset < 3; offset++) {
      const key = base + segment * 3 + offset;
      check(page.includes(`data-key="${key}"`),
            `missing output ${output} seam segment field ${key}`);
    }
  }
}

check(page.includes("Seam ranges"), "missing seam-range UI heading");
check(page.includes('id="cursorTrace"'), "missing cursor trace readout");
check(page.includes("getCursorTraceMsg: 33"), "missing cursor trace request packet");
check(page.includes("cursorTraceMsg: 34"), "missing cursor trace response packet");
check(page.includes("readCursorTraceHandler"), "missing cursor trace reader");

process.exit(failures === 0 ? 0 : 1);

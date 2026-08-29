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

process.exit(failures === 0 ? 0 : 1);

// Page glue for the settings window (#225): which section is shown, which
// computer's rows are under the picture, the connection words, the unsaved
// count and the refusal strip. Presentation only: no field is duplicated and
// nothing here talks to the board.

function showSection(id) {
  document.querySelectorAll('section[data-section]').forEach(s => {s.hidden = s.dataset.section !== id;});
  document.querySelectorAll('#sidebar button').forEach(b => {
    if (b.dataset.section === id) b.setAttribute('aria-current', 'page'); else b.removeAttribute('aria-current');
  });
}

document.getElementById('sidebar').addEventListener('click', event => {
  const button = event.target.closest('button[data-section]');
  if (button) showSection(button.dataset.section);
});

// Click a computer in the picture and its rows appear under it, the way the
// OS shows the settings of the monitor you click.
function selectComputer(letter) {
  document.getElementById('layout').dataset.selected = letter;
  document.querySelectorAll('.computer[data-computer]').forEach(c => {c.hidden = c.dataset.computer !== letter;});
  refreshTitles();
}

// "Output A · MacOS": the group title carries the OS the picture's label shows.
function refreshTitles() {
  document.querySelectorAll('[data-os-of]').forEach(span => {
    const os = [...document.querySelectorAll(`.computer[data-computer="${span.dataset.osOf}"] .row`)]
      .find(row => row.querySelector('label')?.textContent === 'Operating System')?.querySelector('select');
    const name = os && os.selectedIndex > 0 ? os.options[os.selectedIndex].text : '';
    span.textContent = name ? ' · ' + name : '';
  });
}

for (const type of ['pointerdown', 'focusin', 'keydown'])
  document.getElementById('layout').addEventListener(type, event => {
    const computer = event.target.closest('[data-output]');
    if (computer) selectComputer(computer.dataset.output);
  });

// Connect flips every board-facing control on; the words in the toolbar say
// so. The markup starts disabled, so nothing runs at load.
function setConnected(on) {
  const words = document.getElementById('connection');
  words.textContent = on ? 'Connected — config mode' : 'Not connected';
  words.dataset.on = on ? '1' : '';
  document.getElementById('fields').disabled = !on;
  document.querySelectorAll('.online').forEach(b => {b.disabled = !on;});
  // Off: the picture goes back to its connect text, so nothing can be dragged.
  if (!on) redrawLayout();
}

// Unsaved: derived, never stored. A field is unsaved when its value differs
// from what the last Read fetched. Save shows the count; a section with one
// or more carries a dot.
function unsavedFields() {
  return [...document.querySelectorAll('.api, .hotkey-text, .keymap-text')].filter(f =>
    !f.readOnly && f.type !== 'hidden' && f.hasAttribute('fetched-value') && f.getAttribute('fetched-value') != getValue(f));
}

function refreshUnsaved() {
  const fields = unsavedFields();
  const badge = document.getElementById('unsaved');
  badge.textContent = fields.length;
  badge.hidden = !fields.length;
  const sections = new Set(fields.map(f => f.closest('section[data-section]').dataset.section));
  document.querySelectorAll('#sidebar button').forEach(b => b.toggleAttribute('data-unsaved', sections.has(b.dataset.section)));
}

// What every edit, report and toolbar action refreshes.
function refresh() {
  refreshUnsaved();
  refreshTitles();
}

document.getElementById('main').addEventListener('input', event => {
  if (event.target.getAttribute('aria-invalid')) {
    event.target.removeAttribute('aria-invalid');
    event.target.closest('.row').querySelector('small').textContent = '';
  }
  refresh();
});
document.getElementById('main').addEventListener('change', refresh);

// Exit reboots the board, which drops the device: the toolbar says so.
navigator.hid?.addEventListener('disconnect', event => {
  if (event.device === device) setConnected(false);
});

// Toolbar actions run, then the counts follow. A refused Save bands the top of
// the window with the fields that need a fix.
document.getElementById('menu-buttons').addEventListener('click', async event => {
  const handler = event.target.closest('button')?.dataset.handler;
  if (!handler) return;
  const result = await window[handler]();
  if (handler === 'saveHandler') showRefusal(result === false);
  // Read replaces the values the last Save refused, so its errors go too.
  if (handler === 'readHandler') clearErrors();
  refresh();
});

function clearErrors() {
  document.querySelectorAll('.hotkey-error, .keymap-error').forEach(e => {e.textContent = '';});
  showRefusal(false);
}

function revealField(field) {
  showSection(field.closest('section[data-section]').dataset.section);
  const computer = field.closest('.computer[data-computer]');
  if (computer) selectComputer(computer.dataset.computer);
  const advanced = field.closest('details');
  if (advanced) advanced.open = true;
  field.scrollIntoView({block: 'center'});
  field.focus();
}

function showRefusal(refused) {
  const strip = document.getElementById('refusal');
  const rows = refused ? [...document.querySelectorAll('.row')].filter(r => r.querySelector('small')?.textContent) : [];
  const control = row => row.querySelector('input, textarea');
  document.querySelectorAll('[aria-invalid]').forEach(f => f.removeAttribute('aria-invalid'));
  rows.forEach(r => control(r).setAttribute('aria-invalid', 'true'));
  strip.hidden = !rows.length;
  if (!rows.length) return;
  const name = row => {
    const section = row.closest('section[data-section]').querySelector('h2').textContent;
    const computer = row.closest('.computer[data-computer]');
    return `${section} › ${computer ? 'Output ' + computer.dataset.computer + ' ' : ''}${row.querySelector('label').textContent}`;
  };
  const esc = text => text.replace(/[&<>]/g, c => ({'&':'&amp;', '<':'&lt;', '>':'&gt;'}[c]));
  strip.lastElementChild.innerHTML = `<strong>Save refused.</strong> ${rows.length} field${rows.length > 1 ? 's need' : ' needs'} a fix; nothing was sent.<ul>` +
    rows.map(row => `<li><a href="#">${esc(name(row))}</a>: ${esc(row.querySelector('small').textContent)}</li>`).join('') + '</ul>';
  strip.querySelectorAll('a').forEach((a, i) => a.addEventListener('click', e => {e.preventDefault(); revealField(control(rows[i]));}));
  revealField(control(rows[0]));
}

// Service: a button that wipes or reboots takes two clicks. The first arms it
// and says so; the second acts; a click anywhere else disarms it.
window.addEventListener('click', event => {
  const armed = document.querySelector('#service-buttons button[data-armed]');
  if (armed && !armed.contains(event.target)) {armed.textContent = armed.dataset.armed; delete armed.dataset.armed;}
});
document.getElementById('service-buttons').addEventListener('click', event => {
  const button = event.target.closest('button[data-handler]');
  if (!button) return;
  if (button.dataset.arm && !button.dataset.armed) {
    button.dataset.armed = button.textContent;
    button.textContent = button.dataset.arm;
    return;
  }
  if (button.dataset.armed) {button.textContent = button.dataset.armed; delete button.dataset.armed;}
  window[button.dataset.handler]();
});

const fs = require('fs');
const os = require('os');
const path = require('path');
const {pathToFileURL} = require('url');
const {spawn, spawnSync} = require('child_process');
const candidates = [process.env.CHROME, 'google-chrome', 'chromium', 'chromium-browser',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  process.env.PROGRAMFILES && path.join(process.env.PROGRAMFILES, 'Google/Chrome/Application/chrome.exe')].filter(Boolean);
// #239: no pipes, and SIGKILL. With a pipe, spawnSync waits for every process
// holding it, and a Chrome helper can hold it past the timeout, which hung CI.
const chrome = candidates.find(binary => spawnSync(binary, ['--version'], {timeout:5000, killSignal:'SIGKILL', stdio:'ignore'}).status === 0);
if (!chrome) {console.log('SKIP: Chrome is not installed'); process.exit(77);}
console.log('webconfig_chrome_test: using '+chrome);
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'deskhopplus-layout-'));
const file = path.join(dir, 'layout.htm');
let html = fs.readFileSync(process.argv[2], 'utf8');
html = html.replace('<head>', `<head><script>window.addEventListener('error', () => document.documentElement.dataset.error = 'true');</script>`);
html = html.replace('</body>', `<script>
(async () => {
  const fields = {11:2,41:2,14:0,15:32767,44:0,45:32767,16:2,46:3,17:5,47:4,98:1,99:2,
    140:2,141:0,142:65535,143:1,144:0,145:65535,
    152:1,153:0,154:65535,155:2,156:0,157:65535};
  for (let key=140; key<164; key++) fields[key] ??= 0;
  const tick = () => new Promise(resolve => setTimeout(resolve, 0));
  let sets = 0, saves = 0, wipes = 0;
  device = {opened:true, async open() {this.opened = true;}, addEventListener() {}, async sendReport(id, report) {
    if (report[2] === packetType.setValMsg) sets++;
    if (report[2] === packetType.saveConfigMsg) saves++;
    if (report[2] === packetType.wipeConfigMsg) wipes++;
    if (report[2] !== packetType.getValAllMsg) return;
    for (const [key,value] of Object.entries(fields)) {
      const data = new DataView(new ArrayBuffer(12));
      data.setUint8(2, packetType.getValMsg); data.setUint8(3, Number(key));
      data.setUint32(4, value, true);
      await handleInputReport({data});
    }
  }};
  if (document.querySelectorAll('summary').length !== 1) throw Error('Expected one shared Advanced disclosure');
  // #233: the name in title case, with the helper's glyph beside it.
  const h1 = document.querySelector('nav[aria-label="Sections"] h1');
  if (document.title !== 'DeskHopPlus' || h1.textContent.trim() !== 'DeskHopPlus' ||
      h1.querySelector('svg[aria-hidden="true"]').querySelectorAll('rect').length !== 3)
    throw Error('Sidebar title or glyph: '+document.title+' / '+h1.innerHTML);
  // The tab icon, painted and sampled inside the left screen. A tab strip
  // never lends the icon the page's colours, so the icon carries its own
  // dark-mode rule and the --force-dark-mode pass below is what catches its
  // loss. A mis-encoded data URI never loads at all.
  const icon = await new Promise((resolve, reject) => {
    const probe = new Image();
    probe.onload = () => resolve(probe);
    probe.onerror = () => reject(Error('The tab icon data URI does not load'));
    probe.src = document.querySelector('link[rel="icon"]').href;
  });
  const paint = Object.assign(document.createElement('canvas'), {width:16, height:16}).getContext('2d');
  paint.drawImage(icon, 0, 0, 16, 16);
  const ink = paint.getImageData(4, 8, 1, 1).data;
  const onDarkStrip = matchMedia('(prefers-color-scheme: dark)').matches;
  if (ink[3] !== 255 || (onDarkStrip ? ink[0] < 200 : ink[0] > 60))
    throw Error('Tab icon ink does not suit the tab strip (dark: '+onDarkStrip+'): '+[...ink]);
  // #225: one settings window. Six sections in the sidebar, one shown at a time.
  const sidebar = [...document.querySelectorAll('nav[aria-label="Sections"] button')];
  const shownSections = () => [...document.querySelectorAll('section[data-section]')].filter(s => s.checkVisibility()).map(s => s.dataset.section).join();
  if (sidebar.map(b => b.textContent.trim()).join() !== 'Desk,Keyboard & Mouse,Hotkeys,Clipboard,Status,Service')
    throw Error('Sidebar sections: '+sidebar.map(b => b.textContent.trim()));
  if (shownSections() !== 'desk') throw Error('Sections shown at load: '+shownSections());
  sidebar[2].click();
  if (shownSections() !== 'hotkeys' || sidebar[2].getAttribute('aria-current') !== 'page' || sidebar[0].hasAttribute('aria-current'))
    throw Error('Sidebar click did not switch to Hotkeys alone: '+shownSections());
  sidebar[0].click();
  if (shownSections() !== 'desk') throw Error('Sidebar click did not return to Desk');
  // Before Connect the picture says so and the fields are disabled.
  if (!/Connect/.test(document.getElementById('layout').textContent) || !document.querySelector('[data-key="16"]').matches(':disabled') ||
      !document.querySelector('[data-handler="saveHandler"]').disabled || document.getElementById('connection').textContent !== 'Not connected')
    throw Error('Not-connected state is not shown');
  setConnected(true);
  if (document.querySelector('[data-key="16"]').matches(':disabled') || document.querySelector('[data-handler="saveHandler"]').disabled ||
      !/Connected/.test(document.getElementById('connection').textContent))
    throw Error('Connected state is not shown');
  await readHandler();
  if (document.getElementById('refusal').checkVisibility() || document.getElementById('warning').checkVisibility())
    throw Error('A strip is shown with nothing to say');
  if (document.querySelector('.computer[data-computer="A"] h3').textContent !== 'Output A · MacOS')
    throw Error('Read did not put the OS in the group title');
  // #227: every label starts its own box. The key mapping rows are not screensaver rows.
  const keymapBox = document.querySelector('.computer[data-computer="A"] .keymap-field').closest('.group');
  const boxTitle = box => box.previousElementSibling && box.previousElementSibling.matches('h3.sub') ? box.previousElementSibling.textContent : '';
  const byKey = key => document.querySelector('[data-key="'+key+'"]');
  if (boxTitle(keymapBox) !== 'Key mapping' || !keymapBox.contains(byKey(23)) ||
      boxTitle(byKey(19).closest('.group')) !== 'Screensaver' || keymapBox.contains(byKey(19)))
    throw Error('Key mapping rows are not in their own titled box: "'+boxTitle(keymapBox)+'"');
  const layout = document.getElementById('layout');
  if (layout.querySelectorAll('.layout-monitor').length !== 4 ||
      layout.querySelectorAll('[data-segment]').length !== 2 ||
      Number(layout.querySelector('.layout-A .layout-main').getAttribute('x')) <=
        Number(layout.querySelector('.layout-A .layout-monitor:not(.layout-main)').getAttribute('x')))
    throw Error('Read did not draw the maintainer desk');
  const before = layout.innerHTML;
  const chain = document.querySelector('[data-key="98"]');
  chain.value = '2'; chain.dispatchEvent(new Event('change', {bubbles:true}));
  if (layout.innerHTML === before) throw Error('Advanced change did not redraw');
  layout.querySelector('a').click();
  if (!document.getElementById('advanced').open)
    throw Error('Open Advanced did not open the shared panel');
  chain.value = '1'; chain.dispatchEvent(new Event('change', {bubbles:true}));
  const failures = [];
  const check = (ok, message) => {if (!ok) failures.push(message);};
  const canvas = document.createElement('canvas').getContext('2d');
  const panels = document.querySelectorAll('.computer[data-computer]');
  check(panels.length === 4, 'Desk and Advanced must each hold both computers');
  document.getElementById('advanced').open = true;
  for (const panel of panels) {
    selectComputer(panel.dataset.computer);
    const column = panel.getBoundingClientRect();
    const pane = panel.closest('section').getBoundingClientRect();
    check(column.left >= pane.left && column.right <= pane.right, 'computer panel escapes its section');
    for (const input of panel.querySelectorAll('input, select')) {
      const rect = input.getBoundingClientRect(), style = getComputedStyle(input);
      const key = input.dataset.key;
      check(rect.left >= column.left && rect.right <= column.right, 'field '+key+' escapes output column');
      check(input.scrollWidth <= input.clientWidth, 'field '+key+' overflows its border');
      canvas.font = style.font;
      const texts = input.tagName === 'SELECT' ? [...input.options].map(option => option.text) :
        [input.value, ...(input.type === 'number' ? [input.min, input.max] : [])];
      const text = texts.reduce((longest, value) => canvas.measureText(value).width > canvas.measureText(longest).width ? value : longest, '');
      const required = canvas.measureText(text).width + parseFloat(style.paddingLeft) +
        parseFloat(style.paddingRight) + (input.type === 'number' ? 18 : 0);
      check(required <= input.clientWidth, 'field '+key+' clips value '+text);
      check(parseFloat(style.fontSize) + parseFloat(style.paddingTop) + parseFloat(style.paddingBottom) <= input.clientHeight,
        'field '+key+' clips text vertically');
    }
    for (const row of panel.querySelectorAll('.seam-range')) {
      const inputs = [...row.querySelectorAll('input')].map(e => e.getBoundingClientRect());
      for (let n=1; n<inputs.length; n++)
        check(inputs[n].left-inputs[n-1].right >= 4, 'segment inputs touch or overlap');
    }
  }
  // Include the painted stroke extents, not just SVG geometry bounds.
  for (const band of layout.querySelectorAll('.layout-band')) {
    const line = band.getBBox(), halfBand = parseFloat(getComputedStyle(band).strokeWidth)/2;
    for (const monitor of layout.querySelectorAll('.layout-monitor')) {
      const box = monitor.getBBox(), halfBorder = parseFloat(getComputedStyle(monitor).strokeWidth)/2;
      const gap = Math.max(box.x-line.x-line.width, line.x-box.x-box.width,
        box.y-line.y-line.height, line.y-box.y-box.height)-halfBand-halfBorder;
      check(gap >= 4, 'monitor border touches seam band');
    }
  }
  if (failures.length) throw Error(failures.join('; '));
  selectComputer('A');
  document.querySelector('#advanced summary').click();
  for (const key of [98,99])
    if (document.getElementById('advanced').open || document.querySelector('[data-key="'+key+'"]').checkVisibility())
      throw Error('Advanced disclosure did not collapse');
  document.querySelector('#advanced summary').click();
  if (!document.getElementById('advanced').open || !document.querySelector('[data-key="98"]').checkVisibility() ||
      document.querySelector('[data-key="99"]').checkVisibility())
    throw Error('Advanced disclosure did not reveal the selected computer alone');
  // Drag B's label from below A to A's right: a valid drop fills in both
  // borders and the segments but sends nothing until Save; a gap drop shows
  // its reason and changes nothing; Read puts the board's values back.
  const field = key => document.querySelector('[data-key="'+key+'"]');
  const written = () => [17,47,140,143,152,155].map(key => field(key).value).join();
  const pending = () => [...document.querySelectorAll('.api')].filter(e => (e.dataset.key in fields) && e.getAttribute('fetched-value') != getValue(e)).map(e => e.dataset.key);
  const pointer = (type, target, at, init = {}) => target.dispatchEvent(new PointerEvent(type,
    {bubbles:true, isPrimary:true, clientX:at[0], clientY:at[1], ...init}));
  const boxOf = (letter, n) => layout.querySelector('.layout-box[data-output="'+letter+'"][data-monitor="'+n+'"]');
  function press(letter, dx, dy, init, monitor) {
    const handle = monitor ? boxOf(letter, monitor) : layout.querySelector('[data-output="'+letter+'"] .layout-handle');
    const scale = handle.ownerSVGElement.getScreenCTM().a, box = handle.getBoundingClientRect();
    const from = [box.left+box.width/2, box.top+box.height/2], to = [from[0]+dx*100*scale, from[1]+dy*100*scale];
    pointer('pointerdown', handle, from, init);
    pointer('pointermove', window, to);
    const preview = (monitor ? handle : handle.parentNode).getAttribute('transform');
    pointer('pointerup', window, to);
    return preview;
  }
  if (sets !== 0 || pending().length) throw Error('Read left a value unsent or pending: '+pending());
  // #225: click a computer in the picture and its rows appear under it; the other's hide.
  const visibleKeys = () => [16,46,98,99].filter(key => field(key).checkVisibility()).join();
  if (visibleKeys() !== '16,98' || layout.dataset.selected !== 'A') throw Error('A is not the computer shown at first: '+visibleKeys());
  const title = letter => document.querySelector('.computer[data-computer="'+letter+'"] h3').textContent;
  if (title('A') !== 'Output A · MacOS' || title('B') !== 'Output B · Windows') throw Error('Group titles lack the OS: '+title('A')+' / '+title('B'));
  press('B', 0, 0);
  if (visibleKeys() !== '46,99' || layout.dataset.selected !== 'B' || sets !== 0 || pending().length)
    throw Error('Clicking B did not show B alone, or moved something: '+visibleKeys());
  boxOf('A', 2).dispatchEvent(new KeyboardEvent('keydown', {key:'ArrowUp', bubbles:true, cancelable:true}));
  if (visibleKeys() !== '16,98' || layout.dataset.selected !== 'A' || document.activeElement !== boxOf('A', 2))
    throw Error('An arrow key on an A box did not select A and keep focus: '+visibleKeys());
  await readHandler();
  if (visibleKeys() !== '16,98' || pending().length) throw Error('Read changed the selected computer: '+visibleKeys());
  // Drag feedback is live: before release it says whether the snapped drop can apply.
  const feedbackBox = boxOf('A', 1), feedbackRect = feedbackBox.getBoundingClientRect();
  const feedbackScale = feedbackBox.ownerSVGElement.getScreenCTM().a;
  const feedbackFrom = [feedbackRect.left+feedbackRect.width/2, feedbackRect.top+feedbackRect.height/2];
  pointer('pointerdown', feedbackBox, feedbackFrom, {pointerId:20});
  pointer('pointermove', window, [feedbackFrom[0], feedbackFrom[1]-100*feedbackScale], {pointerId:20});
  if (document.getElementById('layout-status').textContent !== 'Not moved: drop Main on monitor 2 to flip it, or drag the label to move the whole computer.')
    throw Error('Invalid Main drop had no useful live feedback');
  pointer('pointercancel', window, feedbackFrom, {pointerId:20});
  pointer('pointerdown', feedbackBox, feedbackFrom, {pointerId:21});
  pointer('pointermove', window, [feedbackFrom[0]-100*feedbackScale, feedbackFrom[1]], {pointerId:21});
  if (document.getElementById('layout-status').textContent !== 'Release to apply.' ||
      !document.getElementById('layout-status').classList.contains('layout-ok'))
    throw Error('Valid Main drop had no useful live feedback');
  pointer('pointercancel', window, feedbackFrom, {pointerId:21});
  // Once a primary drag starts, another finger cannot move or finish it.
  let dragBox = boxOf('A', 2), dragRect = dragBox.getBoundingClientRect(), dragScale = dragBox.ownerSVGElement.getScreenCTM().a;
  let dragFrom = [dragRect.left+dragRect.width/2, dragRect.top+dragRect.height/2];
  pointer('pointerdown', dragBox, dragFrom, {pointerId:1});
  pointer('pointermove', window, [dragFrom[0], dragFrom[1]-100*dragScale], {pointerId:2, isPrimary:false});
  pointer('pointerup', window, [dragFrom[0], dragFrom[1]-100*dragScale], {pointerId:2, isPrimary:false});
  if (pending().length || document.getElementById('layout-status').textContent)
    throw Error('A second pointer completed the primary drag');
  pointer('pointerup', window, dragFrom, {pointerId:1});
  // A canceled drag removes its preview and listeners without applying it.
  dragBox = boxOf('A', 2); dragRect = dragBox.getBoundingClientRect(); dragScale = dragBox.ownerSVGElement.getScreenCTM().a;
  dragFrom = [dragRect.left+dragRect.width/2, dragRect.top+dragRect.height/2];
  const dragTo = [dragFrom[0], dragFrom[1]-100*dragScale];
  pointer('pointerdown', dragBox, dragFrom, {pointerId:3});
  pointer('pointermove', window, dragTo, {pointerId:3});
  if (!dragBox.getAttribute('transform')) throw Error('Canceled-drag check did not create a preview');
  pointer('pointercancel', window, dragTo, {pointerId:3});
  pointer('pointerup', window, dragTo, {pointerId:3});
  if (dragBox.getAttribute('transform') || pending().length)
    throw Error('A canceled drag kept its preview or applied its gesture');
  if (press('B', 2.1, -0.9) !== 'translate(200 -100)') throw Error('Preview did not snap to the half-box grid');
  if (written() !== '2,1,1,0,1,0' || layout.querySelectorAll('[data-segment]').length !== 1)
    throw Error('Drop to the right did not fill in borders and segments: '+written());
  if (sets !== 0 || pending().sort().join() !== '140,143,145,155,157,17,47')
    throw Error('Drop must wait for Save: sent '+sets+', pending '+pending());
  // #227: a drop counts as unsaved on the Save button, like a typed edit.
  await tick();
  if (document.getElementById('unsaved').hidden || document.getElementById('unsaved').textContent !== String(pending().length))
    throw Error('A drop did not show the unsaved count: "'+document.getElementById('unsaved').textContent+'" for '+pending().length);
  // A horizontal layout leaves a visible snapped row above and below for stacking.
  const horizontalSvg = layout.querySelector('svg').getBoundingClientRect();
  const horizontalLabel = layout.querySelector('.layout-B .layout-handle');
  const labelRect = horizontalLabel.getBoundingClientRect();
  const labelY = labelRect.top+labelRect.height/2, rowHeight = 100*horizontalLabel.ownerSVGElement.getScreenCTM().a;
  if (labelY-rowHeight < horizontalSvg.top || labelY+rowHeight > horizontalSvg.bottom)
    throw Error('Horizontal Layout has no in-canvas row above and below');
  press('B', 0, -0.5);
  const offsetSvg = layout.querySelector('svg').getBoundingClientRect();
  for (const handle of layout.querySelectorAll('.layout-handle')) {
    const rect = handle.getBoundingClientRect(), y = rect.top+rect.height/2;
    const row = 100*handle.ownerSVGElement.getScreenCTM().a;
    if (y-row < offsetSvg.top || y+row > offsetSvg.bottom)
      throw Error('Offset horizontal Layout has no in-canvas row above and below');
  }
  press('B', 0, 0.5);
  if (press('B', -2, 1) !== 'translate(-200 100)' || written() !== '5,4,2,1,1,2')
    throw Error('In-canvas downward label drag did not stack the computers: '+written());
  press('B', 2, -1);
  if (press('B', -2, -1) !== 'translate(-200 -100)' || written() !== '4,5,2,1,1,2')
    throw Error('In-canvas upward label drag did not stack the computers: '+written());
  press('B', 2, 1);
  press('B', 1, 0);
  const reason = document.getElementById('layout-status').textContent;
  if (!/^Not moved: .*gap/.test(reason) || written() !== '2,1,1,0,1,0' || layout.querySelectorAll('[data-segment]').length !== 1)
    throw Error('Gap drop changed something: '+reason);
  // A right button or a second finger never starts a drag; the next press clears the reason.
  for (const init of [{button:2}, {isPrimary:false}])
    if (press('B', 1, 0, init)) throw Error('A non-primary press started a drag');
  if (document.getElementById('layout-status').textContent !== reason) throw Error('A non-primary press cleared the reason');
  press('B', 0, 0);
  if (document.getElementById('layout-status').textContent) throw Error('A new drag did not clear the reason');
  // The label bar holds its text, for a wide block and for a one-box block.
  // Names each label that overflows, with its measured box, so a failure says why.
  const labelOverflows = () => [...layout.querySelectorAll('.layout-handle')].map(handle => {
    const bar = handle.querySelector('rect').getBBox(), text = handle.querySelector('text').getBBox();
    return text.x >= bar.x+2 && text.x+text.width <= bar.x+bar.width-2 ? '' :
      '"'+handle.textContent+'" text '+text.x.toFixed(1)+'+'+text.width.toFixed(1)+' bar '+bar.x+'+'+bar.width+' font '+getComputedStyle(handle.querySelector('text')).fontFamily;
  }).filter(Boolean).join('; ');
  let overflows = labelOverflows();
  if (overflows) throw Error('Label bar text overflows a wide bar: '+overflows);
  const count = field(41);
  count.value = '1'; count.dispatchEvent(new Event('change', {bubbles:true}));
  overflows = labelOverflows();
  if (overflows) throw Error('Label bar text overflows a one-box bar: '+overflows);
  // Box gestures (#213) go through the same path: fields fill in, nothing is sent.
  await readHandler();
  const chainA = () => field(98).value, countA = () => field(11).value;
  if (press('A', -1.4, 0.3, {}, 1) !== 'translate(-100 0)') throw Error('Box preview did not snap to whole boxes');
  if (chainA() !== '2' || written() !== '5,4,1,2,1,2' || sets !== 0 || pending().sort().join() !== '140,143,98')
    throw Error('Dragging Main onto box 2 did not flip A: chain '+chainA()+' fields '+written()+' pending '+pending());
  press('A', 0, 0, {}, 2);
  if (document.activeElement !== boxOf('A', 2) || document.getElementById('layout-status').textContent)
    throw Error('A click on a box did not focus it quietly');
  boxOf('A', 1).focus();
  boxOf('A', 1).dispatchEvent(new KeyboardEvent('keydown', {key:'ArrowRight', bubbles:true, cancelable:true}));
  if (chainA() !== '1' || written() !== '5,4,2,1,1,2' || document.activeElement !== boxOf('A', 1))
    throw Error('ArrowRight on Main did not flip A back and keep focus: chain '+chainA()+' fields '+written());
  boxOf('A', 2).dispatchEvent(new KeyboardEvent('keydown', {key:'ArrowUp', bubbles:true, cancelable:true}));
  if (chainA() !== '4' || countA() !== '2' || layout.querySelectorAll('[data-segment]').length !== 1)
    throw Error('ArrowUp on box 2 did not turn A into a column: chain '+chainA());
  layout.querySelector('button[aria-label="Add a monitor to Output A"]').click();
  if (countA() !== '3' || layout.querySelectorAll('[data-output="A"] .layout-box').length !== 3)
    throw Error('+ did not add a third box to A: count '+countA());
  layout.querySelector('button[aria-label="Remove a monitor from Output A"]').click();
  layout.querySelector('button[aria-label="Remove a monitor from Output A"]').click();
  layout.querySelector('button[aria-label="Remove a monitor from Output A"]').click();
  if (countA() !== '1' || !/^Not removed:/.test(document.getElementById('layout-status').textContent))
    throw Error('- did not stop at Main: count '+countA());
  if (sets !== 0) throw Error('A box gesture sent '+sets+' values before Save');
  // Read throws the unsaved gestures away.
  await readHandler();
  if (written() !== '5,4,2,1,1,2' || count.value !== '2' || countA() !== '2' || chainA() !== '1' || sets !== 0 || pending().length ||
      layout.querySelectorAll('[data-segment]').length !== 2)
    throw Error('Read did not restore the board values: '+written()+' pending '+pending());
  // #227: the widest desk the board allows, seven a side (12 boxes across on this desk),
  // keeps a readable scale and scrolls in the well instead of shrinking to a strip.
  for (let n = 0; n < 5; n++) for (const letter of ['A', 'B'])
    layout.querySelector('button[aria-label="Add a monitor to Output '+letter+'"]').click();
  await tick();
  const wideSvg = layout.querySelector('svg'), wideUnits = wideSvg.viewBox.baseVal.width;
  if (layout.querySelectorAll('.layout-box').length !== 14 || wideUnits < 1200 ||
      wideSvg.getBoundingClientRect().width < wideUnits*0.9-1 || layout.scrollWidth <= layout.clientWidth)
    throw Error('A seven-monitor desk shrank instead of scrolling: '+wideSvg.getBoundingClientRect().width+' of '+wideUnits);
  await readHandler();
  // #225: the unsaved count follows edits and Read.
  const unsaved = document.getElementById('unsaved'), saveButton = document.querySelector('[data-handler="saveHandler"]');
  if (!unsaved.hidden) throw Error('Unsaved count shown with nothing changed');
  const os = field(46);
  os.value = '2'; os.dispatchEvent(new Event('change', {bubbles:true}));
  if (unsaved.hidden || unsaved.textContent !== '1' || !sidebar[0].hasAttribute('data-unsaved'))
    throw Error('An edit did not show one unsaved field: '+unsaved.textContent);
  await readHandler();
  if (!unsaved.hidden || sidebar[0].hasAttribute('data-unsaved')) throw Error('Read did not clear the unsaved count');
  // A bad hotkey: Save sends nothing, the strip names the field and lands on it; a fix clears it.
  document.querySelectorAll('.hotkey-text').forEach(h => {h.value = 'lctrl+rshift+c+o'; h.setAttribute('fetched-value', h.value);});
  document.querySelectorAll('.keymap-text').forEach(k => k.setAttribute('fetched-value', ''));
  const hotkey = document.querySelector('.hotkey-text'), strip = document.getElementById('refusal');
  hotkey.value = 'lctrl+nope'; hotkey.dispatchEvent(new Event('input', {bubbles:true}));
  if (unsaved.textContent !== '1' || !sidebar[2].hasAttribute('data-unsaved')) throw Error('A hotkey edit did not count as unsaved');
  saveButton.click(); await tick();
  if (sets !== 0 || saves !== 0 || strip.hidden || !/1 field needs a fix/.test(strip.textContent) ||
      !/Hotkeys › output_toggle/.test(strip.textContent) || !/unknown key name/.test(strip.textContent) ||
      hotkey.getAttribute('aria-invalid') !== 'true' || shownSections() !== 'hotkeys' || document.activeElement !== hotkey)
    throw Error('Refused Save did not band the window and land on the field: sent '+sets+'/'+saves+' strip "'+strip.textContent+'" shown '+shownSections());
  // A strip entry is a link to its field, from any section.
  sidebar[0].click();
  strip.querySelector('a').click();
  if (shownSections() !== 'hotkeys' || document.activeElement !== hotkey) throw Error('Strip entry did not land on its field');
  hotkey.value = 'lctrl+rshift+c+o'; hotkey.dispatchEvent(new Event('input', {bubbles:true}));
  if (hotkey.hasAttribute('aria-invalid') || hotkey.parentElement.querySelector('.hotkey-error').textContent || strip.hidden)
    throw Error('Correcting the field did not clear its own error, or cleared the strip early');
  // A changed chord plus a bad keymap: every field is checked before any is sent.
  const other = document.querySelectorAll('.hotkey-text')[1], overrides = document.querySelector('.keymap-text');
  other.value = 'lctrl+rshift+x'; other.dispatchEvent(new Event('input', {bubbles:true}));
  overrides.value = 'capslock=nope'; overrides.dispatchEvent(new Event('input', {bubbles:true}));
  saveButton.click(); await tick();
  if (sets !== 0 || saves !== 0 || strip.hidden || !/1 field needs a fix/.test(strip.textContent) ||
      !/Desk › Output A Key overrides/.test(strip.textContent) || shownSections() !== 'desk' || !overrides.checkVisibility())
    throw Error('A refused Save sent the changed chord, or the strip missed the keymap: sent '+sets+' strip "'+strip.textContent+'"');
  other.value = other.getAttribute('fetched-value'); other.dispatchEvent(new Event('input', {bubbles:true}));
  // Read replaces the refused values, so the strip and the error lines go with them.
  document.querySelector('[data-handler="readHandler"]').click(); await tick();
  if (!strip.hidden || overrides.hasAttribute('aria-invalid') || overrides.parentElement.querySelector('.keymap-error').textContent)
    throw Error('Read left the last refusal on the page');
  overrides.value = ''; overrides.dispatchEvent(new Event('input', {bubbles:true}));
  saveButton.click(); await tick();
  // The fake board never sent some fields, so this Save fills them in; only the save message is counted.
  if (!strip.hidden || saves !== 1 || !unsaved.hidden) throw Error('A passing Save did not clear the strip and send the save message: '+saves+' strip '+strip.hidden+' unsaved '+unsaved.hidden);
  // #227: two clicks on Save send one save message; the toolbar is disabled while an action runs.
  saveButton.click(); saveButton.click(); await tick();
  if (saves !== 2) throw Error('Two clicks on Save sent '+(saves-1)+' save messages');
  // A board lost mid-action: the page says so and keeps the user's values.
  const send = device.sendReport;
  device.sendReport = async () => {throw new DOMException('The device is not opened.', 'InvalidStateError');};
  os.value = '2'; os.dispatchEvent(new Event('change', {bubbles:true}));
  saveButton.click(); await tick();
  if (document.getElementById('connection').textContent !== 'Not connected — The device is not opened.' ||
      !saveButton.disabled || os.value !== '2')
    throw Error('A lost board was not reported: "'+document.getElementById('connection').textContent+'"');
  if (document.querySelector('[data-handler="connectHandler"]').disabled) throw Error('Connect is disabled after a lost board');
  device.sendReport = send;
  // A cancelled WebHID prompt leaves the page not connected and says why.
  navigator.hid.requestDevice = async () => [];
  device.opened = false;
  document.querySelector('[data-handler="connectHandler"]').click(); await tick();
  if (document.getElementById('connection').textContent !== 'Not connected — No board chosen.' || !saveButton.disabled)
    throw Error('A cancelled Connect was not reported: "'+document.getElementById('connection').textContent+'"');
  // A good Connect from the toolbar turns the page on and leaves Read, Save and Exit enabled.
  navigator.hid.requestDevice = async () => [device];
  document.querySelector('[data-handler="connectHandler"]').click(); await tick();
  if (document.getElementById('connection').textContent !== 'Connected — config mode' || saveButton.disabled ||
      document.querySelector('[data-handler="readHandler"]').disabled || document.querySelector('[data-handler="rebootHandler"]').disabled)
    throw Error('Connect left the toolbar off: "'+document.getElementById('connection').textContent+'" save '+saveButton.disabled);
  // A disconnect event after a reported failure keeps the reason on the page.
  device.sendReport = async () => {throw new DOMException('The device is not opened.', 'InvalidStateError');};
  saveButton.click(); await tick();
  device.opened = false; navigator.hid.dispatchEvent(Object.assign(new Event('disconnect'), {device}));
  if (!/^Not connected — /.test(document.getElementById('connection').textContent)) throw Error('The disconnect event erased the reason');
  device.sendReport = send; device.opened = true; setConnected(true); await readHandler();
  // Service: Wipe Config takes two clicks; any other click disarms it.
  sidebar[5].click();
  const wipe = document.querySelector('[data-handler="wipeConfigHandler"]');
  wipe.click(); await tick();
  if (wipes !== 0 || wipe.textContent !== 'Click again to wipe') throw Error('One click on Wipe Config did not arm it: '+wipes+' "'+wipe.textContent+'"');
  document.querySelector('[data-handler="blinkHandler"]').click();
  if (wipe.hasAttribute('data-armed') || wipe.textContent !== 'Wipe Config') throw Error('Another click did not disarm Wipe Config');
  wipe.click(); sidebar[0].click(); sidebar[5].click();
  if (wipe.hasAttribute('data-armed') || wipes !== 0) throw Error('A click outside Service did not disarm Wipe Config');
  wipe.click(); wipe.click(); await tick();
  if (wipes !== 1 || wipe.textContent !== 'Wipe Config') throw Error('Two clicks did not wipe once: '+wipes);
  sidebar[0].click();
  // An invalid timer on the other computer must stop the whole Save and reveal its row.
  const timerB = document.querySelector('[data-key="51"]');
  timerB.value = '4294.967295';
  if (!timerB.checkValidity()) throw Error('Maximum representable timer is invalid');
  timerB.value = '4294.967296';
  if (timerB.checkValidity()) throw Error('Timer above the wire limit was accepted');
  selectComputer('A');
  const beforeTimerSets = sets, beforeTimerSaves = saves;
  saveButton.click(); await tick();
  if (sets !== beforeTimerSets || saves !== beforeTimerSaves || strip.hidden ||
      !/Output B Idle Time/.test(strip.textContent) || !timerB.checkVisibility() || document.activeElement !== timerB)
    throw Error('Invalid output B timer did not refuse Save and reveal its field');
  timerB.value = ''; timerB.dispatchEvent(new Event('input', {bubbles:true}));
  // Losing the board: the toolbar says so, the picture goes back to its connect text, nothing can be dragged.
  device.opened = false; setConnected(false);
  if (document.getElementById('connection').textContent !== 'Not connected' || layout.querySelector('svg') ||
      !/Connect/.test(layout.textContent) || !saveButton.disabled)
    throw Error('Disconnect did not reset the page');
  document.body.dataset.viewport = innerWidth;
  document.body.dataset.layoutTest = 'passed';
})().catch(error => {document.documentElement.dataset.error = String(error);});
</script></body>`);
fs.writeFileSync(file, html);
// All widths share one deadline inside ctest's TIMEOUT 40, so a stall reports
// its width and stage before ctest kills the run.
const deadline = Date.now()+35000;
// A killed Node leaves a pipe-mode Chrome running, so a stop takes it too.
let running;
for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, () => {running?.kill('SIGKILL'); process.exit(1);});
// Opens the page in headless Chrome at one width and returns its DOM once the
// injected script has finished. Node polls over the DevTools pipe in real time.
// #239: --dump-dom with --virtual-time-budget sometimes never dumped a page
// whose script had already finished, about 1 launch in 100 under load.
function pageDom(width, dark) {
  const url = pathToFileURL(file).href, label = width+'px'+(dark ? ' dark' : '');
  return new Promise((resolve, reject) => {
    const child = spawn(chrome, ['--headless', '--no-sandbox', '--disable-gpu', '--no-first-run',
      '--no-default-browser-check', '--user-data-dir='+path.join(dir,'profile-'+width+(dark ? '-dark' : '')),
      '--window-size='+width+',1000', ...(dark ? ['--force-dark-mode'] : []),
      '--remote-debugging-pipe', url], {stdio:['ignore', 'ignore', 'pipe', 'pipe', 'pipe']});
    // stage names the last step reached, so a timeout says where it stopped.
    running = child;
    let stderr = '', incoming = '', id = 0, stage = 'waiting for the page target';
    const replies = new Map();
    // One DevTools command; the pipe carries NUL-terminated JSON both ways.
    const send = (method, params = {}, sessionId) => new Promise((done, fail) => {
      replies.set(++id, reply => reply.error ? fail(Error(method+': '+reply.error.message)) : done(reply.result));
      child.stdio[3].write(JSON.stringify({id, method, params, sessionId})+'\0');
    });
    child.stdio[4].setEncoding('utf8');  // a chunk can split a character such as "·"
    child.stdio[4].on('data', data => {
      incoming += data;
      for (let end; (end = incoming.indexOf('\0')) >= 0; incoming = incoming.slice(end+1)) {
        const reply = JSON.parse(incoming.slice(0, end));
        replies.get(reply.id)?.(reply);
        replies.delete(reply.id);
      }
    });
    // SIGKILL and no wait for 'close': a Chrome helper can hold a pipe open.
    let settled = false;
    const finish = (error, dom) => {
      if (settled) return;
      settled = true; clearTimeout(timeout); child.kill('SIGKILL');
      if (error) reject(Error('at '+label+': '+error+(stderr ? '\nChrome stderr:\n'+stderr : ''))); else resolve(dom);
    };
    const timeout = setTimeout(() => finish('no result by the 35 s deadline, '+stage), deadline-Date.now());
    child.on('error', error => finish(error.message));
    child.on('exit', code => finish('Chrome exited early with '+code));
    child.stderr.on('data', data => {stderr += data;});
    const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
    (async () => {
      // Attach only once the tab shows our file. Before that it is the blank
      // start page, and the move to the file drops a command sent to it.
      let page;
      while (!(page = (await send('Target.getTargets')).targetInfos.find(target => target.type === 'page' && target.url === url))) await pause(50);
      stage = 'attaching to the page';
      const {sessionId} = await send('Target.attachToTarget', {targetId:page.targetId, flatten:true});
      for (let polls = 1; ; polls++) {
        stage = 'poll '+polls+' sent, no reply';
        const {result} = await send('Runtime.evaluate', {expression:
          "document.body?.dataset.layoutTest || document.documentElement.dataset.error ? document.documentElement.outerHTML : document.readyState"}, sessionId);
        // No value while the document is still being replaced; poll again.
        if (result.value?.startsWith('<html')) return finish(null, result.value);
        stage = 'page script still running after '+polls+' polls, document '+(result.value ?? result.description);
        await pause(100);
      }
    })().catch(error => finish(error.message));
  });
}
(async () => {
  try {
    for (const [width, dark] of [[1209], [1209, true], [801], [800], [640], [600]]) {
      const label = width+'px'+(dark ? ' dark' : '');
      const dom = await pageDom(width, dark);
      if (!dom.includes('data-viewport="'+width+'"') || !dom.includes('data-layout-test="passed"') || /<html[^>]*data-error=/.test(dom))
        throw Error('Chrome page checks failed at '+label+': '+dom.match(/<html[^>]*>|<body[^>]*>/g));
      console.log('webconfig_chrome_test: sections, selection, Read, Advanced, field fit, seam clearance, gestures, unsaved count, refusal strip and service guard passed at '+label);
    }
  } finally {
    if (process.argv.includes('--keep')) console.log(file);
    // Retries: a killed Chrome's helpers can still be writing to their profiles.
    else fs.rmSync(dir, {recursive:true, force:true, maxRetries:10});
  }
  // Exit now: a Chrome helper left behind holding a pipe would keep Node alive.
})().then(() => process.exit(0), error => {console.error(error); process.exit(1);});

const fs = require('fs');
const os = require('os');
const path = require('path');
const {pathToFileURL} = require('url');
const {spawn, spawnSync} = require('child_process');
const candidates = [process.env.CHROME, 'google-chrome', 'chromium', 'chromium-browser',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  process.env.PROGRAMFILES && path.join(process.env.PROGRAMFILES, 'Google/Chrome/Application/chrome.exe')].filter(Boolean);
const chrome = candidates.find(binary => spawnSync(binary, ['--version'], {timeout:5000}).status === 0);
if (!chrome) {console.log('SKIP: Chrome is not installed'); process.exit(77);}
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
  let sets = 0;
  device = {opened:true, async sendReport(id, report) {
    if (report[2] === packetType.setValMsg) sets++;
    if (report[2] !== packetType.getValAllMsg) return;
    for (const [key,value] of Object.entries(fields)) {
      const data = new DataView(new ArrayBuffer(12));
      data.setUint8(2, packetType.getValMsg); data.setUint8(3, Number(key));
      data.setUint32(4, value, true);
      await handleInputReport({data});
    }
  }};
  if (document.querySelectorAll('summary').length !== 1) throw Error('Expected one shared Advanced disclosure');
  await readHandler();
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
  const panels = document.querySelectorAll('#advanced > .row > .column');
  check(panels.length === 2, 'Shared Advanced must contain both outputs');
  for (const panel of panels) {
    const column = panel.getBoundingClientRect();
    const details = document.getElementById('advanced').getBoundingClientRect();
    check(column.left >= details.left && column.right <= details.right, 'Advanced output escapes shared panel');
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
  document.querySelector('#advanced summary').click();
  for (const key of [98,99])
    if (document.getElementById('advanced').open || document.querySelector('[data-key="'+key+'"]').checkVisibility())
      throw Error('Shared Advanced disclosure did not collapse both outputs');
  document.querySelector('#advanced summary').click();
  for (const key of [98,99])
    if (!document.querySelector('[data-key="'+key+'"]').checkVisibility())
      throw Error('Shared Advanced disclosure did not reveal both outputs');
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
  // Once a primary drag starts, another finger cannot move or finish it.
  let dragBox = boxOf('A', 2), dragRect = dragBox.getBoundingClientRect(), dragScale = dragBox.ownerSVGElement.getScreenCTM().a;
  let dragFrom = [dragRect.left+dragRect.width/2, dragRect.top+dragRect.height/2];
  pointer('pointerdown', dragBox, dragFrom, {pointerId:1});
  pointer('pointermove', window, [dragFrom[0], dragFrom[1]-100*dragScale], {pointerId:2, isPrimary:false});
  pointer('pointerup', window, [dragFrom[0], dragFrom[1]-100*dragScale], {pointerId:2, isPrimary:false});
  if (pending().length || document.getElementById('layout-refused').textContent)
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
  press('B', 1, 0);
  const reason = document.getElementById('layout-refused').textContent;
  if (!/^Not moved: .*gap/.test(reason) || written() !== '2,1,1,0,1,0' || layout.querySelectorAll('[data-segment]').length !== 1)
    throw Error('Gap drop changed something: '+reason);
  // A right button or a second finger never starts a drag; the next press clears the reason.
  for (const init of [{button:2}, {isPrimary:false}])
    if (press('B', 1, 0, init)) throw Error('A non-primary press started a drag');
  if (document.getElementById('layout-refused').textContent !== reason) throw Error('A non-primary press cleared the reason');
  press('B', 0, 0);
  if (document.getElementById('layout-refused').textContent) throw Error('A new drag did not clear the reason');
  // The label bar holds its text, for a wide block and for a one-box block.
  const labelFits = () => [...layout.querySelectorAll('.layout-handle')].every(handle => {
    const bar = handle.querySelector('rect').getBBox(), text = handle.querySelector('text').getBBox();
    return text.x >= bar.x+2 && text.x+text.width <= bar.x+bar.width-2;
  });
  if (!labelFits()) throw Error('Label bar text overflows a wide bar');
  const count = field(41);
  count.value = '1'; count.dispatchEvent(new Event('change', {bubbles:true}));
  if (!labelFits()) throw Error('Label bar text overflows a one-box bar');
  // Box gestures (#213) go through the same path: fields fill in, nothing is sent.
  await readHandler();
  const chainA = () => field(98).value, countA = () => field(11).value;
  if (press('A', -1.4, 0.3, {}, 1) !== 'translate(-100 0)') throw Error('Box preview did not snap to whole boxes');
  if (chainA() !== '2' || written() !== '5,4,1,2,1,2' || sets !== 0 || pending().sort().join() !== '140,143,98')
    throw Error('Dragging Main onto box 2 did not flip A: chain '+chainA()+' fields '+written()+' pending '+pending());
  press('A', 0, 0, {}, 2);
  if (document.activeElement !== boxOf('A', 2) || document.getElementById('layout-refused').textContent)
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
  if (countA() !== '1' || !/^Not removed:/.test(document.getElementById('layout-refused').textContent))
    throw Error('- did not stop at Main: count '+countA());
  if (sets !== 0) throw Error('A box gesture sent '+sets+' values before Save');
  // Read throws the unsaved gestures away.
  await readHandler();
  if (written() !== '5,4,2,1,1,2' || count.value !== '2' || countA() !== '2' || chainA() !== '1' || sets !== 0 || pending().length ||
      layout.querySelectorAll('[data-segment]').length !== 2)
    throw Error('Read did not restore the board values: '+written()+' pending '+pending());
  document.body.dataset.viewport = innerWidth;
  document.body.dataset.layoutTest = 'passed';
})().catch(error => {document.documentElement.dataset.error = String(error);});
</script></body>`);
fs.writeFileSync(file, html);
(async () => {
  try {
    for (const width of [1209, 801, 800, 640, 600]) {
      const dom = await new Promise((resolve, reject) => {
        const child = spawn(chrome, ['--headless', '--no-sandbox', '--disable-gpu', '--no-first-run',
          '--no-default-browser-check', '--user-data-dir='+path.join(dir,'profile-'+width),
          '--window-size='+width+',1000', '--virtual-time-budget=2000', '--dump-dom', pathToFileURL(file).href]);
        let stdout = '', stderr = '';
        const timeout = setTimeout(() => {child.kill('SIGKILL'); reject(Error('Chrome DOM dump timed out'));}, 30000);
        child.on('error', reject);
        child.stderr.on('data', data => {stderr += data;});
        child.stdout.on('data', data => {
          stdout += data;
          // Some macOS Chrome builds hang on shutdown after the complete dump.
          if (stdout.trimEnd().endsWith('</html>')) child.kill('SIGKILL');
        });
        child.on('close', () => {
          clearTimeout(timeout);
          if (!stdout.trimEnd().endsWith('</html>')) reject(Error(stderr || 'Incomplete Chrome DOM dump'));
          else resolve(stdout);
        });
      });
      if (!dom.includes('data-viewport="'+width+'"') || !dom.includes('data-layout-test="passed"') || /<html[^>]*data-error=/.test(dom))
        throw Error('Chrome page checks failed: '+dom.match(/<html[^>]*>|<body[^>]*>/g));
      console.log('webconfig_chrome_test: Read, shared Advanced, field fit, seam clearance, label drag and box gestures passed at '+width+'px');
    }
  } finally {
    if (process.argv.includes('--keep')) console.log(file);
    else fs.rmSync(dir, {recursive:true, force:true});
  }
})().catch(error => {console.error(error); process.exitCode = 1;});

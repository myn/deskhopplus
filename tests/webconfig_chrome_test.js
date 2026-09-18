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
  // Drag B's label from below A to A's right: a valid drop writes both borders
  // and the segments and sends them; a gap drop shows its reason and sends nothing.
  const field = key => document.querySelector('[data-key="'+key+'"]');
  const written = () => [17,47,140,143,152,155].map(key => field(key).value).join();
  const pointer = (type, target, at, init = {}) => target.dispatchEvent(new PointerEvent(type,
    {bubbles:true, isPrimary:true, clientX:at[0], clientY:at[1], ...init}));
  function press(letter, dx, dy, init) {
    const handle = layout.querySelector('.layout-handle[data-output="'+letter+'"]');
    const scale = handle.ownerSVGElement.getScreenCTM().a, box = handle.getBoundingClientRect();
    const from = [box.left+box.width/2, box.top+box.height/2], to = [from[0]+dx*100*scale, from[1]+dy*100*scale];
    pointer('pointerdown', handle, from, init);
    pointer('pointermove', window, to);
    const followed = !!handle.parentNode.getAttribute('transform');
    pointer('pointerup', window, to);
    return followed;
  }
  const settle = () => new Promise(resolve => setTimeout(resolve, 50));
  async function drag(letter, dx, dy) {
    if (!press(letter, dx, dy)) throw Error('Block did not follow the pointer');
    await settle();
  }
  if (sets !== 0) throw Error('Read or a select change sent a value');
  await drag('B', 2, -1);
  if (written() !== '2,1,1,0,1,0' || sets === 0 || layout.querySelectorAll('[data-segment]').length !== 1)
    throw Error('Drop to the right did not write borders and segments: '+written()+' sends '+sets);
  const sentBefore = sets;
  await drag('B', 1, 0);
  if (!/gap/.test(document.getElementById('layout-refused').textContent) || sets !== sentBefore ||
      written() !== '2,1,1,0,1,0' || layout.querySelectorAll('[data-segment]').length !== 1)
    throw Error('Gap drop changed something: '+document.getElementById('layout-refused').textContent+' sends '+(sets-sentBefore));
  // A right button or a second finger never starts a drag.
  for (const init of [{button:2}, {isPrimary:false}])
    if (press('B', 1, 0, init)) throw Error('A non-primary press started a drag');
  await settle();
  if (written() !== '2,1,1,0,1,0' || sets !== sentBefore) throw Error('A non-primary press dropped');
  // A press while a drop is still writing is ignored, so two drops never interleave.
  press('B', -2, 1); press('B', 0.5, 0);
  await settle();
  if (written() !== '5,4,2,1,1,2' || layout.querySelectorAll('[data-segment]').length !== 2)
    throw Error('A second press interleaved with the first drop: '+written());
  // The label bar holds its text, for a wide block and for a one-box block.
  const labelFits = () => [...layout.querySelectorAll('.layout-handle')].every(handle => {
    const bar = handle.querySelector('rect').getBBox(), text = handle.querySelector('text').getBBox();
    return text.x >= bar.x+2 && text.x+text.width <= bar.x+bar.width-2;
  });
  if (!labelFits()) throw Error('Label bar text overflows a wide bar');
  const count = field(41);
  count.value = '1'; count.dispatchEvent(new Event('change', {bubbles:true}));
  if (!labelFits()) throw Error('Label bar text overflows a one-box bar');
  count.value = '2'; count.dispatchEvent(new Event('change', {bubbles:true}));
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
      console.log('webconfig_chrome_test: Read, shared Advanced, field fit, seam clearance and label drag passed at '+width+'px');
    }
  } finally {
    if (process.argv.includes('--keep')) console.log(file);
    else fs.rmSync(dir, {recursive:true, force:true});
  }
})().catch(error => {console.error(error); process.exitCode = 1;});

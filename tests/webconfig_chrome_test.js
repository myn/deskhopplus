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
  device = {opened:true, async sendReport(id, report) {
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
      console.log('webconfig_chrome_test: Read, shared Advanced, field fit and seam clearance passed at '+width+'px');
    }
  } finally {
    if (process.argv.includes('--keep')) console.log(file);
    else fs.rmSync(dir, {recursive:true, force:true});
  }
})().catch(error => {console.error(error); process.exitCode = 1;});

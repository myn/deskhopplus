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
  const fields = {11:2,41:2,16:2,46:3,17:5,47:4,98:1,99:2,
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
  await readHandler();
  const layout = document.getElementById('layout');
  if (layout.querySelectorAll('.layout-monitor').length !== 4 ||
      layout.querySelectorAll('[data-segment]').length !== 2 ||
      layout.querySelector('.layout-A .layout-main').getAttribute('x') !== '104')
    throw Error('Read did not draw the maintainer desk');
  const before = layout.innerHTML;
  const chain = document.querySelector('[data-key="98"]');
  chain.value = '2'; chain.dispatchEvent(new Event('change', {bubbles:true}));
  if (layout.innerHTML === before) throw Error('Advanced change did not redraw');
  layout.querySelector('a').click();
  if (!document.getElementById('advanced-A').open || !document.getElementById('advanced-B').open)
    throw Error('Open Advanced did not open the panels');
  chain.value = '1'; chain.dispatchEvent(new Event('change', {bubbles:true}));
  document.getElementById('advanced-A').open = false;
  document.getElementById('advanced-B').open = false;
  document.body.dataset.layoutTest = 'passed';
})().catch(error => {document.documentElement.dataset.error = String(error);});
</script></body>`);
fs.writeFileSync(file, html);
(async () => {
  try {
    const dom = await new Promise((resolve, reject) => {
      const child = spawn(chrome, ['--headless', '--no-sandbox', '--disable-gpu', '--no-first-run',
        '--no-default-browser-check', '--user-data-dir='+path.join(dir,'profile'),
        '--virtual-time-budget=2000', '--dump-dom', pathToFileURL(file).href]);
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
    if (!dom.includes('data-layout-test="passed"') || /<html[^>]*data-error=/.test(dom))
      throw Error('Chrome page checks failed: '+dom.match(/<html[^>]*>|<body[^>]*>/g));
    console.log('webconfig_chrome_test: Read, boxes, Advanced redraw and link passed');
  } finally {
    if (process.argv.includes('--keep')) console.log(file);
    else fs.rmSync(dir, {recursive:true, force:true});
  }
})().catch(error => {console.error(error); process.exitCode = 1;});

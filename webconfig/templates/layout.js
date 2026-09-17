// Coordinates are monitor widths; half-widths are the layout grid.
function layoutFromFields(fields) {
  const vectors = {1:[-1,0], 2:[1,0], 4:[0,-1], 5:[0,1]};
  const opposite = {1:2, 2:1, 4:5, 5:4};
  const border = Number(fields[17]) || 2;
  const normal = vectors[border] || vectors[2];
  const tangent = normal[0] ? 1 : 0;
  const outputs = [0,1].map(i => {
    const base = i ? 40 : 10;
    const chain = Number(fields[98+i]) || 2;
    const direction = vectors[chain] || vectors[2];
    const count = Math.max(1, Math.min(7, Number(fields[base+1]) || 1));
    const monitors = Array.from({length:count}, (_, n) => ({
      number:n+1, x:n*direction[0], y:n*direction[1],
    }));
    const minX = Math.min(...monitors.map(m => m.x));
    const minY = Math.min(...monitors.map(m => m.y));
    monitors.forEach(m => {m.x -= minX; m.y -= minY;});
    return {letter:i ? 'B' : 'A', os:({1:'Linux',2:'MacOS',3:'Windows',4:'Android',255:'Other'})[fields[base+6]] || 'Other',
      chain, border:Number(fields[base+7]) || opposite[border], monitors};
  });
  const segments = Array.from({length:4}, (_, n) => [140,152].map(base => ({
    monitor:Number(fields[base+3*n]) || 0,
    start:Number(fields[base+3*n+1]) || 0, end:Number(fields[base+3*n+2]) || 0,
  })));
  const empty = segments.every(pair => pair.every(s => !s.monitor));
  let custom = !vectors[border] || outputs[1].border !== opposite[border] ||
    outputs.some(o => o.chain === o.border);
  const a = outputs[0].monitors, b = outputs[1].monitors;
  // Start with left/top-aligned lines on the side selected by Output A.
  let shift = normal.map((v, axis) => v > 0 ? Math.max(...a.map(m => axis ? m.y : m.x))+1 :
    v < 0 ? -Math.max(...b.map(m => axis ? m.y : m.x))-1 : 0);
  const edge = (m, side, fraction) => [m.x, m.y].map((v, axis) =>
    v + (axis === tangent ? fraction : (side[axis] > 0 ? 1 : 0)));
  const close = (x,y) => Math.abs(x-y) <= 1/65535;
  let recovered = false;
  const bands = [];
  segments.forEach((pair, index) => {
    if (pair.every(s => !s.monitor)) return;
    if (pair.some((s,i) => !Number.isInteger(s.monitor) || s.monitor < 1 ||
        s.monitor > outputs[i].monitors.length || s.start < 0 || s.end > 65535 || s.start >= s.end)) {
      custom = true; return;
    }
    const points = pair.map((s,i) => [s.start,s.end].map(value =>
      edge(outputs[i].monitors[s.monitor-1], normal.map(v => i ? -v : v), value/65535)));
    const delta = points[0][0].map((v,axis) => v-points[1][0][axis]);
    const snapped = delta.map(v => Math.round(v*2)/2);
    const valid = delta.every((v,axis) => close(v,snapped[axis]) &&
      close(points[0][1][axis]-points[1][1][axis],snapped[axis])) &&
      pair.every(s => [s.start,s.end].every(v => close(v/65535, Math.round(v/65535*2)/2)));
    const origins = pair.map((s,i) => {
      const m = outputs[i].monitors[s.monitor-1];
      return (tangent ? m.y : m.x) + (i ? snapped[tangent] : 0);
    });
    const overlapStart = Math.max(...origins), overlapEnd = Math.min(...origins)+1;
    const collides = a.some(m => b.some(n =>
      Math.abs(m.x-n.x-snapped[0])<1 && Math.abs(m.y-n.y-snapped[1])<1));
    if (!valid || collides || !close(points[0][0][tangent], overlapStart) ||
        !close(points[0][1][tangent], overlapEnd)) {custom = true; return;}
    if (!recovered) {shift = snapped; recovered = true;}
    if (!snapped.every((v,axis) => close(v,shift[axis]))) {custom = true; return;}
    bands.push({number:index+1, start:points[0][0], end:points[0][1]});
  });
  b.forEach(m => {m.x += shift[0]; m.y += shift[1];});
  if (a.some(m => b.some(n => Math.abs(m.x-n.x)<1 && Math.abs(m.y-n.y)<1))) custom = true;
  const minX = Math.min(...a.concat(b).map(m => m.x));
  const minY = Math.min(...a.concat(b).map(m => m.y));
  a.concat(b).forEach(m => {m.x -= minX; m.y -= minY;});
  bands.forEach(s => [s.start,s.end].forEach(p => {p[0] -= minX; p[1] -= minY;}));
  return {outputs, bands, note:empty ? 'Segments are not set. Move a monitor to set them.' :
    custom ? 'Custom values are set in Advanced. Moving a monitor here replaces them.' : ''};
}

function renderLayout(layout) {
  if (!layout) return '<p>Connect to see your monitor layout.</p>';
  const monitors = layout.outputs.flatMap(o => o.monitors);
  const width = Math.max(...monitors.map(m => m.x))+1;
  const height = Math.max(...monitors.map(m => m.y))+1;
  const groups = layout.outputs.map(o => `<g class="layout-${o.letter}" aria-label="Output ${o.letter}: ${o.os}">${o.monitors.map(m =>
    `<rect class="layout-monitor${m.number === 1 ? ' layout-main' : ''}" x="${m.x*100+4}" y="${m.y*100+4}" width="92" height="92" rx="5"/><text x="${m.x*100+50}" y="${m.y*100+47}">${m.number === 1 ? 'Main' : m.number}</text><text class="layout-os" x="${m.x*100+50}" y="${m.y*100+68}">${o.letter} · ${o.os}</text>`).join('')}</g>`).join('');
  const bands = layout.bands.map(s => `<g data-segment="${s.number}"><line class="layout-band" x1="${s.start[0]*100}" y1="${s.start[1]*100}" x2="${s.end[0]*100}" y2="${s.end[1]*100}"/><text class="layout-band-number" x="${(s.start[0]+s.end[0])*50}" y="${(s.start[1]+s.end[1])*50+5}">${s.number}</text></g>`).join('');
  return `<div class="layout-legend">${layout.outputs.map(o => `<span class="layout-${o.letter}">Output ${o.letter} · ${o.os}</span>`).join('')}</div><svg role="img" aria-label="Monitor layout" viewBox="-10 -10 ${width*100+20} ${height*100+20}" style="max-height:${height*100+20}px">${groups}${bands}</svg>${layout.note ? `<p>${layout.note} <a href="#advanced-A" onclick="document.getElementById('advanced-A').open = true; document.getElementById('advanced-B').open = true">Open Advanced</a></p>` : ''}`;
}

function redrawLayout() {
  const fields = Object.fromEntries([...document.querySelectorAll('.api[data-key]')].map(e => [e.dataset.key, getValue(e)]));
  document.getElementById('layout').innerHTML = renderLayout(device && device.opened ? layoutFromFields(fields) : null);
}

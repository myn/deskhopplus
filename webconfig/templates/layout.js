// Coordinates are monitor widths; half-widths are the layout grid.
// Directions are the board's: 1 Left, 2 Right, 4 Top, 5 Bottom.
const vectors = {1:[-1,0], 2:[1,0], 4:[0,-1], 5:[0,1]};
const opposite = {1:2, 2:1, 4:5, 5:4};
const at = (m, axis) => axis ? m.y : m.x;

function layoutFromFields(fields) {
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
    outputs.some(o => o.monitors.length > 1 && o.chain === o.border);
  const a = outputs[0].monitors, b = outputs[1].monitors;
  // Start with left/top-aligned lines on the side selected by Output A.
  let shift = normal.map((v, axis) => v > 0 ? Math.max(...a.map(m => at(m, axis)))+1 :
    v < 0 ? -Math.max(...b.map(m => at(m, axis)))-1 : 0);
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
      return at(m, tangent) + (i ? snapped[tangent] : 0);
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

// Derives every field the layout owns from where the boxes sit: counts, chain
// and border directions, and the segments in seam order. Returns {fields}, or
// {refused} with the one-line reason the board cannot run this layout.
function fieldsFromLayout(layout) {
  const [a, b] = layout.outputs.map(o => o.monitors);
  // A straight line of equal boxes fills its bounding box, so the boxes are the box.
  const bounds = ms => [0,1].map(axis => [Math.min(...ms.map(m => at(m, axis))), Math.max(...ms.map(m => at(m, axis)))+1]);
  const [boxA, boxB] = [bounds(a), bounds(b)];
  const overlap = [0,1].map(axis => Math.min(boxA[axis][1], boxB[axis][1]) - Math.max(boxA[axis][0], boxB[axis][0]));
  if (overlap.some(v => v < 0)) return {refused:'Not moved: that leaves a gap between the computers. Put them edge to edge.'};
  if (overlap.every(v => v > 0)) return {refused:'Not moved: the computers would overlap. Put them edge to edge.'};
  if (overlap.every(v => v === 0)) return {refused:'Not moved: the computers would touch only at a corner. Put an edge against an edge.'};
  const normal = overlap[0] === 0 ? 0 : 1, tangent = 1-normal;
  const forward = boxA[normal][1] === boxB[normal][0];
  const seam = boxA[normal][forward ? 1 : 0];
  const borders = [normal ? (forward ? 5 : 4) : (forward ? 2 : 1)];
  borders.push(opposite[borders[0]]);
  const fields = {};
  for (const [i, o] of layout.outputs.entries()) {
    const ms = o.monitors, base = i ? 40 : 10;
    let chain = o.chain;
    if (ms.length > 1) {
      const dx = ms[1].x-ms[0].x, dy = ms[1].y-ms[0].y;
      chain = dx < 0 ? 1 : dx > 0 ? 2 : dy < 0 ? 4 : 5;
      // The board lets only Main cross when the line points at the other computer.
      if (chain === borders[i]) return {refused:'Not moved: put the main monitor next to the other computer.'};
    }
    fields[base+1] = ms.length; fields[98+i] = chain; fields[base+7] = borders[i];
  }
  const facing = (ms, positive) => ms.filter(m => at(m, normal) + (positive ? 1 : 0) === seam);
  const pairs = [];
  for (const p of facing(a, forward)) for (const q of facing(b, !forward)) {
    const start = Math.max(at(p, tangent), at(q, tangent)), end = Math.min(at(p, tangent), at(q, tangent))+1;
    if (end > start) pairs.push({start, end, sides:[p, q]});
  }
  pairs.sort((u, v) => u.start-v.start);
  if (pairs.length > 4) return {refused:`Not moved: that layout needs ${pairs.length} segments; the board holds 4.`};
  [140, 152].forEach((base, i) => {
    for (let n = 0; n < 4; n++) {
      const pair = pairs[n], m = pair && pair.sides[i];
      fields[base+3*n] = pair ? m.number : 0;
      fields[base+3*n+1] = pair ? Math.round((pair.start-at(m, tangent))*65535) : 0;
      fields[base+3*n+2] = pair ? Math.round((pair.end-at(m, tangent))*65535) : 0;
    }
  });
  return {fields};
}

// Applies one gesture. {output, dx, dy} moves that computer as a block by dx, dy
// boxes. Returns {layout, fields} for a valid drop, or {layout, refused} with the
// layout unchanged.
function moveMonitor(layout, gesture) {
  const outputs = layout.outputs.map(o => o.letter !== gesture.output ? o :
    {...o, monitors:o.monitors.map(m => ({...m, x:m.x+gesture.dx, y:m.y+gesture.dy}))});
  const result = fieldsFromLayout({outputs});
  if (result.refused) return {layout, refused:result.refused};
  const moved = layoutFromFields(result.fields);
  moved.outputs.forEach((o, i) => {o.os = layout.outputs[i].os;});
  return {layout:moved, fields:result.fields};
}

function renderLayout(layout) {
  if (!layout) return '<p>Connect to see your monitor layout.</p>';
  const monitors = layout.outputs.flatMap(o => o.monitors);
  const width = Math.max(...monitors.map(m => m.x))+1;
  const height = Math.max(...monitors.map(m => m.y))+1;
  // Each cell is 100 units: the label bar sits in the top 18, then a gap, then
  // the monitor box. The bar spans the block and is the handle that moves it.
  const groups = layout.outputs.map(o => {
    const left = Math.min(...o.monitors.map(m => m.x))*100+12, top = Math.min(...o.monitors.map(m => m.y))*100+12;
    const right = Math.max(...o.monitors.map(m => m.x))*100+88;
    return `<g class="layout-${o.letter}" aria-label="Output ${o.letter}: ${o.os}"><g class="layout-handle" data-output="${o.letter}"><rect x="${left}" y="${top}" width="${right-left}" height="18" rx="5"/><text x="${(left+right)/2}" y="${top+13}">${right-left > 76 ? 'Output ' : ''}${o.letter} · ${o.os}</text></g>${o.monitors.map(m =>
      `<rect class="layout-monitor${m.number === 1 ? ' layout-main' : ''}" x="${m.x*100+12}" y="${m.y*100+36}" width="76" height="52" rx="5"/><text x="${m.x*100+50}" y="${m.y*100+68}">${m.number === 1 ? 'Main' : m.number}</text>`).join('')}</g>`;
  }).join('');
  const bands = layout.bands.map(s => `<g data-segment="${s.number}"><line class="layout-band" x1="${s.start[0]*100}" y1="${s.start[1]*100}" x2="${s.end[0]*100}" y2="${s.end[1]*100}"/><text class="layout-band-number" x="${(s.start[0]+s.end[0])*50}" y="${(s.start[1]+s.end[1])*50+5}">${s.number}</text></g>`).join('');
  return `<svg role="img" aria-label="Monitor layout" viewBox="-10 -10 ${width*100+20} ${height*100+20}" style="max-height:${height*100+20}px">${groups}${bands}</svg>${layout.note ? `<p>${layout.note} <a href="#advanced" onclick="document.getElementById('advanced').open = true">Open Advanced</a></p>` : ''}`;
}

function readFields() {
  return Object.fromEntries([...document.querySelectorAll('.api[data-key]')].map(e => [e.dataset.key, getValue(e)]));
}

function redrawLayout() {
  document.getElementById('layout').innerHTML = renderLayout(device && device.opened ? layoutFromFields(readFields()) : null);
  document.getElementById('layout-refused').textContent = '';
}

// A valid drop writes the derived values into the Advanced fields, the same
// as picking them by hand: Save sends them to the board, Read throws them
// away. A refused drop shows its reason and touches nothing.
function dropLabel(gesture) {
  const result = moveMonitor(layoutFromFields(readFields()), gesture);
  if (result.refused) {document.getElementById('layout-refused').textContent = result.refused; return;}
  for (const [key, value] of Object.entries(result.fields))
    document.querySelector(`.api[data-key="${key}"]`).value = value;
  redrawLayout();
}

// Pointer glue: the block follows the pointer in half-box steps, so it lands
// where it is shown.
document.getElementById('layout').addEventListener('pointerdown', event => {
  const handle = event.target.closest('.layout-handle');
  if (!handle || event.button !== 0 || !event.isPrimary) return;
  event.preventDefault();
  document.getElementById('layout-refused').textContent = '';
  const group = handle.parentNode, scale = handle.ownerSVGElement.getScreenCTM().a;
  const delta = e => [e.clientX-event.clientX, e.clientY-event.clientY].map(v => Math.round(v/scale/50)/2);
  const follow = e => group.setAttribute('transform', `translate(${delta(e).map(v => v*100).join(' ')})`);
  const drop = e => {
    window.removeEventListener('pointermove', follow); window.removeEventListener('pointerup', drop);
    group.removeAttribute('transform');
    const [dx, dy] = delta(e);
    dropLabel({output:handle.dataset.output, dx, dy});
  };
  window.addEventListener('pointermove', follow); window.addEventListener('pointerup', drop);
});

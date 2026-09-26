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
  const segments = Array.from({length:4}, (_, n) => [{{ seam_field_bases|join(',') }}].map(base => ({
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
  [{{ seam_field_bases|join(', ') }}].forEach((base, i) => {
    for (let n = 0; n < 4; n++) {
      const pair = pairs[n], m = pair && pair.sides[i];
      fields[base+3*n] = pair ? m.number : 0;
      fields[base+3*n+1] = pair ? Math.round((pair.start-at(m, tangent))*65535) : 0;
      fields[base+3*n+2] = pair ? Math.round((pair.end-at(m, tangent))*65535) : 0;
    }
  });
  return {fields};
}

// Applies one gesture to one output and returns {layout, fields}, or
// {layout, refused} with the layout unchanged. Gestures:
//   {dx, dy}           move the computer as a block by dx, dy boxes;
//   {monitor, dx, dy}  drop one box: Main onto its line's far end flips the line
//                      (the boxes stay put, renumbered from Main); another
//                      box turns the line to point from Main toward its cell;
//                      the only box of a one-monitor computer moves the block;
//   {add: 1 | -1}      append a box at the end of the line, or remove the last.
function moveMonitor(layout, gesture) {
  const o = layout.outputs.find(o => o.letter === gesture.output), ms = o.monitors;
  const main = ms[0], last = ms[ms.length-1];
  const dir = ms.length > 1 ? [Math.sign(ms[1].x-main.x), Math.sign(ms[1].y-main.y)] : vectors[o.chain] || [1,0];
  const line = (from, [dx, dy], count) => Array.from({length:count}, (_, n) => ({number:n+1, x:from.x+n*dx, y:from.y+n*dy}));
  let monitors;
  if (gesture.add > 0) {
    if (ms.length >= 7) return {layout, refused:'Not added: a computer holds at most 7 monitors.'};
    monitors = line(main, dir, ms.length+1);
  } else if (gesture.add < 0) {
    if (ms.length < 2) return {layout, refused:'Not removed: a computer keeps its main monitor.'};
    monitors = ms.slice(0, -1);
  } else if (!gesture.monitor || ms.length < 2) {
    monitors = ms.map(m => ({...m, x:m.x+gesture.dx, y:m.y+gesture.dy}));
  } else if (gesture.monitor === 1) {
    if (last.x !== main.x+gesture.dx || last.y !== main.y+gesture.dy)
      return {layout, refused:`Not moved: drop Main on monitor ${last.number} to flip it, or drag the label to move the whole computer.`};
    monitors = line(last, dir.map(v => -v), ms.length);
  } else {
    const m = ms[gesture.monitor-1];
    const d = [m.x+gesture.dx-main.x, m.y+gesture.dy-main.y];
    // A cell off the line turns the line across its axis; a cell on it turns it along.
    const along = dir[0] ? 0 : 1, axis = d[1-along] ? 1-along : along;
    if (!d[axis]) return {layout, refused:'Not moved: drop a monitor beside, above or below the main monitor.'};
    monitors = line(main, axis ? [0, Math.sign(d[1])] : [Math.sign(d[0]), 0], ms.length);
  }
  const outputs = layout.outputs.map(p => p === o ? {...o, monitors} : p);
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
  const spareRow = height < 2 ? 1 : 0;
  const canvasHeight = (height+2*spareRow)*100+20;
  // Each cell is 100 units: the label bar sits in the top 18, then a gap, then
  // the monitor box. The bar spans the block and is the handle that moves it;
  // each box is a focusable handle of its own.
  const groups = layout.outputs.map(o => {
    const left = Math.min(...o.monitors.map(m => m.x))*100+12, top = Math.min(...o.monitors.map(m => m.y))*100+12;
    const right = Math.max(...o.monitors.map(m => m.x))*100+88;
    return `<g class="layout-${o.letter}" data-output="${o.letter}" aria-label="Output ${o.letter}: ${o.os}"><g class="layout-handle"><rect x="${left}" y="${top}" width="${right-left}" height="18" rx="5"/><text x="${(left+right)/2}" y="${top+13}">${right-left > 76 ? 'Output ' : ''}${o.letter} · ${o.os}</text></g>${o.monitors.map(m =>
      `<g class="layout-box" data-output="${o.letter}" data-monitor="${m.number}" tabindex="0" aria-label="Output ${o.letter} ${m.number === 1 ? 'main monitor' : 'monitor '+m.number}"><rect class="layout-monitor${m.number === 1 ? ' layout-main' : ''}" x="${m.x*100+12}" y="${m.y*100+36}" width="76" height="52" rx="5"/><text x="${m.x*100+50}" y="${m.y*100+68}">${m.number === 1 ? 'Main' : m.number}</text></g>`).join('')}</g>`;
  }).join('');
  const bands = layout.bands.map(s => `<g data-segment="${s.number}"><line class="layout-band" x1="${s.start[0]*100}" y1="${s.start[1]*100}" x2="${s.end[0]*100}" y2="${s.end[1]*100}"/><text class="layout-band-number" x="${(s.start[0]+s.end[0])*50}" y="${(s.start[1]+s.end[1])*50+5}">${s.number}</text></g>`).join('');
  const counts = layout.outputs.map(o => `<span>Output ${o.letter}: <button type="button" aria-label="Remove a monitor from Output ${o.letter}" onclick="applyGesture({output:'${o.letter}',add:-1})">−</button> ${o.monitors.length} <button type="button" aria-label="Add a monitor to Output ${o.letter}" onclick="applyGesture({output:'${o.letter}',add:1})">+</button></span>`).join('');
  return `<svg role="group" aria-label="Monitor layout" viewBox="-10 ${-10-spareRow*100} ${width*100+20} ${canvasHeight}" style="max-height:${canvasHeight}px">${groups}${bands}</svg><p class="layout-counts">${counts}</p>${layout.note ? `<p>${layout.note} <a href="#advanced" onclick="document.getElementById('advanced').open = true">Open Advanced</a></p>` : ''}`;
}

function readFields() {
  return Object.fromEntries([...document.querySelectorAll('.api[data-key]')].map(e => [e.dataset.key, getValue(e)]));
}

function showLayoutStatus(message = '', ok = false) {
  const status = document.getElementById('layout-status');
  if (status.textContent !== message) status.textContent = message;
  status.classList.toggle('layout-ok', ok);
}

function redrawLayout() {
  document.getElementById('layout').innerHTML = renderLayout(device && device.opened ? layoutFromFields(readFields()) : null);
  showLayoutStatus();
}

// Every gesture ends here. A valid one writes the derived values into the
// Advanced fields, the same as picking them by hand: Save sends them to the
// board, Read throws them away. A refused one shows its reason and touches nothing.
function applyGesture(gesture) {
  const result = moveMonitor(layoutFromFields(readFields()), gesture);
  if (result.refused) {showLayoutStatus(result.refused); return;}
  for (const [key, value] of Object.entries(result.fields))
    document.querySelector(`.api[data-key="${key}"]`).value = value;
  redrawLayout();
}

// Pointer glue: a label bar drags the computer in half-box steps, a box drags
// in whole boxes, so each lands where it is shown. The only box of a
// one-monitor computer is its label.
document.getElementById('layout').addEventListener('pointerdown', event => {
  const target = event.target.closest('.layout-handle, .layout-box');
  if (!target || event.button !== 0 || !event.isPrimary) return;
  event.preventDefault();
  showLayoutStatus();
  const computer = target.parentNode, scale = target.ownerSVGElement.getScreenCTM().a;
  const box = target.dataset.monitor && computer.querySelectorAll('.layout-box').length > 1 ? target : null;
  const moving = box || computer, step = box ? 1 : 0.5;
  const delta = e => [e.clientX-event.clientX, e.clientY-event.clientY].map(v => Math.round(v/scale/100/step)*step);
  const gesture = e => {const [dx, dy] = delta(e); return {output:computer.dataset.output, monitor:Number(target.dataset.monitor), dx, dy};};
  const samePointer = e => e.pointerId === event.pointerId;
  const stop = () => {
    window.removeEventListener('pointermove', follow); window.removeEventListener('pointerup', drop);
    window.removeEventListener('pointercancel', cancel); moving.removeAttribute('transform');
  };
  const follow = e => {
    if (!samePointer(e)) return;
    const current = gesture(e);
    moving.setAttribute('transform', `translate(${[current.dx,current.dy].map(v => v*100).join(' ')})`);
    if (!current.dx && !current.dy) return showLayoutStatus();
    const result = moveMonitor(layoutFromFields(readFields()), current);
    showLayoutStatus(result.refused || 'Release to apply.', !result.refused);
  };
  const drop = e => {
    if (!samePointer(e)) return;
    stop();
    const current = gesture(e);
    // A click on a box only focuses it.
    if (target.dataset.monitor && !current.dx && !current.dy) return target.focus();
    applyGesture(current);
  };
  const cancel = e => {if (samePointer(e)) {stop(); showLayoutStatus();}};
  window.addEventListener('pointermove', follow); window.addEventListener('pointerup', drop);
  window.addEventListener('pointercancel', cancel);
});

// Arrow keys on a focused box are a one-step drop; the box keeps focus.
document.getElementById('layout').addEventListener('keydown', event => {
  const box = event.target.closest('.layout-box');
  const step = {ArrowLeft:[-1,0], ArrowRight:[1,0], ArrowUp:[0,-1], ArrowDown:[0,1]}[event.key];
  if (!box || !step) return;
  event.preventDefault();
  const {output, monitor} = box.dataset;
  applyGesture({output, monitor:Number(monitor), dx:step[0], dy:step[1]});
  document.querySelector(`.layout-box[data-output="${output}"][data-monitor="${monitor}"]`).focus();
});

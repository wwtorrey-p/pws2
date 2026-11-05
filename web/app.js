'use strict';

const $ = (sel, root = document) => root.querySelector(sel);
const DIRS = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];
const RANGES = [['6h', 6], ['24h', 24], ['3d', 72], ['7d', 168]];
const SVG_W = 1000, SVG_H = 190;

const UNIT_OPTIONS = {
  temp: ['C', 'F'],
  wind: ['m/s', 'mph', 'km/h'],
  pressure: ['mmHg', 'inHg', 'hPa'],
  rain: ['mm', 'in'],
};
const UNIT_LABELS = { temp: 'Temperature', wind: 'Wind', pressure: 'Pressure', rain: 'Rain' };
const DEFAULT_UNITS = { temp: 'F', wind: 'mph', pressure: 'inHg', rain: 'in' };

let units = { ...DEFAULT_UNITS, ...JSON.parse(localStorage.getItem('wx.units') || '{}') };
let timers = [];
let panelTimer = null;
let activePanel = null;
let rangeHours = Number(localStorage.getItem('wx.range')) || 24;
let auth = sessionStorage.getItem('wx.auth');
let username = 'admin';
if (auth) {
  try { username = atob(auth).split(':', 1)[0] || username; } catch { auth = null; }
}
let rows = [];
let current = null;

// Conversions from the units the hub reports (C, m/s, mb, mm).
const conv = {
  temp: v => (units.temp === 'F' ? v * 9 / 5 + 32 : v),
  wind: v => (units.wind === 'mph' ? v * 2.236936 : units.wind === 'km/h' ? v * 3.6 : v),
  pressure: v => (units.pressure === 'inHg' ? v * 0.0295300 : units.pressure === 'mmHg' ? v * 0.750062 : v),
  rain: v => (units.rain === 'in' ? v * 0.0393701 : v),
};
const unitText = {
  temp: () => '°' + units.temp,
  wind: () => ' ' + units.wind,
  pressure: () => ' ' + units.pressure,
  rain: () => ' ' + units.rain,
  pct: () => '%',
  solar: () => ' W/m²',
};
const decimals = { temp: () => 1, wind: () => 1, pressure: () => (units.pressure === 'inHg' ? 2 : 1), rain: () => (units.rain === 'in' ? 3 : 1), pct: () => 0, solar: () => 0 };
const fmt = (kind, v) => (v == null || isNaN(v) ? '--' : conv[kind] ? conv[kind](v).toFixed(decimals[kind]()) : v.toFixed(decimals[kind]()));

const dirName = deg => DIRS[Math.round((((deg % 360) + 360) % 360) / 45) % 8];

function encodeCredentials(user, password) {
  const bytes = new TextEncoder().encode(user + ':' + password);
  let binary = '';
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary);
}

// Magnus formula.
function dewPoint(t, rh) {
  if (!rh || rh <= 0) return null;
  const a = 17.62, b = 243.12;
  const g = Math.log(rh / 100) + a * t / (b + t);
  return b * g / (a - g);
}

async function api(path, opts = {}) {
  const res = await fetch(path, { ...opts, headers: { ...(opts.headers || {}), Authorization: 'Basic ' + auth } });
  if (res.status === 401) {
    showLogin();
    throw new Error('unauthorized');
  }
  return res;
}

function showLogin() {
  timers.forEach(clearInterval);
  closePanel();
  auth = null;
  sessionStorage.removeItem('wx.auth');
  $('#loginUser').value = username;
  $('#loginPass').value = '';
  $('#loginError').hidden = true;
  $('#login').hidden = false;
  $('#loginPass').focus();
}

$('#loginForm').addEventListener('submit', async e => {
  e.preventDefault();
  auth = encodeCredentials($('#loginUser').value, $('#loginPass').value);
  const res = await fetch('/api/current', { headers: { Authorization: 'Basic ' + auth } });
  if (res.ok) {
    username = $('#loginUser').value;
    sessionStorage.setItem('wx.auth', auth);
    $('#login').hidden = true;
    $('#loginError').hidden = true;
    $('#loginPass').value = '';
    start();
  } else {
    $('#loginError').hidden = false;
  }
});

$('#passwordToggle').addEventListener('click', () => {
  const password = $('#loginPass');
  const show = password.type === 'password';
  password.type = show ? 'text' : 'password';
  $('#passwordToggle').setAttribute('aria-label', show ? 'Hide password' : 'Show password');
  $('#passwordToggle').setAttribute('aria-pressed', String(show));
  password.focus();
});

// ---- Current conditions ----

function compassSvg(deg) {
  return `<svg class="compass" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10" opacity=".4"/><path d="M12 4l4 12-4-3-4 3z" fill="currentColor" transform="rotate(${deg} 12 12)"/></svg>`;
}

function sumSince(key, seconds) {
  const since = Date.now() / 1000 - seconds;
  let total = 0;
  for (const r of rows) if (r.t >= since) total += r[key];
  return total;
}

function renderStatus() {
  const o = current && current.obs;
  const w = current && current.wind;
  const liveWind = w && w.ageS < 15;
  const speed = liveWind ? w.speed : o && o.avg;
  const dir = liveWind ? w.dir : o && o.dir;
  const items = [];
  const dew = o ? dewPoint(o.temp, o.hum) : null;
  items.push(['TEMPERATURE', o ? fmt('temp', o.temp) + `<small>${unitText.temp()}</small>` : '--', dew == null ? '' : `Dew point ${fmt('temp', dew)}${unitText.temp()}`]);
  items.push(['HUMIDITY', o ? o.hum.toFixed(0) + '<small>%</small>' : '--', '']);
  items.push(['WIND', speed == null ? '--' : fmt('wind', speed) + `<small>${units.wind}</small>`, o ? `Gust ${fmt('wind', o.gust)} · Lull ${fmt('wind', o.lull)}` : '']);
  items.push(['DIRECTION', dir == null ? '--' : compassSvg(dir) + Math.round(dir) + '<small>° ' + dirName(dir) + '</small>', '']);

  const trend = pressureTrend();
  items.push(['PRESSURE', o ? fmt('pressure', o.pressure) + `<small>${units.pressure}</small>` : '--', trend]);
  const rain = rows.length ? sumSince('rain', 86400) : null;
  items.push(['RAIN 24H', rain == null ? '--' : fmt('rain', rain) + `<small>${units.rain}</small>`, o ? `Last minute ${fmt('rain', o.rain)} ${units.rain}` : '']);
  items.push(['UV INDEX', o ? o.uv.toFixed(1) : '--', o ? `Solar ${Math.round(o.solar)} W/m² · ${Math.round(o.lux)} lux` : '']);
  const strikes = rows.length ? sumSince('lightning', 86400) : null;
  items.push(['LIGHTNING 24H', strikes == null ? '--' : strikes + '<small> strikes</small>', o && o.lightCount > 0 ? `Latest ${o.lightDist} km away` : '']);

  $('#status').innerHTML = items.map(([label, value, sub]) =>
    `<div class="status-item"><div class="section-label">${label}</div><span class="status-value">${value}</span><span class="status-sub">${sub || '&nbsp;'}</span></div>`).join('');
}

function pressureTrend() {
  if (!rows.length) return '';
  const now = rows[rows.length - 1];
  const target = now.t - 3 * 3600;
  const past = rows.find(r => r.t >= target);
  if (!past || now.t - past.t < 2 * 3600) return '';
  const d = now.press - past.press;
  const arrow = Math.abs(d) < 0.5 ? '→' : d > 0 ? '▲' : '▼';
  const shown = conv.pressure(d);
  return `${arrow} ${shown >= 0 ? '+' : ''}${shown.toFixed(decimals.pressure())} in 3h`;
}

function renderHeader() {
  const dot = $('#liveDot'), text = $('#liveText');
  const age = current ? current.packetAgeS : -1;
  dot.className = 'dot';
  if (age >= 0 && age < 15) { dot.classList.add('live'); text.textContent = 'LIVE'; }
  else if (age >= 0 && age < 180) { dot.classList.add('stale'); text.textContent = 'DELAYED'; }
  else { text.textContent = age < 0 ? 'NO DATA' : 'STALE'; }

  const o = current && current.obs;
  const pct = o ? Math.max(0, Math.min(100, o.battery / 2.77 * 100)) : null;
  $('#batteryFill').style.width = (pct || 0) + '%';
  $('#batteryText').textContent = pct == null ? '--' : Math.round(pct) + '%';
  $('#batteryText').parentElement.title = o ? o.battery.toFixed(2) + ' V' : 'Battery unavailable';
  $('#obsAge').textContent = o ? '· UPDATED ' + (o.ageS < 90 ? o.ageS + 's AGO' : Math.round(o.ageS / 60) + ' MIN AGO') : '';

  const banner = $('#banner');
  if (current && current.defaultPassword) {
    banner.hidden = false;
    banner.textContent = 'The password is still the default (admin). Change it under Settings.';
  } else if (current && age < 0 && current.uptimeS > 120) {
    banner.hidden = false;
    banner.textContent = 'No Tempest broadcasts received yet. The hub and this device must be on the same network.';
  } else {
    banner.hidden = true;
  }
  $('#footInfo').textContent = current ? `${current.serial || 'no station'} · Wi-Fi ${current.rssi ?? '--'} dBm · up ${Math.floor(current.uptimeS / 3600)}h` : '';
  $('#footVer').textContent = current ? 'v' + current.version : '';
}

// ---- Charts ----

const CHARTS = [
  { id: 'temp', title: 'Temperature', kind: 'temp', series: [{ name: 'Temperature', key: 'temp', color: '#52b9ff' }, { name: 'Dew point', key: 'dew', color: '#6de0e3' }] },
  { id: 'hum', title: 'Humidity', kind: 'pct', min: 0, max: 100, series: [{ name: 'Humidity', key: 'hum', color: '#6de0e3', area: true }] },
  { id: 'wind', title: 'Wind', kind: 'wind', min: 0, series: [{ name: 'Average', key: 'wind', color: '#52b9ff' }, { name: 'Gust', key: 'gust', color: '#ffb86b' }] },
  { id: 'press', title: 'Pressure', kind: 'pressure', series: [{ name: 'Pressure', key: 'press', color: '#8ab4ff' }] },
  { id: 'rain', title: 'Rain', kind: 'rain', wide: true, bars: true, series: [{ name: 'Rain', key: 'rain', color: '#52b9ff' }] },
  { id: 'solar', title: 'Solar radiation', kind: 'solar', min: 0, wide: true, series: [{ name: 'Solar', key: 'solar', color: '#ffb86b', area: true }] },
];

function buildCharts() {
  $('#charts').innerHTML = CHARTS.map(c => `
    <div class="panel-card chart-card${c.wide ? ' wide' : ''}" id="card-${c.id}">
      <div class="card-heading"><h2>${c.title}</h2><span class="current" id="cur-${c.id}"></span></div>
      <div class="chart-wrap">
        <div class="y-axis"></div>
        <svg viewBox="0 0 ${SVG_W} ${SVG_H}" preserveAspectRatio="none"></svg>
        <div class="hover-line"></div><div class="chart-tooltip"></div>
      </div>
      <div class="chart-axis"></div>
      <div class="legend">${c.series.map(s => `<span style="--key:${s.color}">${s.name}</span>`).join('')}</div>
    </div>`).join('');
  CHARTS.forEach(c => {
    const wrap = $(`#card-${c.id} .chart-wrap`);
    wrap.addEventListener('pointermove', e => hover(c, wrap, e));
    wrap.addEventListener('pointerleave', () => {
      $('.hover-line', wrap).style.display = 'none';
      $('.chart-tooltip', wrap).style.display = 'none';
    });
  });
}

function parseRows(data) {
  return data.rows.map(r => {
    const temp = r[1] / 100, hum = r[2] / 10;
    return { t: r[0], temp, hum, dew: dewPoint(temp, hum), press: r[3] / 10, wind: r[4] / 100, gust: r[5] / 100, dir: r[6], rain: r[7] / 100, solar: r[8], uv: r[9] / 10, lux: r[10], batt: r[11] / 1000, lightning: r[12] || 0 };
  });
}

function timeLabel(t, long) {
  const d = new Date(t * 1000);
  const hm = d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
  return long ? d.toLocaleDateString([], { weekday: 'short' }) + ' ' + hm : hm;
}

// Bars: sums per bin so a week of 5-minute rain samples stays readable.
function binRain(list, t0, t1, bins) {
  const step = (t1 - t0) / bins;
  const out = Array.from({ length: bins }, (_, i) => [t0 + step * (i + 0.5), 0]);
  for (const r of list) {
    const i = Math.min(bins - 1, Math.floor((r.t - t0) / step));
    if (i >= 0) out[i][1] += r.rain;
  }
  return { pts: out, step };
}

const chartState = {};

function drawChart(c, t0, t1) {
  const card = $(`#card-${c.id}`);
  const wrap = $('.chart-wrap', card);
  const svg = $('svg', wrap);
  const list = rows.filter(r => r.t >= t0);
  const series = [];
  let step = 300;
  if (c.bars) {
    const b = binRain(list, t0, t1, rangeHours <= 6 ? 36 : rangeHours <= 24 ? 48 : 56);
    step = b.step;
    series.push({ ...c.series[0], pts: b.pts });
  } else {
    for (const s of c.series) series.push({ ...s, pts: list.filter(r => r[s.key] != null).map(r => [r.t, r[s.key]]) });
  }
  chartState[c.id] = { series, step, t0, t1 };

  const all = series.flatMap(s => s.pts.map(p => p[1]));
  $('.empty', wrap)?.remove();
  if (!all.length) {
    svg.innerHTML = '';
    $('.y-axis', wrap).innerHTML = '';
    $('.chart-axis', card).innerHTML = '';
    $('#cur-' + c.id).textContent = '';
    wrap.insertAdjacentHTML('beforeend', '<div class="empty">No data yet</div>');
    return;
  }
  let lo = Math.min(...all), hi = Math.max(...all);
  const minSpan = { temp: 2, wind: 2, pressure: 1, rain: 0.2, solar: 100, pct: 10 }[c.kind] || 1;
  const floorFixed = c.min != null || c.bars;
  if (c.min != null) lo = c.min;
  else if (c.bars) lo = 0;
  if (c.max != null) {
    hi = c.max;
  } else if (floorFixed) {
    hi = Math.max(hi, lo + minSpan);
    hi += (hi - lo) * 0.08;
  } else {
    if (hi - lo < minSpan) {
      const mid = (hi + lo) / 2;
      lo = mid - minSpan / 2;
      hi = mid + minSpan / 2;
    }
    const pad = (hi - lo) * 0.08;
    lo -= pad;
    hi += pad;
  }
  const y = v => SVG_H - (v - lo) / (hi - lo) * SVG_H;
  const x = t => (t - t0) / (t1 - t0) * SVG_W;

  let html = '';
  for (const s of series) {
    if (c.bars) {
      const bw = Math.max(2, SVG_W / s.pts.length * 0.7);
      html += s.pts.filter(p => p[1] > 0).map(p => `<rect x="${(x(p[0]) - bw / 2).toFixed(1)}" y="${y(p[1]).toFixed(1)}" width="${bw.toFixed(1)}" height="${(SVG_H - y(p[1])).toFixed(1)}" fill="${s.color}" opacity=".85"/>`).join('');
      continue;
    }
    let d = '', area = '', prev = null;
    for (const p of s.pts) {
      const px = x(p[0]).toFixed(1), py = y(p[1]).toFixed(1);
      if (prev === null || p[0] - prev > step * 3) {
        if (s.area && prev !== null) area += `L${x(prev).toFixed(1)} ${SVG_H}Z`;
        d += `M${px} ${py}`;
        area += `M${px} ${SVG_H}L${px} ${py}`;
      } else {
        d += `L${px} ${py}`;
        area += `L${px} ${py}`;
      }
      prev = p[0];
    }
    if (s.area && prev !== null) area += `L${x(prev).toFixed(1)} ${SVG_H}Z`;
    if (s.area) html += `<path d="${area}" fill="${s.color}" opacity=".16"/>`;
    html += `<path d="${d}" fill="none" stroke="${s.color}" stroke-width="2" stroke-linejoin="round"/>`;
  }
  svg.innerHTML = html;

  const tick = (hi - lo) / 3;
  const yl = [hi, hi - tick, hi - 2 * tick, lo].map(v => {
    const shown = c.kind === 'pct' ? v.toFixed(0) : fmt(c.kind, v);
    return `<span>${shown}</span>`;
  });
  $('.y-axis', wrap).innerHTML = yl.join('');
  const long = rangeHours > 24;
  $('.chart-axis', card).innerHTML = [0, 0.25, 0.5, 0.75, 1].map(f => `<span>${timeLabel(t0 + (t1 - t0) * f, long)}</span>`).join('');

  const last = list[list.length - 1];
  const curText = {
    temp: () => last && fmt('temp', last.temp) + unitText.temp(),
    hum: () => last && last.hum.toFixed(0) + '%',
    wind: () => last && fmt('wind', last.wind) + unitText.wind(),
    press: () => last && fmt('pressure', last.press) + unitText.pressure(),
    rain: () => fmt('rain', all.reduce((a, b) => a + b, 0)) + unitText.rain() + ' total',
    solar: () => last && Math.round(last.solar) + ' W/m²',
  }[c.id];
  $('#cur-' + c.id).textContent = curText() || '';
}

function nearest(pts, t) {
  let lo = 0, hi = pts.length - 1;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (pts[mid][0] < t) lo = mid + 1; else hi = mid;
  }
  if (lo > 0 && Math.abs(pts[lo - 1][0] - t) < Math.abs(pts[lo][0] - t)) lo--;
  return pts[lo];
}

function hover(c, wrap, e) {
  const st = chartState[c.id];
  if (!st || !st.series[0].pts.length) return;
  const rect = wrap.getBoundingClientRect();
  const f = Math.min(1, Math.max(0, (e.clientX - rect.left) / rect.width));
  const t = st.t0 + f * (st.t1 - st.t0);
  const line = $('.hover-line', wrap), tip = $('.chart-tooltip', wrap);
  const rowsHtml = [];
  let shownT = null;
  for (const s of st.series) {
    const p = nearest(s.pts, t);
    if (!p || Math.abs(p[0] - t) > st.step * 1.5) continue;
    shownT = p[0];
    const v = c.kind === 'pct' ? p[1].toFixed(0) + '%' : c.kind === 'solar' ? Math.round(p[1]) + ' W/m²' : fmt(c.kind, p[1]) + unitText[c.kind]();
    rowsHtml.push(`<div style="color:${s.color}">${s.name}: ${v}</div>`);
  }
  line.style.display = tip.style.display = 'block';
  line.style.left = f * 100 + '%';
  tip.style.left = f < 0.6 ? `calc(${f * 100}% + 12px)` : '';
  tip.style.right = f < 0.6 ? '' : `calc(${(1 - f) * 100}% + 12px)`;
  tip.innerHTML = `<strong>${shownT == null ? timeLabel(t, rangeHours > 24) : timeLabel(shownT, true)}</strong>` + (rowsHtml.join('') || 'No data');
}

function renderCharts() {
  const t1 = Date.now() / 1000;
  const t0 = t1 - rangeHours * 3600;
  CHARTS.forEach(c => drawChart(c, t0, t1));
}

// ---- Data loading ----

async function loadCurrent() {
  try {
    current = await (await api('/api/current')).json();
    renderHeader();
    renderStatus();
  } catch (e) { /* login or network error: retried on the next poll */ }
}

async function loadHistory() {
  try {
    const hours = Math.max(rangeHours, 24);
    const data = await (await api('/api/history?hours=' + hours)).json();
    rows = parseRows(data);
    // Rows carry lightning counts, so the 24h strike total comes straight from history.
    renderCharts();
    renderStatus();
  } catch (e) { /* retried on the next refresh */ }
}

// ---- Settings ----

function renderRange() {
  const seg = $('#range');
  seg.innerHTML = RANGES.map(([l, h]) => `<button class="action-button${h === rangeHours ? ' on' : ''}" data-h="${h}">${l}</button>`).join('');
  seg.querySelectorAll('button').forEach(b => b.addEventListener('click', () => {
    rangeHours = Number(b.dataset.h);
    localStorage.setItem('wx.range', rangeHours);
    renderRange();
    loadHistory();
  }));
}

function renderUnits() {
  $('#units').innerHTML = Object.keys(UNIT_OPTIONS).map(k =>
    `<span class="muted">${UNIT_LABELS[k]}</span><span class="seg">${UNIT_OPTIONS[k].map(u => `<button class="action-button${units[k] === u ? ' on' : ''}" data-k="${k}" data-u="${u}">${u}</button>`).join('')}</span>`).join('');
  $('#units').querySelectorAll('button').forEach(b => b.addEventListener('click', () => {
    units[b.dataset.k] = b.dataset.u;
    localStorage.setItem('wx.units', JSON.stringify(units));
    renderUnits();
    renderStatus();
    renderCharts();
  }));
}

function closeMenu() {
  $('#appMenu').hidden = true;
  $('#menuBtn').setAttribute('aria-expanded', 'false');
}

function closePanel() {
  if (panelTimer) clearInterval(panelTimer);
  panelTimer = null;
  activePanel = null;
  document.querySelectorAll('.overlay:not(#login)').forEach(panel => { panel.hidden = true; });
}

function renderInfoTable(container, rows) {
  const table = document.createElement('table');
  const body = document.createElement('tbody');
  for (const [label, value] of rows.filter(([, value]) => value != null && value !== '')) {
    const row = document.createElement('tr');
    const heading = document.createElement('th');
    const cell = document.createElement('td');
    heading.scope = 'row';
    heading.textContent = label;
    cell.textContent = String(value);
    row.append(heading, cell);
    body.append(row);
  }
  table.append(body);
  container.replaceChildren(table);
}

function showPanelError(container, error) {
  container.textContent = error.message === 'unauthorized' ? '' : error.message;
  container.classList.toggle('error', error.message !== 'unauthorized');
}

async function loadNetwork() {
  const content = $('#networkContent');
  try {
    const res = await api('/api/network');
    if (!res.ok) throw new Error((await res.text()) || 'Could not load network information.');
    const n = await res.json();
    content.classList.remove('error', 'muted');
    renderInfoTable(content, [
      ['Status', n.connected ? 'Connected' : 'Not connected'],
      ['Wi-Fi network', n.ssid],
      ['Signal', n.rssi != null ? `${n.rssi} dBm` : null],
      ['Channel', n.channel],
      ['IP address', n.ip],
      ['Subnet mask', n.netmask],
      ['Gateway', n.gateway],
      ['DNS', n.dns],
      ['Hostname', n.hostname],
      ['MAC address', n.mac],
    ]);
  } catch (error) {
    showPanelError(content, error);
  }
}

function formatUptime(seconds) {
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor(seconds % 86400 / 3600);
  const minutes = Math.floor(seconds % 3600 / 60);
  return `${days ? `${days}d ` : ''}${hours}h ${minutes}m`;
}

const formatKB = bytes => `${(bytes / 1024).toFixed(1)} KB`;
const formatMB = bytes => `${(bytes / 1048576).toFixed(bytes % 1048576 ? 1 : 0)} MB`;

function renderHardware(s) {
  const content = $('#hardwareContent');
  const groups = [
    ['Chip', [
      ['Model', `${s.chip} rev ${s.chipRevision}`],
      ['Cores', s.cores],
      ['CPU speed', `${s.cpuMHz} MHz`],
      ['Temperature', s.tempC != null ? `${s.tempC.toFixed(1)} °C (chip die)` : null],
      ['Uptime', formatUptime(s.uptimeS)],
      ['Last reset', s.resetReason],
    ]],
    ['Memory', [
      ['Free RAM', `${formatKB(s.heapFree)} of ${formatKB(s.heapTotal)}`],
      ['Lowest free since boot', formatKB(s.heapMinFree)],
      ['Largest free block', formatKB(s.heapLargestBlock)],
      ['PSRAM free', s.psramTotal ? `${formatMB(s.psramFree)} of ${formatMB(s.psramTotal)}` : null],
      ['Web server stack free', `${formatKB(s.httpStackFree)} (lowest)`],
    ]],
    ['Storage and firmware', [
      ['Flash size', formatMB(s.flashSize)],
      ['Running slot', s.otaSlot ? `${s.otaSlot} (${formatMB(s.otaSlotSize)})` : null],
      ['Settings storage', s.nvsTotal ? `${s.nvsUsed} of ${s.nvsTotal} entries used` : null],
      ['Firmware', s.version],
      ['ESP-IDF', s.idfVersion],
    ]],
    ['Wi-Fi reliability', [
      ['Disconnects since boot', s.wifiDisconnects],
      ['Last disconnect reason', s.wifiDisconnects ? s.wifiLastReason : null],
    ]],
  ];
  const sections = groups.map(([title, rows]) => {
    const section = document.createElement('section');
    section.className = 'info-section';
    const heading = document.createElement('div');
    heading.className = 'section-label';
    heading.textContent = title;
    const table = document.createElement('div');
    renderInfoTable(table, rows);
    section.append(heading, table);
    return section;
  });
  content.classList.remove('error', 'muted');
  content.replaceChildren(...sections);
}

async function loadHardware() {
  const content = $('#hardwareContent');
  try {
    const res = await api('/api/system');
    if (!res.ok) throw new Error((await res.text()) || 'Could not load hardware information.');
    renderHardware(await res.json());
  } catch (error) {
    showPanelError(content, error);
  }
}

function openPanel(name) {
  closeMenu();
  closePanel();
  activePanel = name;
  const panel = $('#' + name);
  panel.hidden = false;
  if (name === 'settings') {
    $('#credentialUser').value = username;
    $('#about').textContent = current ? `Firmware ${current.version}, built ${current.built}` : '';
    return;
  }
  const content = $('#' + name + 'Content');
  content.classList.remove('error');
  content.classList.add('muted');
  content.textContent = 'Loading...';
  const refresh = name === 'network' ? loadNetwork : loadHardware;
  refresh();
  panelTimer = setInterval(refresh, 5000);
}

$('#menuBtn').addEventListener('click', () => {
  const menu = $('#appMenu');
  menu.hidden = !menu.hidden;
  $('#menuBtn').setAttribute('aria-expanded', String(!menu.hidden));
});
$('#appMenu').addEventListener('click', e => {
  const button = e.target.closest('[data-panel]');
  if (button) openPanel(button.dataset.panel);
});
$('#menuWrap').addEventListener('focusout', e => {
  if (!$('#menuWrap').contains(e.relatedTarget)) closeMenu();
});
document.addEventListener('click', e => {
  if (!$('#menuWrap').contains(e.target)) closeMenu();
});
document.addEventListener('keydown', e => {
  if (e.key === 'Escape') {
    closeMenu();
    closePanel();
  }
});
document.querySelectorAll('[data-close-panel]').forEach(button => button.addEventListener('click', closePanel));
document.querySelectorAll('.overlay:not(#login)').forEach(panel => panel.addEventListener('click', e => {
  if (e.target === panel) closePanel();
}));
$('#logoutBtn').addEventListener('click', () => {
  closeMenu();
  showLogin();
});

$('#credentialsForm').addEventListener('submit', async e => {
  e.preventDefault();
  const nextUsername = $('#credentialUser').value;
  const password = $('#credentialPassword').value;
  try {
    const res = await api('/api/password', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username: nextUsername, password }),
    });
    if (!res.ok) {
      $('#credentialsMsg').textContent = (await res.text()) || 'Could not change account credentials.';
      return;
    }
    username = nextUsername;
    auth = encodeCredentials(username, password);
    sessionStorage.setItem('wx.auth', auth);
    $('#credentialPassword').value = '';
    $('#credentialsMsg').textContent = 'Account credentials changed.';
    loadCurrent();
  } catch (err) {
    if (err.message !== 'unauthorized') $('#credentialsMsg').textContent = 'Could not change account credentials: ' + err.message;
  }
});

function uploadFirmware(file, onProgress) {
  return new Promise((resolve, reject) => {
    const request = new XMLHttpRequest();
    request.open('POST', '/api/firmware');
    request.setRequestHeader('Authorization', 'Basic ' + auth);
    request.upload.addEventListener('progress', e => {
      if (e.lengthComputable) onProgress(e.loaded, e.total);
    });
    request.addEventListener('load', () => {
      if (request.status === 401) {
        showLogin();
        reject(new Error('unauthorized'));
        return;
      }
      resolve({ ok: request.status >= 200 && request.status < 300, text: request.responseText });
    });
    request.addEventListener('error', () => reject(new Error('network error')));
    request.addEventListener('abort', () => reject(new Error('upload canceled')));
    request.send(file);
  });
}

$('#fwBtn').addEventListener('click', async () => {
  const file = $('#fwFile').files[0];
  const msg = $('#fwMsg');
  if (!file) { msg.textContent = 'Choose a firmware .bin first.'; return; }
  const progress = $('#fwProgress');
  const button = $('#fwBtn');
  progress.value = 0;
  progress.hidden = false;
  button.disabled = true;
  msg.textContent = 'Uploading 0%. Do not power off...';
  try {
    const res = await uploadFirmware(file, (loaded, total) => {
      const percent = Math.floor(loaded / total * 100);
      progress.value = percent;
      msg.textContent = `Uploading ${percent}%. Do not power off...`;
    });
    if (!res.ok) { msg.textContent = 'Update failed: ' + (res.text || 'server returned an error'); return; }
    progress.value = 100;
    msg.textContent = 'Installed. The device is restarting; reloading shortly.';
    setTimeout(() => location.reload(), 20000);
  } catch (err) {
    if (err.message !== 'unauthorized') msg.textContent = 'Upload failed: ' + err.message;
  } finally {
    button.disabled = false;
  }
});

// ---- Start ----

function start() {
  timers.forEach(clearInterval);
  loadCurrent();
  loadHistory();
  timers = [setInterval(loadCurrent, 3000), setInterval(loadHistory, 60000)];
}

buildCharts();
renderRange();
renderUnits();
if (auth) start(); else showLogin();

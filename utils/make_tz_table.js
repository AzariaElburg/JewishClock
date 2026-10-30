// Builds Data/tz.csv: every IANA time zone -> the POSIX TZ rule the clock's C
// library needs ("Europe/London" -> "GMT0BST,M3.5.0/1,M10.5.0"). The location
// services only give the zone's name; the clock can't derive the rule itself.
// Source: Node's own time-zone data (Intl, the same ICU data browsers use).
// Every rule is checked against that data hour by hour for CHECK_YEARS; a
// zone whose switches don't follow a fixed weekday rule (e.g. Israel's "Friday
// before the last Sunday", some years) is left out and reported - the clock
// keeps its built-in Israel rule for Israel (DEFAULT_TZ_POSIX).
// Run when the time-zone rules of the world change: node utils/make_tz_table.js
'use strict';
const fs = require('fs');
const path = require('path');

const RULE_YEAR = 2027;                       // the year the rule is read from
const CHECK_YEARS = [2026, 2035];             // ... and checked against, inclusive
const OUT = path.join(__dirname, '..', 'Data', 'tz.csv');

// --- what Intl says ---------------------------------------------------------
const fmtCache = {};
function fmt(zone) {
  return fmtCache[zone] || (fmtCache[zone] = new Intl.DateTimeFormat('en-US', {
    timeZone: zone, hourCycle: 'h23', year: 'numeric', month: 'numeric', day: 'numeric',
    hour: 'numeric', minute: 'numeric', second: 'numeric' }));
}
// UTC offset in minutes at instant t (ms).
function offsetAt(zone, t) {
  const p = {};
  for (const x of fmt(zone).formatToParts(new Date(t))) p[x.type] = x.value;
  const asUtc = Date.UTC(+p.year, +p.month - 1, +p.day, +p.hour, +p.minute, +p.second);
  return Math.round((asUtc - Math.floor(t / 1000) * 1000) / 60000);
}
function abbreviation(zone, t, offMin) {   // a name like "BST" if a locale has one, else "<+01>"
  for (const loc of ['en-US', 'en-GB']) {
    const name = new Intl.DateTimeFormat(loc, { timeZone: zone, timeZoneName: 'short' })
      .formatToParts(new Date(t)).find(x => x.type === 'timeZoneName').value;
    if (/^[A-Z]{3,5}$/.test(name)) return name;
  }
  const a = Math.abs(offMin), s = offMin < 0 ? '-' : '+';
  return '<' + s + String(Math.floor(a / 60)).padStart(2, '0') + (a % 60 ? String(a % 60).padStart(2, '0') : '') + '>';
}

// --- POSIX pieces ------------------------------------------------------------
// POSIX offsets are west-positive: UTC+2 -> "-2".
function posixOffset(offMin) {
  const w = -offMin, a = Math.abs(w);
  return (w < 0 ? '-' : '') + Math.floor(a / 60) + (a % 60 ? ':' + String(a % 60).padStart(2, '0') : '');
}
function posixTime(min) {   // minutes after local midnight -> "/h[:mm]", "" for the default 2:00
  if (min === 120) return '';
  const sign = min < 0 ? '-' : '', a = Math.abs(min);
  return '/' + sign + Math.floor(a / 60) + (a % 60 ? ':' + String(a % 60).padStart(2, '0') : '');
}
// The day `rule` (month m, week w 1..5 = 5 meaning last, weekday d) falls on in year y.
function ruleDay(y, m, w, d) {
  const first = new Date(Date.UTC(y, m - 1, 1)).getUTCDay();
  let day = 1 + ((d - first + 7) % 7) + (w - 1) * 7;
  const dim = new Date(Date.UTC(y, m, 0)).getUTCDate();
  while (day > dim) day -= 7;
  return day;
}

// --- read a year's switches ------------------------------------------------------
// [{ t: ms of the switch, from: offset before, to: offset after }]
function transitions(zone, year) {
  const out = [], H = 3600e3;
  let t = Date.UTC(year, 0, 1) - 14 * H, prev = offsetAt(zone, t);
  const end = Date.UTC(year + 1, 0, 1) + 14 * H;
  for (t += 6 * H; t <= end; t += 6 * H) {
    const o = offsetAt(zone, t);
    if (o === prev) continue;
    let lo = t - 6 * H, hi = t;   // binary search to the minute
    while (hi - lo > 60e3) { const mid = lo + Math.floor((hi - lo) / 120e3) * 60e3; if (offsetAt(zone, mid) === prev) lo = mid; else hi = mid; }
    out.push({ t: hi, from: prev, to: o });
    prev = o;
  }
  return out.filter(x => new Date(x.t + x.from * 60e3).getUTCFullYear() === year);
}

function buildRule(zone) {
  const tr = transitions(zone, RULE_YEAR);
  const mid = Date.UTC(RULE_YEAR, 0, 15), off = offsetAt(zone, mid);
  if (tr.length === 0) return { rule: abbreviation(zone, mid, off) + posixOffset(off) };
  if (tr.length !== 2) return { error: tr.length + ' switches in ' + RULE_YEAR };
  const std = Math.min(tr[0].from, tr[0].to), dst = Math.max(tr[0].from, tr[0].to);
  const on = tr.find(x => x.to === dst), off2 = tr.find(x => x.to === std);
  if (!on || !off2) return { error: 'switches are not one up, one down' };
  const part = (x, localOff) => {
    const local = new Date(x.t + localOff * 60e3);   // wall clock just before the switch
    const m = local.getUTCMonth() + 1, d = local.getUTCDay(), day = local.getUTCDate();
    const dim = new Date(Date.UTC(RULE_YEAR, m, 0)).getUTCDate();
    const w = day + 7 > dim ? 5 : Math.ceil(day / 7);
    let min = local.getUTCHours() * 60 + local.getUTCMinutes();
    return { m, w, d, min, text: 'M' + m + '.' + w + '.' + d + posixTime(min) };
  };
  const a = part(on, std), b = part(off2, dst);
  const stdT = mid, dstT = on.t + 3600e3;
  const stdName = abbreviation(zone, offsetAt(zone, stdT) === std ? stdT : off2.t + 3600e3, std);
  const dstName = abbreviation(zone, dstT, dst);
  const dstOff = dst - std === 60 ? '' : posixOffset(dst);
  return { rule: stdName + posixOffset(std) + dstName + dstOff + ',' + a.text + ',' + b.text,
           parsed: { std, dst, a, b } };
}

// --- check a rule against Intl, hour by hour ------------------------------------
function ruleOffsetAt(p, t) {
  if (!p) return null;
  const y = new Date(t + p.std * 60e3).getUTCFullYear();
  const at = (r, localOff) => Date.UTC(y, r.m - 1, ruleDay(y, r.m, r.w, r.d)) + r.min * 60e3 - localOff * 60e3;
  const start = at(p.a, p.std), end = at(p.b, p.dst);
  const inDst = start < end ? (t >= start && t < end) : (t >= start || t < end);   // south: across new year
  return inDst ? p.dst : p.std;
}
function check(zone, r) {
  const H = 3600e3;
  for (let y = CHECK_YEARS[0]; y <= CHECK_YEARS[1]; y++) {
    for (let t = Date.UTC(y, 0, 1, 0, 30); t < Date.UTC(y + 1, 0, 1); t += H) {
      const want = offsetAt(zone, t);
      const got = r.parsed ? ruleOffsetAt(r.parsed, t) : offsetAt(zone, Date.UTC(RULE_YEAR, 0, 15));
      if (want !== got) return new Date(t).toISOString().slice(0, 16) + 'Z: rule ' + got + ' min, real ' + want + ' min';
    }
  }
  return '';
}

// --- all zones -------------------------------------------------------------------
const zones = Intl.supportedValuesOf('timeZone');
const lines = [], skipped = [];
for (const z of zones) {
  const r = buildRule(z);
  const problem = r.error || check(z, r);
  if (problem) { skipped.push(z + ': ' + problem); continue; }
  lines.push(z + '\t' + r.rule);
}
fs.writeFileSync(OUT, '# zone<TAB>POSIX rule - made by utils/make_tz_table.js, checked ' + CHECK_YEARS.join('-') +
  ' against ICU ' + process.versions.icu + ' (tz ' + (process.versions.tz || '?') + ')\n' + lines.join('\n') + '\n');
console.log(lines.length + ' zones -> ' + OUT + ' (' + fs.statSync(OUT).size + ' bytes)');
console.log(skipped.length + ' left out (rule does not hold every year):');
for (const s of skipped) console.log('  ' + s);

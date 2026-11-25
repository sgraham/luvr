#!/usr/bin/env python3
"""Writes a static HTML page of the perf history in bench/history.json.

  python3 bench/history_html.py [--log bench/history.json] [--out out/history.html]

The page has a section per input and mode. Each has a small sparkline per
counter, with changes outside the noise marked, then a full width chart of one
selected counter that can be zoomed to a range of commits, and a table of every
commit, newest first, with the change from the previous measured commit.
Sections with fewer than two successful runs only show the table. The data is
embedded in the page, so it can be opened directly from disk.
"""

import argparse
import html
import json
import os

# Changes smaller than these are shown as noise. Instructions barely vary
# between runs, cycles and branch misses a little more. Times also use their
# own stddev, see delta() in the page.
NOISE_PCT = {"instructions": 0.1, "cycles": 0.75, "branch_misses": 1.0, "time": 1.0}

REPO_URL = "https://github.com/sgraham/luvr"


def cell(bench, tokens):
    if not bench["ok"]:
        return {"ok": False, "error": bench["error"].strip().split("\n")[-1][:300]}
    perf = bench["perf"]
    times = bench["times"]
    derived = bench["derived"]

    def counter(name):
        c = perf.get(name)
        return c["value"] if c else None

    return {
        "ok": True,
        "time": times["mean_s"],
        "time_sd": times["stddev_s"],
        "instructions": counter("instructions"),
        "cycles": counter("cycles"),
        "branch_misses": counter("branch-misses"),
        "ipc": derived.get("ipc"),
        "ghz": derived.get("ghz"),
        "mtok_s": derived.get("tokens_per_s", 0) / 1e6,
        "rss_mb": bench.get("max_rss_kb", 0) / 1024,
        "temp": [bench.get("temp_c_before", 0),
                 bench.get("temp_c_after", 0)],
        "tokens": tokens,
    }


def rows_from_log(log):
    rows = []
    for e in sorted(log["entries"], key=lambda e: e["date"]):
        cells = {}
        for b in e["benches"]:
            cells[f"{b['input']} {b['mode']}"] = cell(b, e["tokens"].get(b["input"], 0))
        m = e["machine"]
        rows.append({
            "commit": e["commit"],
            "short": e["short"],
            "date": e["date"][:10],
            "subject": e["subject"],
            "build_ok": e["build_ok"],
            "build_error": e["build_error"].strip().split("\n")[-1][:300],
            "clocks": f"boost {m.get('boost', '?')}, {m.get('energy_performance_preference', '?')}",
            "cells": cells,
        })
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--log", default="bench/history.json")
    ap.add_argument("--out", default="out/history.html")
    args = ap.parse_args()

    with open(args.log) as f:
        log = json.load(f)
    rows = rows_from_log(log)
    # Each input's parse before its il.
    keys = sorted({k
                   for r in rows
                   for k in r["cells"]},
                  key=lambda k: (k.split()[0], k.split()[1] != "parse"))
    latest = max(log["entries"], key=lambda e: e["date"]) if log["entries"] else None
    data = {
        "rows": rows,
        "keys": keys,
        "noise": NOISE_PCT,
        "repo": REPO_URL,
        "inputs": log["inputs"],
        "machine": latest["machine"] if latest else {},
        "toolchain": latest["toolchain"] if latest else {},
        "settings": latest["settings"] if latest else {},
    }
    # </script> can't appear inside the embedded JSON.
    blob = json.dumps(data).replace("</", "<\\/")
    page = TEMPLATE.replace("/*DATA*/", blob)
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w") as f:
        f.write(page)
    print(f"wrote {args.out}: {len(rows)} commits, {html.escape(', '.join(keys))}")


TEMPLATE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>luvr perf history</title>
<style>
:root {
  color-scheme: light;
  --surface-0: #f5f5f3;
  --surface-1: #fcfcfb;
  --border: #e2e1dc;
  --grid: #ecebe7;
  --text-primary: #0b0b0b;
  --text-secondary: #52514e;
  --text-muted: #7a7974;
  --series-1: #2a78d6;
  --good: #006300;
  --bad: #b42323;
}
@media (prefers-color-scheme: dark) {
  :root {
    color-scheme: dark;
    --surface-0: #121211;
    --surface-1: #1a1a19;
    --border: #33332f;
    --grid: #2a2a27;
    --text-primary: #f0efec;
    --text-secondary: #c3c2b7;
    --text-muted: #8d8c85;
    --series-1: #3987e5;
    --good: #3fbf3f;
    --bad: #ef7070;
  }
}
* { box-sizing: border-box; }
body {
  margin: 0;
  padding: 24px 32px 48px;
  background: var(--surface-0);
  color: var(--text-primary);
  font: 14px/1.45 system-ui, -apple-system, "Segoe UI", sans-serif;
}
h1 { font-size: 20px; margin: 0 0 4px; }
h2 { font-size: 16px; margin: 32px 0 12px; }
.meta { color: var(--text-secondary); font-size: 13px; }
.meta code { font-size: 12px; }
.tiles {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 10px;
}
@media (max-width: 760px) { .tiles { grid-template-columns: repeat(2, minmax(0, 1fr)); } }
.tile {
  background: var(--surface-1);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 8px 10px 6px;
  text-align: left;
  font: inherit;
  color: inherit;
  cursor: pointer;
}
.tile:hover { border-color: var(--text-muted); }
.tile.sel { border-color: var(--series-1); box-shadow: inset 0 0 0 1px var(--series-1); }
.tile .th { display: flex; justify-content: space-between; font-size: 12px; color: var(--text-secondary); }
.tile .tv { font-size: 16px; font-weight: 600; font-variant-numeric: tabular-nums; }
.tile svg { display: block; width: 100%; height: 40px; margin-top: 4px; }
.big {
  margin-top: 10px;
  background: var(--surface-1);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 10px 12px 6px;
  position: relative;
}
.bighead { display: flex; align-items: baseline; gap: 12px; font-size: 12px; color: var(--text-muted); }
.bighead b { font-size: 13px; color: var(--text-primary); }
.bighead button {
  font: inherit;
  color: var(--series-1);
  background: none;
  border: none;
  padding: 0;
  cursor: pointer;
}
.big svg { display: block; user-select: none; cursor: crosshair; }
.big svg text { fill: var(--text-muted); font-size: 10.5px; }
.tip {
  position: absolute;
  pointer-events: none;
  background: var(--surface-1);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 6px 8px;
  font-size: 12px;
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.12);
  max-width: 340px;
  display: none;
  z-index: 2;
}
.tip.pinned { pointer-events: auto; border-color: var(--series-1); }
.tip .sub { color: var(--text-secondary); }
.tip table { width: auto; margin-top: 4px; font-size: 12px; }
.tip td { padding: 1px 6px 1px 0; border: none; }
a { color: var(--series-1); text-decoration: none; }
a:hover { text-decoration: underline; }
details { margin-top: 4px; }
details summary { cursor: pointer; color: var(--text-secondary); font-size: 13px; }
.tablewrap {
  margin-top: 12px;
  overflow-x: auto;
  background: var(--surface-1);
  border: 1px solid var(--border);
  border-radius: 8px;
}
table { border-collapse: collapse; width: 100%; font-size: 12.5px; }
th, td { padding: 5px 7px; text-align: right; white-space: nowrap; }
th { color: var(--text-secondary); font-weight: 600; border-bottom: 1px solid var(--border); position: sticky; top: 0; background: var(--surface-1); }
td { border-bottom: 1px solid var(--grid); font-variant-numeric: tabular-nums; }
tr.hl td { background: var(--grid); }
td.l, th.l { text-align: left; }
td.subject { max-width: 240px; overflow: hidden; text-overflow: ellipsis; }
td.muted, span.muted { color: var(--text-muted); }
.good { color: var(--good); }
.bad { color: var(--bad); }
.fail { color: var(--text-muted); font-style: italic; }
code { font-family: ui-monospace, "SF Mono", Menlo, monospace; }
</style>
</head>
<body>
<h1>luvr perf history</h1>
<div class="meta" id="meta"></div>
<div id="sections"></div>
<script>
const DATA = /*DATA*/;

const METRICS = [
  {key: "time", label: "Time", scale: 1, unit: " s", dp: 3},
  {key: "instructions", label: "Instructions", scale: 1e9, unit: "G", dp: 3},
  {key: "cycles", label: "Cycles", scale: 1e9, unit: "G", dp: 3},
  {key: "branch_misses", label: "Branch misses", scale: 1e6, unit: "M", dp: 1},
];
for (const m of METRICS) m.fmt = (v, dp = m.dp) => (v / m.scale).toFixed(dp) + m.unit;

const NS = "http://www.w3.org/2000/svg";

// Enough decimals that neighbouring axis ticks, `step` apart, don't print the
// same.
function axisDp(m, step) {
  return Math.max(m.dp, Math.min(6, Math.ceil(-Math.log10(step / m.scale)) + 1));
}

function esc(s) {
  return String(s).replace(/[&<>"]/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;"})[c]);
}

function commitLink(r) {
  return `<a href="${esc(DATA.repo)}/commit/${esc(r.commit)}" target="_blank" rel="noopener"><code>${esc(r.short)}</code></a>`;
}

function rowId(key, r) {
  return "row-" + key.replace(/\W/g, "_") + "-" + r.short;
}

// The change from prev to cur in percent, and whether it's outside the noise.
// Lower is better for every metric here.
function delta(metric, cur, prev) {
  if (!cur || !prev || !cur.ok || !prev.ok || cur[metric] == null || !prev[metric]) return null;
  const pct = (cur[metric] - prev[metric]) / prev[metric] * 100;
  let noise = DATA.noise[metric];
  if (metric === "time") {
    noise = Math.max(noise, 200 * Math.max(cur.time_sd, prev.time_sd) / prev.time);
  }
  return {pct, cls: Math.abs(pct) < noise ? "muted" : pct > 0 ? "bad" : "good"};
}

function deltaHtml(d) {
  if (!d) return '<span class="muted">-</span>';
  const arrow = d.cls === "bad" ? "▲ " : d.cls === "good" ? "▼ " : "";
  const sign = d.pct > 0 ? "+" : "";
  return `<span class="${d.cls}">${arrow}${sign}${d.pct.toFixed(2)}%</span>`;
}

// The successful measurements of one metric, each with its change from the
// previous successful one, which may be several commits earlier.
function points(key, metric) {
  const pts = [];
  let prev = null;
  DATA.rows.forEach((r, i) => {
    const c = r.cells[key];
    if (!c || !c.ok || c[metric] == null) return;
    const p = {i, v: c[metric], r, c, d: delta(metric, c, prev && prev.c)};
    pts.push(p);
    prev = p;
  });
  return pts;
}

// A gap in the line where a commit failed or wasn't measured.
function linePath(pts, x, y) {
  let d = "", prevI = -2;
  for (const p of pts) {
    d += (p.i === prevI + 1 ? "L" : "M") + x(p.i).toFixed(1) + "," + y(p.v).toFixed(1);
    prevI = p.i;
  }
  return d;
}

function vrange(pts) {
  let lo = Math.min(...pts.map(p => p.v)), hi = Math.max(...pts.map(p => p.v));
  const pad = (hi - lo) * 0.1 || hi * 0.01 || 1;
  return [lo - pad, hi + pad];
}

function renderMeta() {
  const m = DATA.machine, t = DATA.toolchain, s = DATA.settings;
  const inputs = Object.values(DATA.inputs).map(i =>
    `${esc(i.name)} (${(i.bytes / 1e6).toFixed(0)} MB, ${i.lines.toLocaleString()} lines, md5 <code>${esc(i.md5.slice(0, 8))}</code>)`).join(", ");
  document.getElementById("meta").innerHTML =
    `${DATA.rows.length} commits. ${esc(m.cpu_model || "")}, CPU ${s.cpu} pinned, boost ${esc(m.boost)}, ` +
    `${esc(m.energy_performance_preference || m.governor || "")}. ${esc(t.clang || "")}. ` +
    `perf stat &times;${s.perf_runs}, hyperfine &times;${s.hyperfine_runs} after ${s.hyperfine_warmup} warmups.<br>` +
    `Inputs: ${inputs}. Marked points are changes outside the noise.`;
}

// Draws a significant change as a dot in its direction's colour.
function marks(pts, x, y, r) {
  let s = "";
  for (const p of pts) {
    if (p.d && p.d.cls !== "muted") {
      s += `<circle cx="${x(p.i).toFixed(1)}" cy="${y(p.v).toFixed(1)}" r="${r}" fill="var(--${p.d.cls})" stroke="var(--surface-1)" stroke-width="1"/>`;
    }
  }
  return s;
}

function tile(sec, metric) {
  const pts = points(sec.key, metric.key);
  const btn = document.createElement("button");
  btn.className = "tile";
  if (!pts.length) {
    btn.innerHTML = `<div class="th"><span>${metric.label}</span></div><div class="tv muted">-</div>`;
    return btn;
  }
  const last = pts[pts.length - 1];
  const H = 40;
  const [lo, hi] = vrange(pts);
  const n = Math.max(DATA.rows.length - 1, 1);
  const y = v => 3 + (H - 6) * (1 - (v - lo) / (hi - lo));
  btn.innerHTML = `<div class="th"><span>${metric.label}</span>${deltaHtml(last.d)}</div>` +
    `<div class="tv">${metric.fmt(last.v)}</div><svg></svg>`;
  const svg = btn.querySelector("svg");
  sec.redraw.push(() => {
    const W = svg.clientWidth || 240;
    const x = i => 3 + (W - 6) * i / n;
    svg.setAttribute("viewBox", `0 0 ${W} ${H}`);
    svg.innerHTML = `<path d="${linePath(pts, x, y)}" fill="none" stroke="var(--series-1)" stroke-width="1.5" stroke-linejoin="round"/>` +
      marks(pts, x, y, 2.5);
  });
  btn.addEventListener("click", () => {
    sec.metric = metric.key;
    sec.pinned = null;
    renderSection(sec);
  });
  btn.dataset.metric = metric.key;
  return btn;
}

function tipHtml(sec, p, metric) {
  let s = `<b>${metric.fmt(p.v)}</b> ${deltaHtml(p.d)}<br>${commitLink(p.r)} ${esc(p.r.date)}<br><span class="sub">${esc(p.r.subject)}</span><table>`;
  for (const m of METRICS) {
    const q = sec.pts[m.key].find(q => q.i === p.i);
    if (q) s += `<tr><td class="l muted">${m.label}</td><td>${m.fmt(q.v)}</td><td>${deltaHtml(q.d)}</td></tr>`;
  }
  s += "</table>";
  if (sec.pinned) {
    s += `<a href="${esc(DATA.repo)}/commit/${esc(p.r.commit)}" target="_blank" rel="noopener">diff on GitHub</a> &middot; <a href="#${rowId(sec.key, p.r)}">table row</a>`;
  }
  return s;
}

function bigChart(sec) {
  const metric = METRICS.find(m => m.key === sec.metric);
  const all = sec.pts[metric.key];
  const box = sec.big;
  const zoomed = sec.i0 > 0 || sec.i1 < DATA.rows.length - 1;
  box.innerHTML = `<div class="bighead"><b>${metric.label}</b><span>drag to zoom, click a point to pin it</span>` +
    (zoomed ? `<button>reset zoom</button>` : "") + `</div>`;
  if (zoomed) {
    box.querySelector("button").addEventListener("click", () => {
      sec.i0 = 0;
      sec.i1 = DATA.rows.length - 1;
      bigChart(sec);
    });
  }
  const pts = all.filter(p => p.i >= sec.i0 && p.i <= sec.i1);
  if (!pts.length) return;
  const W = Math.max(box.clientWidth - 24, 300), H = 300, L = 64, R = 12, T = 10, B = 26;
  const span = Math.max(sec.i1 - sec.i0, 1);
  const x = i => L + (W - L - R) * (i - sec.i0) / span;
  const xinv = px => sec.i0 + (px - L) / (W - L - R) * span;
  const [lo, hi] = vrange(pts);
  const y = v => T + (H - T - B) * (1 - (v - lo) / (hi - lo));
  let s = "";
  const dp = axisDp(metric, (hi - lo) / 5);
  for (let k = 0; k <= 5; k++) {
    const v = lo + (hi - lo) * k / 5;
    s += `<line x1="${L}" x2="${W - R}" y1="${y(v)}" y2="${y(v)}" stroke="var(--grid)"/>`;
    s += `<text x="${L - 6}" y="${y(v) + 3.5}" text-anchor="end">${metric.fmt(v, dp)}</text>`;
  }
  const step = Math.max(1, Math.ceil(span / ((W - L - R) / 64)));
  for (let i = sec.i0; i <= sec.i1; i += step) {
    s += `<text x="${x(i)}" y="${H - 8}" text-anchor="middle">${esc(DATA.rows[i].short)}</text>`;
  }
  s += `<path d="${linePath(pts, x, y)}" fill="none" stroke="var(--series-1)" stroke-width="2" stroke-linejoin="round"/>`;
  if (pts.length < (W - L - R) / 6) {
    for (const p of pts) s += `<circle cx="${x(p.i)}" cy="${y(p.v)}" r="2" fill="var(--series-1)"/>`;
  }
  s += marks(pts, x, y, 4);
  s += `<rect class="brush" y="${T}" height="${H - T - B}" fill="var(--series-1)" fill-opacity="0.12" visibility="hidden"/>`;
  s += `<line class="cross" y1="${T}" y2="${H - B}" stroke="var(--text-muted)" visibility="hidden"/>`;
  s += `<circle class="dot" r="5" fill="none" stroke="var(--text-primary)" stroke-width="2" visibility="hidden"/>`;
  const svg = document.createElementNS(NS, "svg");
  svg.setAttribute("width", W);
  svg.setAttribute("height", H);
  svg.innerHTML = s;
  box.appendChild(svg);
  const tip = document.createElement("div");
  tip.className = "tip";
  box.appendChild(tip);
  const cross = svg.querySelector(".cross"), dot = svg.querySelector(".dot"), brush = svg.querySelector(".brush");

  const nearest = px => {
    let best = pts[0];
    for (const p of pts) if (Math.abs(x(p.i) - px) < Math.abs(x(best.i) - px)) best = p;
    return best;
  };
  const show = p => {
    cross.setAttribute("x1", x(p.i));
    cross.setAttribute("x2", x(p.i));
    cross.setAttribute("visibility", "visible");
    dot.setAttribute("cx", x(p.i));
    dot.setAttribute("cy", y(p.v));
    dot.setAttribute("visibility", "visible");
    tip.className = sec.pinned ? "tip pinned" : "tip";
    tip.innerHTML = tipHtml(sec, p, metric);
    tip.style.display = "block";
    // SVG elements don't have offsetLeft/offsetTop.
    const sb = svg.getBoundingClientRect(), bb = box.getBoundingClientRect();
    const px = x(p.i) + sb.left - bb.left;
    tip.style.left = (px + 14 + tip.offsetWidth < box.clientWidth ? px + 14 : px - 14 - tip.offsetWidth) + "px";
    tip.style.top = (sb.top - bb.top + 8) + "px";
  };
  const hide = () => {
    tip.style.display = "none";
    cross.setAttribute("visibility", "hidden");
    dot.setAttribute("visibility", "hidden");
  };
  const highlight = () => {
    for (const tr of sec.el.querySelectorAll("tr.hl")) tr.classList.remove("hl");
    if (sec.pinned) document.getElementById(rowId(sec.key, sec.pinned.r))?.classList.add("hl");
  };
  const pinned = sec.pinned && pts.find(p => p.i === sec.pinned.i);
  if (pinned) show(pinned);

  const local = ev => ev.clientX - svg.getBoundingClientRect().left;
  let dragFrom = null;
  svg.addEventListener("mousedown", ev => {
    dragFrom = local(ev);
    ev.preventDefault();
  });
  svg.addEventListener("mousemove", ev => {
    const px = local(ev);
    if (dragFrom != null && Math.abs(px - dragFrom) > 4) {
      brush.setAttribute("x", Math.min(px, dragFrom));
      brush.setAttribute("width", Math.abs(px - dragFrom));
      brush.setAttribute("visibility", "visible");
      return;
    }
    if (!sec.pinned) show(nearest(px));
  });
  window.addEventListener("mouseup", function up(ev) {
    if (!svg.isConnected) {
      window.removeEventListener("mouseup", up);
      return;
    }
    if (dragFrom == null) return;
    const px = local(ev), from = dragFrom;
    dragFrom = null;
    brush.setAttribute("visibility", "hidden");
    if (Math.abs(px - from) > 4) {
      const a = Math.max(sec.i0, Math.floor(xinv(Math.min(px, from))));
      const b = Math.min(sec.i1, Math.ceil(xinv(Math.max(px, from))));
      if (b > a) {
        sec.i0 = a;
        sec.i1 = b;
        bigChart(sec);
      }
      return;
    }
    const p = nearest(px);
    sec.pinned = sec.pinned && sec.pinned.i === p.i ? null : p;
    highlight();
    show(p);
  });
  svg.addEventListener("dblclick", () => {
    sec.i0 = 0;
    sec.i1 = DATA.rows.length - 1;
    bigChart(sec);
  });
  svg.addEventListener("mouseleave", () => {
    if (dragFrom == null && !sec.pinned) hide();
  });
}

function table(key) {
  const head = ["Commit", "Date", "Subject", "Time", "", "Instructions", "", "Cycles", "", "Branch misses", "", "IPC", "GHz", "Mtok/s", "RSS", "Temp"];
  let s = "<table><thead><tr>" + head.map((h, k) => `<th class="${k < 3 ? "l" : ""}">${h}</th>`).join("") + "</tr></thead><tbody>";
  let prevCell = null;
  const out = [];
  for (const r of DATA.rows) {
    const c = r.cells[key];
    let tds = `<td class="l">${commitLink(r)}</td><td class="l muted">${esc(r.date)}</td><td class="l subject" title="${esc(r.subject)}">${esc(r.subject)}</td>`;
    if (!r.build_ok) {
      tds += `<td class="l fail" colspan="13">build failed: ${esc(r.build_error)}</td>`;
    } else if (!c) {
      tds += `<td class="l fail" colspan="13">not measured</td>`;
    } else if (!c.ok) {
      tds += `<td class="l fail" colspan="13" title="${esc(c.error)}">failed: ${esc(c.error)}</td>`;
    } else {
      for (const m of METRICS) {
        const sd = m.key === "time" ? ` <span class="muted">&plusmn;${(c.time_sd * 1000).toFixed(0)}ms</span>` : "";
        tds += `<td>${m.fmt(c[m.key])}${sd}</td><td>${deltaHtml(delta(m.key, c, prevCell))}</td>`;
      }
      tds += `<td>${c.ipc.toFixed(3)}</td><td>${c.ghz ? c.ghz.toFixed(2) : "-"}</td><td>${c.mtok_s.toFixed(1)}</td>`;
      tds += `<td>${c.rss_mb.toFixed(0)} MB</td><td class="muted">${c.temp[0] ? c.temp[0].toFixed(0) + "&rarr;" + c.temp[1].toFixed(0) + "&deg;C" : "-"}</td>`;
      prevCell = c;
    }
    out.push(`<tr id="${rowId(key, r)}">${tds}</tr>`);
  }
  s += out.reverse().join("") + "</tbody></table>";
  const wrap = document.createElement("div");
  wrap.className = "tablewrap";
  wrap.innerHTML = s;
  return wrap;
}

function renderSection(sec) {
  for (const t of sec.tiles.children) t.classList.toggle("sel", t.dataset.metric === sec.metric);
  bigChart(sec);
}

renderMeta();
const sections = document.getElementById("sections");
const live = [];
for (const key of DATA.keys) {
  const el = document.createElement("section");
  sections.appendChild(el);
  const h = document.createElement("h2");
  h.textContent = key;
  el.appendChild(h);
  const sec = {key, el, metric: "time", i0: 0, i1: DATA.rows.length - 1, pinned: null, redraw: [], pts: {}};
  for (const m of METRICS) sec.pts[m.key] = points(key, m.key);
  const ok = sec.pts.time.length;
  if (ok < 2) {
    const last = DATA.rows[DATA.rows.length - 1].cells[key];
    const why = last && !last.ok ? `, latest failed: ${esc(last.error)}` : "";
    const det = document.createElement("details");
    det.innerHTML = `<summary>${ok} successful run${ok === 1 ? "" : "s"} of ${DATA.rows.length}${why}</summary>`;
    det.appendChild(table(key));
    el.appendChild(det);
    continue;
  }
  sec.tiles = document.createElement("div");
  sec.tiles.className = "tiles";
  el.appendChild(sec.tiles);
  for (const m of METRICS) sec.tiles.appendChild(tile(sec, m));
  sec.big = document.createElement("div");
  sec.big.className = "big";
  el.appendChild(sec.big);
  el.appendChild(table(key));
  live.push(sec);
}
function redrawAll() {
  for (const sec of live) {
    for (const f of sec.redraw) f();
    renderSection(sec);
  }
}
redrawAll();
let resizeTimer = null;
window.addEventListener("resize", () => {
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(redrawAll, 100);
});
</script>
</body>
</html>
"""

if __name__ == "__main__":
    main()

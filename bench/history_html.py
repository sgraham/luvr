#!/usr/bin/env python3
"""Writes a static HTML page of the perf history in bench/history.json.

  python3 bench/history_html.py [--log bench/history.json] [--out out/history.html]

The page has a section per input and mode, each with line charts of the main
counters over the commits, and a table of every commit, newest first, with the
change from the previous measured commit. The data is embedded in the page, so
it can be opened directly from disk.
"""

import argparse
import html
import json
import os

# Changes smaller than these are shown as noise. Instructions barely vary
# between runs, cycles and branch misses a little more. Times also use their
# own stddev, see delta() in the page.
NOISE_PCT = {"instructions": 0.1, "cycles": 0.75, "branch_misses": 1.0, "time": 1.0}


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
.charts {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(320px, 1fr));
  gap: 12px;
}
.chart {
  background: var(--surface-1);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 10px 12px 6px;
  position: relative;
}
.chart h3 { font-size: 13px; font-weight: 600; margin: 0; }
.chart .latest { font-size: 12px; color: var(--text-secondary); }
.chart svg { display: block; width: 100%; height: auto; overflow: visible; }
.chart svg text { fill: var(--text-muted); font-size: 10px; }
.tip {
  position: absolute;
  pointer-events: none;
  background: var(--surface-1);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 6px 8px;
  font-size: 12px;
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.12);
  max-width: 280px;
  display: none;
  z-index: 2;
}
.tip .sub { color: var(--text-secondary); }
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

// Enough decimals that neighbouring axis ticks, `step` apart, don't print the
// same.
function axisDp(m, step) {
  return Math.max(m.dp, Math.min(6, Math.ceil(-Math.log10(step / m.scale)) + 1));
}

function esc(s) {
  return String(s).replace(/[&<>"]/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;"})[c]);
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

function renderMeta() {
  const m = DATA.machine, t = DATA.toolchain, s = DATA.settings;
  const inputs = Object.values(DATA.inputs).map(i =>
    `${esc(i.name)} (${(i.bytes / 1e6).toFixed(0)} MB, ${i.lines.toLocaleString()} lines, md5 <code>${esc(i.md5.slice(0, 8))}</code>)`).join(", ");
  document.getElementById("meta").innerHTML =
    `${DATA.rows.length} commits. ${esc(m.cpu_model || "")}, CPU ${s.cpu} pinned, boost ${esc(m.boost)}, ` +
    `${esc(m.energy_performance_preference || m.governor || "")}. ${esc(t.clang || "")}. ` +
    `perf stat &times;${s.perf_runs}, hyperfine &times;${s.hyperfine_runs} after ${s.hyperfine_warmup} warmups.<br>` +
    `Inputs: ${inputs}.`;
}

function chart(container, key, metric) {
  const pts = [];
  DATA.rows.forEach((r, i) => {
    const c = r.cells[key];
    if (c && c.ok && c[metric.key] != null) pts.push({i, v: c[metric.key], r, c});
  });
  if (!pts.length) return;
  const div = document.createElement("div");
  div.className = "chart";
  const last = pts[pts.length - 1];
  div.innerHTML = `<h3>${metric.label}</h3><div class="latest">latest ${metric.fmt(last.v)} at <code>${esc(last.r.short)}</code></div>`;
  container.appendChild(div);

  const W = 320, H = 150, L = 44, R = 8, T = 10, B = 18;
  const n = Math.max(DATA.rows.length - 1, 1);
  let lo = Math.min(...pts.map(p => p.v)), hi = Math.max(...pts.map(p => p.v));
  const pad = (hi - lo) * 0.1 || hi * 0.01 || 1;
  lo -= pad; hi += pad;
  const x = i => L + (W - L - R) * (DATA.rows.length === 1 ? 0.5 : i / n);
  const y = v => T + (H - T - B) * (1 - (v - lo) / (hi - lo));
  const ns = "http://www.w3.org/2000/svg";
  const svg = document.createElementNS(ns, "svg");
  svg.setAttribute("viewBox", `0 0 ${W} ${H}`);
  let s = "";
  const dp = axisDp(metric, (hi - lo) / 3);
  for (let k = 0; k <= 3; k++) {
    const v = lo + (hi - lo) * k / 3;
    s += `<line x1="${L}" x2="${W - R}" y1="${y(v)}" y2="${y(v)}" stroke="var(--grid)" stroke-width="1" vector-effect="non-scaling-stroke"/>`;
    s += `<text x="${L - 6}" y="${y(v) + 3}" text-anchor="end">${metric.fmt(v, dp)}</text>`;
  }
  s += `<text x="${L}" y="${H - 4}">${esc(DATA.rows[0].short)}</text>`;
  s += `<text x="${W - R}" y="${H - 4}" text-anchor="end">${esc(DATA.rows[DATA.rows.length - 1].short)}</text>`;
  // A gap in the line where a commit failed or wasn't measured.
  let d = "", prevI = -2;
  for (const p of pts) {
    d += (p.i === prevI + 1 ? "L" : "M") + x(p.i).toFixed(1) + "," + y(p.v).toFixed(1);
    prevI = p.i;
  }
  s += `<path d="${d}" fill="none" stroke="var(--series-1)" stroke-width="2" vector-effect="non-scaling-stroke" stroke-linejoin="round"/>`;
  for (const p of pts) {
    if (pts.length < 40 || p === last) {
      s += `<circle cx="${x(p.i)}" cy="${y(p.v)}" r="2.5" fill="var(--series-1)" stroke="var(--surface-1)" stroke-width="1"/>`;
    }
  }
  s += `<line class="cross" x1="0" x2="0" y1="${T}" y2="${H - B}" stroke="var(--text-muted)" stroke-width="1" vector-effect="non-scaling-stroke" visibility="hidden"/>`;
  s += `<circle class="dot" r="4" fill="var(--series-1)" stroke="var(--surface-1)" stroke-width="2" visibility="hidden"/>`;
  svg.innerHTML = s;
  div.appendChild(svg);

  const tip = document.createElement("div");
  tip.className = "tip";
  div.appendChild(tip);
  const cross = svg.querySelector(".cross"), dot = svg.querySelector(".dot");
  svg.addEventListener("mousemove", ev => {
    const box = svg.getBoundingClientRect();
    const sx = (ev.clientX - box.left) / box.width * W;
    let best = pts[0];
    for (const p of pts) if (Math.abs(x(p.i) - sx) < Math.abs(x(best.i) - sx)) best = p;
    cross.setAttribute("x1", x(best.i)); cross.setAttribute("x2", x(best.i));
    cross.setAttribute("visibility", "visible");
    dot.setAttribute("cx", x(best.i)); dot.setAttribute("cy", y(best.v));
    dot.setAttribute("visibility", "visible");
    const prev = pts[pts.indexOf(best) - 1];
    const dl = prev ? delta(metric.key, best.c, prev.c) : null;
    tip.innerHTML = `<b>${metric.fmt(best.v)}</b> ${deltaHtml(dl)}<br><code>${esc(best.r.short)}</code> ${esc(best.r.date)}<br><span class="sub">${esc(best.r.subject)}</span>`;
    tip.style.display = "block";
    const px = (x(best.i) / W) * box.width + svg.offsetLeft;
    tip.style.left = Math.min(px + 12, div.clientWidth - tip.offsetWidth - 4) + "px";
    tip.style.top = (svg.offsetTop + 8) + "px";
  });
  svg.addEventListener("mouseleave", () => {
    tip.style.display = "none";
    cross.setAttribute("visibility", "hidden");
    dot.setAttribute("visibility", "hidden");
  });
}

function table(container, key) {
  const head = ["Commit", "Date", "Subject", "Time", "", "Instructions", "", "Cycles", "", "Branch misses", "", "IPC", "GHz", "Mtok/s", "RSS", "Temp"];
  let s = "<table><thead><tr>" + head.map((h, k) => `<th class="${k < 3 ? "l" : ""}">${h}</th>`).join("") + "</tr></thead><tbody>";
  let prevCell = null;
  const out = [];
  for (const r of DATA.rows) {
    const c = r.cells[key];
    let tds = `<td class="l"><code>${esc(r.short)}</code></td><td class="l muted">${esc(r.date)}</td><td class="l subject" title="${esc(r.subject)}">${esc(r.subject)}</td>`;
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
    out.push(`<tr>${tds}</tr>`);
  }
  s += out.reverse().join("") + "</tbody></table>";
  const wrap = document.createElement("div");
  wrap.className = "tablewrap";
  wrap.innerHTML = s;
  container.appendChild(wrap);
}

renderMeta();
const sections = document.getElementById("sections");
for (const key of DATA.keys) {
  const h = document.createElement("h2");
  h.textContent = key;
  sections.appendChild(h);
  const charts = document.createElement("div");
  charts.className = "charts";
  sections.appendChild(charts);
  for (const m of METRICS) chart(charts, key, m);
  table(sections, key);
}
</script>
</body>
</html>
"""

if __name__ == "__main__":
    main()

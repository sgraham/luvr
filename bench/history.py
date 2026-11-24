#!/usr/bin/env python3
"""Measures the parser at each commit and appends the results to a JSON log.

Runs on the benchmark machine, in a git repository that has the commits:

  python3 bench/history.py --repo REPO --log history.json \
      --input betterbench=inputs/betterbench.luv --input dumbbench=inputs/dumbbench.luv \
      [REV_RANGE]

REV_RANGE defaults to b06070e^..main, from when parsecheck became `luv parse`.
Each commit is extracted with `git archive`, built with its own
`m r`, and each input is measured with perf stat and hyperfine, pinned to one
CPU, in each mode: `luv parse` (the checking build) and `luv il` (the
emitting build). Boost is turned off for the run with cpu_fixed_clocks.sh,
and back on at the end with cpu_normal_clocks.sh, unless --keep-clocks is
given. Commits
that are already in the log are skipped, unless --force is given. The log is
rewritten after each commit, so an interrupted run loses at most one.

The same input files are used for every commit, so that the numbers can be
compared. A commit that fails to build, or a mode that fails on an input, is
recorded with the error, so that it shows as a gap rather than being measured
again.
"""

import argparse
import datetime
import hashlib
import json
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import tempfile
from dataclasses import asdict, dataclass, field

SCHEMA = 1
DEFAULT_RANGE = "b06070e^..main"
# Root-owned copies of bench/cpu_fixed_clocks.sh and cpu_normal_clocks.sh,
# which sudoers allows without a password. See cpu_fixed_clocks.sh.
FIXED_CLOCKS = "/usr/local/sbin/cpu_fixed_clocks.sh"
NORMAL_CLOCKS = "/usr/local/sbin/cpu_normal_clocks.sh"
PERF_EVENTS = ["instructions", "cycles", "branches", "branch-misses", "task-clock", "page-faults"]


@dataclass
class Settings:
    cpu: int  # pinned with taskset
    perf_runs: int
    hyperfine_runs: int
    hyperfine_warmup: int
    modes: dict  # mode name -> command, with {luv} and {input} placeholders


@dataclass
class Input:
    name: str
    path: str
    md5: str
    bytes: int
    lines: int


@dataclass
class Counter:
    value: float  # the mean over Settings.perf_runs
    unit: str
    stddev_pct: float


@dataclass
class Times:
    runs: int
    mean_s: float
    stddev_s: float
    median_s: float
    min_s: float
    max_s: float
    user_s: float
    system_s: float


@dataclass
class BenchResult:
    input: str  # Input.name
    mode: str  # a key of Settings.modes
    ok: bool
    error: str = ""
    max_rss_kb: int = 0
    # The CPU package temperature before and after the perf and hyperfine runs,
    # since boost clocks drop as it heats up. 0 if it can't be read.
    temp_c_before: float = 0
    temp_c_after: float = 0
    perf: dict = field(default_factory=dict)  # event name -> Counter
    times: Times | None = None
    derived: dict = field(default_factory=dict)


@dataclass
class Entry:
    commit: str
    short: str
    date: str  # committer date
    subject: str
    measured_at: str
    machine: dict
    toolchain: dict
    settings: Settings
    build_ok: bool
    build_error: str = ""
    cflags: str = ""  # from the commit's own build.ninja
    tokens: dict = field(default_factory=dict)  # Input.name -> count by this commit's lexdump
    benches: list = field(default_factory=list)  # BenchResult


def run(args, cwd=None, check=True, quiet=False):
    # quiet discards stdout, which is the whole IL dump for `luv il`.
    stdout = subprocess.DEVNULL if quiet else subprocess.PIPE
    r = subprocess.run(args, cwd=cwd, stdout=stdout, stderr=subprocess.PIPE, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(f"{' '.join(args)} failed ({r.returncode}):\n{r.stderr.strip()}")
    return r


def first_line(args):
    try:
        return run(args).stdout.strip().split("\n")[0]
    except (OSError, RuntimeError):
        return ""


def read_file(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return ""


def machine_info(cpu):
    lscpu = run(["lscpu"]).stdout
    info = {}
    for key in ["Model name", "CPU(s)", "Thread(s) per core", "CPU max MHz", "CPU min MHz"]:
        m = re.search(rf"^{re.escape(key)}:\s*(.*)$", lscpu, re.M)
        info[key] = m.group(1).strip() if m else ""
    os_name = re.search(r'^PRETTY_NAME="(.*)"$', read_file("/etc/os-release"), re.M)
    mem = re.search(r"^MemTotal:\s*(\d+) kB", read_file("/proc/meminfo"), re.M)
    cpufreq = f"/sys/devices/system/cpu/cpu{cpu}/cpufreq"
    return {
        "cpu_model": info["Model name"],
        "logical_cpus": info["CPU(s)"],
        "threads_per_core": info["Thread(s) per core"],
        "cpu_max_mhz": info["CPU max MHz"],
        "cpu_min_mhz": info["CPU min MHz"],
        "scaling_driver": read_file(f"{cpufreq}/scaling_driver"),
        "governor": read_file(f"{cpufreq}/scaling_governor"),
        "energy_performance_preference": read_file(f"{cpufreq}/energy_performance_preference"),
        "boost": read_file("/sys/devices/system/cpu/cpufreq/boost"),
        "perf_event_paranoid": read_file("/proc/sys/kernel/perf_event_paranoid"),
        "nmi_watchdog": read_file("/proc/sys/kernel/nmi_watchdog"),
        "mem_total_kb": int(mem.group(1)) if mem else 0,
        "kernel": platform.release(),
        "os": os_name.group(1) if os_name else "",
    }


def toolchain_info():
    return {
        "clang": first_line(["clang", "--version"]),
        "perf": first_line(["perf", "--version"]),
        "hyperfine": first_line(["hyperfine", "--version"]),
        "python": platform.python_version(),
    }


def cpu_temp_c():
    """Returns AMD's Tctl, or Intel's package temperature, or 0."""
    for hwmon in sorted(os.listdir("/sys/class/hwmon")):
        d = os.path.join("/sys/class/hwmon", hwmon)
        if read_file(os.path.join(d, "name")) in ("k10temp", "coretemp"):
            millis = read_file(os.path.join(d, "temp1_input"))
            return int(millis) / 1000 if millis else 0
    return 0


def describe_input(spec):
    name, path = spec.split("=", 1)
    path = os.path.abspath(path)
    md5 = hashlib.md5()
    lines = 0
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            md5.update(chunk)
            lines += chunk.count(b"\n")
    return Input(name, path, md5.hexdigest(), os.path.getsize(path), lines)


def commits_in(repo, rev_range):
    out = run(["git", "log", "--reverse", "--format=%H%x00%h%x00%cI%x00%s", rev_range],
              cwd=repo).stdout
    return [line.split("\0") for line in out.strip().split("\n") if line]


def build(repo, sha, work):
    """Extracts and builds sha in work. Returns the cflags and the out dir."""
    archive = subprocess.Popen(["git", "archive", sha], cwd=repo, stdout=subprocess.PIPE)
    subprocess.run(["tar", "-x", "-C", work], stdin=archive.stdout, check=True)
    archive.wait()
    out = os.path.join(work, "out", "lr")
    run(["./m", "r", "luv", "lexdump"], cwd=work)
    ninja = read_file(os.path.join(out, "build.ninja"))
    m = re.search(r"^cflags = (.*)$", ninja, re.M)
    return (m.group(1) if m else ""), out


def count_tokens(lexdump, path):
    # lexdump prints a summary that includes the token count.
    r = run([lexdump, path])
    m = re.search(r"(\d+) tokens", r.stdout + r.stderr)
    return int(m.group(1)) if m else 0


def perf_stat(settings, cmd):
    r = run([
        "taskset", "-c",
        str(settings.cpu), "perf", "stat", "-x,", "-r",
        str(settings.perf_runs), "-e", ",".join(PERF_EVENTS), "--"
    ] + cmd,
            quiet=True)
    counters = {}
    for line in r.stderr.strip().split("\n"):
        parts = line.split(",")
        if len(parts) >= 4 and parts[2] in PERF_EVENTS and parts[0] not in ("", "<not counted>"):
            stddev = parts[3].rstrip("%")
            counters[parts[2]] = Counter(float(parts[0]), parts[1], float(stddev or 0))
    return counters


def hyperfine(settings, cmd, scratch):
    out = os.path.join(scratch, "hyperfine.json")
    run([
        "taskset", "-c",
        str(settings.cpu), "hyperfine", "-N", "-w",
        str(settings.hyperfine_warmup), "-r",
        str(settings.hyperfine_runs), "--export-json", out, " ".join(cmd)
    ])
    with open(out) as f:
        res = json.load(f)["results"][0]
    return Times(len(res["times"]), res["mean"], res["stddev"], res["median"], res["min"],
                 res["max"], res["user"], res["system"])


def max_rss_kb(cmd):
    r = run(["/usr/bin/time", "-f", "%M"] + cmd, quiet=True)
    return int(r.stderr.strip().split("\n")[-1])


def measure(settings, mode, luv, inp, tokens, scratch):
    cmd = settings.modes[mode].format(luv=luv, input=inp.path).split()
    r = run(cmd, check=False, quiet=True)
    if r.returncode != 0:
        return BenchResult(inp.name, mode, False, error=r.stderr.strip()[-2000:])
    res = BenchResult(inp.name, mode, True)
    res.max_rss_kb = max_rss_kb(cmd)
    res.temp_c_before = cpu_temp_c()
    res.perf = perf_stat(settings, cmd)
    res.times = hyperfine(settings, cmd, scratch)
    res.temp_c_after = cpu_temp_c()
    t = res.times.mean_s
    instructions = res.perf.get("instructions")
    cycles = res.perf.get("cycles")
    task_clock = res.perf.get("task-clock")
    res.derived = {
        "mb_per_s": inp.bytes / 1e6 / t,
        "lines_per_s": inp.lines / t,
        "tokens_per_s": tokens / t if tokens else 0,
        "ipc": instructions.value / cycles.value if instructions and cycles else 0,
        # The average clock while running, from the cycles and the msec of CPU time.
        "ghz": cycles.value / (task_clock.value * 1e6) if cycles and task_clock else 0,
        "instructions_per_token": instructions.value / tokens if instructions and tokens else 0,
        "cycles_per_token": cycles.value / tokens if cycles and tokens else 0,
    }
    return res


def load_log(path):
    if not os.path.exists(path):
        return {"schema": SCHEMA, "inputs": {}, "entries": []}
    with open(path) as f:
        log = json.load(f)
    if log.get("schema") != SCHEMA:
        sys.exit(f"{path}: schema {log.get('schema')}, expected {SCHEMA}")
    return log


def save_log(path, log):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(log, f, indent=1)
        f.write("\n")
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--repo", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--input", action="append", required=True, help="name=path")
    ap.add_argument("--cpu", type=int, default=6)
    ap.add_argument("--perf-runs", type=int, default=10)
    ap.add_argument("--hyperfine-runs", type=int, default=20)
    ap.add_argument("--hyperfine-warmup", type=int, default=3)
    ap.add_argument("--force", action="store_true", help="measure commits already in the log")
    ap.add_argument("--keep-clocks",
                    action="store_true",
                    help="measure with the current boost and energy preference settings, instead "
                    "of fixing the clocks for the run")
    ap.add_argument("range", nargs="?", default=DEFAULT_RANGE)
    args = ap.parse_args()

    settings = Settings(args.cpu, args.perf_runs, args.hyperfine_runs, args.hyperfine_warmup, {
        "parse": "{luv} parse -q {input}",
        "il": "{luv} il {input}",
    })
    inputs = [describe_input(spec) for spec in args.input]
    log = load_log(args.log)
    for inp in inputs:
        known = log["inputs"].get(inp.name)
        if known and known["md5"] != inp.md5:
            sys.exit(f"{inp.name}: md5 {inp.md5} differs from the log's {known['md5']}")
        log["inputs"][inp.name] = asdict(inp)
    if args.keep_clocks:
        measure_commits(args, settings, inputs, log)
        return
    # kill's SIGTERM would otherwise skip the finally, and leave boost off.
    signal.signal(signal.SIGTERM, lambda signum, frame: sys.exit(1))
    run(["sudo", "-n", FIXED_CLOCKS])
    try:
        measure_commits(args, settings, inputs, log)
    finally:
        run(["sudo", "-n", NORMAL_CLOCKS])


def measure_commits(args, settings, inputs, log):
    machine = machine_info(args.cpu)
    toolchain = toolchain_info()
    done = {e["commit"] for e in log["entries"]}
    for sha, short, date, subject in commits_in(args.repo, args.range):
        if sha in done and not args.force:
            continue
        print(f"{short} {subject}", flush=True)
        entry = Entry(sha,
                      short,
                      date,
                      subject,
                      datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
                      machine,
                      toolchain,
                      settings,
                      build_ok=False)
        work = tempfile.mkdtemp(prefix="luvr-history-")
        try:
            try:
                entry.cflags, out = build(args.repo, sha, work)
                entry.build_ok = True
            except RuntimeError as e:
                entry.build_error = str(e)[-2000:]
            if entry.build_ok:
                luv = os.path.join(out, "luv")
                lexdump = os.path.join(out, "lexdump")
                for inp in inputs:
                    entry.tokens[inp.name] = count_tokens(lexdump, inp.path)
                    for mode in settings.modes:
                        res = measure(settings, mode, luv, inp, entry.tokens[inp.name], work)
                        entry.benches.append(res)
                        note = f"{res.times.mean_s:.3f} s" if res.ok else "failed"
                        print(f"  {inp.name} {mode}: {note}", flush=True)
        finally:
            shutil.rmtree(work, ignore_errors=True)
        log["entries"] = [e for e in log["entries"] if e["commit"] != sha] + [asdict(entry)]
        save_log(args.log, log)


if __name__ == "__main__":
    main()

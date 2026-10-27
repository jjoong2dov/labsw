"""Long run: random NETCONF edits, changes behind the agent's back, backup/restore
and oper reads for LABSW_SOAK_SECONDS (skipped if not set), with a fixed seed
(LABSW_SOAK_SEED, default 1). After every step the device must be what running says.

Every few seconds the agent's RSS and number of open files are sampled to
<LABSW_SOAK_OUT or the test directory>/soak.csv. Verdict:
  - open files: the last sample may not be above the maximum of the first quarter
  - RSS: the median of the last quarter minus the median of the second quarter
    (the first is warm-up: caches, first allocations) may not be above
    LABSW_SOAK_MAX_KB (default 256)
Also printed: that growth per step and the least-squares slope per hour. Not used
for the verdict: RSS moves in whole pages, so in a 2-minute run one 4 kB page more
already extrapolates to hundreds of kB per hour (first version of this test).
RSS only shows memory that grows; LeakSanitizer (LABSW_ASAN=ON) finds memory
that is lost at exit. A small leak needs a long run to rise above the limit.
SPDX-License-Identifier: MIT
"""
import csv
from statistics import median
import os
import random
import time
from pathlib import Path

import pytest

from conftest import NS, wait_for
from test_agent import oper_leaf
from test_ops import rpc
from test_routes import RNS

SECONDS = float(os.environ.get("LABSW_SOAK_SECONDS", "0"))
SEED = int(os.environ.get("LABSW_SOAK_SEED", "1"))
MAX_KB = float(os.environ.get("LABSW_SOAK_MAX_KB", "256"))
PORTS = ("p1", "p2", "p3")
VIDS = list(range(10, 18))


def random_config(rnd):
    """{port: {vid: (pvid, untagged)}} that the YANG model accepts"""
    cfg = {}
    for p in PORTS:
        vids = rnd.sample(VIDS, rnd.randint(0, 3))
        pv = rnd.choice(vids + [None])
        cfg[p] = {v: (v == pv, rnd.random() < 0.5) for v in vids}
    return cfg


def config_xml(cfg):
    vids = sorted({v for vl in cfg.values() for v in vl})
    body = "".join(f"<vlan><vid>{v}</vid></vlan>" for v in vids)
    for p, vl in cfg.items():
        if not vl:
            continue
        body += f"<port><name>{p}</name>" + "".join(
            f"<vlan><vid>{v}</vid><pvid>{str(pv).lower()}</pvid><egress>{'untagged' if u else 'tagged'}</egress></vlan>"
            for v, (pv, u) in vl.items()) + "</port>"
    return ('<config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"><switch xmlns="%s" '
            'xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0" nc:operation="replace">%s</switch></config>'
            % (NS, body))


def routes_xml(rnd):
    n = rnd.randint(0, 3)
    body = "".join(f"<route><prefix>198.51.100.{i * 64}/26</prefix><gateway>203.0.113.{rnd.randint(1, 9)}</gateway></route>"
                   for i in range(n))
    return ('<config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"><routes xmlns="%s" '
            'xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0" nc:operation="replace">%s</routes></config>'
            % (RNS, body))


def sample(pid):
    rss = 0
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith("VmRSS:"):
            rss = int(line.split()[1])
    return rss, len(os.listdir(f"/proc/{pid}/fd"))


def slope_per_hour(points):
    """least squares slope of (t seconds, value) in value per hour"""
    n = len(points)
    mt = sum(t for t, _ in points) / n
    mv = sum(v for _, v in points) / n
    den = sum((t - mt) ** 2 for t, _ in points)
    return 0.0 if den == 0 else sum((t - mt) * (v - mv) for t, v in points) / den * 3600


@pytest.mark.skipif(SECONDS <= 0, reason="set LABSW_SOAK_SECONDS to run the soak test")
def test_soak(lab):
    rnd = random.Random(SEED)
    out = Path(os.environ.get("LABSW_SOAK_OUT", lab.base))
    out.mkdir(parents=True, exist_ok=True)
    agent = lab.start_agent(every=1)
    pid = agent.pid  # `ip netns exec` execs the agent: same pid
    want = {p: {} for p in PORTS}
    steps = {"edit": 0, "drift": 0, "backup": 0, "restore": 0, "routes": 0, "oper": 0}
    samples = []
    t0 = last = time.monotonic()
    with lab.nc() as m, open(out / "soak.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["seconds", "rss_kb", "fds", "steps"])
        rss, fds = sample(pid)
        samples.append((0.0, rss, fds))
        w.writerow([0, rss, fds, 0])
        while time.monotonic() - t0 < SECONDS:
            op = rnd.choices(list(steps), weights=[50, 10, 5, 5, 15, 15])[0]
            if op == "edit":
                want = random_config(rnd)
                m.edit_config(target="running", config=config_xml(want))
                assert lab.ports() == want, f"after edit {sum(steps.values())}"
            elif op == "drift":
                lab.dev.set_ports(random_config(rnd))
                wait_for(lambda: lab.ports() == want, "the agent to put running back", timeout=15)
            elif op == "backup":
                rpc(m, "backup", "soak")
            elif op == "restore" and (lab.backups / "soak.json").exists():
                rpc(m, "restore", "soak")
                # running is now the backup: read it back as the new expectation
                want = {p: v for p, v in lab.ports().items()}
                time.sleep(1.5)  # a reconcile round must find nothing to do
                assert lab.ports() == want
            elif op == "routes":
                m.edit_config(target="running", config=routes_xml(rnd))
            else:
                assert oper_leaf(m, "health") == "ok"
            steps[op] += 1
            now = time.monotonic()
            if now - last >= 5:
                last = now
                rss, fds = sample(pid)
                samples.append((now - t0, rss, fds))
                w.writerow([round(now - t0, 1), rss, fds, sum(steps.values())])
                f.flush()
        repairs = oper_leaf(m, "repairs")
    assert len(samples) >= 8, "too short to judge; run longer"
    q = len(samples) // 4
    second, last = samples[q:2 * q], samples[-q:]
    growth = median([r for _, r, _ in last]) - median([r for _, r, _ in second])
    nsteps = sum(steps.values())
    steps_between = max(1, nsteps * (last[0][0] - second[0][0]) / max(1.0, samples[-1][0]))
    slope = slope_per_hour([(t, r) for t, r, _ in samples[q:]])
    warm_fds = max(fd for _, _, fd in samples[:q])
    summary = (f"seed {SEED}, {SECONDS:.0f} s, steps {steps}, repairs {repairs}, samples {len(samples)}, "
               f"RSS {samples[0][1]} -> {samples[-1][1]} kB, growth after warm-up {growth:.0f} kB "
               f"(~{growth * 1024 / steps_between:.0f} B/step, slope {slope:.0f} kB/h), "
               f"fds {samples[0][2]} -> {samples[-1][2]} (first quarter max {warm_fds})")
    (out / "soak_summary.txt").write_text(summary + "\n")
    print(summary)
    assert samples[-1][2] <= warm_fds, "open files keep growing"
    assert growth <= MAX_KB, f"RSS grew {growth:.0f} kB after warm-up"

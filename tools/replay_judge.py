#!/usr/bin/env python3
"""replay_judge.py CH [REV] [VAR=value ...] : judge a replay (book chapter 12, 16 or 17).

The replay of chapter CH is the branch replay/CH of this repository: what Claude Code
made from the tag CH-start with the prompt replay/prompts/CH.txt. Its session never saw
these tests. This takes the COMMITTED state of REV (default replay/CH; give your own
branch to judge your own run), puts the integration tests of the finished commit of
that chapter next to it as tests/accept/, relaxes the assertions that check the exact
wording of the book's implementation (the prompt did not ask for it; see RELAX),
builds with LABSW_TESTING=ON, runs ctest and pytest, writes results/judge-<CH>-<time>/.
VAR=value is passed to pytest (BUILD = the build directory), e.g. for chapter 17 of
the book's replay: LABSW_DUMP=BUILD/labsw-dump.
Needs the stack on PATH/LD_LIBRARY_PATH/PKG_CONFIG_PATH as for the tests (README).
SPDX-License-Identifier: MIT
"""
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
LAB = REPLAY = REPO
VENV_PY = sys.executable

# chapter: (reference commit with the finished feature, test files to take from it)
CHAPTERS = {
    "ch12": ("5c87f2e", ["conftest.py", "test_agent.py", "test_routes.py"]),
    "ch16": ("08fc1ea", ["conftest.py", "test_agent.py", "test_routes.py", "test_ops.py"]),
    "ch17": ("4fdeb8b", ["conftest.py", "test_agent.py", "test_routes.py", "test_ops.py", "test_upgrade.py"]),
}

# (file, exact text in the reference test, what the prompt actually asked for)
RELAX = {
    "ch12": [
        ("test_agent.py", '"<difference>p1 VLAN 10: not a member, expected pvid untagged</difference>" in x',
         're.search(r"<difference>[^<]*p1[^<]*10[^<]*</difference>", x)'),
    ],
    "ch16": [
        ("test_ops.py", "    assert text in e.value.message\n", ""),
        ("test_ops.py", '        assert "write failed after 10 of" in e.value.message\n', ""),
        # the prompt did not say the agent makes the directory at start (mine does, a backup also may)
        ("test_ops.py", '        edit(m, ACCESS10)\n        (lab.backups / "p9.json")',
         '        edit(m, ACCESS10)\n        lab.backups.mkdir(exist_ok=True)\n        (lab.backups / "p9.json")'),
    ],
    "ch17": [
        ("test_upgrade.py", 'UP = REPO / "tests/fixtures/upgrade"',
         'UP = Path(__file__).parent / "fixtures_upgrade"  # the reference examples, not the session\'s'),
        ("test_upgrade.py", 'assert r.returncode == 1 and "REFUSED" in r.stdout and "management-vlan-0020" in r.stdout',
         'assert r.returncode != 0 and "management-vlan-0020" in r.stdout + r.stderr'),
        ("test_upgrade.py", 'assert r.returncode == 1 and "DATA LOSS" in r.stdout',
         'assert r.returncode != 0 and "management-vlan-0020" in r.stdout + r.stderr'),
        ("test_upgrade.py", "    assert \"running: /labsw-switch:switch/vlan[vid=10]/name = 'management-vlan-0020'\" in r.stdout\n", ""),
        ("test_upgrade.py", 'saved = r.stdout.split("rollback: ")[1].split("--rollback ")[1].rstrip(")\\n")',
         'saved = re.findall(r"--rollback (\\S+)", r.stdout.strip().splitlines()[-1])[0].rstrip(")")'),
    ],
}

# extra acceptance tests written from the prompt alone
EXTRA = {
    "ch17": '''

def test_check_exit_codes_from_the_prompt(lab):
    """0 keeps all, 2 refused, 3 would lose configuration; the repository is only read"""
    lab.start_agent()
    with lab.nc() as m:
        edit(m, NAMED)
    env = dict(lab.env, LABSW_STATE=str(lab.base / "state"))
    codes = {}
    for rev in ("2026-11-01", "2026-11-02", "2026-11-03"):
        r = subprocess.run([str(REPO / "tools/labsw-upgrade-check"), str(UP / f"labsw-switch@{rev}.yang")],
                           env=env, capture_output=True, text=True)
        codes[rev] = r.returncode
    assert codes == {"2026-11-01": 0, "2026-11-02": 2, "2026-11-03": 3}
    assert revision(lab) == "2026-10-27"
''',
}


def main(ch, src=None, extra_env=""):
    """src: what to judge, default the branch replay/<ch>"""
    ref, files = CHAPTERS[ch]
    repo, rev = REPO, (src or f"replay/{ch}")
    # a fresh clone has the book's branches only as origin/replay/<ch>
    for r in (rev, f"origin/{rev}"):
        h = subprocess.run(["git", "-C", str(repo), "rev-parse", "--verify", "-q", "--short", f"{r}^{{commit}}"],
                           capture_output=True, text=True)
        if h.returncode == 0:
            rev, head = r, h.stdout.strip()
            break
    else:
        sys.exit(f"replay_judge.py: no commit {rev} (nor origin/{rev}); git fetch --all --tags?")
    out = REPO / "results" / f"judge-{ch}-{time.strftime('%Y%m%d-%H%M%S')}"
    work = out / "tree"
    work.mkdir(parents=True)
    # the tree this runs in (the judged REV is taken as committed, whatever this says)
    dirty = subprocess.run(["git", "-C", str(repo), "status", "--short"], capture_output=True, text=True).stdout
    subprocess.run(f"git -C {repo} archive {rev} | tar -x -C {work}", shell=True, check=True)
    acc = work / "tests/accept"
    acc.mkdir(parents=True)
    for f in files:
        text = subprocess.run(["git", "-C", str(LAB), "show", f"{ref}:tests/int/{f}"], capture_output=True,
                              text=True, check=True).stdout
        for rf, old, new in RELAX.get(ch, []):
            if rf == f:
                assert old in text, (f, old)
                text = text.replace(old, new)
        if f.startswith("test_"):
            text = "import re\nfrom pathlib import Path\n" + text
        if f == "test_upgrade.py":
            text += EXTRA.get(ch, "")
        (acc / f).write_text(text)
    # the example revisions of chapter 17 (the session may have made its own of the same name)
    if ch == "ch17":
        subprocess.run(f"git -C {LAB} archive {ref} tests/fixtures/upgrade | tar -x -C {out} && "
                       f"cp -r {out}/tests/fixtures/upgrade {acc}/fixtures_upgrade", shell=True, check=True)
    env_sh = "true"  # the stack is already on PATH etc. (README)
    build = (f"{env_sh} && cmake -S {work} -B {work}/build -DLABSW_TESTING=ON > {out}/cmake.log 2>&1 && "
             f"cmake --build {work}/build -j16 >> {out}/cmake.log 2>&1 && "
             f"(cd {work}/build && ctest --output-junit {out}/unit.xml > {out}/ctest.log 2>&1)")
    b = subprocess.run(["bash", "-c", build])
    t0 = time.monotonic()
    # the reference tests of that time read their NETCONF key from LABSW_NC_KEY (or a path
    # under the stack): give them one, or every test skips — and skipped is not passed
    subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(out / "nc_key")], check=True)
    extra_env = f"LABSW_NC_KEY={out / 'nc_key'} " + extra_env
    p = subprocess.run(["bash", "-c", f"{env_sh} && cd {acc} && {extra_env.replace('BUILD', str(work / 'build'))} {VENV_PY} -m pytest -q -p no:cacheprovider -rfE "
                                      f"--junitxml={out}/junit.xml > {out}/pytest.log 2>&1"])
    tail = (out / "pytest.log").read_text().strip().splitlines()[-1:] if (out / "pytest.log").exists() else []
    summary = {"chapter": ch, "judged": f"{repo.name}:{head}", "extra_env": extra_env, "main_tree_uncommitted": dirty, "reference": ref,
               "build_and_unit_ok": b.returncode == 0, "pytest_exit": p.returncode,
               "pytest": tail[0] if tail else None, "seconds": round(time.monotonic() - t0, 1)}
    (out / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps(summary, ensure_ascii=False))
    return 0 if b.returncode == 0 and p.returncode == 0 else 1


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in CHAPTERS:
        sys.exit(f"usage: replay_judge.py {{{'|'.join(CHAPTERS)}}} [REV|-] [VAR=value ...]")
    src = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] != "-" else None
    sys.exit(main(sys.argv[1], src, " ".join(sys.argv[3:])))

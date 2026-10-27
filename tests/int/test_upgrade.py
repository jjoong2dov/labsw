"""tools/labsw-upgrade-check and labsw-upgrade: a new revision of labsw-switch on a
repository that already has configuration. Test revisions in upgrades/:
  compatible  2026-11-01  new optional leaf vlan/description
  rejected    2026-11-02  VLAN IDs 1..15 (VLAN 20 of ACCESS10 becomes invalid)
  lossy       2026-11-03  vlan/name removed
SPDX-License-Identifier: MIT
"""
import hashlib
import re
import subprocess
from pathlib import Path

from conftest import BUILD, NS
from test_agent import ACCESS10, ACCESS10_DEV, edit, running_switch, port_xml

UP = Path(__file__).resolve().parent / "upgrades"
COMPAT = UP / "compatible/labsw-switch@2026-11-01.yang"
REJECT = UP / "rejected/labsw-switch@2026-11-02.yang"
LOSSY = UP / "lossy/labsw-switch@2026-11-03.yang"
BASE_REV = "2026-10-27"


def named(vid, name):
    return f"<vlan><vid>{vid}</vid><name>{name}</name></vlan>"


def revision(lab, module="labsw-switch"):
    for line in lab.run("sysrepoctl", "-l").stdout.splitlines():
        cols = [c.strip() for c in line.split("|")]
        if cols[0] == module:
            return cols[1]
    return None


def snapshot(lab):
    """everything of the repository a check must not change"""
    files = {str(f.relative_to(lab.base / "repo")): hashlib.sha256(f.read_bytes()).hexdigest()
             for f in sorted((lab.base / "repo").rglob("*")) if f.is_file()}
    dumps = {ds: subprocess.run([str(BUILD / "labsw-dump"), ds], env=lab.env, check=True,
                                capture_output=True, text=True, timeout=30).stdout
             for ds in ("running", "startup")}
    return files, dumps, lab.run("sysrepoctl", "-l").stdout


def section(out, ds):
    """the lost lines listed under '<ds> (n):'"""
    m = re.search(rf"^{ds} \(\d+\):\n((?:  .*\n?)*)", out, re.M)
    return sorted(line.strip() for line in m.group(1).splitlines()) if m else []


def rollback_cmd(out):
    last = out.rstrip().splitlines()[-1]
    m = re.fullmatch(r"to go back: (\S+) --rollback (\S+)", last)
    assert m, last
    return Path(m.group(2))


def test_check_compatible_changes_nothing(lab):
    lab.start_agent()
    with lab.nc() as m:
        edit(m, ACCESS10 + named(10, "ten"))
    before, shm = snapshot(lab), set(Path("/dev/shm").glob("lswu*"))
    r = lab.tool("labsw-upgrade-check", str(COMPAT))
    assert r.returncode == 0, r.stdout + r.stderr
    assert "OK" in r.stdout
    assert snapshot(lab) == before
    assert revision(lab) == BASE_REV
    assert not list(lab.base.glob("labsw-upgrade-check.*"))   # the copy and its SHM are removed
    assert set(Path("/dev/shm").glob("lswu*")) <= shm


def test_check_rejected_shows_sysrepo_error(lab):
    lab.start_agent()
    with lab.nc() as m:
        edit(m, ACCESS10)
    before = snapshot(lab)
    r = lab.tool("labsw-upgrade-check", str(REJECT))
    assert r.returncode == 2, r.stdout + r.stderr
    assert "REJECTED" in r.stdout and "20" in r.stdout, r.stdout
    assert snapshot(lab) == before


def test_check_lossy_lists_losses_per_datastore(lab):
    lab.start_agent()
    with lab.nc() as m:
        edit(m, ACCESS10 + named(10, "ten"))
        m.copy_config(source="running", target="startup")
        edit(m, named(10, "zehn") + named(20, "twenty"))
    before = snapshot(lab)
    r = lab.tool("labsw-upgrade-check", str(LOSSY))
    assert r.returncode == 3, r.stdout + r.stderr
    assert section(r.stdout, "running") == ["/labsw-switch:switch/vlan[vid='10']/name = zehn",
                                            "/labsw-switch:switch/vlan[vid='20']/name = twenty"], r.stdout
    assert section(r.stdout, "startup") == ["/labsw-switch:switch/vlan[vid='10']/name = ten"], r.stdout
    assert snapshot(lab) == before


def test_check_without_names_is_not_lossy(lab):
    """the lossy revision loses nothing where no VLAN has a name"""
    lab.start_agent()
    with lab.nc() as m:
        edit(m, ACCESS10)
    r = lab.tool("labsw-upgrade-check", str(LOSSY))
    assert r.returncode == 0, r.stdout + r.stderr


def test_upgrade_stops_on_reject_and_loss(lab):
    lab.start_agent()
    with lab.nc() as m:
        edit(m, ACCESS10 + named(10, "ten"))
    before = snapshot(lab)
    r = lab.tool("labsw-upgrade", str(REJECT))
    assert r.returncode == 2, r.stdout + r.stderr
    r = lab.tool("labsw-upgrade", str(LOSSY))
    assert r.returncode == 3, r.stdout + r.stderr
    assert "--allow-loss" in r.stderr
    assert snapshot(lab) == before
    assert not (lab.base / "state/upgrade").exists() or not any((lab.base / "state/upgrade").iterdir())


def test_compatible_upgrade_while_agent_runs_then_rollback(lab):
    lab.start_agent()
    with lab.nc() as m:
        edit(m, ACCESS10)
        m.copy_config(source="running", target="startup")
        saved = running_switch(m)
    r = lab.tool("labsw-upgrade", str(COMPAT))
    assert r.returncode == 0, r.stdout + r.stderr
    d = rollback_cmd(r.stdout)
    assert revision(lab) == "2026-11-01"
    assert (d / f"labsw-switch@{BASE_REV}.yang").exists()
    # the agent keeps working with the new revision, also with the new leaf
    with lab.nc() as m:
        assert running_switch(m) == saved
        edit(m, "<vlan><vid>30</vid><description>new leaf</description></vlan>" +
             port_xml("p3", {30: (True, "untagged")}))
        assert "new leaf" in running_switch(m)
    assert lab.ports() == {**ACCESS10_DEV, "p3": {30: (True, True)}}

    lab.stop("labswd")
    r = lab.tool("labsw-upgrade", "--rollback", str(d))
    assert r.returncode == 0, r.stdout + r.stderr
    assert revision(lab) == BASE_REV
    with lab.nc() as m:
        assert running_switch(m) == saved
    lab.start_agent()   # applies running to the device again
    assert lab.ports() == ACCESS10_DEV


def test_lossy_upgrade_with_allow_loss_then_rollback(lab):
    lab.start_agent()
    with lab.nc() as m:
        edit(m, ACCESS10 + named(10, "ten"))
        m.copy_config(source="running", target="startup")
        edit(m, named(20, "twenty"))
        saved = running_switch(m)
        saved_startup = m.get_config("startup", filter=("subtree", f'<switch xmlns="{NS}"/>')).data_xml
    lab.stop("labswd")
    r = lab.tool("labsw-upgrade", str(LOSSY), "--allow-loss")
    assert r.returncode == 0, r.stdout + r.stderr
    assert "/labsw-switch:switch/vlan[vid='20']/name = twenty" in r.stdout
    d = rollback_cmd(r.stdout)
    assert revision(lab) == "2026-11-03"
    with lab.nc() as m:
        assert "twenty" not in running_switch(m)

    r = lab.tool("labsw-upgrade", "--rollback", str(d))
    assert r.returncode == 0, r.stdout + r.stderr
    assert revision(lab) == BASE_REV
    with lab.nc() as m:
        assert running_switch(m) == saved
        assert m.get_config("startup", filter=("subtree", f'<switch xmlns="{NS}"/>')).data_xml == saved_startup
    lab.start_agent()
    assert lab.ports() == ACCESS10_DEV


def module_line(lab, module="labsw-switch"):
    """the `sysrepoctl -l` columns of a module but the revision"""
    for line in lab.run("sysrepoctl", "-l").stdout.splitlines():
        cols = [c.strip() for c in line.split("|")]
        if cols[0] == module:
            return cols[:1] + cols[2:]
    return None


def test_rollback_keeps_module_settings(lab):
    lab.run("sysrepoctl", "-c", "labsw-switch", "-r", "on")
    lab.run("sysrepoctl", "-c", "labsw-switch", "-D", "running", "-p", "640")
    settings = module_line(lab)
    r = lab.tool("labsw-upgrade", str(COMPAT))
    assert r.returncode == 0, r.stdout + r.stderr
    r = lab.tool("labsw-upgrade", "--rollback", str(rollback_cmd(r.stdout)))
    assert r.returncode == 0, r.stdout + r.stderr
    assert revision(lab) == BASE_REV
    assert module_line(lab) == settings


def test_failed_rollback_puts_the_new_revision_back(lab):
    lab.start_agent()
    with lab.nc() as m:
        edit(m, ACCESS10 + named(10, "ten"))
    lab.stop("labswd")
    r = lab.tool("labsw-upgrade", str(COMPAT))
    assert r.returncode == 0, r.stdout + r.stderr
    d = rollback_cmd(r.stdout)
    with lab.nc() as m:
        edit(m, "<vlan><vid>10</vid><description>after the upgrade</description></vlan>")
    before = snapshot(lab)[1:]
    (d / f"labsw-switch@{BASE_REV}.yang").write_text("module labsw-switch { broken")
    r = lab.tool("labsw-upgrade", "--rollback", str(d))
    assert r.returncode == 1
    assert "putting labsw-switch@2026-11-01 back" in r.stderr, r.stderr
    assert revision(lab) == "2026-11-01"
    assert snapshot(lab)[1:] == before    # data and module list, files may differ


def test_rollback_refuses_a_directory_that_is_not_one(lab):
    (lab.base / "nothing").mkdir()
    r = lab.tool("labsw-upgrade", "--rollback", str(lab.base / "nothing"))
    assert r.returncode == 1 and "not an upgrade directory" in r.stderr
    assert revision(lab) == BASE_REV

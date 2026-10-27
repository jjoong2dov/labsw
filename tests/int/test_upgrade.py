"""Schema upgrade of labsw-switch on a configured device (tools/upgrade_check,
tools/upgrade). The new revisions are examples in tests/fixtures/upgrade.
SPDX-License-Identifier: MIT
"""
import subprocess

from conftest import NS, REPO
from test_agent import ACCESS10, ACCESS10_DEV, edit, running_switch

UP = REPO / "tests/fixtures/upgrade"
NAMED = ACCESS10.replace("<vlan><vid>10</vid></vlan>", "<vlan><vid>10</vid><name>management-vlan-0020</name></vlan>", 1)


def tool(lab, *argv):
    env = dict(lab.env, LABSW_STATE=str(lab.base / "state"))
    return subprocess.run([str(REPO / "tools" / argv[0]), *argv[1:]], env=env, capture_output=True, text=True)


def revision(lab):
    out = lab.run("sysrepoctl", "-l").stdout
    return [l.split("|")[1].strip() for l in out.splitlines() if l.split("|")[0].strip() == "labsw-switch"][0]


def test_compatible_upgrade_while_the_agent_runs(lab):
    lab.start_agent()
    with lab.nc() as m:
        edit(m, NAMED)
        before = running_switch(m)
    r = tool(lab, "labsw-upgrade", str(UP / "labsw-switch@2026-11-01.yang"))
    assert r.returncode == 0, r.stdout + r.stderr
    assert revision(lab) == "2026-11-01"
    with lab.nc() as m:   # a new session sees the new schema
        assert running_switch(m) == before
        edit(m, "<port><name>p1</name><description>uplink</description></port>")
        edit(m, "<vlan><vid>30</vid></vlan><port><name>p3</name><vlan><vid>30</vid></vlan></port>")
    assert lab.procs["labswd"].poll() is None
    assert lab.ports() == dict(ACCESS10_DEV, p3={30: (False, False)})


def test_refused_upgrade_changes_nothing(lab):
    lab.start_agent()
    with lab.nc() as m:
        edit(m, NAMED)
        before = running_switch(m)
    r = tool(lab, "labsw-upgrade", str(UP / "labsw-switch@2026-11-02.yang"))
    assert r.returncode == 1 and "REFUSED" in r.stdout and "management-vlan-0020" in r.stdout
    assert revision(lab) == "2026-10-27"
    with lab.nc() as m:
        assert running_switch(m) == before


def test_data_loss_is_found_before_and_rollback_brings_it_back(lab):
    lab.start_agent()
    with lab.nc() as m:
        edit(m, NAMED)
        before = running_switch(m)
    new = str(UP / "labsw-switch@2026-11-03.yang")
    r = tool(lab, "labsw-upgrade", new)
    assert r.returncode == 1 and "DATA LOSS" in r.stdout
    assert "running: /labsw-switch:switch/vlan[vid=10]/name = 'management-vlan-0020'" in r.stdout
    assert revision(lab) == "2026-10-27"

    r = tool(lab, "labsw-upgrade", new, "--allow-loss")
    assert r.returncode == 0, r.stdout + r.stderr
    saved = r.stdout.split("rollback: ")[1].split("--rollback ")[1].rstrip(")\n")
    with lab.nc() as m:
        assert "management-vlan-0020" not in running_switch(m)     # gone, as the check said

    lab.stop("labswd")    # rollback uninstalls the module the agent is subscribed to
    r = tool(lab, "labsw-upgrade", "--rollback", saved)
    assert r.returncode == 0, r.stdout + r.stderr
    assert revision(lab) == "2026-10-27"
    lab.start_agent()
    with lab.nc() as m:
        assert running_switch(m) == before
    assert lab.ports() == ACCESS10_DEV

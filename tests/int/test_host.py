"""NETCONF -> sysrepo -> labswd -> mock device, on the host (no root).

A PASS here says the sysrepo orchestration works; it says nothing about the
Linux bridge. The same scenarios against a real bridge run on vm/pi.
SPDX-License-Identifier: MIT
"""
import pytest
from ncclient.operations import RPCError

from conftest import NS


def vlan_xml(vids):
    return "".join(f"<vlan><vid>{v}</vid></vlan>" for v in vids)


def port_xml(name, vlans, op=None):
    """vlans: {vid: (pvid, egress)}"""
    attr = f' xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0" nc:operation="{op}"' if op else ""
    body = "".join(f"<vlan><vid>{v}</vid><pvid>{str(p).lower()}</pvid><egress>{e}</egress></vlan>"
                   for v, (p, e) in vlans.items())
    return f"<port{attr}><name>{name}</name>{body}</port>"


def config(inner):
    return f'<config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"><switch xmlns="{NS}">{inner}</switch></config>'


def edit(m, inner):
    return m.edit_config(target="running", config=config(inner))


ACCESS10 = vlan_xml([10, 20]) + port_xml("p1", {10: (True, "untagged")}) + \
    port_xml("p2", {10: (False, "tagged"), 20: (False, "tagged")})
ACCESS10_DEV = {"p1": {10: (True, True)}, "p2": {10: (False, False), 20: (False, False)}, "p3": {}}


def running_switch(m):
    return m.get_config("running", filter=("subtree", f'<switch xmlns="{NS}"/>')).data_xml


def test_capability_and_schema(lab):
    lab.start_agent()
    with lab.nc() as m:
        # a YANG 1.1 module is not listed in <hello> (RFC 7950 5.6.4), only in the YANG library
        assert not [c for c in m.server_capabilities if NS in c]
        lib = m.get(filter=("subtree", '<yang-library xmlns="urn:ietf:params:xml:ns:yang:ietf-yang-library"/>')).data_xml
        assert "<name>labsw-switch</name>" in lib and "<revision>2026-10-26</revision>" in lib
        assert "module labsw-switch" in m.get_schema("labsw-switch").data


def test_edit_reaches_device_and_oper(lab):
    lab.start_agent()
    with lab.nc() as m:
        edit(m, ACCESS10)
        assert lab.ports() == ACCESS10_DEV
        st = m.get(filter=("subtree", f'<switch-state xmlns="{NS}"/>')).data_xml
        assert "<health>ok</health>" in st
        assert "<name>p1</name><vlan><vid>10</vid><pvid>true</pvid><egress>untagged</egress>" in st.replace("\n", "").replace(" ", "")


def test_same_edit_twice_sends_no_command(lab):
    lab.start_agent()
    with lab.nc() as m:
        edit(m, ACCESS10)
        n = len(lab.trace_lines())
        assert n == 3  # p1 10, p2 10, p2 20
        edit(m, ACCESS10)
        assert len(lab.trace_lines()) == n


@pytest.mark.parametrize("bad,why", [
    (vlan_xml([4095]), "range"),
    (vlan_xml([10]) + port_xml("p1", {20: (False, "tagged")}), "leafref"),
    (vlan_xml([10, 20]) + port_xml("p1", {10: (True, "untagged"), 20: (True, "untagged")}), "one PVID"),
])
def test_yang_rejects_before_agent(lab, bad, why):
    lab.start_agent()
    with lab.nc() as m:
        before = running_switch(m)
        with pytest.raises(RPCError):
            edit(m, bad)
        assert running_switch(m) == before
    assert lab.trace_lines() == []
    assert "change" not in lab.log("labswd").split("start, applying running")[-1]


def test_agent_rejects_unknown_port_with_path(lab):
    lab.start_agent()
    with lab.nc() as m:
        with pytest.raises(RPCError) as e:
            edit(m, vlan_xml([10]) + port_xml("p9", {10: (True, "untagged")}))
        assert e.value.tag == "invalid-value"
        assert 'Port "p9" is not a port of the bridge.' in e.value.message
        assert "port[lsw:name='p9']" in (e.value.path or "")
        assert "p9" not in running_switch(m)
    assert lab.trace_lines() == []


def health(m):
    st = m.get(filter=("subtree", f'<switch-state xmlns="{NS}"/>')).data_xml
    return st.split("<health>")[1].split("</health>")[0]


def test_vlan_filtering_off_agent_starts_degraded(lab):
    st = lab.read_state()
    st["vlan_filtering"] = False
    lab.write_state(st)
    lab.start_agent()  # must not exit: without a subscriber every edit would be accepted
    with lab.nc() as m:
        assert health(m) == "degraded"
        with pytest.raises(RPCError) as e:
            edit(m, ACCESS10)
        assert "vlan_filtering is off" in e.value.message
        assert lab.trace_lines() == []
        st["vlan_filtering"] = True
        lab.write_state(st)
        edit(m, ACCESS10)
        assert health(m) == "ok"
    assert lab.ports() == ACCESS10_DEV


@pytest.mark.parametrize("fail_at", [1, 2, 3, 4])
def test_failure_at_each_step_restores_device(lab, fail_at):
    lab.start_agent()
    with lab.nc() as m:
        edit(m, vlan_xml([30]) + port_xml("p3", {30: (True, "untagged")}))
    before_dev, before_trace = lab.ports(), len(lab.trace_lines())
    lab.stop("labswd")
    lab.start_agent(fail_at=fail_at)  # start applies running again: no command, nothing to fail
    assert len(lab.trace_lines()) == before_trace
    with lab.nc() as m:
        before_cfg = running_switch(m)
        with pytest.raises(RPCError) as e:  # 4 commands: p1 10, p2 10, p2 20, del p3 30
            edit(m, ACCESS10 + port_xml("p3", {}, op="delete"))
        assert f"injected failure at command {fail_at}" in e.value.message
        assert running_switch(m) == before_cfg
        st = m.get(filter=("subtree", f'<switch-state xmlns="{NS}"/>')).data_xml
        assert "<health>ok</health>" in st
    assert lab.ports() == before_dev


@pytest.mark.parametrize("agent_prio", [10, 0], ids=["agent-first", "same-priority"])
def test_abort_from_other_subscriber_restores_device(lab, agent_prio):
    """agent-first: sysrepo sends ABORT after the rejector (priority 0) fails.
    same-priority: both run at once; sysrepo 5.1.0 makes the ABORT itself and
    gives it the request id instead of the operation id."""
    lab.start_agent(prio=agent_prio)
    lab.start_rejector()
    with lab.nc() as m:
        with pytest.raises(RPCError) as e:
            edit(m, ACCESS10)
        assert "rejected by the test subscriber" in e.value.message
        assert "<port>" not in running_switch(m)
        assert health(m) == "ok"
    log = lab.log("labswd")
    assert lab.ports() == {"p1": {}, "p2": {}, "p3": {}}, log
    # applied in CHANGE (3 commands), undone on ABORT (3 commands)
    assert "aborted by another subscriber, restoring" in log, log
    assert len(lab.trace_lines()) == 6
    assert ("taking the only open one" in log) == (agent_prio == 0)


def test_two_sessions_edit_at_once(lab):
    """Changes of one module are serialized by sysrepo; the device ends equal to running."""
    import threading
    lab.start_agent()
    errors = []

    def worker(port, vids):
        try:
            with lab.nc() as m:
                for v in vids:
                    edit(m, vlan_xml([v]) + port_xml(port, {v: (False, "tagged")}))
        except Exception as e:  # noqa: BLE001 - reported below
            errors.append(e)

    ts = [threading.Thread(target=worker, args=(p, range(base, base + 20)))
          for p, base in (("p1", 100), ("p2", 200))]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    assert not errors
    dev = lab.ports()
    assert dev["p1"] == {v: (False, False) for v in range(100, 120)}
    assert dev["p2"] == {v: (False, False) for v in range(200, 220)}
    assert "no such change" not in lab.log("labswd")


def test_start_applies_running_and_fixes_drift(lab):
    lab.start_agent()
    with lab.nc() as m:
        edit(m, ACCESS10)
    lab.stop("labswd")
    st = lab.read_state()                  # someone changes the device while
    st["ports"]["p1"] = []                 # the agent is not running
    st["ports"]["p3"] = [{"vid": 99, "pvid": True, "untagged": True}]
    lab.write_state(st)
    lab.start_agent()
    assert lab.ports() == ACCESS10_DEV


def test_agent_killed_between_change_and_done(lab):
    """SIGKILL: nothing restores in-flight state; the next start makes the device equal running."""
    lab.start_agent()
    with lab.nc() as m:
        edit(m, ACCESS10)
    lab.stop("labswd", sig=9)
    lab.write_state({"vlan_filtering": True, "ports": {"p1": [], "p2": [], "p3": []}})
    lab.start_agent()
    assert lab.ports() == ACCESS10_DEV


def test_delete_port_removes_its_vlans(lab):
    lab.start_agent()
    with lab.nc() as m:
        edit(m, ACCESS10)
        edit(m, port_xml("p2", {}, op="delete"))
    assert lab.ports()["p2"] == {}
    assert lab.ports()["p1"] == {10: (True, True)}

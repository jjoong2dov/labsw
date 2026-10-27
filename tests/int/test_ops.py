"""labsw-ops: backup running to a file on the device, restore it in one transaction.
SPDX-License-Identifier: MIT
"""
import json
import re

import pytest
from lxml import etree
from ncclient.operations import RPCError

from conftest import NS
from test_agent import ACCESS10, ACCESS10_DEV, edit, port_xml, vlan_xml
from test_routes import RNS, TWO, TWO_DEV, redit, route_xml

ONS = "urn:example:labsw-ops"
EMPTY_DEV = {"p1": {}, "p2": {}, "p3": {}}


def rpc(m, op, name):
    x = etree.fromstring(f'<{op} xmlns="{ONS}"><name>{name}</name></{op}>')
    return m.dispatch(x)


def backup(m, name):
    """bytes from the reply"""
    return int(re.search(r"<(?:\w+:)?bytes[^>]*>(\d+)<", rpc(m, "backup", name).xml).group(1))


def restore(m, name):
    return rpc(m, "restore", name)


def running(m):
    return (m.get_config("running", filter=("subtree", f'<switch xmlns="{NS}"/>')).data_xml +
            m.get_config("running", filter=("subtree", f'<routes xmlns="{RNS}"/>')).data_xml)


def both(m, sw, rt):
    m.edit_config(target="running", config=f'<config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">'
                                           f'<switch xmlns="{NS}">{sw}</switch><routes xmlns="{RNS}">{rt}</routes></config>')


OTHER = vlan_xml([30]) + port_xml("p3", {30: (True, "untagged")})
OTHER_DEV = {"p1": {}, "p2": {}, "p3": {30: (True, True)}}
OTHER_RT = route_xml("192.0.2.0/24", "203.0.113.1")
OTHER_RT_DEV = {"192.0.2.0/24": "203.0.113.1"}


def to_other(m):
    """replace both modules with something else than ACCESS10 + TWO"""
    m.edit_config(target="running", default_operation="replace",
                  config=f'<config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">'
                         f'<switch xmlns="{NS}">{OTHER}</switch><routes xmlns="{RNS}">{OTHER_RT}</routes></config>')


def test_backup_then_restore(lab):
    lab.start_agent()
    with lab.nc() as m:
        both(m, ACCESS10, TWO)
        saved = running(m)
        n = backup(m, "before-upgrade_1.0")
        f = lab.backup_dir / "before-upgrade_1.0.json"
        assert f.stat().st_size == n
        assert set(json.loads(f.read_text())) == {"labsw-switch:switch", "labsw-route:routes"}
        to_other(m)
        assert lab.ports() == OTHER_DEV and lab.dev.routes() == OTHER_RT_DEV
        restore(m, "before-upgrade_1.0")
        assert running(m) == saved
    assert lab.ports() == ACCESS10_DEV
    assert lab.dev.routes() == TWO_DEV
    assert sorted(p.name for p in lab.backup_dir.iterdir()) == ["before-upgrade_1.0.json"]


def test_restore_empties_module_missing_from_backup(lab):
    lab.start_agent()
    with lab.nc() as m:
        edit(m, ACCESS10)                      # no routes yet
        backup(m, "sw")
        assert set(json.loads((lab.backup_dir / "sw.json").read_text())) == {"labsw-switch:switch"}
        redit(m, TWO)
        edit(m, port_xml("p2", {}, op="delete"))
        restore(m, "sw")
        assert "<route>" not in running(m)
    assert lab.dev.routes() == {}
    assert lab.ports() == ACCESS10_DEV


def test_backup_of_empty_running_restores_to_empty(lab):
    lab.start_agent()
    with lab.nc() as m:
        backup(m, "empty")
        both(m, ACCESS10, TWO)
        restore(m, "empty")
        assert "<port>" not in running(m) and "<route>" not in running(m)
    assert lab.ports() == EMPTY_DEV
    assert lab.dev.routes() == {}


def test_failed_backup_keeps_previous_one(lab):
    lab.start_agent()
    with lab.nc() as m:
        both(m, ACCESS10, TWO)
        backup(m, "a")
    old = (lab.backup_dir / "a.json").read_bytes()
    lab.stop("labswd")
    lab.start_agent(backup_fail_at=10)
    with lab.nc() as m:
        to_other(m)
        with pytest.raises(RPCError) as e:
            backup(m, "a")
        assert "injected failure after 10 bytes" in e.value.message
        with pytest.raises(RPCError):
            backup(m, "new")                   # no file at all, not half of one
        assert (lab.backup_dir / "a.json").read_bytes() == old
        assert sorted(p.name for p in lab.backup_dir.iterdir()) == ["a.json"]
        restore(m, "a")                        # the old backup still works
    assert lab.ports() == ACCESS10_DEV
    assert lab.dev.routes() == TWO_DEV


def test_unfinished_file_from_crash_removed_at_start(lab):
    lab.backup_dir.mkdir()
    (lab.backup_dir / ".tmp-a.Xy12Zq").write_text('{"labsw-sw')  # as a power loss leaves it
    (lab.backup_dir / "a.json").write_text("{}")
    lab.start_agent()
    assert sorted(p.name for p in lab.backup_dir.iterdir()) == ["a.json"]


@pytest.mark.parametrize("name", ["../x", "a/b", "..", ".hidden", "", "x" * 33])
def test_bad_name_rejected(lab, name):
    lab.start_agent()
    with lab.nc() as m:
        with pytest.raises(RPCError):
            backup(m, name)
        with pytest.raises(RPCError):
            restore(m, name)
    assert not (lab.base / "x.json").exists()
    assert not lab.backup_dir.exists() or list(lab.backup_dir.iterdir()) == []


# --- restore that must change nothing -----------------------------------------

def bad_file(lab, kind):
    """write backup 'b' of the given kind (the good one is ACCESS10 + TWO)"""
    good = (lab.backup_dir / "good.json").read_text()
    f = lab.backup_dir / "b.json"
    if kind == "missing":
        return
    if kind == "truncated":
        f.write_text(good[: len(good) // 2])
    elif kind == "empty":
        f.write_text("")
    elif kind == "other-module":
        js = json.loads(good)
        js["ietf-netconf-acm:nacm"] = {"enable-nacm": False}
        f.write_text(json.dumps(js))
    elif kind == "state-data":
        js = json.loads(good)
        js["labsw-switch:switch-state"] = {"health": "ok"}
        f.write_text(json.dumps(js))
    elif kind == "unknown-module":
        js = json.loads(good)
        js["no-such-module:x"] = {}
        f.write_text(json.dumps(js))
    elif kind == "yang-invalid":           # p1 uses VLAN 99, which is not configured (leafref)
        js = json.loads(good)
        js["labsw-switch:switch"]["port"][0]["vlan"][0]["vid"] = 99
        f.write_text(json.dumps(js))
    elif kind == "unknown-port":
        js = json.loads(good)
        js["labsw-switch:switch"]["port"].append({"name": "p9", "vlan": [{"vid": 10}]})
        f.write_text(json.dumps(js))
    else:
        raise ValueError(kind)


@pytest.mark.parametrize("kind,why", [
    ("missing", 'no backup named "b"'),
    ("truncated", "not a valid backup"),
    ("empty", "not a valid backup"),
    ("other-module", "has /ietf-netconf-acm:nacm"),
    ("state-data", 'not a valid backup: Unexpected data state node "switch-state"'),
    ("unknown-module", 'not a valid backup: No module named "no-such-module"'),
    ("yang-invalid", 'Invalid leafref value "99"'),
    ("unknown-port", 'Port "p9" is not a port of the bridge.'),
])
def test_bad_restore_changes_nothing(lab, kind, why):
    lab.start_agent()
    with lab.nc() as m:
        both(m, ACCESS10, TWO)
        backup(m, "good")
        to_other(m)
        bad_file(lab, kind)
        before, n = running(m), len(lab.trace_lines())
        with pytest.raises(RPCError) as e:
            restore(m, "b")
        assert why in e.value.message
        assert running(m) == before
    assert lab.ports() == OTHER_DEV
    assert lab.dev.routes() == OTHER_RT_DEV
    if kind != "unknown-port":                 # the agent saw no change at all
        assert len(lab.trace_lines()) == n


@pytest.mark.parametrize("route_fail_at", [1, 2, 3])
def test_restore_route_failure_undoes_both_modules(lab, route_fail_at):
    """the route part of the restore fails in the agent: the switch part, applied
    or not yet (sysrepo's module order), ends as before, and so does FRR"""
    lab.start_agent()
    with lab.nc() as m:
        both(m, ACCESS10, TWO)
        backup(m, "good")
        to_other(m)
    lab.stop("labswd")
    lab.start_agent(route_fail_at=route_fail_at)
    with lab.nc() as m:
        before = running(m)
        with pytest.raises(RPCError) as e:     # add /24 .9, add /25, del 192.0.2.0/24
            restore(m, "good")
        assert f"injected failure at command {route_fail_at}" in e.value.message
        assert running(m) == before
    assert lab.ports() == OTHER_DEV
    assert lab.dev.routes() == OTHER_RT_DEV


def test_restore_aborted_by_other_subscriber(lab):
    lab.start_agent(prio=10)
    with lab.nc() as m:
        both(m, ACCESS10, TWO)
        backup(m, "good")
        to_other(m)
    lab.start_rejector()
    with lab.nc() as m:
        with pytest.raises(RPCError) as e:
            restore(m, "good")
        assert "rejected by the test subscriber" in e.value.message
    assert lab.ports() == OTHER_DEV
    assert lab.dev.routes() == OTHER_RT_DEV


def test_ops_module_in_yang_library(lab):
    lab.start_agent()
    with lab.nc() as m:
        lib = m.get(filter=("subtree", '<yang-library xmlns="urn:ietf:params:xml:ns:yang:ietf-yang-library"/>')).data_xml
        assert "<name>labsw-ops</name>" in lib

"""labsw-ops RPCs: backup and restore of labsw-switch + labsw-route.
SPDX-License-Identifier: MIT
"""
import pytest
from ncclient.operations import RPCError
from ncclient.xml_ import to_ele

from test_agent import ACCESS10, ACCESS10_DEV, edit, running_switch, port_xml, vlan_xml
from test_routes import TWO, TWO_DEV, redit, running_routes, route_xml

ONS = "urn:example:labsw-ops"


def rpc(m, name, backup):
    return m.dispatch(to_ele(f'<{name} xmlns="{ONS}"><name>{backup}</name></{name}>'))


OTHER = vlan_xml([30]) + port_xml("p3", {30: (True, "untagged")})
OTHER_DEV = {"p1": {}, "p2": {}, "p3": {30: (True, True)}}


def test_backup_and_restore(lab):
    lab.start_agent()
    with lab.nc() as m:
        edit(m, ACCESS10)
        redit(m, TWO)
        r = rpc(m, "backup", "before")
        size = (lab.backups / "before.json").stat().st_size
        assert f'<bytes xmlns="{ONS}">{size}</bytes>' in r.xml
        saved_sw, saved_rt = running_switch(m), running_routes(m)
        # change both modules completely
        m.edit_config(target="running", config='<config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">'
                      '<switch xmlns="urn:example:labsw-switch" '
                      'xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0" nc:operation="replace">'
                      f'{OTHER}</switch></config>')
        redit(m, route_xml("198.51.100.0/24", op="delete") + route_xml("198.51.100.128/25", op="delete"))
        assert lab.ports() == OTHER_DEV and lab.dev.routes() == {}
        rpc(m, "restore", "before")
        assert running_switch(m) == saved_sw and running_routes(m) == saved_rt
    assert lab.ports() == ACCESS10_DEV
    assert lab.dev.routes() == TWO_DEV


def test_restore_empty_backup_clears_both(lab):
    lab.start_agent()
    with lab.nc() as m:
        rpc(m, "backup", "empty")
        edit(m, ACCESS10)
        redit(m, TWO)
        rpc(m, "restore", "empty")
    assert lab.ports() == {"p1": {}, "p2": {}, "p3": {}}
    assert lab.dev.routes() == {}


def restore_fails(lab, m, name, text):
    sw, rt = running_switch(m), running_routes(m)
    ports, routes = lab.ports(), lab.dev.routes()
    with pytest.raises(RPCError) as e:
        rpc(m, "restore", name)
    assert text in e.value.message
    assert running_switch(m) == sw and running_routes(m) == rt
    assert lab.ports() == ports and lab.dev.routes() == routes
    return e.value


def test_restore_missing_or_broken_backup_changes_nothing(lab):
    lab.start_agent()
    with lab.nc() as m:
        edit(m, ACCESS10)
        rpc(m, "backup", "good")
        restore_fails(lab, m, "nosuch", "no such backup")
        whole = (lab.backups / "good.json").read_text()
        (lab.backups / "cut.json").write_text(whole[: len(whole) // 2])   # e.g. a copy that stopped half way
        restore_fails(lab, m, "cut", "not a valid backup")
        (lab.backups / "alien.json").write_text('{"ietf-netconf-acm:nacm": {"enable-nacm": false}}')
        restore_fails(lab, m, "alien", "is not part of a backup")


def test_name_cannot_leave_the_directory(lab):
    lab.start_agent()
    with lab.nc() as m:
        with pytest.raises(RPCError):
            rpc(m, "backup", "../x")
    assert not (lab.backups.parent / "x.json").exists()


def test_restore_the_agent_rejects_changes_nothing(lab):
    """valid YANG, but p9 is not a port of the bridge: the agent rejects the change"""
    lab.start_agent()
    with lab.nc() as m:
        edit(m, ACCESS10)
        lab.backups.mkdir(exist_ok=True)   # made by the first backup; none was taken here
        (lab.backups / "p9.json").write_text(
            '{"labsw-switch:switch": {"vlan": [{"vid": 10}], "port": [{"name": "p9", "vlan": [{"vid": 10}]}]}}')
        restore_fails(lab, m, "p9", "p9")
    assert lab.ports() == ACCESS10_DEV


def test_restore_route_failure_undoes_the_switch_part(lab):
    """one transaction for both modules: the switch part is applied first, the
    route part fails, sysrepo sends ABORT to the switch part, which undoes it"""
    lab.start_agent(route_fail_at=1)
    with lab.nc() as m:
        rpc(m, "backup", "empty")
        edit(m, ACCESS10)
        (lab.backups / "both.json").write_text(
            '{"labsw-switch:switch": {"vlan": [{"vid": 30}], "port": [{"name": "p3", "vlan": '
            '[{"vid": 30, "pvid": true, "egress": "untagged"}]}]},'
            ' "labsw-route:routes": {"route": [{"prefix": "198.51.100.0/24", "gateway": "203.0.113.9"}]}}')
        restore_fails(lab, m, "both", "injected failure")
    assert lab.ports() == ACCESS10_DEV
    assert lab.dev.routes() == {}


def test_failed_backup_keeps_the_old_one(lab):
    lab.start_agent(backup_fail=10)       # every backup write stops after 10 bytes
    with lab.nc() as m:
        lab.backups.mkdir(exist_ok=True)
        (lab.backups / "keep.json").write_text("{}\n")
        edit(m, ACCESS10)
        with pytest.raises(RPCError) as e:
            rpc(m, "backup", "keep")
        assert "write failed after 10 of" in e.value.message
    assert (lab.backups / "keep.json").read_text() == "{}\n"
    assert sorted(p.name for p in lab.backups.iterdir()) == ["keep.json"]   # no .tmp left


def test_backup_directory_is_made_when_needed(lab):
    """-B pointing at a directory whose parents do not exist yet: the agent starts
    (a bad -B must not take the subscriber away) and the first backup makes it"""
    lab.backups = lab.base / "not" / "yet" / "there"
    lab.start_agent()
    with lab.nc() as m:
        edit(m, ACCESS10)
        rpc(m, "backup", "first")
    assert (lab.backups / "first.json").exists()
    assert oct(lab.backups.stat().st_mode & 0o777) == "0o700"


def test_rpcs_need_nacm_permission(lab):
    """NACM lets anyone execute an RPC unless the RPC is default-deny-all, and restore
    writes running through the agent's own session: without that, a user who may not
    edit could change running by restoring a backup (seen on a VM before the fix)"""
    lab.start_agent()
    with lab.nc() as m:
        edit(m, ACCESS10)
        rpc(m, "backup", "a")
        m.edit_config(target="running", config='<config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">'
                      '<switch xmlns="urn:example:labsw-switch" '
                      'xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0" nc:operation="replace">'
                      f'{OTHER}</switch></config>')
        before = running_switch(m)
    with lab.nc("reader") as r:
        assert "<switch" in running_switch(r)              # reading is allowed
        with pytest.raises(RPCError) as e:
            edit(r, ACCESS10)
        assert e.value.tag == "access-denied"
        for name in ("restore", "backup"):
            with pytest.raises(RPCError) as e:
                rpc(r, name, "a")
            assert e.value.tag == "access-denied", name
    with lab.nc() as m:
        assert running_switch(m) == before
    assert lab.ports() == OTHER_DEV
    assert sorted(p.name for p in lab.backups.iterdir()) == ["a.json"]

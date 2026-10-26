"""NETCONF -> labswd -> static routes (mock file, or FRR with LABSW_TARGET=bridge).
SPDX-License-Identifier: MIT
"""
import time

import pytest
from ncclient.operations import RPCError

from conftest import wait_for
from test_agent import ACCESS10, ACCESS10_DEV, edit

RNS = "urn:example:labsw-route"
NC = 'xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"'


def route_xml(prefix, gw=None, op=None):
    attr = f' {NC} nc:operation="{op}"' if op else ""
    hop = f"<gateway>{gw}</gateway>" if gw else "<blackhole/>"
    return f"<route{attr}><prefix>{prefix}</prefix>{hop if op != 'delete' else ''}</route>"


def redit(m, inner):
    return m.edit_config(target="running", config=f'<config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">'
                                                   f'<routes xmlns="{RNS}">{inner}</routes></config>')


def rstate(m, leaf):
    st = m.get(filter=("subtree", f'<routes-state xmlns="{RNS}"/>')).data_xml
    return st.split(f"<{leaf}>")[1].split(f"</{leaf}>")[0] if f"<{leaf}>" in st else None


def running_routes(m):
    return m.get_config("running", filter=("subtree", f'<routes xmlns="{RNS}"/>')).data_xml


TWO = route_xml("198.51.100.0/24", "203.0.113.9") + route_xml("198.51.100.128/25")
TWO_DEV = {"198.51.100.0/24": "203.0.113.9", "198.51.100.128/25": "blackhole"}


def test_routes_reach_device(lab):
    lab.start_agent()
    with lab.nc() as m:
        redit(m, TWO)
        assert lab.dev.routes() == TWO_DEV
        assert rstate(m, "health") == "ok"
        n = len(lab.trace_lines())
        redit(m, TWO)                      # same again: no command
        assert len(lab.trace_lines()) == n
        redit(m, route_xml("198.51.100.128/25", op="delete"))
    assert lab.dev.routes() == {"198.51.100.0/24": "203.0.113.9"}


def test_gateway_change_adds_before_deleting(lab):
    lab.start_agent()
    with lab.nc() as m:
        redit(m, TWO)
        n = len(lab.trace_lines())
        redit(m, route_xml("198.51.100.0/24", "203.0.113.10"))
    assert lab.trace_lines()[n:] == ["add route 198.51.100.0/24 203.0.113.10",
                                     "del route 198.51.100.0/24 203.0.113.9"]
    assert lab.dev.routes()["198.51.100.0/24"] == "203.0.113.10"


def test_host_bits_become_zero(lab):
    """ietf-inet-types: the canonical ipv4-prefix has zero host bits. libyang stores
    it that way, so the agent (and FRR) only ever see 203.0.113.0/24."""
    lab.start_agent()
    with lab.nc() as m:
        redit(m, route_xml("203.0.113.77/24"))
        assert "<prefix>203.0.113.0/24</prefix>" in running_routes(m)
    assert lab.dev.routes() == {"203.0.113.0/24": "blackhole"}


@pytest.mark.parametrize("fail_at", [1, 2, 3])
def test_route_failure_restores(lab, fail_at):
    lab.start_agent()
    with lab.nc() as m:
        redit(m, TWO)
    lab.stop("labswd")
    lab.start_agent(route_fail_at=fail_at)
    with lab.nc() as m:
        before = running_routes(m)
        with pytest.raises(RPCError) as e:  # add .10, del .9, del /25
            redit(m, route_xml("198.51.100.0/24", "203.0.113.10") + route_xml("198.51.100.128/25", op="delete"))
        assert f"injected failure at command {fail_at}" in e.value.message
        assert running_routes(m) == before
        assert rstate(m, "health") == "ok"
    assert lab.dev.routes() == TWO_DEV


def test_route_drift_repaired(lab):
    lab.start_agent(every=1)
    with lab.nc() as m:
        redit(m, TWO)
        lab.dev.set_routes({"198.51.100.0/24": "203.0.113.99", "192.0.2.0/24": "blackhole"})
        wait_for(lambda: lab.dev.routes() == TWO_DEV, "the agent to repair the routes", timeout=30)
        assert rstate(m, "repairs") == "1"


def test_unsupported_route_outside_is_not_touched(lab):
    """two next hops for one prefix: the agent cannot express it, so it does not guess"""
    lab.start_agent(every=1)
    with lab.nc() as m:
        redit(m, TWO)
        ecmp = {"198.51.100.0/24": ["203.0.113.10", "203.0.113.9"], "198.51.100.128/25": "blackhole"}
        lab.dev.set_routes(ecmp)
        wait_for(lambda: rstate(m, "health") == "degraded", "routes health degraded", timeout=30)
        assert "2 next hops, only one is supported" in rstate(m, "last-error")
        time.sleep(2)
        assert lab.dev.routes() == ecmp
        lab.dev.set_routes({})             # someone removes them; the next round puts running back
        wait_for(lambda: lab.dev.routes() == TWO_DEV, "routes back", timeout=30)
        wait_for(lambda: rstate(m, "health") == "ok", "routes health ok", timeout=30)


def test_one_edit_two_modules_aborted_together(lab):
    """VLANs and routes in one edit-config; another subscriber rejects: both are undone"""
    lab.start_agent(prio=10)
    lab.start_rejector()
    with lab.nc() as m:
        cfg = (f'<config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">'
               f'<switch xmlns="urn:example:labsw-switch">{ACCESS10}</switch>'
               f'<routes xmlns="{RNS}">{TWO}</routes></config>')
        with pytest.raises(RPCError):
            m.edit_config(target="running", config=cfg)
    assert lab.dev.routes() == {}
    assert lab.ports() == {"p1": {}, "p2": {}, "p3": {}}
    log = lab.log("labswd")
    assert "routes op" in log and "aborted by another subscriber" in log


def test_one_edit_two_modules_applied_together(lab):
    lab.start_agent()
    with lab.nc() as m:
        cfg = (f'<config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">'
               f'<switch xmlns="urn:example:labsw-switch">{ACCESS10}</switch>'
               f'<routes xmlns="{RNS}">{TWO}</routes></config>')
        m.edit_config(target="running", config=cfg)
    assert lab.dev.routes() == TWO_DEV
    assert lab.ports() == ACCESS10_DEV

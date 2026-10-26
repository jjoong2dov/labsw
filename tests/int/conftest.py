"""Integration tests: real sysrepo + netopeer2, labswd, and a device that is either
  LABSW_TARGET=mock    (default) the mock adapter, a JSON file; any user
  LABSW_TARGET=bridge  a real Linux bridge in its own network namespace; root
Every test gets its own repository, SHM prefix, port and (bridge) namespaces, so
the tests do not touch a labswd or netopeer2 already running on the machine.

Needs the stack of the same versions (LABSW_PREFIX, default ~/opt/labsw-host),
a test build of labsw (LABSW_BUILD, default <repo>/build) and an SSH key pair
for NETCONF (LABSW_NC_KEY, default $LABSW_PREFIX/keys/nc_test).
SPDX-License-Identifier: MIT
"""
import getpass
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import time
import uuid
from pathlib import Path

import pytest
from ncclient import manager

REPO = Path(__file__).resolve().parents[2]
PREFIX = Path(os.environ.get("LABSW_PREFIX", Path.home() / "opt/labsw-host"))
BUILD = Path(os.environ.get("LABSW_BUILD", REPO / "build"))
KEY = Path(os.environ.get("LABSW_NC_KEY", PREFIX / "keys/nc_test"))
TARGET = os.environ.get("LABSW_TARGET", "mock")
NS = "urn:example:labsw-switch"

SERVER_XML = """\
<netconf-server xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-server">
 <listen><endpoints><endpoint><name>test</name><ssh>
  <tcp-server-parameters><local-bind>
   <local-address>127.0.0.1</local-address><local-port>{port}</local-port>
  </local-bind></tcp-server-parameters>
  <ssh-server-parameters>
   <server-identity><host-key><name>default-key</name><public-key>
    <central-keystore-reference>genkey</central-keystore-reference>
   </public-key></host-key></server-identity>
   <client-authentication><users><user><name>{user}</name><public-keys><inline-definition>
    <public-key><name>test</name>
     <public-key-format xmlns:ct="urn:ietf:params:xml:ns:yang:ietf-crypto-types">ct:ssh-public-key-format</public-key-format>
     <public-key>{pubkey}</public-key>
    </public-key>
   </inline-definition></public-keys></user></users></client-authentication>
  </ssh-server-parameters>
 </ssh></endpoint></endpoints></listen>
</netconf-server>
<nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
 <groups><group><name>admin</name><user-name>{user}</user-name></group></groups>
 <rule-list><name>admin-all</name><group>admin</group>
  <rule><name>permit-all</name><module-name>*</module-name><access-operations>*</access-operations><action>permit</action></rule>
 </rule-list>
</nacm>
"""


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for(cond, what, timeout=10.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if cond():
            return
        time.sleep(0.05)
    raise TimeoutError(f"timed out after {timeout}s waiting for {what}")

class MockDevice:
    """The mock adapter's JSON file; trace.txt has every command the adapter ran."""

    def __init__(self, lab):
        self.state = lab.base / "state.json"
        self.trace = lab.base / "trace.txt"

    def setup(self, ports):
        self._write({"vlan_filtering": True, "ports": {p: [] for p in ports}})

    def teardown(self):
        pass

    def agent_args(self):
        return ["-a", "mock", "-m", str(self.state), "-t", str(self.trace)]

    def _write(self, st):
        self.state.write_text(json.dumps(st, indent=1))

    def ports(self):
        """{port: {vid: (pvid, untagged)}}"""
        st = json.loads(self.state.read_text())
        return {p: {e["vid"]: (e.get("pvid", False), e.get("untagged", False)) for e in lst}
                for p, lst in st["ports"].items()}

    def set_ports(self, ports):
        """change the device behind the agent's back"""
        st = json.loads(self.state.read_text())
        for p, vlans in ports.items():
            st["ports"][p] = [{"vid": v, "pvid": pv, "untagged": u} for v, (pv, u) in vlans.items()]
        self._write(st)

    def set_filtering(self, on):
        st = json.loads(self.state.read_text())
        st["vlan_filtering"] = on
        self._write(st)

    def commands(self, lab):
        return self.trace.read_text().splitlines() if self.trace.exists() else []


class BridgeDevice:
    """br0 with ports p1..pN in a fresh network namespace; the other end of port pN
    is eN in namespace <ns>-hN (a host for traffic tests)."""

    def __init__(self, lab):
        self.ns = lab.prefix
        self.lab = lab
        self.ports_ = []

    def sh(self, *argv, ns=None, check=True):
        ns = ns or self.ns
        return subprocess.run(["ip", "netns", "exec", ns, *argv], check=check, capture_output=True, text=True)

    def setup(self, ports):
        self.ports_ = list(ports)
        subprocess.run(["ip", "netns", "add", self.ns], check=True)
        # vlan_default_pvid 0: a new port has no VLAN at all, like an unlisted port in the model
        self.sh("ip", "link", "add", "br0", "type", "bridge", "vlan_filtering", "1", "vlan_default_pvid", "0")
        self.sh("ip", "link", "set", "br0", "up")
        for i, p in enumerate(self.ports_, 1):
            h = f"{self.ns}-h{i}"
            subprocess.run(["ip", "netns", "add", h], check=True)
            self.sh("ip", "link", "add", p, "type", "veth", "peer", "name", f"e{i}")
            self.sh("ip", "link", "set", f"e{i}", "netns", h)
            self.sh("ip", "link", "set", p, "master", "br0", "up")
            self.sh("ip", "link", "set", f"e{i}", "up", ns=h)
            self.sh("ip", "link", "set", "lo", "up", ns=h)

    def teardown(self):
        for i in range(1, len(self.ports_) + 1):
            subprocess.run(["ip", "netns", "del", f"{self.ns}-h{i}"], capture_output=True)
        subprocess.run(["ip", "netns", "del", self.ns], capture_output=True)

    def agent_args(self):
        return ["-b", "br0"]

    def agent_prefix(self):
        return ["ip", "netns", "exec", self.ns]

    def ports(self):
        out = json.loads(self.sh("bridge", "-j", "vlan", "show").stdout or "[]")
        res = {p: {} for p in self.ports_}
        for e in out:
            if e["ifname"] not in res:
                continue  # br0 itself
            for v in e.get("vlans", []):
                flags = v.get("flags", [])
                for vid in range(v["vlan"], v.get("vlanEnd", v["vlan"]) + 1):
                    res[e["ifname"]][vid] = ("PVID" in flags, "Egress Untagged" in flags)
        return res

    def set_ports(self, ports):
        now = self.ports()
        for p, vlans in ports.items():
            for vid in now[p]:
                self.sh("bridge", "vlan", "del", "dev", p, "vid", str(vid))
            for vid, (pv, u) in vlans.items():
                self.sh("bridge", "vlan", "add", "dev", p, "vid", str(vid),
                        *(["pvid"] if pv else []), *(["untagged"] if u else []))

    def set_filtering(self, on):
        self.sh("ip", "link", "set", "br0", "type", "bridge", "vlan_filtering", "1" if on else "0")

    CMD = re.compile(r"^labswd: (?:restore: )?((?:set|del) .*) -> ok$", re.M)

    def commands(self, lab):
        """device commands that succeeded, from the agent's log"""
        return self.CMD.findall(lab.log("labswd"))

    def host_ip(self, i, addr):
        h = f"{self.ns}-h{i}"
        self.sh("ip", "addr", "add", addr, "dev", f"e{i}", ns=h)

    def ping(self, i, addr):
        h = f"{self.ns}-h{i}"
        return self.sh("ping", "-c", "1", "-W", "1", addr, ns=h, check=False).returncode == 0


class Lab:
    def __init__(self, base: Path):
        self.base = base
        self.prefix = "lswt" + uuid.uuid4().hex[:8]
        self.port = free_port()
        self.user = getpass.getuser()
        self.dev = BridgeDevice(self) if TARGET == "bridge" else MockDevice(self)
        self.env = dict(os.environ,
                        SYSREPO_REPOSITORY_PATH=str(base / "repo"),
                        SYSREPO_SHM_PREFIX=self.prefix,
                        PATH=f"{PREFIX}/bin:{PREFIX}/sbin:" + os.environ["PATH"],
                        LD_LIBRARY_PATH=f"{PREFIX}/lib:" + os.environ.get("LD_LIBRARY_PATH", ""))
        self.procs = {}

    def run(self, *argv, check=True):
        return subprocess.run(argv, env=self.env, check=check, capture_output=True, text=True)

    def setup(self, ports=("p1", "p2", "p3")):
        self.run(str(REPO / "tests/int/mkrepo.sh"), str(self.base / "repo"))
        pub = KEY.with_suffix(".pub").read_text().split()[1]
        xml = self.base / "server.xml"
        xml.write_text(SERVER_XML.format(port=self.port, user=self.user, pubkey=pub))
        self.run("sysrepocfg", f"--edit={xml}", "-d", "running")
        self.dev.setup(ports)  # ports with no VLAN at all

    # --- processes ---------------------------------------------------------
    def _start(self, name, argv, ready):
        log = open(self.base / f"{name}.log", "a")
        p = subprocess.Popen(argv, env=self.env, stdout=log, stderr=subprocess.STDOUT)
        self.procs[name] = p
        try:
            wait_for(lambda: p.poll() is not None or ready(), f"{name} to start")
        except TimeoutError:
            raise TimeoutError(f"{name} did not start, log:\n{self.log(name)}")
        if p.poll() is not None:
            raise RuntimeError(f"{name} exited with {p.returncode}, log:\n{self.log(name)}")
        return p

    def start_server(self):
        def listening():
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                return True
            except OSError:
                return False
        (self.base / "np2").mkdir(exist_ok=True)
        return self._start("netopeer2", ["netopeer2-server", "-d", "-v1",
                                         "-p", str(self.base / "np2.pid"),
                                         "-f", str(self.base / "np2")], listening)

    def start_agent(self, fail_at=None, prio=None, every=None):
        argv = getattr(self.dev, "agent_prefix", list)() + [str(BUILD / "labswd")] + self.dev.agent_args()
        if prio is not None:
            argv += ["-P", str(prio)]
        if every is not None:
            argv += ["-R", str(every)]
        if fail_at:
            argv += ["-F", str(fail_at)]
        start = self.log("labswd").count("labswd: running")
        return self._start("labswd", argv, lambda: self.log("labswd").count("labswd: running") > start)

    def start_rejector(self):
        return self._start("reject_sub", [str(BUILD / "reject_sub")], lambda: "ready" in self.log("reject_sub"))

    def stop(self, name, sig=signal.SIGTERM):
        p = self.procs.pop(name, None)
        if p and p.poll() is None:
            p.send_signal(sig)
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        return p.returncode if p else None

    def log(self, name):
        f = self.base / f"{name}.log"
        return f.read_text(errors="replace") if f.exists() else ""

    def teardown(self):
        for name in list(self.procs):
            self.stop(name)
        self.dev.teardown()
        for f in Path("/dev/shm").glob(self.prefix + "*"):
            f.unlink(missing_ok=True)

    # --- the device --------------------------------------------------------
    def ports(self):
        return self.dev.ports()

    def trace_lines(self):
        return self.dev.commands(self)

    # --- NETCONF -----------------------------------------------------------
    def nc(self):
        # password="": ncclient 0.6.17 (Debian 13) fails with AttributeError on None
        return manager.connect(host="127.0.0.1", port=self.port, username=self.user,
                               password="", key_filename=str(KEY), hostkey_verify=False,
                               allow_agent=False, look_for_keys=False, timeout=20)


@pytest.fixture
def lab(tmp_path_factory):
    for need in (BUILD / "labswd", BUILD / "reject_sub", PREFIX / "sbin/netopeer2-server", KEY):
        if not need.exists():
            pytest.skip(f"missing {need}")
    if TARGET == "bridge" and os.geteuid() != 0:
        pytest.skip("LABSW_TARGET=bridge needs root (network namespaces)")
    # short paths: sysrepo and netopeer2 keep UNIX sockets under them
    base = Path(tmp_path_factory.mktemp("lab", numbered=True))
    lab = Lab(base)
    try:
        lab.setup()
        lab.start_server()
        yield lab
    finally:
        lab.teardown()
        if os.environ.get("LABSW_KEEP") != "1":
            shutil.rmtree(base / "repo", ignore_errors=True)

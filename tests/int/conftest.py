"""Host integration tests: real sysrepo + netopeer2 as the current user, labswd
with the mock adapter. Every test gets its own repository, SHM prefix and port.

Needs the stack of the same versions (LABSW_PREFIX, default ~/opt/labsw-host),
a test build of labsw (LABSW_BUILD, default <repo>/build) and an SSH key pair
for NETCONF (LABSW_NC_KEY, default $LABSW_PREFIX/keys/nc_test).
SPDX-License-Identifier: MIT
"""
import getpass
import json
import os
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


class Lab:
    def __init__(self, base: Path):
        self.base = base
        self.prefix = "lswt" + uuid.uuid4().hex[:8]
        self.port = free_port()
        self.user = getpass.getuser()
        self.state = base / "state.json"
        self.trace = base / "trace.txt"
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
        # ports with no VLAN at all, like freshly enslaved bridge ports
        self.write_state({"vlan_filtering": True, "ports": {p: [] for p in ports}})

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

    def start_agent(self, fail_at=None, prio=None):
        argv = [str(BUILD / "labswd"), "-a", "mock", "-m", str(self.state), "-t", str(self.trace)]
        if prio is not None:
            argv += ["-P", str(prio)]
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
        for f in Path("/dev/shm").glob(self.prefix + "*"):
            f.unlink(missing_ok=True)

    # --- the fake device ---------------------------------------------------
    def read_state(self):
        return json.loads(self.state.read_text())

    def write_state(self, st):
        self.state.write_text(json.dumps(st, indent=1))

    def ports(self):
        """{port: {vid: (pvid, untagged)}} of the mock device"""
        return {p: {e["vid"]: (e.get("pvid", False), e.get("untagged", False)) for e in lst}
                for p, lst in self.read_state()["ports"].items()}

    def trace_lines(self):
        return self.trace.read_text().splitlines() if self.trace.exists() else []

    # --- NETCONF -----------------------------------------------------------
    def nc(self):
        return manager.connect(host="127.0.0.1", port=self.port, username=self.user,
                               key_filename=str(KEY), hostkey_verify=False,
                               allow_agent=False, look_for_keys=False, timeout=20)


@pytest.fixture
def lab(tmp_path_factory):
    for need in (BUILD / "labswd", BUILD / "reject_sub", PREFIX / "sbin/netopeer2-server", KEY):
        if not need.exists():
            pytest.skip(f"missing {need}")
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

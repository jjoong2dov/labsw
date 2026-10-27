#!/usr/bin/env python3
"""devlab.py start|stop|status : a NETCONF device on this PC to play with, no root needed.

start  makes a sysrepo repository of its own in DIR (default ~/.labsw-devlab), with the
       labsw modules, a host key and a client key, then starts netopeer2-server on
       127.0.0.1:PORT (default 8301) and labswd on a mock device (ports p1 p2 p3 in
       DIR/device.json; every command it runs is added to DIR/trace.txt).
       Logs: DIR/netopeer2.log, DIR/labswd.log. Use tools/nc.py to talk to it.
stop   stops both and removes the shared memory of the repository (DIR stays).
       `start --fresh` begins with an empty repository again.

Needs the stack (LABSW_PREFIX, default ~/opt/labsw-host, from tools/build_stack.sh host)
and a test build of labsw in build/ (cmake -DLABSW_TESTING=ON).
SPDX-License-Identifier: MIT
"""
import argparse
import getpass
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
PREFIX = Path(os.environ.get("LABSW_PREFIX", Path.home() / "opt/labsw-host"))
SHM = "labswdev"

SERVER_XML = """\
<netconf-server xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-server">
 <listen><endpoints><endpoint><name>devlab</name><ssh>
  <tcp-server-parameters><local-bind>
   <local-address>127.0.0.1</local-address><local-port>{port}</local-port>
  </local-bind></tcp-server-parameters>
  <ssh-server-parameters>
   <server-identity><host-key><name>default-key</name><public-key>
    <central-keystore-reference>genkey</central-keystore-reference>
   </public-key></host-key></server-identity>
   <client-authentication><users><user><name>{user}</name><public-keys><inline-definition>
    <public-key><name>devlab</name>
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


def env_for(d):
    return dict(os.environ, SYSREPO_REPOSITORY_PATH=str(d / "repo"), SYSREPO_SHM_PREFIX=SHM,
                PATH=f"{PREFIX}/bin:{PREFIX}/sbin:" + os.environ["PATH"],
                LD_LIBRARY_PATH=f"{PREFIX}/lib:" + os.environ.get("LD_LIBRARY_PATH", ""))


def pid_alive(f):
    try:
        pid = int(f.read_text())
        os.kill(pid, 0)
        return pid
    except (OSError, ValueError):
        return None


def listening(port):
    try:
        socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
        return True
    except OSError:
        return False


def start(a):
    d = a.dir
    if pid_alive(d / "netopeer2.pid") or pid_alive(d / "labswd.pid"):
        sys.exit(f"devlab: already running in {d} (devlab.py stop first)")
    for need in (REPO / "build/labswd", PREFIX / "sbin/netopeer2-server"):
        if not need.exists():
            sys.exit(f"devlab: missing {need}")
    env = env_for(d)
    if a.fresh and d.exists():
        shutil.rmtree(d)
    d.mkdir(parents=True, exist_ok=True)
    if not (d / "repo").exists():
        print(f"making the repository in {d / 'repo'} ...")
        subprocess.run([str(REPO / "tests/int/mkrepo.sh"), str(d / "repo")], env=env, check=True,
                       stdout=subprocess.DEVNULL)
        subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(d / "client_key")], check=True)
        pub = (d / "client_key.pub").read_text().split()[1]
        xml = d / "server.xml"
        xml.write_text(SERVER_XML.format(port=a.port, user=getpass.getuser(), pubkey=pub))
        for ds in ("running", "startup"):   # running is what the server reads now, startup after a reboot
            subprocess.run(["sysrepocfg", f"--edit={xml}", "-d", ds], env=env, check=True)
        (d / "device.json").write_text(json.dumps({"vlan_filtering": True, "ports": {"p1": [], "p2": [], "p3": []}},
                                                  indent=1))
        (d / "routes.json").write_text('{"routes": {}}')
        (d / "port").write_text(str(a.port))
    port = int((d / "port").read_text())
    if listening(port):
        sys.exit(f"devlab: something already listens on 127.0.0.1:{port}")
    log = open(d / "netopeer2.log", "a")
    # -d: stay in the foreground, log to stdout. -p is needed (the default is under /var/run)
    # but with -d the server does not write it, so the pid is kept here
    np2 = subprocess.Popen(["netopeer2-server", "-d", "-v1", "-p", str(d / "np2.pid"), "-f", str(d / "np2")],
                           env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    (d / "netopeer2.pid").write_text(str(np2.pid))
    for _ in range(100):
        if listening(port):
            break
        time.sleep(0.1)
    else:
        sys.exit(f"devlab: netopeer2-server did not start, see {d / 'netopeer2.log'}")
    log = open(d / "labswd.log", "a")
    p = subprocess.Popen([str(REPO / "build/labswd"), "-a", "mock", "-m", str(d / "device.json"),
                          "-r", str(d / "routes.json"), "-t", str(d / "trace.txt"), "-B", str(d / "backup"),
                          "-R", str(a.every)],
                         env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    (d / "labswd.pid").write_text(str(p.pid))
    for _ in range(100):
        if "labswd: running" in (d / "labswd.log").read_text(errors="replace").split("signal")[-1]:
            break
        if p.poll() is not None:
            sys.exit(f"devlab: labswd exited, see {d / 'labswd.log'}")
        time.sleep(0.1)
    print(f"devlab: NETCONF on 127.0.0.1:{port}, user {getpass.getuser()}, key {d / 'client_key'}")
    print(f"        device {d / 'device.json'}, commands {d / 'trace.txt'}, logs {d}/*.log")
    print("        try: tools/nc.py hello")


def stop(a):
    d = a.dir
    for name in ("labswd", "netopeer2"):
        pid = pid_alive(d / f"{name}.pid")
        if pid:
            os.kill(pid, signal.SIGTERM)
            for _ in range(100):
                try:
                    os.kill(pid, 0)
                except OSError:
                    break
                time.sleep(0.1)
            print(f"devlab: stopped {name} ({pid})")
        (d / f"{name}.pid").unlink(missing_ok=True)
    for f in Path("/dev/shm").glob(SHM + "*"):
        f.unlink(missing_ok=True)


def status(a):
    d = a.dir
    for name in ("netopeer2", "labswd"):
        pid = pid_alive(d / f"{name}.pid")
        print(f"{name}: {'running, pid ' + str(pid) if pid else 'not running'}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["start", "stop", "status"])
    ap.add_argument("--dir", type=Path, default=Path(os.environ.get("LABSW_DEVLAB", Path.home() / ".labsw-devlab")))
    ap.add_argument("--port", type=int, default=8301, help="first start only")
    ap.add_argument("--every", type=int, default=5, help="labswd -R: seconds between reconcile rounds")
    ap.add_argument("--fresh", action="store_true", help="start: throw the old repository away first")
    a = ap.parse_args()
    a.dir = a.dir.expanduser().resolve()
    {"start": start, "stop": stop, "status": status}[a.command](a)


if __name__ == "__main__":
    main()

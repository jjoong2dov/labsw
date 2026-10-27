#!/usr/bin/env python3
"""nc.py COMMAND ... : a small NETCONF client (ncclient) for tools/devlab.py or a device.

  hello                      session id, NETCONF capabilities and the YANG modules of the server
  get-config [FILTER]        <get-config> of running (or --source startup|candidate)
  get [FILTER]               <get>: configuration and state
  edit FILE|-                <edit-config> to running with the <config> in FILE (- = stdin);
                             FILE may hold just the inside of <config>
  copy SOURCE TARGET         <copy-config>, e.g. copy running startup
  rpc FILE|-                 any RPC: the XML of its element, e.g. <backup xmlns="urn:example:labsw-ops">...
  listen [SECONDS]           <create-subscription>, then print notifications (default 60 s)

FILTER is a subtree filter, e.g. '<switch xmlns="urn:example:labsw-switch"/>'.
Connection: --host/--port/--user/--key, default the devlab of this user
(127.0.0.1, the port and key in ~/.labsw-devlab). Errors are printed as the server sent them.
SPDX-License-Identifier: MIT
"""
import argparse
import getpass
import os
import sys
import time
from pathlib import Path

from ncclient import manager
from ncclient.operations import RPCError
from ncclient.xml_ import to_ele, to_xml

NC = "urn:ietf:params:xml:ns:netconf:base:1.0"
YLIB = '<yang-library xmlns="urn:ietf:params:xml:ns:yang:ietf-yang-library"/>'


def read(arg):
    text = sys.stdin.read() if arg == "-" else Path(arg).read_text()
    return text.strip()


def pretty(xml):
    try:
        from xml.dom import minidom
        lines = minidom.parseString(xml).toprettyxml(indent="  ").splitlines()[1:]
        return "\n".join(l for l in lines if l.strip())
    except Exception:
        return xml


def main():
    devlab = Path(os.environ.get("LABSW_DEVLAB", Path.home() / ".labsw-devlab"))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["hello", "get-config", "get", "edit", "copy", "rpc", "listen"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int,
                    default=int((devlab / "port").read_text()) if (devlab / "port").exists() else 830)
    ap.add_argument("--user", default=getpass.getuser())
    ap.add_argument("--key", default=str(devlab / "client_key"))
    ap.add_argument("--source", default="running")
    a = ap.parse_args()

    # password="": ncclient 0.6.x fails on None when it falls back to password authentication
    m = manager.connect(host=a.host, port=a.port, username=a.user, password="", key_filename=a.key,
                        hostkey_verify=False, allow_agent=False, look_for_keys=False, timeout=30)
    try:
        if a.command == "hello":
            print(f"session-id {m.session_id}")
            caps = sorted(m.server_capabilities)
            print(f"{len(caps)} capabilities in <hello>:")
            for c in caps:
                print("  " + c)
            lib = m.get(filter=("subtree", YLIB)).data_ele
            mods = sorted({(e.findtext("{*}name"), e.findtext("{*}revision") or "")
                           for e in lib.iter("{*}module")})
            print(f"{len(mods)} modules in the YANG library (labsw ones):")
            for name, rev in mods:
                if name.startswith("labsw"):
                    print(f"  {name}@{rev}")
        elif a.command in ("get-config", "get"):
            flt = ("subtree", a.args[0]) if a.args else None
            r = m.get_config(a.source, filter=flt) if a.command == "get-config" else m.get(filter=flt)
            print(pretty(r.data_xml))
        elif a.command == "edit":
            body = read(a.args[0])
            if not body.startswith("<config"):
                body = f'<config xmlns="{NC}">{body}</config>'
            print(pretty(m.edit_config(target="running", config=body).xml))
        elif a.command == "copy":
            print(pretty(m.copy_config(source=a.args[0], target=a.args[1]).xml))
        elif a.command == "rpc":
            print(pretty(m.dispatch(to_ele(read(a.args[0]))).xml))
        elif a.command == "listen":
            secs = float(a.args[0]) if a.args else 60
            m.create_subscription()
            print(f"subscribed, waiting {secs:.0f} s for notifications ...", flush=True)
            end = time.monotonic() + secs
            while time.monotonic() < end:
                n = m.take_notification(block=True, timeout=max(0.1, end - time.monotonic()))
                if n is not None:
                    print(pretty(n.notification_xml), flush=True)
    except RPCError as e:
        print(f"rpc-error: {e.tag}: {e.message}", file=sys.stderr)
        if e.path:
            print(f"  error-path: {e.path}", file=sys.stderr)
        sys.exit(1)
    finally:
        m.close_session()


if __name__ == "__main__":
    main()

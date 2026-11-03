#!/usr/bin/env python3
"""Minimal syslog (UDP) receiver for the dongles' `syslog:` stream (common/syslog.yaml).

Every datagram is written, prefixed with the receive time, to
``dist/logs/<sender-ip>/<YYYY-MM-DD>.log`` (one file per sender and local day, so files rotate
daily). Nothing is printed per message; start-up prints the bound port and the output directory.

    python3 -I tools/syslog_receiver.py                # UDP 5514 (common/syslog.yaml default)
    python3 -I tools/syslog_receiver.py --port 514     # needs root / CAP_NET_BIND_SERVICE

Stdlib only.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import re
import socket
import sys
from pathlib import Path

DEFAULT_PORT = 5514
_PRI_RE = re.compile(rb"^<(\d{1,3})>")
_SEVERITY = ("EMERG", "ALERT", "CRIT", "ERROR", "WARN", "NOTICE", "INFO", "DEBUG")


def format_line(data: bytes, now: _dt.datetime) -> str:
    """One log line for a received datagram: receive time, severity, then the payload without PRI."""
    sev = "-"
    m = _PRI_RE.match(data)
    if m:
        pri = int(m.group(1))
        sev = _SEVERITY[pri & 7]
        data = data[m.end():]
    text = data.decode("utf-8", errors="replace").rstrip("\r\n\x00")
    return f"{now.strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]} {sev:<6} {text}\n"


class Sink:
    """Per-sender, per-day append-only files."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.files: dict[str, tuple[str, object]] = {}

    def write(self, host: str, line: str, now: _dt.datetime) -> None:
        day = now.strftime("%Y-%m-%d")
        cur = self.files.get(host)
        if cur is None or cur[0] != day:
            if cur is not None:
                cur[1].close()
            d = self.root / host
            d.mkdir(parents=True, exist_ok=True)
            f = open(d / f"{day}.log", "a", encoding="utf-8", buffering=1)
            self.files[host] = (day, f)
            cur = self.files[host]
        cur[1].write(line)


def main(argv: list[str] | None = None) -> int:
    here = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--out", type=Path, default=here / "dist" / "logs")
    args = ap.parse_args(argv)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind((args.bind, args.port))
    except PermissionError:
        print(f"cannot bind UDP {args.port} (privileged port?); use --port {DEFAULT_PORT}", file=sys.stderr)
        return 2
    sink = Sink(args.out)
    print(f"syslog receiver: UDP {args.bind}:{args.port} -> {args.out}/<host>/<date>.log", flush=True)
    while True:
        data, (host, _port) = sock.recvfrom(4096)
        now = _dt.datetime.now()
        sink.write(host, format_line(data, now), now)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)

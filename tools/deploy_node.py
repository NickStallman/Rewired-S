#!/usr/bin/env python3
"""Careful native-OTA deploy of an ESPHome node, with logs captured across the reboot.

    python3 -I tools/deploy_node.py sites/<name>.yaml --device 192.0.2.10
    make deploy NODE=sites/<name> DEVICE=192.0.2.10

Steps:
  1. `esphome compile`; read the new config hash + build time (build_info.json).
  2. Archive firmware.elf / firmware.ota.bin / sdkconfig to dist/elf/<node>/<buildtime>_<hash>/,
     so every backtrace can be decoded against the build that produced it.
  3. Read the running build (web server /events: "ESPHome version" -> config hash, Uptime).
  4. Stream `esphome logs` to dist/logs/<node>/deploy_<ts>.log, started before the upload and
     restarted whenever it exits, so the old build's last lines, the reboot and the new build's
     first boot are all in one file.
  5. `esphome upload` with retries.
  6. Watch the running hash/uptime for --watch seconds: "new build running and marked valid"
     (new hash, uptime past --valid-after) or "rolled back" (old hash back, or "OTA rollback
     detected" in the log). Reboots seen during the watch are reported.
  7. Save GET /api/diag (reset journal) next to the log when the build serves it (404 tolerated).
  8. Decode code addresses from crash/backtrace lines in the log and the diag text with
     xtensa-*-addr2line against the archived ELF of the build that recorded them (matched by
     config hash or build time), else the new build's ELF; written to <log>.decoded.txt.

Exit status: 0 = new build running and valid, 1 = upload failed, 2 = rolled back / never came
up, 3 = compile failed. Credentials (web_username / web_password) are read from secrets.yaml
inside this process and never printed. Stdlib only; run with `python3 -I`.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import glob
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ============================ pure helpers (host-tested) ============================

_HASH_RE = re.compile(r"config hash (0x[0-9a-fA-F]{8})")
# Addresses that can be code on ESP32 / ESP32-S3: IRAM 0x4037.., flash text 0x4200.., ROM 0x4000..
_ADDR_RE = re.compile(r"0x(4[0-2][0-9a-fA-F]{6})\b")
_CRASH_LINE_RE = re.compile(
    r"(Backtrace:|\bBT\d+:|\bbt\d+:|\bPC:|\bpc:|Guru Meditation|CRASH DETECTED|panic|EXCVADDR|"
    r"\bbacktrace\b|\babort\(\))", re.IGNORECASE)
# Build references a crash record / journal may carry.
_REF_HASH_RE = re.compile(r"(?:config[ _]hash|hash|build)[\s:=]*\b(0x[0-9a-fA-F]{8})\b", re.IGNORECASE)
_REF_TIME_RE = re.compile(r"build[ _]time[\s:=]*(\d{10})", re.IGNORECASE)


def parse_version_hash(text: str) -> str | None:
    """Config hash from the "ESPHome version" text sensor ("2026.9.1 (config hash 0xbfdd4f7f)")."""
    m = _HASH_RE.search(text or "")
    return m.group(1).lower() if m else None


def format_hash(value: int) -> str:
    return f"0x{value & 0xFFFFFFFF:08x}"


def archive_name(build_time: int, config_hash: str) -> str:
    """Archive directory name: local build time (sortable) + config hash."""
    ts = _dt.datetime.fromtimestamp(build_time).strftime("%Y%m%d-%H%M%S")
    return f"{ts}_{config_hash.lower()}"


def crash_lines(text: str) -> list[str]:
    """Lines that belong to a crash record or backtrace and carry at least one code address."""
    out = []
    for line in text.splitlines():
        if _CRASH_LINE_RE.search(line) and _ADDR_RE.search(line):
            out.append(line)
    return out


def code_addresses(lines: list[str]) -> list[str]:
    """Unique code addresses (0x4xxxxxxx), in order of first appearance."""
    seen: dict[str, None] = {}
    for line in lines:
        for m in _ADDR_RE.finditer(line):
            a = "0x" + m.group(1).lower()
            seen.setdefault(a, None)
    return list(seen)


def build_refs(text: str) -> tuple[set[str], set[int]]:
    """Config hashes and build times referenced in a crash record / journal text."""
    hashes = {m.group(1).lower() for m in _REF_HASH_RE.finditer(text)}
    times = {int(m.group(1)) for m in _REF_TIME_RE.finditer(text)}
    return hashes, times


def pick_archives(archives: list[Path], hashes: set[str], times: set[int]) -> list[Path]:
    """Archive dirs (named <YYYYmmdd-HHMMSS>_<hash>) whose hash or build time is referenced."""
    picked = []
    for a in archives:
        name = a.name
        h = name.rsplit("_", 1)[-1].lower()
        if h in hashes:
            picked.append(a)
            continue
        meta = a / "build_info.json"
        if times and meta.exists():
            try:
                if int(json.loads(meta.read_text()).get("build_time", -1)) in times:
                    picked.append(a)
            except (ValueError, OSError):
                pass
    return picked


def read_secrets(path: Path) -> dict[str, str]:
    """Flat `key: value` pairs from an ESPHome secrets.yaml (values are never printed)."""
    sec = {}
    for line in path.read_text().splitlines():
        m = re.match(r'^(\w+):\s*"?([^"\n]*?)"?\s*$', line)
        if m:
            sec[m.group(1)] = m.group(2)
    return sec


# ============================ device / tool access ============================

class Web:
    """Digest-auth HTTP access to the node's web server."""

    def __init__(self, host: str, user: str | None, password: str | None) -> None:
        self.base = f"http://{host}"
        handlers = []
        if user and password:
            pm = urllib.request.HTTPPasswordMgrWithDefaultRealm()
            pm.add_password(None, self.base + "/", user, password)
            handlers = [urllib.request.HTTPDigestAuthHandler(pm), urllib.request.HTTPBasicAuthHandler(pm)]
        self.opener = urllib.request.build_opener(*handlers)

    def states(self, seconds: float = 4.0) -> dict[str, object]:
        """Entity states from the /events stream (the initial state burst)."""
        seen: dict[str, object] = {}
        try:
            r = self.opener.open(self.base + "/events", timeout=6)
        except (urllib.error.URLError, OSError, socket.timeout):
            return seen
        end = time.time() + seconds
        try:
            while time.time() < end:
                line = r.readline().decode(errors="replace").strip()
                if line.startswith("data:"):
                    try:
                        d = json.loads(line[5:])
                    except ValueError:
                        continue
                    if isinstance(d, dict) and "id" in d:
                        # Numeric sensors carry the number in "value" and "<n> <unit>" in "state".
                        seen[d["id"]] = d["value"] if "value" in d else d.get("state")
        except (socket.timeout, OSError):
            pass
        finally:
            try:
                r.close()
            except OSError:
                pass
        return seen

    def get(self, path: str) -> tuple[int, bytes]:
        try:
            r = self.opener.open(self.base + path, timeout=15)
            return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, b""
        except (urllib.error.URLError, OSError, socket.timeout):
            return 0, b""


def running_build(web: Web) -> tuple[str | None, float | None, str | None]:
    """(config hash, uptime s, reset reason) of the running firmware; None when unreachable."""
    st = web.states()
    ver = uptime = reason = None
    for k, v in st.items():
        kl = k.lower()
        if kl.endswith("esphome_version") or kl.endswith("esphome version"):
            ver = parse_version_hash(str(v))
        elif kl.endswith("/uptime") or kl.endswith("-uptime") or kl == "uptime":
            try:
                uptime = float(v)
            except (TypeError, ValueError):
                pass
        elif kl.endswith("reset_reason") or kl.endswith("reset reason"):
            reason = str(v)
    return ver, uptime, reason


class LogStream:
    """`esphome logs` to a file with a timestamp per line; restarted whenever it exits."""

    def __init__(self, esphome: str, yaml: str, device: str, path: Path) -> None:
        self.cmd = [esphome, "logs", yaml, "--device", device]
        self.path = path
        self.stop = threading.Event()
        self.lines: list[str] = []
        self.lock = threading.Lock()
        self.proc: subprocess.Popen | None = None
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.thread.start()

    def _run(self) -> None:
        with open(self.path, "a", encoding="utf-8", buffering=1) as f:
            while not self.stop.is_set():
                try:
                    self.proc = subprocess.Popen(self.cmd, cwd=ROOT, stdout=subprocess.PIPE,
                                                 stderr=subprocess.STDOUT, text=True, errors="replace")
                except OSError as e:
                    f.write(f"{_now()} [deploy] cannot start esphome logs: {e}\n")
                    return
                assert self.proc.stdout is not None
                for line in self.proc.stdout:
                    stamped = f"{_now()} {line}"
                    f.write(stamped)
                    with self.lock:
                        self.lines.append(stamped)
                    if self.stop.is_set():
                        break
                self.proc.wait()
                if not self.stop.is_set():
                    f.write(f"{_now()} [deploy] esphome logs exited ({self.proc.returncode}); restarting\n")
                    self.stop.wait(2)

    def text(self, since: int = 0) -> str:
        """Captured lines from index `since` on (see mark())."""
        with self.lock:
            return "".join(self.lines[since:])

    def mark(self) -> int:
        with self.lock:
            return len(self.lines)

    def close(self) -> None:
        self.stop.set()
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.thread.join(10)


def _now() -> str:
    return _dt.datetime.now().strftime("%H:%M:%S.%f")[:-3]


def say(msg: str) -> None:
    print(f"{_now()} {msg}", flush=True)


def esphome_dir(yaml: str) -> Path:
    """ESPHome keeps storage, idedata and builds under .esphome next to the configuration file."""
    return Path(yaml).resolve().parent / ".esphome"


def find_addr2line(build_path: Path, yaml: str) -> str | None:
    """xtensa addr2line next to the compiler of this build (idedata), else any cached toolchain."""
    node = build_path.name
    idedata = esphome_dir(yaml) / "idedata" / f"{node}.json"
    try:
        cc = json.loads(idedata.read_text()).get("cc_path", "")
        cand = re.sub(r"-gcc$", "-addr2line", cc)
        if cand != cc and os.path.exists(cand):
            return cand
    except (OSError, ValueError):
        pass
    pats = [os.path.expanduser("~/.cache/esphome/idf/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-addr2line"),
            os.path.expanduser("~/.platformio/packages/toolchain-xtensa*/bin/xtensa-*-elf-addr2line")]
    for p in pats:
        hits = sorted(glob.glob(p))
        if hits:
            return hits[-1]
    return shutil.which("xtensa-esp32s3-elf-addr2line")


def decode(addr2line: str, elf: Path, addrs: list[str]) -> list[str]:
    if not addrs:
        return []
    try:
        out = subprocess.run([addr2line, "-pfiaC", "-e", str(elf), *addrs], capture_output=True, text=True,
                             timeout=60).stdout
    except (OSError, subprocess.TimeoutExpired) as e:
        return [f"addr2line failed: {e}"]
    return out.splitlines()


# ============================ deploy ============================

def compile_node(esphome: str, yaml: str) -> bool:
    say(f"compiling {yaml}")
    r = subprocess.run([esphome, "compile", yaml], cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout[-4000:])
        sys.stdout.write(r.stderr[-4000:])
        return False
    return True


def build_info(yaml: str) -> tuple[str, Path, dict]:
    storage = json.loads((esphome_dir(yaml) / "storage" / f"{Path(yaml).name}.json").read_text())
    build_path = Path(storage["build_path"])
    info = json.loads((build_path / "build_info.json").read_text())
    return storage["name"], build_path, info


def archive_build(node: str, build_path: Path, info: dict) -> Path:
    h = format_hash(int(info["config_hash"]))
    dest = ROOT / "dist" / "elf" / node / archive_name(int(info["build_time"]), h)
    dest.mkdir(parents=True, exist_ok=True)
    b = build_path / "build"
    for src, name in ((b / "firmware.elf", "firmware.elf"), (b / "firmware.ota.bin", "firmware.ota.bin"),
                      (b / "firmware.bin", "firmware.bin")):
        if src.exists():
            shutil.copy2(src, dest / name)
    for sdk in build_path.glob("sdkconfig.*"):
        if not sdk.name.endswith(".esphomeinternal"):
            shutil.copy2(sdk, dest / sdk.name)
    (dest / "build_info.json").write_text(json.dumps(info, indent=2) + "\n")
    return dest


def upload(esphome: str, yaml: str, device: str, retries: int, log_path: Path) -> bool:
    for attempt in range(1, retries + 1):
        say(f"upload attempt {attempt}/{retries}")
        try:
            r = subprocess.run([esphome, "upload", yaml, "--device", device], cwd=ROOT, capture_output=True,
                               text=True, timeout=300)
            out = r.stdout + r.stderr
        except subprocess.TimeoutExpired as e:
            out = f"timeout: {e}"
        with open(log_path.with_suffix(".upload.txt"), "a", encoding="utf-8") as f:
            f.write(f"--- attempt {attempt} {_now()}\n{out}\n")
        if "OTA successful" in out:
            say("upload: OTA successful")
            return True
        say("upload failed; retrying in 20 s" if attempt < retries else "upload failed")
        if attempt < retries:
            time.sleep(20)
    return False


def watch(web: Web, logs: LogStream, new_hash: str, old_hash: str | None, seconds: int,
          valid_after: int) -> tuple[str, list[str]]:
    """Returns (verdict, notes); verdict in {"valid", "rolled_back", "unreachable", "pending"}."""
    notes: list[str] = []
    end = time.time() + seconds
    prev_up: float | None = None
    last_hash = None
    t0 = time.time()
    since = logs.mark()  # an older rollback line in the running build's boot dump does not count
    while time.time() < end:
        h, up, reason = running_build(web)
        if h is None:
            if last_hash is not None:
                notes.append(f"{_now()} unreachable")
            last_hash = None
        else:
            if prev_up is not None and up is not None and up < prev_up:
                notes.append(f"{_now()} REBOOT: uptime {prev_up:.0f} -> {up:.0f} s, reset reason: {reason}")
            say(f"running {h} uptime {up} reset reason {reason}")
            prev_up = up if up is not None else prev_up
            last_hash = h
            # The old build only counts once it has rebooted since the upload (uptime shorter than
            # the time since then); before the OTA reboot it is still the pre-upload run.
            rebooted = up is not None and up < time.time() - t0 + 5
            if "OTA rollback detected" in logs.text(since) or (
                    old_hash and h == old_hash and h != new_hash and rebooted):
                return "rolled_back", notes
            if h == new_hash and up is not None and up >= valid_after:
                return "valid", notes
        time.sleep(10)
    if last_hash is None:
        return "unreachable", notes
    return "pending", notes


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("yaml")
    ap.add_argument("--device", required=True)
    ap.add_argument("--esphome", default=str(ROOT / ".venv" / "bin" / "esphome"))
    ap.add_argument("--secrets", default=str(ROOT / "secrets.yaml"))
    ap.add_argument("--retries", type=int, default=3)
    ap.add_argument("--watch", type=int, default=240, help="seconds to watch the new build after upload")
    ap.add_argument("--valid-after", type=int, default=60,
                    help="uptime (s) after which the new build counts as marked valid (safe_mode: 30 s)")
    ap.add_argument("--settle", type=int, default=30, help="extra log capture after the verdict (s)")
    ap.add_argument("--no-compile", action="store_true", help="deploy the existing build")
    args = ap.parse_args(argv)

    if not args.no_compile and not compile_node(args.esphome, args.yaml):
        say("compile FAILED")
        return 3
    node, build_path, info = build_info(args.yaml)
    new_hash = format_hash(int(info["config_hash"]))
    arch = archive_build(node, build_path, info)
    say(f"build {new_hash} ({info.get('build_time_str')}) archived to {arch.relative_to(ROOT)}")

    sec = read_secrets(Path(args.secrets)) if os.path.exists(args.secrets) else {}
    web = Web(args.device, sec.get("web_username"), sec.get("web_password"))
    old_hash, up, reason = running_build(web)
    say(f"running before: {old_hash} uptime {up} reset reason {reason}")
    if old_hash == new_hash:
        say("the device already runs this build; uploading anyway")

    ts = _dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    log_path = ROOT / "dist" / "logs" / node / f"deploy_{ts}.log"
    logs = LogStream(args.esphome, args.yaml, args.device, log_path)
    logs.start()
    say(f"streaming logs to {log_path.relative_to(ROOT)}")
    time.sleep(8)

    rc = 0
    try:
        if not upload(args.esphome, args.yaml, args.device, args.retries, log_path):
            rc = 1
        else:
            verdict, notes = watch(web, logs, new_hash, old_hash, args.watch, args.valid_after)
            for n in notes:
                say(n)
            if verdict == "valid":
                say(f"new build {new_hash} running and marked valid")
            elif verdict == "rolled_back":
                say(f"ROLLED BACK to {old_hash}")
                rc = 2
            else:
                say(f"new build not confirmed ({verdict})")
                rc = 2
            time.sleep(args.settle)
    finally:
        status, body = web.get("/api/diag")
        diag_text = ""
        if status == 200:
            diag_path = log_path.with_suffix(".diag.txt")
            diag_path.write_bytes(body)
            diag_text = body.decode(errors="replace")
            say(f"saved /api/diag to {diag_path.relative_to(ROOT)}")
        else:
            say(f"/api/diag not available ({'HTTP %d' % status if status else 'no response'})")
        logs.close()

    text = log_path.read_text(errors="replace") + "\n" + diag_text
    lines = crash_lines(text)
    if lines:
        addrs = code_addresses(lines)
        hashes, times = build_refs(text)
        archives = sorted((ROOT / "dist" / "elf" / node).glob("*_0x*"))
        elfs = [a / "firmware.elf" for a in pick_archives(archives, hashes, times)] or [arch / "firmware.elf"]
        a2l = find_addr2line(build_path, args.yaml)
        out = [f"crash/backtrace lines: {len(lines)}, addresses: {len(addrs)}", *lines, ""]
        for elf in elfs:
            if a2l is None or not elf.exists():
                out.append(f"cannot decode against {elf} (addr2line {a2l})")
                continue
            out.append(f"== decoded against {elf.relative_to(ROOT)}")
            out.extend(decode(a2l, elf, addrs))
        dec = log_path.with_suffix(".decoded.txt")
        dec.write_text("\n".join(out) + "\n")
        say(f"crash records found; decoded into {dec.relative_to(ROOT)}")
    return rc


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
#
# SPDX-License-Identifier: MIT
"""Firmware builder: a small web app to pick a board and its modules, build
CircuitPython for the espressif, raspberrypi, atmel-samd and nordic ports,
keep the results and flash them.

Run it from WSL in the repository and open http://localhost:8080:

    python3 tools/fwbuilder/server.py

Only the standard library. Flashing goes through the Windows Python, which sees
the COM ports and the UF2 drives that WSL cannot: esptool for espressif, a copy
to the bootloader drive for the others.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import threading
import time
from datetime import datetime
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

REPO = Path(__file__).resolve().parents[2]
ESP_DIR = REPO / "ports" / "espressif"
STATIC = Path(__file__).resolve().parent / "static"
DATA = Path(os.environ.get("FWB_DATA", Path.home() / "cp-fwbuilder"))
FIRMWARE_DIR = DATA / "firmware"
PROFILES_FILE = DATA / "profiles.json"
WIN_PYTHON = os.environ.get("FWB_WIN_PYTHON", "/mnt/c/Python312/python.exe")
ARM_BIN = os.environ.get("FWB_ARM_BIN", "/opt/arm-gcc14/bin")
BUILD_PREFIX = "build-fwb-"

# The ports offered, and for the ARM ones the board variables that name the
# chip: the first for the chip filter, the second for the exact part.
PORTS = ("espressif", "raspberrypi", "atmel-samd", "nordic")
CHIP_VARIABLES = {
    "raspberrypi": ("CHIP_VARIANT", "CHIP_VARIANT"),
    "atmel-samd": ("CHIP_FAMILY", "CHIP_VARIANT"),
    "nordic": ("MCU_CHIP", "MCU_CHIP"),
}


def port_dir(port):
    return REPO / "ports" / port

# Switches the fork adds; shown as their own group.
FORK_SWITCHES = {
    "CIRCUITPY_ESPIDF_CSI", "CIRCUITPY_ESPIDF_FTM", "CIRCUITPY_ESPIDF_SDM", "CIRCUITPY_ESPIDF_MCPWM",
    "CIRCUITPY_ESPIDF_RMT", "CIRCUITPY_ESPIDF_PCNT", "CIRCUITPY_ESPIDF_I2S_TDM", "CIRCUITPY_ESPIDF_LEDC_FADE",
    "CIRCUITPY_ESPIDF_ANA_CMPR", "CIRCUITPY_ESPIDF_ETM", "CIRCUITPY_ESPIDF_PARLIO",
    "CIRCUITPY_ESPIDF_BITSCRAMBLER", "CIRCUITPY_ESPIDF_LPCORE", "CIRCUITPY_ESPIDF_BLE_PERIODIC",
    "CIRCUITPY_ESPIDF_BLE_MESH", "CIRCUITPY_ESPIDF_BLE_HID", "CIRCUITPY_ESPIDF_BLE_L2CAP",
    "CIRCUITPY_ESPIDF_NAN", "CIRCUITPY_ESPSDR",
    "CIRCUITPY_IEEE802154",
    "CIRCUITPY_ZIGBEE", "CIRCUITPY_TTFRAST", "CIRCUITPY_HASHLIB_EXTENDED", "CIRCUITPY_PARTITION_DISK",
    "CIRCUITPY_USB_TMC", "CIRCUITPY_USB_NET",
}

# What each switch turns on and what it is for, in Czech and English, keyed by
# the name without CIRCUITPY_.
DESCRIPTIONS = json.loads((Path(__file__).resolve().parent / "descriptions.json").read_text(encoding="utf-8"))

def mk_files(port):
    files = [REPO / "py" / "circuitpy_mpconfig.mk", port_dir(port) / "mpconfigport.mk", port_dir(port) / "fork.mk"]
    return [f for f in files if f.exists()]


DUMP_RULE = ("fwb-dump: ; @$(foreach v,$(sort $(filter CIRCUITPY_% IDF_TARGET UF2_BOOTLOADER,"
             "$(.VARIABLES))),$(info FWB $(v)=$($(v))))")


def toolchain(port):
    """The shell prefix that puts the port's compiler on the PATH."""
    if port == "espressif":
        return ". esp-idf/export.sh >/dev/null 2>&1 && "
    return f'export PATH="{ARM_BIN}:$PATH" && ' if Path(ARM_BIN).is_dir() else ""


def run_make(port, board, overrides, *targets, eval_rule=None):
    """Runs make in the port with its toolchain and returns stdout."""
    args = " ".join(f"{k}={int(v)}" for k, v in sorted(overrides.items()))
    extra = f"--eval='{eval_rule}'" if eval_rule else ""
    cmd = f"{toolchain(port)}make BOARD={board} {args} {extra} {' '.join(targets)} 2>/dev/null"
    return subprocess.run(["bash", "-c", cmd], cwd=port_dir(port), capture_output=True, text=True).stdout


def board_variables(port, board, overrides=None):
    out = run_make(port, board, overrides or {}, "fwb-dump", eval_rule=DUMP_RULE)
    variables = {}
    for line in out.splitlines():
        if line.startswith("FWB "):
            name, _, value = line[4:].partition("=")
            variables[name] = value
    return variables


def binding_dirs(port):
    return [d for d in (REPO / "shared-bindings", port_dir(port) / "bindings") if d.is_dir()]


def module_names(port):
    names = set()
    for d in binding_dirs(port):
        names.update(p.name.lower() for p in d.iterdir() if p.is_dir())
    return names


def module_docstring(short, port):
    """The first paragraph of a module's docstring, from its bindings."""
    for d in binding_dirs(port):
        f = d / short / "__init__.c"
        if not f.exists():
            continue
        lines = []
        started = False
        for line in f.read_text(errors="ignore").splitlines():
            if not started:
                if line.startswith('//| """'):
                    started = True
                    line = line[len('//| """'):]
                else:
                    continue
            elif line.startswith("//|"):
                line = line[3:]
            else:
                break
            if '"""' in line:
                lines.append(line.split('"""')[0].strip())
                break
            if not line.strip() and any(lines):
                break
            lines.append(line.strip())
        return " ".join(x for x in lines if x)
    return ""


def mk_comment(name, port):
    """The comment right above a switch's first definition. Short ones are
    section headings (\"Modules\", \"Has no USB\") rather than descriptions."""
    for f in mk_files(port):
        lines = f.read_text(errors="ignore").splitlines()
        for i, line in enumerate(lines):
            if re.match(rf"^{name}\s*\??=", line):
                comment = []
                j = i - 1
                while j >= 0 and lines[j].startswith("#"):
                    comment.insert(0, lines[j].lstrip("# ").strip())
                    j -= 1
                text = " ".join(c for c in comment if c)
                if len(text.split()) >= 5:
                    return text
                break
    return ""


# The peripheral or radio feature each fork switch drives, as the IDF's
# soc_caps.h names it. fork.mk cannot know these: on a chip without the
# hardware the build fails in the driver, not with a message.
SOC_REQUIRED = {
    "CIRCUITPY_ESPIDF_CSI": "SOC_WIFI_CSI_SUPPORT",
    "CIRCUITPY_ESPIDF_FTM": "SOC_WIFI_FTM_SUPPORT",
    "CIRCUITPY_ESPIDF_NAN": "SOC_WIFI_NAN_SUPPORT",
    "CIRCUITPY_ESPIDF_SDM": "SOC_SDM_SUPPORTED",
    "CIRCUITPY_ESPIDF_RMT": "SOC_RMT_SUPPORTED",
    "CIRCUITPY_ESPIDF_PCNT": "SOC_PCNT_SUPPORTED",
    "CIRCUITPY_ESPIDF_MCPWM": "SOC_MCPWM_SUPPORTED",
    "CIRCUITPY_ESPIDF_I2S_TDM": "SOC_I2S_SUPPORTS_TDM",
    "CIRCUITPY_ESPIDF_LEDC_FADE": "SOC_LEDC_SUPPORTED",
    "CIRCUITPY_ESPIDF_ANA_CMPR": "SOC_ANA_CMPR_SUPPORTED",
    "CIRCUITPY_ESPIDF_ETM": "SOC_ETM_SUPPORTED",
    "CIRCUITPY_ESPIDF_PARLIO": "SOC_PARLIO_SUPPORTED",
    "CIRCUITPY_ESPIDF_BITSCRAMBLER": "SOC_BITSCRAMBLER_SUPPORTED",
    "CIRCUITPY_ESPIDF_LPCORE": "SOC_LP_CORE_SUPPORTED",
    "CIRCUITPY_ESPIDF_BLE_PERIODIC": "SOC_BLE_50_SUPPORTED",
    "CIRCUITPY_ESPIDF_BLE_MESH": "SOC_BLE_MESH_SUPPORTED",
    "CIRCUITPY_ESPIDF_BLE_HID": "SOC_BLE_SUPPORTED",
    "CIRCUITPY_ESPIDF_BLE_L2CAP": "SOC_BLE_SUPPORTED",
    "CIRCUITPY_IEEE802154": "SOC_IEEE802154_SUPPORTED",
}


def soc_caps(target):
    """The SOC_* capabilities of a chip that soc_caps.h sets to a number."""
    f = ESP_DIR / "esp-idf" / "components" / "soc" / target / "include" / "soc" / "soc_caps.h"
    caps = {}
    if f.exists():
        for m in re.finditer(r"^#define\s+(SOC_\w+)\s+\(?(\d+)\)?", f.read_text(errors="ignore"), re.M):
            caps[m.group(1)] = int(m.group(2))
    return caps


def fork_rules():
    """What fork.mk asks of each switch: the chips it is limited to and the
    switches it needs (an $(error) under an IDF_TARGET or a ",0" test, or a
    default taken from other switches), and the paths per chip it uses, which
    for prebuilt libraries exist for some chips only."""
    text = (ESP_DIR / "fork.mk").read_text(errors="ignore").replace("\\\n", " ")
    rules = {}

    def rule(name):
        return rules.setdefault(name, {"targets": [], "needs": [], "paths": []})

    stack = []
    for line in text.splitlines():
        s = line.strip()
        m = re.match(r"if(n?eq) \(\$\((\w+)\),(\w*)\)$", s)
        if m:
            stack.append(m.groups())
            continue
        # "ifeq ($(filter $(IDF_TARGET),a b),)" is true off those chips.
        m = re.match(r"ifeq \(\$\(filter \$\(IDF_TARGET\),([\w ]+)\),\)$", s)
        if m:
            stack.append(("neq", "IDF_TARGET", m.group(1).split()))
            continue
        if s.startswith("if"):
            stack.append(None)
            continue
        if s == "else":
            stack[-1] = None
            continue
        if s == "endif":
            stack.pop()
            continue
        m = re.match(r"(CIRCUITPY_\w+) \?= (.*)", s)
        if m and not stack:
            rule(m.group(1))["needs"] += re.findall(r"\$\((CIRCUITPY_\w+)\)", m.group(2))
            continue
        # The innermost "switch is on" block this line is in.
        openers = [c for c in stack if c and c[1].startswith("CIRCUITPY_") and
                   (c[0], c[2]) in (("neq", "0"), ("eq", "1"))]
        if not openers:
            continue
        r = rule(openers[-1][1])
        if s.startswith("$(error") and stack[-1]:
            op, var, value = stack[-1]
            if (op, var) == ("neq", "IDF_TARGET"):
                r["targets"] += value if isinstance(value, list) else [value]
            elif (op, value) == ("eq", "0"):
                r["needs"].append(var)
        elif s.startswith(("INC +=", "FORK_BINARY_BLOBS +=")):
            r["paths"] += [p for p in s.split() if "$(IDF_TARGET)" in p]
    return rules


def requirements(name, variables, switches, caps, rules):
    """Why a fork switch cannot be built for this board (static), and which
    other switches it needs on (checked by the page as they change)."""
    target = variables.get("IDF_TARGET", "")
    blocked, needs = [], []
    r = rules.get(name, {"targets": [], "needs": [], "paths": []})
    if r["targets"] and target not in r["targets"]:
        blocked.append({"kind": "target", "value": r["targets"]})
    cap = SOC_REQUIRED.get(name)
    if cap and not caps.get(cap):
        blocked.append({"kind": "soc", "value": cap})
    for p in r["paths"]:
        if not (ESP_DIR / p.replace("$(IDF_TARGET)", target)).exists():
            blocked.append({"kind": "file", "value": p})
    for dep in r["needs"]:
        if dep in switches:
            needs.append(dep)
        elif variables.get(dep, "0") in ("", "0"):
            blocked.append({"kind": "needs", "value": dep})
    return blocked, needs


def describe_modules(variables, port):
    """The 0/1 switches of a board, grouped: fork additions, modules, other,
    each with its description and what the source says about it. Fork
    switches also say what keeps them from this board (espressif only: the
    rules come from its fork.mk and the IDF)."""
    modules = module_names(port)
    switches = {n for n, v in variables.items() if v in ("0", "1") and n.startswith("CIRCUITPY_")}
    esp = port == "espressif"
    caps = soc_caps(variables.get("IDF_TARGET", "")) if esp else {}
    rules = fork_rules() if esp else {}
    result = []
    for name, value in variables.items():
        if value not in ("0", "1") or not name.startswith("CIRCUITPY_"):
            continue
        short = name[len("CIRCUITPY_"):]
        if name in FORK_SWITCHES:
            group = "fork"
        elif short.lower() in modules:
            group = "module"
        else:
            group = "other"
        source = module_docstring(short.lower(), port) if group == "module" else ""
        blocked, needs = (requirements(name, variables, switches, caps, rules) if group == "fork" and esp
                          else ([], []))
        result.append({
            "name": name,
            "value": value == "1",
            "group": group,
            "info": DESCRIPTIONS.get(short, {}),
            "source": source or mk_comment(name, port),
            "blocked": blocked,
            "needs": needs,
        })
    return result


def mk_value(text, name):
    m = re.search(rf"^{name}\s*=\s*(\S+)", text, re.M)
    return m.group(1) if m else ""


def board_list():
    """Every board of the offered ports. Board directory names do not repeat
    across these ports, so the name alone is the board's id."""
    boards = []
    for port in PORTS:
        for d in sorted((port_dir(port) / "boards").iterdir()):
            header = d / "mpconfigboard.h"
            mk = d / "mpconfigboard.mk"
            if not header.exists() or not mk.exists():
                continue
            m = re.search(r'MICROPY_HW_BOARD_NAME\s+"([^"]*)"', header.read_text(errors="ignore"))
            text = mk.read_text(errors="ignore")
            if port == "espressif":
                target = mk_value(text, "IDF_TARGET")
                # esp32s3 -> S3, esp32 -> ESP32
                chip = target[len("esp32"):].upper() or "ESP32" if target.startswith("esp32") else target.upper()
            else:
                family, part = CHIP_VARIABLES[port]
                chip = mk_value(text, family).upper()
                target = mk_value(text, part)
            boards.append({"id": d.name, "name": m.group(1) if m else d.name, "port": port,
                           "target": target, "chip": chip})
    return boards


def board_port(board):
    for port in PORTS:
        if (port_dir(port) / "boards" / board / "mpconfigboard.mk").exists():
            return port
    raise ValueError(f"no board {board}")


def board_target(port, board, variables):
    if port == "espressif":
        return variables.get("IDF_TARGET", "")
    text = (port_dir(port) / "boards" / board / "mpconfigboard.mk").read_text(errors="ignore")
    return mk_value(text, CHIP_VARIABLES[port][1])


# --- persistent data ----------------------------------------------------------

def load_profiles():
    if PROFILES_FILE.exists():
        return json.loads(PROFILES_FILE.read_text())
    return {}


def save_profiles(profiles):
    DATA.mkdir(parents=True, exist_ok=True)
    tmp = PROFILES_FILE.with_suffix(".tmp")
    tmp.write_text(json.dumps(profiles, indent=2, sort_keys=True))
    tmp.replace(PROFILES_FILE)


def firmware_list():
    items = []
    if FIRMWARE_DIR.exists():
        for d in FIRMWARE_DIR.iterdir():
            manifest = d / "manifest.json"
            if manifest.exists():
                items.append(json.loads(manifest.read_text()))
    return sorted(items, key=lambda m: m["created"], reverse=True)


def safe_id(value):
    if not re.fullmatch(r"[A-Za-z0-9._-]+", value or ""):
        raise ValueError("bad id")
    return value


# --- jobs ----------------------------------------------------------------------

class Job:
    """One build or flash at a time, its output kept for the browser."""

    def __init__(self, kind, title):
        self.kind = kind
        self.title = title
        self.lines = []
        self.state = "running"
        self.result = None
        self.proc = None
        self.started = time.time()
        self.cond = threading.Condition()

    def add(self, line):
        with self.cond:
            self.lines.append(line.rstrip("\n"))
            self.cond.notify_all()

    def finish(self, state, result=None):
        with self.cond:
            self.state = state
            self.result = result
            self.cond.notify_all()

    def summary(self):
        return {"kind": self.kind, "title": self.title, "state": self.state, "result": self.result,
                "lines": len(self.lines), "started": self.started}


_job = None
_job_lock = threading.Lock()


def start_job(kind, title, target):
    global _job
    with _job_lock:
        if _job is not None and _job.state == "running":
            raise RuntimeError(f"{_job.kind} is already running")
        _job = Job(kind, title)
        job = _job
    threading.Thread(target=target, args=(job,), daemon=True).start()
    return job


def stream(job, cmd, cwd=None):
    """Runs cmd, adding its output to the job; returns the exit code."""
    job.add("$ " + (cmd if isinstance(cmd, str) else " ".join(cmd)))
    job.proc = subprocess.Popen(cmd, cwd=cwd, shell=isinstance(cmd, str), executable="/bin/bash" if isinstance(cmd, str) else None,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
    for line in job.proc.stdout:
        job.add(line)
    return job.proc.wait()


def build_dir_name(board, overrides):
    key = json.dumps({"board": board, "overrides": overrides}, sort_keys=True)
    return BUILD_PREFIX + board + "-" + hashlib.sha1(key.encode()).hexdigest()[:8]


def do_build(job, board, overrides, profile, clean):
    port = board_port(board)
    pdir = port_dir(port)
    build = build_dir_name(board, overrides)
    if clean and (pdir / build).exists():
        job.add(f"removing {build} for a clean build")
        shutil.rmtree(pdir / build)
    args = " ".join(f"{k}={int(v)}" for k, v in sorted(overrides.items()))
    cmd = f"{toolchain(port)}make BOARD={board} BUILD={build} {args} -j$(nproc)"
    try:
        code = stream(job, cmd, cwd=pdir)
    except Exception as e:  # noqa: BLE001 -- reported to the browser
        job.add(f"error: {e}")
        job.finish("failed")
        return
    if job.state == "cancelled":
        return
    if code != 0:
        job.finish("failed", {"exit": code})
        return
    out = pdir / build
    created = datetime.now()
    fw_id = created.strftime("%Y%m%d-%H%M%S-") + board
    dest = FIRMWARE_DIR / fw_id
    dest.mkdir(parents=True)
    files = []
    for name in ("firmware.bin", "firmware.uf2"):
        if (out / name).exists():
            shutil.copy2(out / name, dest / name)
            files.append({"name": name, "size": (dest / name).stat().st_size})
    variables = board_variables(port, board, overrides)
    free = re.search(r"(-?\d+) bytes free in flash firmware space out of (\d+)", "\n".join(job.lines))
    git = subprocess.run(["git", "describe", "--tags", "--dirty", "--always"], cwd=REPO,
                         capture_output=True, text=True).stdout.strip()
    manifest = {
        "id": fw_id,
        "board": board,
        "port": port,
        "target": board_target(port, board, variables),
        "profile": profile,
        "created": created.isoformat(timespec="seconds"),
        "git": git,
        "build_dir": build,
        "overrides": overrides,
        "enabled": sorted(n for n, v in variables.items() if v == "1" and n.startswith("CIRCUITPY_")),
        "flash_free": int(free.group(1)) if free else None,
        "flash_total": int(free.group(2)) if free else None,
        "files": files,
    }
    (dest / "manifest.json").write_text(json.dumps(manifest, indent=2))
    job.add(f"saved as {fw_id}")
    job.finish("done", {"firmware": fw_id})


def win_path(path):
    return subprocess.run(["wslpath", "-w", str(path)], capture_output=True, text=True).stdout.strip()


def do_flash(job, fw_id, port, erase, drive):
    manifest = json.loads((FIRMWARE_DIR / fw_id / "manifest.json").read_text())
    if manifest.get("port", "espressif") != "espressif":
        if not drive and manifest["port"] == "nordic":
            # CircuitPython asks the nRF bootloader for its serial-only DFU
            # (BOOTLOADER_DFU_MAGIC 0x4e), which has no drive.
            job.add("an nRF board does not show its UF2 drive after the 1200 baud touch; "
                    "double-press RESET and choose the drive")
            job.finish("failed")
            return
        return flash_uf2(job, fw_id, port, drive)
    image = win_path(FIRMWARE_DIR / fw_id / "firmware.bin")
    base = [WIN_PYTHON, "-m", "esptool", "--chip", manifest["target"], "-p", port]
    try:
        if erase:
            if stream(job, base + ["erase-flash"]) != 0:
                job.finish("failed")
                return
        # A watchdog reset, not a hard one: on native USB chips a hard reset can
        # leave the chip in the download mode.
        code = stream(job, base + ["--after", "watchdog-reset", "write-flash", "0x0", image])
    except Exception as e:  # noqa: BLE001 -- reported to the browser
        job.add(f"error: {e}")
        job.finish("failed")
        return
    if job.state != "cancelled":
        job.finish("done" if code == 0 else "failed", {"exit": code})


# Run by the Windows Python: the drives with a UF2 bootloader's INFO_UF2.TXT.
UF2_DRIVES_PY = r"""
import json, os
found = []
for d in os.listdrives():
    try:
        with open(os.path.join(d, "INFO_UF2.TXT"), errors="replace") as f:
            found.append({"drive": d.rstrip("\\"), "info": f.read()})
    except OSError:
        pass  # no bootloader there, or an empty card reader
print(json.dumps(found))
"""


def uf2_drives():
    code, out = run_win(["-c", UF2_DRIVES_PY], timeout=20)
    if code != 0:
        raise OSError(out.strip())
    drives = json.loads(out)
    for d in drives:
        m = re.search(r"^Board-ID:\s*(.+)$", d["info"], re.M) or re.search(r"^Model:\s*(.+)$", d["info"], re.M)
        d["board"] = m.group(1).strip() if m else ""
        del d["info"]
    return drives


UF2_WAIT = 20


def flash_uf2(job, fw_id, port, drive):
    """Copies firmware.uf2 to the bootloader drive. Without a drive the board
    on port is sent there first: CircuitPython resets into its UF2 bootloader
    when the port is opened at 1200 baud and closed."""
    image = win_path(FIRMWARE_DIR / fw_id / "firmware.uf2")
    try:
        if not drive:
            before = {d["drive"] for d in uf2_drives()}
            job.add(f"{port}: 1200 baud touch to restart the board into its UF2 bootloader")
            code, out = run_win(["-c", "import serial, sys; serial.Serial(sys.argv[1], 1200).close()", port])
            if code != 0:
                job.add(out)
                job.finish("failed")
                return
            deadline = time.time() + UF2_WAIT
            while not drive and time.time() < deadline and job.state == "running":
                time.sleep(1)
                new = [d for d in uf2_drives() if d["drive"] not in before]
                if new:
                    drive = new[0]["drive"]
                    job.add(f"bootloader drive {drive} ({new[0]['board']})")
            if not drive:
                if job.state == "running":
                    job.add(f"no UF2 drive appeared in {UF2_WAIT} s; put the board in its bootloader "
                            "by hand and choose the drive")
                    job.finish("failed")
                return
        code = stream(job, [WIN_PYTHON, "-c", "import shutil, sys; shutil.copyfile(sys.argv[1], sys.argv[2])",
                            image, drive + "\\firmware.uf2"])
    except Exception as e:  # noqa: BLE001 -- reported to the browser
        job.add(f"error: {e}")
        job.finish("failed")
        return
    if job.state != "cancelled":
        if code == 0:
            job.add("copied; the board restarts into the new firmware")
        job.finish("done" if code == 0 else "failed", {"exit": code})


# Internal SRAM by chip family, from the datasheets; esptool cannot read it.
CHIP_SRAM_KB = {"ESP32": 520, "ESP32-S2": 320, "ESP32-S3": 512, "ESP32-C2": 272, "ESP32-C3": 400,
                "ESP32-C5": 384, "ESP32-C6": 512, "ESP32-C61": 320, "ESP32-H2": 320, "ESP32-P4": 768}


def run_win(args, timeout=40):
    """Runs the Windows Python; returns (exit code, text)."""
    proc = subprocess.run([WIN_PYTHON] + args, capture_output=True, timeout=timeout)
    raw = proc.stdout + proc.stderr
    # The Windows Python prints in the console's code page, Czech here.
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        text = raw.decode("cp1250", errors="replace")
    return proc.returncode, text


def probe(port):
    """What esptool and the eFuses say about the chip on port. Reading only."""
    code, ident = run_win(["-m", "esptool", "-p", port, "--after", "watchdog-reset", "flash-id"])
    info = {}
    for key, pattern in (("chip", r"Chip type:\s*(.+)"), ("features", r"Features:\s*(.+)"),
                         ("crystal", r"Crystal frequency:\s*(.+)"), ("mac", r"^MAC:\s*(.+)"),
                         ("base_mac", r"^BASE MAC:\s*(.+)"),
                         ("flash_size", r"Detected flash size:\s*(.+)"), ("flash_manufacturer", r"Manufacturer:\s*(.+)"),
                         ("flash_device", r"Device:\s*(.+)")):
        m = re.search(pattern, ident, re.M)
        if m:
            info[key] = next(g for g in m.groups() if g).strip()
    if code != 0 or "chip" not in info:
        return {"ok": False, "info": info, "raw": ident}
    raw = ident
    _, fuses = run_win(["-m", "espefuse", "-p", port, "summary"])
    raw += "\n" + "\n".join(line for line in fuses.splitlines()
                            if re.match(r"(PSRAM|FLASH_CAP|FLASH_VENDOR|PKG_VERSION|WAFER_VERSION)", line))
    for key, fuse in (("psram_cap", "PSRAM_CAP"), ("psram_vendor", "PSRAM_VENDOR"), ("flash_cap", "FLASH_CAP"),
                      ("package", "PKG_VERSION")):
        m = re.search(rf"^{fuse} \(BLOCK\d+\).*?= (\S+)", fuses, re.M)
        if m:
            info[key] = m.group(1)
    family = info["chip"].split(" (")[0]
    if family in CHIP_SRAM_KB:
        info["sram_kb"] = CHIP_SRAM_KB[family]
    psram = re.search(r"Embedded PSRAM (\S+)", info.get("features", ""))
    if psram:
        info["psram"] = psram.group(1)
    elif info.get("psram_cap") in ("0", None):
        info["psram"] = "none in package" if info.get("psram_cap") == "0" else ""
    flash = re.search(r"Embedded Flash (\S+)", info.get("features", ""))
    if flash:
        info["embedded_flash"] = flash.group(1)
    return {"ok": True, "info": info, "raw": raw}


def list_ports():
    _, out = run_win(["-m", "serial.tools.list_ports", "-v"], timeout=20)
    ports = []
    for line in out.splitlines():
        if re.match(r"^COM\d+", line):
            ports.append({"port": line.split()[0], "desc": ""})
        elif ports and line.startswith("    desc:"):
            ports[-1]["desc"] = line.split(":", 1)[1].strip()
        elif ports and line.startswith("    hwid:"):
            ports[-1]["hwid"] = line.split(":", 1)[1].strip()
    return ports


# --- HTTP ----------------------------------------------------------------------

_module_cache = {}


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(STATIC), **kwargs)

    def log_message(self, fmt, *args):
        pass

    def end_headers(self):
        # The page is edited while the server runs; a cached old script or
        # style sheet would not match the rest.
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def send_json(self, data, status=HTTPStatus.OK):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def read_json(self):
        length = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(length) or b"{}")

    def fail(self, message, status=HTTPStatus.BAD_REQUEST):
        self.send_json({"error": message}, status)

    def do_GET(self):
        path = urlparse(self.path).path
        try:
            if path == "/api/boards":
                return self.send_json(board_list())
            m = re.fullmatch(r"/api/boards/([\w.-]+)/modules", path)
            if m:
                board = safe_id(m.group(1))
                if board not in _module_cache:
                    port = board_port(board)
                    variables = board_variables(port, board)
                    _module_cache[board] = {
                        "port": port,
                        "target": board_target(port, board, variables),
                        # The other ports always make a .uf2.
                        "uf2": port != "espressif" or variables.get("UF2_BOOTLOADER") == "1",
                        "modules": describe_modules(variables, port),
                    }
                return self.send_json(_module_cache[board])
            if path == "/api/profiles":
                return self.send_json(load_profiles())
            if path == "/api/firmwares":
                return self.send_json(firmware_list())
            if path == "/api/ports":
                return self.send_json(list_ports())
            if path == "/api/uf2drives":
                return self.send_json(uf2_drives())
            if path == "/api/probe":
                port = parse_qs(urlparse(self.path).query).get("port", [""])[0]
                if not re.fullmatch(r"COM\d+", port):
                    raise ValueError("bad port")
                if _job is not None and _job.state == "running" and _job.kind == "flash":
                    raise ValueError("flashing is running")
                return self.send_json(probe(port))
            if path == "/api/job":
                return self.send_json(_job.summary() if _job else None)
            if path == "/api/job/stream":
                return self.stream_job()
            m = re.fullmatch(r"/firmware/([\w.-]+)/(firmware\.(?:bin|uf2))", path)
            if m:
                return self.send_firmware(safe_id(m.group(1)), m.group(2))
        except (ValueError, OSError, subprocess.SubprocessError) as e:
            return self.fail(str(e))
        return super().do_GET()

    def do_POST(self):
        path = urlparse(self.path).path
        try:
            body = self.read_json()
            if path == "/api/build":
                board = safe_id(body["board"])
                overrides = {k: bool(v) for k, v in body.get("overrides", {}).items()
                             if re.fullmatch(r"CIRCUITPY_[A-Z0-9_]+", k)}
                profile = body.get("profile") or ""
                clean = bool(body.get("clean"))
                job = start_job("build", f"{board}" + (f" ({profile})" if profile else ""),
                                lambda j: do_build(j, board, overrides, profile, clean))
                return self.send_json(job.summary())
            if path == "/api/flash":
                fw_id = safe_id(body["firmware"])
                port = body.get("port") or ""
                drive = body.get("drive") or ""
                if drive and not re.fullmatch(r"[A-Z]:", drive):
                    raise ValueError("bad drive")
                if not drive and not re.fullmatch(r"COM\d+", port):
                    raise ValueError("bad port")
                job = start_job("flash", f"{fw_id} -> {drive or port}",
                                lambda j: do_flash(j, fw_id, port, bool(body.get("erase")), drive))
                return self.send_json(job.summary())
            if path == "/api/job/cancel":
                if _job and _job.state == "running":
                    # The UF2 flash waits for its drive without a process.
                    _job.state = "cancelled"
                    if _job.proc:
                        _job.proc.terminate()
                    _job.add("cancelled")
                    _job.finish("cancelled")
                return self.send_json({"ok": True})
            if path == "/api/profiles":
                name = body.get("name", "").strip()
                if not name:
                    raise ValueError("profile needs a name")
                profiles = load_profiles()
                profiles[name] = {"board": safe_id(body["board"]), "overrides": body.get("overrides", {})}
                save_profiles(profiles)
                return self.send_json(profiles)
        except RuntimeError as e:
            return self.fail(str(e), HTTPStatus.CONFLICT)
        except (KeyError, ValueError) as e:
            return self.fail(f"bad request: {e}")
        return self.fail("not found", HTTPStatus.NOT_FOUND)

    def do_DELETE(self):
        path = unquote(urlparse(self.path).path)
        try:
            m = re.fullmatch(r"/api/firmwares/([\w.-]+)", path)
            if m:
                fw_id = safe_id(m.group(1))
                manifest = json.loads((FIRMWARE_DIR / fw_id / "manifest.json").read_text())
                shutil.rmtree(FIRMWARE_DIR / fw_id)
                # The build directory goes too once no kept firmware came from it.
                build = manifest.get("build_dir", "")
                if build.startswith(BUILD_PREFIX) and not any(f.get("build_dir") == build for f in firmware_list()):
                    shutil.rmtree(port_dir(manifest.get("port", "espressif")) / build, ignore_errors=True)
                return self.send_json(firmware_list())
            m = re.fullmatch(r"/api/profiles/(.+)", path)
            if m:
                profiles = load_profiles()
                profiles.pop(m.group(1), None)
                save_profiles(profiles)
                return self.send_json(profiles)
        except (ValueError, OSError) as e:
            return self.fail(str(e))
        return self.fail("not found", HTTPStatus.NOT_FOUND)

    def send_firmware(self, fw_id, name):
        path = FIRMWARE_DIR / fw_id / name
        if not path.exists():
            return self.fail("no such file", HTTPStatus.NOT_FOUND)
        manifest = json.loads((FIRMWARE_DIR / fw_id / "manifest.json").read_text())
        download = f"circuitpython-{manifest['board']}-{fw_id[:15]}.{name.split('.')[-1]}"
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(path.stat().st_size))
        self.send_header("Content-Disposition", f'attachment; filename="{download}"')
        self.end_headers()
        with open(path, "rb") as f:
            shutil.copyfileobj(f, self.wfile)

    def stream_job(self):
        """Server-sent events: the job's lines as they come, then its end."""
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        job = _job
        if job is None:
            return
        sent = 0
        try:
            while True:
                with job.cond:
                    while sent >= len(job.lines) and job.state == "running":
                        job.cond.wait(15)
                        if sent >= len(job.lines) and job.state == "running":
                            break
                    lines = job.lines[sent:]
                    state = job.state
                for line in lines:
                    self.wfile.write(f"data: {json.dumps(line)}\n\n".encode())
                sent += len(lines)
                if not lines and state == "running":
                    self.wfile.write(b": keep-alive\n\n")
                self.wfile.flush()
                if state != "running" and sent >= len(job.lines):
                    self.wfile.write(f"event: end\ndata: {json.dumps(job.summary())}\n\n".encode())
                    self.wfile.flush()
                    return
        except (BrokenPipeError, ConnectionResetError):
            return


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--host", default="127.0.0.1")
    args = parser.parse_args()
    FIRMWARE_DIR.mkdir(parents=True, exist_ok=True)
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"Firmware builder on http://localhost:{args.port}  (data in {DATA})")
    server.serve_forever()


if __name__ == "__main__":
    main()

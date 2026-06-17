#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""MCP server that lets an AI assistant (e.g. Claude Code) drive Dolphin.

It speaks the Model Context Protocol over stdio and proxies tool calls to a
running Dolphin instance through the DebugRPC JSON-RPC TCP server (see
``docs/DebugRPC.md``). It can also launch and quit the headless Dolphin
frontend itself, so "start a game" is just another tool call.

Only the Python standard library is used, so it runs anywhere Python 3.9+ is
available without installing anything.

Configuration via environment variables:
  DOLPHIN_NOGUI      Path to the dolphin-emu-nogui executable
                     (default: "dolphin-emu-nogui" on PATH).
  DOLPHIN_RPC_HOST   DebugRPC host to connect to (default: 127.0.0.1).
  DOLPHIN_RPC_PORT   Default DebugRPC port (default: 6090).
"""

import json
import os
import socket
import subprocess
import sys
import time

PROTOCOL_VERSION = "2025-06-18"
SERVER_NAME = "dolphin-debug-rpc"
SERVER_VERSION = "1.0.0"

DEFAULT_HOST = os.environ.get("DOLPHIN_RPC_HOST", "127.0.0.1")
DEFAULT_PORT = int(os.environ.get("DOLPHIN_RPC_PORT", "6090"))
DOLPHIN_NOGUI = os.environ.get("DOLPHIN_NOGUI", "dolphin-emu-nogui")


def log(message):
    """Diagnostics go to stderr; stdout is reserved for the MCP protocol."""
    print(f"[dolphin-mcp] {message}", file=sys.stderr, flush=True)


class DebugRPCClient:
    """A thin client for Dolphin's line-delimited JSON-RPC DebugRPC server."""

    def __init__(self, host=DEFAULT_HOST, port=DEFAULT_PORT):
        self.host = host
        self.port = port
        self._sock = None
        self._buffer = b""
        self._next_id = 1
        self._process = None

    # -- connection management ------------------------------------------------
    def connect(self, timeout=2.0):
        self.close()
        sock = socket.create_connection((self.host, self.port), timeout=timeout)
        sock.settimeout(10.0)
        self._sock = sock
        self._buffer = b""

    def is_connected(self):
        return self._sock is not None

    def ensure_connected(self):
        if self._sock is None:
            self.connect()

    def close(self):
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
        self._sock = None
        self._buffer = b""

    def wait_for_server(self, timeout=30.0):
        """Poll the TCP port until the DebugRPC server accepts a connection."""
        deadline = time.time() + timeout
        last_error = None
        while time.time() < deadline:
            if self._process is not None and self._process.poll() is not None:
                raise RuntimeError(
                    f"Dolphin exited early with code {self._process.returncode}"
                )
            try:
                self.connect(timeout=1.0)
                return True
            except OSError as exc:  # not listening yet
                last_error = exc
                time.sleep(0.25)
        raise RuntimeError(f"Timed out waiting for DebugRPC on "
                           f"{self.host}:{self.port}: {last_error}")

    # -- request/response -----------------------------------------------------
    def call(self, method, params=None):
        self.ensure_connected()
        request_id = self._next_id
        self._next_id += 1
        payload = {"id": request_id, "method": method, "params": params or {}}
        line = (json.dumps(payload) + "\n").encode("utf-8")
        try:
            self._sock.sendall(line)
            response = self._read_line()
        except OSError as exc:
            self.close()
            raise RuntimeError(f"DebugRPC connection error: {exc}") from exc

        message = json.loads(response)
        if "error" in message and message["error"] is not None:
            err = message["error"]
            raise RuntimeError(f"DebugRPC error {err.get('code')}: {err.get('message')}")
        return message.get("result", {})

    def _read_line(self):
        while b"\n" not in self._buffer:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise OSError("connection closed by Dolphin")
            self._buffer += chunk
        line, _, rest = self._buffer.partition(b"\n")
        self._buffer = rest
        return line.decode("utf-8")

    # -- process lifecycle ----------------------------------------------------
    def launch(self, rom_path, port=None, dolphin_path=None, platform="headless",
               extra_args=None):
        if port is not None:
            self.port = int(port)
        executable = dolphin_path or DOLPHIN_NOGUI
        args = [executable, "--exec", rom_path,
                "--debug-rpc-port", str(self.port),
                "-p", platform]
        if extra_args:
            args.extend(extra_args)
        log(f"launching: {' '.join(args)}")
        self._process = subprocess.Popen(args)
        self.wait_for_server()
        return self.port

    def quit(self):
        try:
            if self.is_connected():
                self.call("core.stop")
        except Exception:
            pass
        self.close()
        if self._process is not None:
            try:
                self._process.terminate()
                self._process.wait(timeout=10)
            except Exception:
                try:
                    self._process.kill()
                except Exception:
                    pass
            self._process = None


CLIENT = DebugRPCClient()


# --- Tool definitions --------------------------------------------------------
# Each tool maps to a Python handler. Many handlers are thin forwarders to a
# DebugRPC method with the same parameters.

def _u32_to_hex_le(value):
    return f"{value & 0xffffffff:08x}"


def tool_launch_game(args):
    rom = args["romPath"]
    port = args.get("port")
    dolphin_path = args.get("dolphinPath")
    platform = args.get("platform", "headless")
    used_port = CLIENT.launch(rom, port=port, dolphin_path=dolphin_path,
                              platform=platform)
    status = CLIENT.call("core.status")
    return {"launched": True, "port": used_port, "status": status}


def tool_quit(args):
    CLIENT.quit()
    return {"stopped": True}


def tool_connect(args):
    host = args.get("host", CLIENT.host)
    port = args.get("port", CLIENT.port)
    CLIENT.host = host
    CLIENT.port = int(port)
    CLIENT.connect()
    return {"connected": True, "host": host, "port": CLIENT.port}


def tool_status(args):
    return CLIENT.call("core.status")


def tool_set_state(args):
    state = args["state"]
    if state == "run":
        return CLIENT.call("core.run")
    if state == "pause":
        return CLIENT.call("core.pause")
    if state == "stop":
        return CLIENT.call("core.stop")
    raise ValueError("state must be one of: run, pause, stop")


def tool_step(args):
    return CLIENT.call("cpu.step")


def tool_registers(args):
    result = CLIENT.call("cpu.registers")
    # Add hex views for readability.
    if "pc" in result:
        result["pcHex"] = f"0x{int(result['pc']) & 0xffffffff:08x}"
    if "gpr" in result:
        result["gprHex"] = [f"0x{int(v) & 0xffffffff:08x}" for v in result["gpr"]]
    return result


def tool_disassemble(args):
    params = {"address": args["address"]}
    if "count" in args:
        params["count"] = args["count"]
    return CLIENT.call("cpu.disassemble", params)


def tool_read_memory(args):
    params = {"address": args["address"], "size": args["size"]}
    if "addressSpace" in args:
        params["addressSpace"] = args["addressSpace"]
    return CLIENT.call("memory.read", params)


def tool_write_memory(args):
    params = {"address": args["address"], "data": args["data"]}
    if "addressSpace" in args:
        params["addressSpace"] = args["addressSpace"]
    return CLIENT.call("memory.write", params)


def tool_read_u32(args):
    result = CLIENT.call("memory.read", {"address": args["address"], "size": 4})
    data = bytes.fromhex(result["data"])
    value = int.from_bytes(data, "big")
    return {"address": args["address"], "value": value,
            "valueHex": f"0x{value:08x}", "bytes": result["data"]}


def tool_write_u32(args):
    value = int(args["value"])
    data = value.to_bytes(4, "big").hex()
    params = {"address": args["address"], "data": data}
    return CLIENT.call("memory.write", params)


def tool_cheat_search_begin(args):
    params = {"dataType": args.get("dataType", "u32")}
    for key in ("addressSpace", "aligned", "ranges"):
        if key in args:
            params[key] = args[key]
    return CLIENT.call("cheatSearch.begin", params)


def tool_cheat_search_next(args):
    params = {"sessionId": args["sessionId"]}
    for key in ("compareType", "filterType", "value", "hex"):
        if key in args:
            params[key] = args[key]
    return CLIENT.call("cheatSearch.next", params)


def tool_cheat_search_results(args):
    params = {"sessionId": args["sessionId"]}
    for key in ("offset", "limit"):
        if key in args:
            params[key] = args[key]
    return CLIENT.call("cheatSearch.results", params)


def tool_cheat_search_generate_ar(args):
    return CLIENT.call("cheatSearch.generateAR",
                       {"sessionId": args["sessionId"], "index": args["index"]})


def tool_cheat_search_end(args):
    return CLIENT.call("cheatSearch.end", {"sessionId": args["sessionId"]})


def tool_apply_ar(args):
    params = {"ops": args["ops"]}
    if "name" in args:
        params["name"] = args["name"]
    return CLIENT.call("cheat.applyAR", params)


def tool_apply_gecko(args):
    params = {"lines": args["lines"]}
    if "name" in args:
        params["name"] = args["name"]
    return CLIENT.call("cheat.applyGecko", params)


def tool_save_state(args):
    params = {}
    if "file" in args:
        params["file"] = args["file"]
    elif "slot" in args:
        params["slot"] = args["slot"]
    return CLIENT.call("state.save", params)


def tool_load_state(args):
    params = {}
    if "file" in args:
        params["file"] = args["file"]
    elif "slot" in args:
        params["slot"] = args["slot"]
    return CLIENT.call("state.load", params)


def tool_screenshot(args):
    params = {}
    if "name" in args:
        params["name"] = args["name"]
    return CLIENT.call("core.screenshot", params)


def tool_frame_advance(args):
    return CLIENT.call("core.frameAdvance")


def tool_symbol_from_address(args):
    return CLIENT.call("symbol.fromAddress", {"address": args["address"]})


def tool_symbol_from_name(args):
    return CLIENT.call("symbol.fromName", {"name": args["name"]})


def tool_add_breakpoint(args):
    params = {"address": args["address"]}
    for key in ("break", "log"):
        if key in args:
            params[key] = args[key]
    return CLIENT.call("breakpoint.add", params)


def tool_remove_breakpoint(args):
    return CLIENT.call("breakpoint.remove", {"address": args["address"]})


def tool_list_breakpoints(args):
    return CLIENT.call("breakpoint.list")


def tool_clear_breakpoints(args):
    return CLIENT.call("breakpoint.clear")


def tool_add_watchpoint(args):
    params = {"address": args["address"]}
    for key in ("end", "read", "write", "break", "log"):
        if key in args:
            params[key] = args[key]
    return CLIENT.call("memcheck.add", params)


def tool_remove_watchpoint(args):
    return CLIENT.call("memcheck.remove", {"address": args["address"]})


def tool_list_watchpoints(args):
    return CLIENT.call("memcheck.list")


_STRUCT_FORMATS = {
    "u8": (1, ">B"), "s8": (1, ">b"),
    "u16": (2, ">H"), "s16": (2, ">h"),
    "u32": (4, ">I"), "s32": (4, ">i"),
    "u64": (8, ">Q"), "s64": (8, ">q"),
    "f32": (4, ">f"), "f64": (8, ">d"),
}


def tool_read_value(args):
    import struct
    value_type = args.get("type", "u32")
    if value_type not in _STRUCT_FORMATS:
        raise ValueError(f"unknown type: {value_type}")
    size, fmt = _STRUCT_FORMATS[value_type]
    params = {"address": args["address"], "size": size}
    if "addressSpace" in args:
        params["addressSpace"] = args["addressSpace"]
    result = CLIENT.call("memory.read", params)
    raw = bytes.fromhex(result["data"])
    value = struct.unpack(fmt, raw)[0]
    out = {"address": args["address"], "type": value_type, "value": value,
           "bytes": result["data"]}
    if value_type.startswith(("u", "s")):
        out["valueHex"] = f"0x{value & ((1 << (size * 8)) - 1):0{size * 2}x}"
    return out


def tool_read_string(args):
    address = int(args["address"])
    max_length = int(args.get("maxLength", 256))
    params = {"address": address, "size": max_length}
    if "addressSpace" in args:
        params["addressSpace"] = args["addressSpace"]
    raw = bytes.fromhex(CLIENT.call("memory.read", params)["data"])
    nul = raw.find(b"\x00")
    if nul != -1:
        raw = raw[:nul]
    encoding = args.get("encoding", "utf-8")
    return {"address": address,
            "value": raw.decode(encoding, errors="replace"),
            "length": len(raw)}


def tool_search_memory(args):
    params = {}
    for key in ("pattern", "string", "address", "size", "max", "addressSpace"):
        if key in args:
            params[key] = args[key]
    if "pattern" not in params and "string" not in params:
        raise ValueError("provide 'pattern' (hex) or 'string'")
    result = CLIENT.call("memory.search", params)
    result["addressesHex"] = [f"0x{a:08x}" for a in result.get("addresses", [])]
    return result


# Friendly name -> (group, control) maps for each controller device.
_GC_BUTTONS = {"a": "A", "b": "B", "x": "X", "y": "Y", "z": "Z", "start": "Start"}
_GC_DPAD = {"up": "Up", "down": "Down", "left": "Left", "right": "Right"}
_GC_TRIGGERS = {"l": "L", "r": "R", "l_analog": "L-Analog", "r_analog": "R-Analog"}

_WII_BUTTONS = {"a": "A", "b": "B", "one": "1", "1": "1", "two": "2", "2": "2",
                "plus": "+", "+": "+", "minus": "-", "-": "-", "home": "Home"}
_WII_DPAD = {"up": "Up", "down": "Down", "left": "Left", "right": "Right"}


def _bool_value(value):
    if isinstance(value, bool):
        return 1.0 if value else 0.0
    return float(value)


def _direction_overrides(group, axes):
    """Drive a directional control group (stick / IR pointer) from x/y in [-1, 1]."""
    x = float(axes.get("x", 0.0))
    y = float(axes.get("y", 0.0))
    return [
        {"group": group, "control": "Right", "value": max(x, 0.0)},
        {"group": group, "control": "Left", "value": max(-x, 0.0)},
        {"group": group, "control": "Up", "value": max(y, 0.0)},
        {"group": group, "control": "Down", "value": max(-y, 0.0)},
    ]


def _mapped_buttons(group, mapping, items, kind):
    overrides = []
    for name, value in (items or {}).items():
        control = mapping.get(str(name).lower())
        if control is None:
            raise ValueError(f"unknown {kind}: {name}")
        overrides.append({"group": group, "control": control, "value": _bool_value(value)})
    return overrides


def tool_set_input(args):
    device = args.get("device", "gc")
    pad = args.get("pad", 0)
    overrides = []

    if device == "wii":
        overrides += _mapped_buttons("Buttons", _WII_BUTTONS, args.get("buttons"), "button")
        overrides += _mapped_buttons("D-Pad", _WII_DPAD, args.get("dpad"), "dpad direction")
        if "pointer" in args:
            overrides += _direction_overrides("IR", args["pointer"])
        # Motion (Shake / Tilt / Swing) is available via the raw 'overrides' field.
    elif device == "gc":
        overrides += _mapped_buttons("Buttons", _GC_BUTTONS, args.get("buttons"), "button")
        overrides += _mapped_buttons("D-Pad", _GC_DPAD, args.get("dpad"), "dpad direction")
        for name, value in (args.get("triggers") or {}).items():
            control = _GC_TRIGGERS.get(name.lower())
            if control is None:
                raise ValueError(f"unknown trigger: {name}")
            overrides.append({"group": "Triggers", "control": control, "value": _bool_value(value)})
        if "mainStick" in args:
            overrides += _direction_overrides("Main Stick", args["mainStick"])
        if "cStick" in args:
            overrides += _direction_overrides("C-Stick", args["cStick"])
    else:
        raise ValueError("device must be 'gc' or 'wii'")

    if "overrides" in args:  # raw escape hatch: list of {group, control, value}
        overrides.extend(args["overrides"])

    if not overrides:
        raise ValueError("no inputs specified")

    return CLIENT.call("input.set", {"device": device, "pad": pad, "overrides": overrides,
                                     "clear": args.get("clear", False)})


def tool_clear_input(args):
    return CLIENT.call("input.clear", {"device": args.get("device", "gc"),
                                       "pad": args.get("pad", 0)})


def tool_raw_rpc(args):
    return CLIENT.call(args["method"], args.get("params", {}))


_ADDRESS_SPACE_SCHEMA = {
    "type": "string",
    "enum": ["effective", "physical", "virtual"],
    "description": "Address space to use (default: effective).",
}

TOOLS = [
    {
        "name": "dolphin_launch_game",
        "description": "Launch a GameCube/Wii game in headless Dolphin with the "
                       "DebugRPC server enabled, then connect to it.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "romPath": {"type": "string", "description": "Path to the ISO/WBFS/ELF/DOL to boot."},
                "port": {"type": "integer", "description": "DebugRPC TCP port (default 6090)."},
                "dolphinPath": {"type": "string", "description": "Path to dolphin-emu-nogui."},
                "platform": {"type": "string", "description": "Window platform (default headless)."},
            },
            "required": ["romPath"],
        },
        "handler": tool_launch_game,
    },
    {
        "name": "dolphin_quit",
        "description": "Stop the running game and terminate the Dolphin process started by this server.",
        "inputSchema": {"type": "object", "properties": {}},
        "handler": tool_quit,
    },
    {
        "name": "dolphin_connect",
        "description": "Connect to an already-running Dolphin DebugRPC server (host/port).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "host": {"type": "string"},
                "port": {"type": "integer"},
            },
        },
        "handler": tool_connect,
    },
    {
        "name": "dolphin_status",
        "description": "Get core run state and the currently loaded game's ID/title/revision.",
        "inputSchema": {"type": "object", "properties": {}},
        "handler": tool_status,
    },
    {
        "name": "dolphin_set_state",
        "description": "Control emulation: run, pause, or stop.",
        "inputSchema": {
            "type": "object",
            "properties": {"state": {"type": "string", "enum": ["run", "pause", "stop"]}},
            "required": ["state"],
        },
        "handler": tool_set_state,
    },
    {
        "name": "dolphin_step",
        "description": "Execute a single CPU instruction (steps the PowerPC core).",
        "inputSchema": {"type": "object", "properties": {}},
        "handler": tool_step,
    },
    {
        "name": "dolphin_registers",
        "description": "Read the PowerPC registers (pc, lr, ctr, msr, gpr[0..31]).",
        "inputSchema": {"type": "object", "properties": {}},
        "handler": tool_registers,
    },
    {
        "name": "dolphin_disassemble",
        "description": "Disassemble PowerPC instructions starting at an address.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "address": {"type": "integer", "description": "Start address, e.g. 0x80003100."},
                "count": {"type": "integer", "description": "Number of instructions (default 16)."},
            },
            "required": ["address"],
        },
        "handler": tool_disassemble,
    },
    {
        "name": "dolphin_read_memory",
        "description": "Read raw bytes of emulated memory; returns a hex string.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "address": {"type": "integer"},
                "size": {"type": "integer", "description": "Number of bytes (1..0x100000)."},
                "addressSpace": _ADDRESS_SPACE_SCHEMA,
            },
            "required": ["address", "size"],
        },
        "handler": tool_read_memory,
    },
    {
        "name": "dolphin_write_memory",
        "description": "Write raw bytes (hex string) to emulated memory.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "address": {"type": "integer"},
                "data": {"type": "string", "description": "Hex string of bytes, e.g. 'deadbeef'."},
                "addressSpace": _ADDRESS_SPACE_SCHEMA,
            },
            "required": ["address", "data"],
        },
        "handler": tool_write_memory,
    },
    {
        "name": "dolphin_read_u32",
        "description": "Read a big-endian 32-bit value from emulated memory.",
        "inputSchema": {
            "type": "object",
            "properties": {"address": {"type": "integer"}},
            "required": ["address"],
        },
        "handler": tool_read_u32,
    },
    {
        "name": "dolphin_write_u32",
        "description": "Write a big-endian 32-bit value to emulated memory.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "address": {"type": "integer"},
                "value": {"type": "integer"},
            },
            "required": ["address", "value"],
        },
        "handler": tool_write_u32,
    },
    {
        "name": "dolphin_cheat_search_begin",
        "description": "Start a cheat (memory) search session over a value type and memory range.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "dataType": {"type": "string",
                             "enum": ["u8", "u16", "u32", "u64", "s8", "s16", "s32", "s64", "f32", "f64"],
                             "description": "Value type to search (default u32)."},
                "addressSpace": _ADDRESS_SPACE_SCHEMA,
                "aligned": {"type": "boolean"},
                "ranges": {
                    "type": "array",
                    "items": {
                        "type": "object",
                        "properties": {
                            "start": {"type": "integer"},
                            "length": {"type": "integer"},
                        },
                    },
                    "description": "Memory ranges to scan (defaults to MEM1/MEM2).",
                },
            },
        },
        "handler": tool_cheat_search_begin,
    },
    {
        "name": "dolphin_cheat_search_next",
        "description": "Run a search step within a session (new search or refine previous results).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "sessionId": {"type": "integer"},
                "compareType": {"type": "string",
                                "enum": ["eq", "ne", "lt", "le", "gt", "ge"]},
                "filterType": {"type": "string", "enum": ["value", "last", "none"]},
                "value": {"type": "string", "description": "Value to compare against (filterType=value)."},
                "hex": {"type": "boolean", "description": "Parse 'value' as hex."},
            },
            "required": ["sessionId"],
        },
        "handler": tool_cheat_search_next,
    },
    {
        "name": "dolphin_cheat_search_results",
        "description": "List current results of a cheat search session.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "sessionId": {"type": "integer"},
                "offset": {"type": "integer"},
                "limit": {"type": "integer"},
            },
            "required": ["sessionId"],
        },
        "handler": tool_cheat_search_results,
    },
    {
        "name": "dolphin_cheat_search_generate_ar",
        "description": "Generate an Action Replay code for a result in a cheat search session.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "sessionId": {"type": "integer"},
                "index": {"type": "integer"},
            },
            "required": ["sessionId", "index"],
        },
        "handler": tool_cheat_search_generate_ar,
    },
    {
        "name": "dolphin_cheat_search_end",
        "description": "Close a cheat search session and free its results.",
        "inputSchema": {
            "type": "object",
            "properties": {"sessionId": {"type": "integer"}},
            "required": ["sessionId"],
        },
        "handler": tool_cheat_search_end,
    },
    {
        "name": "dolphin_apply_ar",
        "description": "Apply an Action Replay code at runtime (list of {address,value} ops).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string"},
                "ops": {
                    "type": "array",
                    "items": {
                        "type": "object",
                        "properties": {
                            "address": {"type": "integer"},
                            "value": {"type": "integer"},
                        },
                        "required": ["address", "value"],
                    },
                },
            },
            "required": ["ops"],
        },
        "handler": tool_apply_ar,
    },
    {
        "name": "dolphin_apply_gecko",
        "description": "Apply a Gecko code at runtime (list of 'AAAAAAAA DDDDDDDD' lines). "
                       "Replaces the active Gecko set.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string"},
                "lines": {"type": "array", "items": {"type": "string"}},
            },
            "required": ["lines"],
        },
        "handler": tool_apply_gecko,
    },
    {
        "name": "dolphin_read_value",
        "description": "Read a single typed value (big-endian) from emulated memory.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "address": {"type": "integer"},
                "type": {"type": "string",
                         "enum": ["u8", "u16", "u32", "u64", "s8", "s16", "s32", "s64", "f32", "f64"],
                         "description": "Value type (default u32)."},
                "addressSpace": _ADDRESS_SPACE_SCHEMA,
            },
            "required": ["address"],
        },
        "handler": tool_read_value,
    },
    {
        "name": "dolphin_read_string",
        "description": "Read a NUL-terminated string from emulated memory.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "address": {"type": "integer"},
                "maxLength": {"type": "integer", "description": "Max bytes to scan (default 256)."},
                "encoding": {"type": "string", "description": "Text encoding (default utf-8)."},
                "addressSpace": _ADDRESS_SPACE_SCHEMA,
            },
            "required": ["address"],
        },
        "handler": tool_read_string,
    },
    {
        "name": "dolphin_save_state",
        "description": "Save a save state to a numbered slot or a file path.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "slot": {"type": "integer", "description": "Save-state slot number."},
                "file": {"type": "string", "description": "Explicit .sav file path."},
            },
        },
        "handler": tool_save_state,
    },
    {
        "name": "dolphin_load_state",
        "description": "Load a save state from a numbered slot or a file path.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "slot": {"type": "integer"},
                "file": {"type": "string"},
            },
        },
        "handler": tool_load_state,
    },
    {
        "name": "dolphin_screenshot",
        "description": "Capture a screenshot (requires a rendering backend).",
        "inputSchema": {
            "type": "object",
            "properties": {"name": {"type": "string", "description": "Optional output file name."}},
        },
        "handler": tool_screenshot,
    },
    {
        "name": "dolphin_frame_advance",
        "description": "Advance emulation by exactly one frame, then pause.",
        "inputSchema": {"type": "object", "properties": {}},
        "handler": tool_frame_advance,
    },
    {
        "name": "dolphin_symbol_from_address",
        "description": "Resolve the symbol (function) containing an address.",
        "inputSchema": {
            "type": "object",
            "properties": {"address": {"type": "integer"}},
            "required": ["address"],
        },
        "handler": tool_symbol_from_address,
    },
    {
        "name": "dolphin_symbol_from_name",
        "description": "Look up a symbol's address/size by name.",
        "inputSchema": {
            "type": "object",
            "properties": {"name": {"type": "string"}},
            "required": ["name"],
        },
        "handler": tool_symbol_from_name,
    },
    {
        "name": "dolphin_add_breakpoint",
        "description": "Add a PowerPC code breakpoint (halts only in debugging mode).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "address": {"type": "integer"},
                "break": {"type": "boolean", "description": "Halt on hit (default true)."},
                "log": {"type": "boolean", "description": "Log on hit (default false)."},
            },
            "required": ["address"],
        },
        "handler": tool_add_breakpoint,
    },
    {
        "name": "dolphin_remove_breakpoint",
        "description": "Remove a code breakpoint at an address.",
        "inputSchema": {
            "type": "object",
            "properties": {"address": {"type": "integer"}},
            "required": ["address"],
        },
        "handler": tool_remove_breakpoint,
    },
    {
        "name": "dolphin_list_breakpoints",
        "description": "List all code breakpoints.",
        "inputSchema": {"type": "object", "properties": {}},
        "handler": tool_list_breakpoints,
    },
    {
        "name": "dolphin_clear_breakpoints",
        "description": "Remove all code breakpoints.",
        "inputSchema": {"type": "object", "properties": {}},
        "handler": tool_clear_breakpoints,
    },
    {
        "name": "dolphin_add_watchpoint",
        "description": "Add a memory watchpoint (memcheck) that fires on read/write.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "address": {"type": "integer"},
                "end": {"type": "integer", "description": "End address for a ranged watch."},
                "read": {"type": "boolean", "description": "Watch reads (default true)."},
                "write": {"type": "boolean", "description": "Watch writes (default true)."},
                "break": {"type": "boolean", "description": "Halt on hit (default true)."},
                "log": {"type": "boolean", "description": "Log on hit (default false)."},
            },
            "required": ["address"],
        },
        "handler": tool_add_watchpoint,
    },
    {
        "name": "dolphin_remove_watchpoint",
        "description": "Remove a memory watchpoint at an address.",
        "inputSchema": {
            "type": "object",
            "properties": {"address": {"type": "integer"}},
            "required": ["address"],
        },
        "handler": tool_remove_watchpoint,
    },
    {
        "name": "dolphin_list_watchpoints",
        "description": "List all memory watchpoints.",
        "inputSchema": {"type": "object", "properties": {}},
        "handler": tool_list_watchpoints,
    },
    {
        "name": "dolphin_search_memory",
        "description": "Fast server-side scan of emulated RAM for a byte pattern or string; "
                       "returns matching addresses.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "pattern": {"type": "string", "description": "Hex byte pattern, e.g. '3f800000'."},
                "string": {"type": "string", "description": "ASCII string to find (alternative to pattern)."},
                "address": {"type": "integer", "description": "Start address (default 0x80000000)."},
                "size": {"type": "integer", "description": "Bytes to scan (default: rest of the region)."},
                "max": {"type": "integer", "description": "Max results to return (default 1000)."},
            },
        },
        "handler": tool_search_memory,
    },
    {
        "name": "dolphin_set_input",
        "description": "Inject GameCube or Wii Remote input (overrides real input until cleared). "
                       "Buttons/dpad are booleans; sticks and the Wii pointer take x/y in [-1, 1]. "
                       "For Wii motion (Shake/Tilt/Swing) use the raw 'overrides' field.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "device": {"type": "string", "enum": ["gc", "wii"],
                           "description": "Controller device (default gc)."},
                "pad": {"type": "integer", "description": "Controller port 0..3 (default 0)."},
                "buttons": {
                    "type": "object",
                    "description": "GC: a,b,x,y,z,start. Wii: a,b,one,two,plus,minus,home. -> true/false.",
                },
                "dpad": {
                    "type": "object",
                    "description": "Any of up, down, left, right -> true/false.",
                },
                "triggers": {
                    "type": "object",
                    "description": "GC only: l, r (bool) and/or l_analog, r_analog (0..1).",
                },
                "mainStick": {
                    "type": "object",
                    "properties": {"x": {"type": "number"}, "y": {"type": "number"}},
                    "description": "GC control stick position, x/y in [-1, 1].",
                },
                "cStick": {
                    "type": "object",
                    "properties": {"x": {"type": "number"}, "y": {"type": "number"}},
                    "description": "GC C-stick position, x/y in [-1, 1].",
                },
                "pointer": {
                    "type": "object",
                    "properties": {"x": {"type": "number"}, "y": {"type": "number"}},
                    "description": "Wii IR pointer position, x/y in [-1, 1].",
                },
                "overrides": {
                    "type": "array",
                    "items": {
                        "type": "object",
                        "properties": {
                            "group": {"type": "string"},
                            "control": {"type": "string"},
                            "value": {"type": "number"},
                        },
                    },
                    "description": "Raw group/control overrides (e.g. Wii 'Shake'/'Tilt'/'Swing').",
                },
                "clear": {"type": "boolean", "description": "Clear existing overrides for this port first."},
            },
        },
        "handler": tool_set_input,
    },
    {
        "name": "dolphin_clear_input",
        "description": "Remove all injected input for a controller port (returns control to the user).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "device": {"type": "string", "enum": ["gc", "wii"], "description": "Default gc."},
                "pad": {"type": "integer", "description": "Controller port 0..3 (default 0)."},
            },
        },
        "handler": tool_clear_input,
    },
    {
        "name": "dolphin_rpc",
        "description": "Escape hatch: call any DebugRPC method directly with raw params.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "method": {"type": "string"},
                "params": {"type": "object"},
            },
            "required": ["method"],
        },
        "handler": tool_raw_rpc,
    },
]

TOOLS_BY_NAME = {tool["name"]: tool for tool in TOOLS}


# --- MCP stdio server --------------------------------------------------------
def _public_tool(tool):
    return {k: v for k, v in tool.items() if k != "handler"}


def handle_request(request):
    method = request.get("method")
    request_id = request.get("id")

    if method == "initialize":
        client_version = (request.get("params") or {}).get("protocolVersion")
        return {
            "jsonrpc": "2.0",
            "id": request_id,
            "result": {
                "protocolVersion": client_version or PROTOCOL_VERSION,
                "capabilities": {"tools": {}},
                "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
            },
        }

    if method in ("notifications/initialized", "initialized"):
        return None  # notification, no response

    if method == "ping":
        return {"jsonrpc": "2.0", "id": request_id, "result": {}}

    if method == "tools/list":
        return {
            "jsonrpc": "2.0",
            "id": request_id,
            "result": {"tools": [_public_tool(t) for t in TOOLS]},
        }

    if method == "tools/call":
        params = request.get("params") or {}
        name = params.get("name")
        arguments = params.get("arguments") or {}
        tool = TOOLS_BY_NAME.get(name)
        if tool is None:
            return _tool_error(request_id, f"Unknown tool: {name}")
        try:
            result = tool["handler"](arguments)
            text = json.dumps(result, indent=2)
            return {
                "jsonrpc": "2.0",
                "id": request_id,
                "result": {"content": [{"type": "text", "text": text}], "isError": False},
            }
        except Exception as exc:  # surface errors back to the model
            return _tool_error(request_id, f"{type(exc).__name__}: {exc}")

    # Unknown method.
    if request_id is None:
        return None
    return {
        "jsonrpc": "2.0",
        "id": request_id,
        "error": {"code": -32601, "message": f"Method not found: {method}"},
    }


def _tool_error(request_id, message):
    return {
        "jsonrpc": "2.0",
        "id": request_id,
        "result": {"content": [{"type": "text", "text": message}], "isError": True},
    }


def main():
    log(f"starting (default RPC {DEFAULT_HOST}:{DEFAULT_PORT})")
    stdin = sys.stdin
    while True:
        line = stdin.readline()
        if not line:
            break
        line = line.strip()
        if not line:
            continue
        try:
            request = json.loads(line)
        except json.JSONDecodeError as exc:
            log(f"failed to parse line: {exc}")
            continue

        response = handle_request(request)
        if response is not None:
            sys.stdout.write(json.dumps(response) + "\n")
            sys.stdout.flush()

    CLIENT.quit()
    log("shutting down")


if __name__ == "__main__":
    main()

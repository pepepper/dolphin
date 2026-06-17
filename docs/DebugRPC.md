# DebugRPC + MCP: driving Dolphin from an AI assistant

DebugRPC exposes Dolphin's debugger and cheat subsystems over a small
line-delimited JSON-RPC TCP server. A companion
[Model Context Protocol](https://modelcontextprotocol.io) (MCP) server
(`Tools/mcp/dolphin_mcp_server.py`) wraps that protocol as tools, so an
assistant such as Claude Code can launch a game, read and write emulated
memory, disassemble code, run cheat searches, and generate and apply cheat
codes directly.

```
Claude Code  <--MCP/stdio-->  dolphin_mcp_server.py  <--JSON-RPC/TCP-->  Dolphin (DebugRPC)
```

## Quick start

1. Build Dolphin as usual (the server is compiled into `core`; no extra CMake
   flags are required).

2. Start a game with the RPC server enabled. With the headless frontend:

   ```sh
   dolphin-emu-nogui --exec /path/to/game.iso --debug-rpc-port 6090 -p headless
   ```

   Any frontend works too, because the server is started from the core; set the
   config value directly:

   ```sh
   dolphin-emu -C Dolphin.General.DebugRPCPort=6090 -e /path/to/game.iso
   ```

3. Point your assistant at the MCP server. This repository ships an `.mcp.json`
   that Claude Code picks up automatically; it runs
   `python3 Tools/mcp/dolphin_mcp_server.py`. The MCP server can also launch
   Dolphin for you via the `dolphin_launch_game` tool, so step 2 is optional.

The server binds to `127.0.0.1` only. It is disabled in RetroAchievements
hardcore mode, and the port defaults to `-1` (off).

## MCP tools

| Tool | Purpose |
| --- | --- |
| `dolphin_launch_game` | Boot a game in headless Dolphin with DebugRPC enabled and connect. |
| `dolphin_quit` | Stop the game and terminate the launched Dolphin process. |
| `dolphin_connect` | Connect to an already-running Dolphin DebugRPC server. |
| `dolphin_status` | Core state + loaded game ID/title/revision. |
| `dolphin_set_state` | `run` / `pause` / `stop`. |
| `dolphin_step` | Single-step the PowerPC CPU. |
| `dolphin_registers` | Read pc, lr, ctr, msr and the 32 GPRs. |
| `dolphin_disassemble` | Disassemble N instructions from an address. |
| `dolphin_read_memory` / `dolphin_write_memory` | Raw byte access (hex strings). |
| `dolphin_read_u32` / `dolphin_write_u32` | Convenience big-endian 32-bit access. |
| `dolphin_read_value` / `dolphin_read_string` | Typed value / NUL-terminated string reads. |
| `dolphin_search_memory` | Fast server-side scan of RAM for a byte pattern or string. |
| `dolphin_set_input` / `dolphin_clear_input` | Inject / release GameCube or Wii Remote input. |
| `dolphin_cheat_search_begin` / `_next` / `_results` / `_end` | Stateful memory search. |
| `dolphin_cheat_search_generate_ar` | Turn a search result into an Action Replay code. |
| `dolphin_apply_ar` / `dolphin_apply_gecko` | Apply cheat codes at runtime (merged with existing). |
| `dolphin_save_state` / `dolphin_load_state` | Save states by slot or file. |
| `dolphin_screenshot` | Capture a screenshot. |
| `dolphin_frame_advance` | Advance exactly one frame and pause. |
| `dolphin_symbol_from_address` / `dolphin_symbol_from_name` | Symbol lookup. |
| `dolphin_add_breakpoint` / `_remove_breakpoint` / `_list_breakpoints` / `_clear_breakpoints` | Code breakpoints. |
| `dolphin_add_watchpoint` / `_remove_watchpoint` / `_list_watchpoints` | Memory watchpoints. |
| `dolphin_rpc` | Escape hatch to call any DebugRPC method directly. |

### Configuration (environment variables)

- `DOLPHIN_NOGUI` — path to `dolphin-emu-nogui` (default: found on `PATH`).
- `DOLPHIN_RPC_HOST` — host to connect to (default `127.0.0.1`).
- `DOLPHIN_RPC_PORT` — default port (default `6090`).

## Wire protocol

One JSON request object is sent per line (`\n`-terminated); the server replies
with exactly one JSON object per request.

```jsonc
// request
{"id": 1, "method": "memory.read", "params": {"address": 2155905152, "size": 4}}
// success
{"id": 1, "result": {"address": 2155905152, "size": 4, "data": "00002710"}}
// failure
{"id": 1, "error": {"code": 2, "message": "memory at 0x80000000 not accessible"}}
```

Addresses and values may be given as JSON numbers or as `"0x"`-prefixed strings.
Byte payloads are lowercase hex strings.

### Methods

| Method | Params | Result |
| --- | --- | --- |
| `ping` | – | `{pong:true}` |
| `core.status` | – | `{state, running, gameId, title, revision}` |
| `core.run` / `core.pause` / `core.stop` | – | `{}` |
| `cpu.step` | – | `{}` |
| `cpu.registers` | – | `{pc, lr, ctr, msr, gpr[32]}` |
| `cpu.disassemble` | `{address, count?}` | `{instructions:[{address, raw, text}]}` |
| `memory.read` | `{address, size, addressSpace?}` | `{address, size, data}` |
| `memory.write` | `{address, data, addressSpace?}` | `{written}` |
| `memory.search` | `{pattern\|string, address?, size?, max?}` | `{count, truncated, addresses}` |
| `input.set` | `{device?, pad?, overrides:[{group, control, value}], clear?}` | `{set, device, pad, applied}` |
| `input.clear` | `{device?, pad?}` | `{cleared, device, pad}` |
| `cheatSearch.begin` | `{dataType?, addressSpace?, aligned?, ranges?}` | `{sessionId}` |
| `cheatSearch.next` | `{sessionId, compareType?, filterType?, value?, hex?}` | `{resultCount}` |
| `cheatSearch.results` | `{sessionId, offset?, limit?}` | `{total, results:[{address, value, valueHex}]}` |
| `cheatSearch.generateAR` | `{sessionId, index}` | `{name, ops:[{address, value}], lines:[…]}` |
| `cheatSearch.end` | `{sessionId}` | `{}` |
| `cheat.applyAR` | `{name?, ops:[{address, value}]}` | `{applied}` |
| `cheat.applyGecko` | `{name?, lines:["AAAAAAAA DDDDDDDD", …]}` | `{applied, activeCount}` |
| `state.save` / `state.load` | `{slot}` or `{file}` | `{saved/loaded, …}` |
| `core.screenshot` | `{name?}` | `{requested}` |
| `core.frameAdvance` | – | `{advanced}` |
| `symbol.fromAddress` | `{address}` | `{found, name, address, size}` |
| `symbol.fromName` | `{name}` | `{found, name, address, size}` |
| `breakpoint.add` | `{address, break?, log?}` | `{added, address}` |
| `breakpoint.remove` / `breakpoint.list` / `breakpoint.clear` | `{address}` / – / – | … |
| `memcheck.add` | `{address, end?, read?, write?, break?, log?}` | `{added}` |
| `memcheck.remove` / `memcheck.list` | `{address}` / – | … |

`addressSpace` is one of `effective` (default), `physical`, or `virtual`.
`dataType` is one of `u8 u16 u32 u64 s8 s16 s32 s64 f32 f64`.
`compareType` is `eq ne lt le gt ge`; `filterType` is `value last none`.

### Cheats and breakpoints

- Applying a cheat enables cheats in the config automatically. `cheat.applyAR`
  appends to the active Action Replay set; `cheat.applyGecko` merges with the
  active Gecko set, replacing any existing code with the same name.
- A code breakpoint or watchpoint is always registered, but it only halts the
  CPU when Dolphin runs with debugging enabled (e.g. `-d` /
  `Dolphin.General.EnableDebugging`, or the interpreter CPU core). The JIT cache
  is cleared on breakpoint changes so newly compiled blocks honor them.
- Screenshots require an active rendering backend; with the pure `headless`
  platform there may be nothing to capture.

### Memory search and input injection

- `memory.search` scans a single contiguous RAM region (MEM1 from
  `0x80000000`, or MEM2 from `0x90000000`) directly in the emulator, so it is
  far faster than reading memory out and scanning client-side. Give a `pattern`
  (hex bytes) or a `string`.
- `input.set` injects controller input using the same override mechanism as the
  TAS Input window. Set `device` to `gc` (GameCube pad, default) or `wii` (Wii
  Remote). Overrides persist until changed or cleared with `input.clear`, so
  they "hold" across frames. Buttons use `value` 1.0 (pressed) / 0.0 (released);
  analog sticks and the Wii IR pointer are driven via their `Up`/`Down`/`Left`/
  `Right` controls.
- The `dolphin_set_input` MCP tool maps friendly names to these controls:
  - GC: `buttons` (a, b, x, y, z, start), `dpad`, `triggers` (l, r, l_analog,
    r_analog), `mainStick`/`cStick` with `x`/`y` in [-1, 1].
  - Wii: `buttons` (a, b, one, two, plus, minus, home), `dpad`, and `pointer`
    with `x`/`y` in [-1, 1] for the IR cursor.
  - Wii motion (the `Shake`, `Tilt`, and `Swing` groups) and any other control
    can be driven through the raw `overrides` field (`{group, control, value}`).

## Typical cheat-search workflow

1. `cheatSearch.begin` with the value type → `sessionId`.
2. `cheatSearch.next` with `filterType:"value"` and the known value (e.g. current
   rupee count) → narrows results.
3. Change the value in-game, then `cheatSearch.next` again with the new value.
   Repeat until a handful of addresses remain.
4. `cheatSearch.results` to inspect addresses, `cheatSearch.generateAR` to mint
   an Action Replay code, and `cheat.applyAR` to lock the value.

## Security notes

- The server listens on loopback only and has no authentication; do not forward
  the port. Anyone who can reach it can read and write the game's memory.
- It is intended for development, debugging, TAS/cheat work and automation, and
  is off by default.

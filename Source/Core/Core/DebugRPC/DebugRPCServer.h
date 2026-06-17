// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// DebugRPC exposes Dolphin's debugger / cheat subsystems over a small
// line-delimited JSON-RPC TCP server so that external tooling (for example an
// MCP server driving the emulator from an AI assistant) can read and write
// emulated memory, disassemble code, run cheat searches, generate and apply
// cheat codes, and control the run state of the core.
//
// One JSON request object is sent per line (terminated by '\n'); the server
// replies with exactly one JSON object per request. See docs/DebugRPC.md for
// the protocol description.

#pragma once

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

namespace DebugRPC
{
// Starts the JSON-RPC server listening on the given TCP port. Safe to call when
// a server is already running (the call is ignored). Intended to be invoked
// from the emulation thread while a game is running.
void Init(Core::System& system, u16 port);

// Stops the server and joins its worker thread. Safe to call when no server is
// running.
void Deinit();

// Returns true while the server is listening.
bool IsActive();
}  // namespace DebugRPC

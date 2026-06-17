// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/DebugRPC/DebugRPCServer.h"

#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <WinSock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <fmt/format.h>
#include <picojson.h>

#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/SocketContext.h"
#include "Common/SymbolDB.h"

#include "Core/ActionReplay.h"
#include "Core/CheatGeneration.h"
#include "Core/CheatSearch.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/GeckoCode.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/State.h"
#include "Core/System.h"

namespace DebugRPC
{
namespace
{
#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
static void CloseSocket(SocketHandle s)
{
  closesocket(s);
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
static void CloseSocket(SocketHandle s)
{
  close(s);
}
#endif

constexpr char kHexDigits[] = "0123456789abcdef";

std::string BytesToHex(const u8* data, size_t size)
{
  std::string out;
  out.reserve(size * 2);
  for (size_t i = 0; i < size; ++i)
  {
    out.push_back(kHexDigits[(data[i] >> 4) & 0xf]);
    out.push_back(kHexDigits[data[i] & 0xf]);
  }
  return out;
}

std::optional<u8> HexNibble(char c)
{
  if (c >= '0' && c <= '9')
    return static_cast<u8>(c - '0');
  if (c >= 'a' && c <= 'f')
    return static_cast<u8>(c - 'a' + 10);
  if (c >= 'A' && c <= 'F')
    return static_cast<u8>(c - 'A' + 10);
  return std::nullopt;
}

std::optional<std::vector<u8>> HexToBytes(const std::string& hex)
{
  if (hex.size() % 2 != 0)
    return std::nullopt;
  std::vector<u8> out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2)
  {
    const std::optional<u8> hi = HexNibble(hex[i]);
    const std::optional<u8> lo = HexNibble(hex[i + 1]);
    if (!hi || !lo)
      return std::nullopt;
    out.push_back(static_cast<u8>((*hi << 4) | *lo));
  }
  return out;
}

PowerPC::RequestedAddressSpace ParseAddressSpace(const picojson::object& params)
{
  const auto it = params.find("addressSpace");
  if (it != params.end() && it->second.is<std::string>())
  {
    const std::string& s = it->second.get<std::string>();
    if (s == "physical")
      return PowerPC::RequestedAddressSpace::Physical;
    if (s == "virtual")
      return PowerPC::RequestedAddressSpace::Virtual;
  }
  return PowerPC::RequestedAddressSpace::Effective;
}

std::optional<Cheats::DataType> ParseDataType(const std::string& s)
{
  if (s == "u8")
    return Cheats::DataType::U8;
  if (s == "u16")
    return Cheats::DataType::U16;
  if (s == "u32")
    return Cheats::DataType::U32;
  if (s == "u64")
    return Cheats::DataType::U64;
  if (s == "s8")
    return Cheats::DataType::S8;
  if (s == "s16")
    return Cheats::DataType::S16;
  if (s == "s32")
    return Cheats::DataType::S32;
  if (s == "s64")
    return Cheats::DataType::S64;
  if (s == "f32")
    return Cheats::DataType::F32;
  if (s == "f64")
    return Cheats::DataType::F64;
  return std::nullopt;
}

std::optional<Cheats::CompareType> ParseCompareType(const std::string& s)
{
  if (s == "eq" || s == "equal")
    return Cheats::CompareType::Equal;
  if (s == "ne" || s == "notEqual")
    return Cheats::CompareType::NotEqual;
  if (s == "lt" || s == "less")
    return Cheats::CompareType::Less;
  if (s == "le" || s == "lessOrEqual")
    return Cheats::CompareType::LessOrEqual;
  if (s == "gt" || s == "greater")
    return Cheats::CompareType::Greater;
  if (s == "ge" || s == "greaterOrEqual")
    return Cheats::CompareType::GreaterOrEqual;
  return std::nullopt;
}

std::optional<Cheats::FilterType> ParseFilterType(const std::string& s)
{
  if (s == "value" || s == "specific")
    return Cheats::FilterType::CompareAgainstSpecificValue;
  if (s == "last")
    return Cheats::FilterType::CompareAgainstLastValue;
  if (s == "none")
    return Cheats::FilterType::DoNotFilter;
  return std::nullopt;
}

// Reads a key as an unsigned integer, accepting either a JSON number or a
// (possibly "0x"-prefixed) hex/decimal string.
std::optional<u64> ReadUInt(const picojson::object& obj, const std::string& key)
{
  const auto it = obj.find(key);
  if (it == obj.end())
    return std::nullopt;
  if (it->second.is<double>())
    return static_cast<u64>(it->second.get<double>());
  if (it->second.is<std::string>())
  {
    const std::string& s = it->second.get<std::string>();
    try
    {
      return std::stoull(s, nullptr, 0);
    }
    catch (...)
    {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<std::string> ReadString(const picojson::object& obj, const std::string& key)
{
  const auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<std::string>())
    return std::nullopt;
  return it->second.get<std::string>();
}

std::optional<bool> ReadBool(const picojson::object& obj, const std::string& key)
{
  const auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<bool>())
    return std::nullopt;
  return it->second.get<bool>();
}

picojson::value Num(double v)
{
  return picojson::value(v);
}

picojson::value Str(std::string v)
{
  return picojson::value(std::move(v));
}

class Server
{
public:
  Server(Core::System& system, u16 port) : m_system(system), m_port(port) {}

  bool Start()
  {
    m_socket_context.emplace();

    m_listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listen_socket == kInvalidSocket)
    {
      ERROR_LOG_FMT(CORE, "DebugRPC: failed to create socket");
      return false;
    }

    int yes = 1;
    setsockopt(m_listen_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes),
               sizeof(yes));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(m_port);

    if (bind(m_listen_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
      ERROR_LOG_FMT(CORE, "DebugRPC: failed to bind port {}", m_port);
      CloseSocket(m_listen_socket);
      m_listen_socket = kInvalidSocket;
      return false;
    }

    if (listen(m_listen_socket, 1) != 0)
    {
      ERROR_LOG_FMT(CORE, "DebugRPC: failed to listen on port {}", m_port);
      CloseSocket(m_listen_socket);
      m_listen_socket = kInvalidSocket;
      return false;
    }

    m_running.store(true);
    m_thread = std::thread(&Server::Run, this);
    INFO_LOG_FMT(CORE, "DebugRPC: listening on 127.0.0.1:{}", m_port);
    return true;
  }

  void Stop()
  {
    m_running.store(false);
    if (m_listen_socket != kInvalidSocket)
    {
      CloseSocket(m_listen_socket);
      m_listen_socket = kInvalidSocket;
    }
    if (m_thread.joinable())
      m_thread.join();
    m_socket_context.reset();
  }

private:
  void Run()
  {
    while (m_running.load())
    {
      const SocketHandle listen_socket = m_listen_socket;
      if (listen_socket == kInvalidSocket)
        break;

      fd_set read_fds;
      FD_ZERO(&read_fds);
      FD_SET(listen_socket, &read_fds);
      timeval tv = {1, 0};
      const int ready =
          select(static_cast<int>(listen_socket) + 1, &read_fds, nullptr, nullptr, &tv);
      if (ready <= 0)
        continue;

      const SocketHandle client = accept(listen_socket, nullptr, nullptr);
      if (client == kInvalidSocket)
        continue;

      INFO_LOG_FMT(CORE, "DebugRPC: client connected");
      HandleClient(client);
      CloseSocket(client);
      INFO_LOG_FMT(CORE, "DebugRPC: client disconnected");
    }
  }

  void HandleClient(SocketHandle client)
  {
    std::string buffer;
    char chunk[4096];
    while (m_running.load())
    {
      fd_set read_fds;
      FD_ZERO(&read_fds);
      FD_SET(client, &read_fds);
      timeval tv = {1, 0};
      const int ready = select(static_cast<int>(client) + 1, &read_fds, nullptr, nullptr, &tv);
      if (ready < 0)
        break;
      if (ready == 0)
        continue;

      const int received = static_cast<int>(recv(client, chunk, sizeof(chunk), 0));
      if (received <= 0)
        break;

      buffer.append(chunk, received);

      size_t newline;
      while ((newline = buffer.find('\n')) != std::string::npos)
      {
        std::string line = buffer.substr(0, newline);
        buffer.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        if (line.empty())
          continue;

        const std::string response = ProcessLine(line) + "\n";
        if (!SendAll(client, response))
          return;
      }
    }
  }

  static bool SendAll(SocketHandle client, const std::string& data)
  {
    size_t sent = 0;
    while (sent < data.size())
    {
      const int n =
          static_cast<int>(send(client, data.data() + sent, static_cast<int>(data.size() - sent), 0));
      if (n <= 0)
        return false;
      sent += static_cast<size_t>(n);
    }
    return true;
  }

  std::string ProcessLine(const std::string& line)
  {
    picojson::value request;
    const std::string parse_error = picojson::parse(request, line);
    if (!parse_error.empty() || !request.is<picojson::object>())
      return Serialize(MakeError(picojson::value(), -32700, "parse error: " + parse_error));

    const picojson::object& obj = request.get<picojson::object>();
    picojson::value id;
    if (const auto it = obj.find("id"); it != obj.end())
      id = it->second;

    const auto method_it = obj.find("method");
    if (method_it == obj.end() || !method_it->second.is<std::string>())
      return Serialize(MakeError(id, -32600, "missing method"));
    const std::string method = method_it->second.get<std::string>();

    picojson::object params;
    if (const auto it = obj.find("params"); it != obj.end() && it->second.is<picojson::object>())
      params = it->second.get<picojson::object>();

    try
    {
      return Serialize(Dispatch(id, method, params));
    }
    catch (const std::exception& e)
    {
      return Serialize(MakeError(id, -32000, std::string("internal error: ") + e.what()));
    }
  }

  static std::string Serialize(const picojson::value& v) { return v.serialize(false); }

  static picojson::value MakeResult(const picojson::value& id, picojson::value result)
  {
    picojson::object out;
    out["id"] = id;
    out["result"] = std::move(result);
    return picojson::value(out);
  }

  static picojson::value MakeError(const picojson::value& id, int code, const std::string& message)
  {
    picojson::object err;
    err["code"] = Num(code);
    err["message"] = Str(message);
    picojson::object out;
    out["id"] = id;
    out["error"] = picojson::value(err);
    return picojson::value(out);
  }

  bool EmulationActive() const
  {
    return Core::GetState(m_system) != Core::State::Uninitialized;
  }

  picojson::value Dispatch(const picojson::value& id, const std::string& method,
                           const picojson::object& params)
  {
    if (method == "ping")
    {
      picojson::object r;
      r["pong"] = picojson::value(true);
      return MakeResult(id, picojson::value(r));
    }
    if (method == "core.status")
      return HandleCoreStatus(id);
    if (method == "core.run")
      return HandleSetState(id, Core::State::Running);
    if (method == "core.pause")
      return HandleSetState(id, Core::State::Paused);
    if (method == "core.stop")
    {
      Core::Stop(m_system);
      return MakeResult(id, picojson::value(picojson::object{}));
    }
    if (method == "cpu.step")
    {
      if (!EmulationActive())
        return MakeError(id, 1, "no emulation active");
      m_system.GetPowerPC().GetDebugInterface().Step();
      return MakeResult(id, picojson::value(picojson::object{}));
    }
    if (method == "cpu.registers")
      return HandleRegisters(id);
    if (method == "cpu.disassemble")
      return HandleDisassemble(id, params);
    if (method == "memory.read")
      return HandleMemoryRead(id, params);
    if (method == "memory.write")
      return HandleMemoryWrite(id, params);
    if (method == "cheatSearch.begin")
      return HandleCheatSearchBegin(id, params);
    if (method == "cheatSearch.next")
      return HandleCheatSearchNext(id, params);
    if (method == "cheatSearch.results")
      return HandleCheatSearchResults(id, params);
    if (method == "cheatSearch.generateAR")
      return HandleCheatSearchGenerateAR(id, params);
    if (method == "cheatSearch.end")
      return HandleCheatSearchEnd(id, params);
    if (method == "cheat.applyAR")
      return HandleApplyAR(id, params);
    if (method == "cheat.applyGecko")
      return HandleApplyGecko(id, params);
    if (method == "state.save")
      return HandleStateSave(id, params);
    if (method == "state.load")
      return HandleStateLoad(id, params);
    if (method == "core.screenshot")
      return HandleScreenshot(id, params);
    if (method == "core.frameAdvance")
      return HandleFrameAdvance(id, params);
    if (method == "symbol.fromAddress")
      return HandleSymbolFromAddress(id, params);
    if (method == "symbol.fromName")
      return HandleSymbolFromName(id, params);
    if (method == "breakpoint.add")
      return HandleBreakpointAdd(id, params);
    if (method == "breakpoint.remove")
      return HandleBreakpointRemove(id, params);
    if (method == "breakpoint.list")
      return HandleBreakpointList(id);
    if (method == "breakpoint.clear")
      return HandleBreakpointClear(id);
    if (method == "memcheck.add")
      return HandleMemcheckAdd(id, params);
    if (method == "memcheck.remove")
      return HandleMemcheckRemove(id, params);
    if (method == "memcheck.list")
      return HandleMemcheckList(id);

    return MakeError(id, -32601, "unknown method: " + method);
  }

  picojson::value HandleCoreStatus(const picojson::value& id)
  {
    const Core::State state = Core::GetState(m_system);
    const char* state_str = "uninitialized";
    switch (state)
    {
    case Core::State::Paused:
      state_str = "paused";
      break;
    case Core::State::Running:
      state_str = "running";
      break;
    case Core::State::Stopping:
      state_str = "stopping";
      break;
    case Core::State::Starting:
      state_str = "starting";
      break;
    default:
      break;
    }

    const SConfig& config = SConfig::GetInstance();
    picojson::object r;
    r["state"] = Str(state_str);
    r["running"] = picojson::value(state == Core::State::Running || state == Core::State::Paused);
    r["gameId"] = Str(config.GetGameID());
    r["title"] = Str(config.GetTitleDescription());
    r["revision"] = Num(config.GetRevision());
    return MakeResult(id, picojson::value(r));
  }

  picojson::value HandleSetState(const picojson::value& id, Core::State state)
  {
    if (!EmulationActive())
      return MakeError(id, 1, "no emulation active");
    Core::SetState(m_system, state);
    return MakeResult(id, picojson::value(picojson::object{}));
  }

  picojson::value HandleRegisters(const picojson::value& id)
  {
    if (!EmulationActive())
      return MakeError(id, 1, "no emulation active");

    const Core::CPUThreadGuard guard(m_system);
    const auto& ppc = m_system.GetPowerPC().GetPPCState();

    picojson::array gpr;
    for (u32 i = 0; i < 32; ++i)
      gpr.emplace_back(static_cast<double>(ppc.gpr[i]));

    picojson::object r;
    r["pc"] = Num(ppc.pc);
    r["lr"] = Num(ppc.spr[SPR_LR]);
    r["ctr"] = Num(ppc.spr[SPR_CTR]);
    r["msr"] = Num(ppc.msr.Hex);
    r["gpr"] = picojson::value(gpr);
    return MakeResult(id, picojson::value(r));
  }

  picojson::value HandleDisassemble(const picojson::value& id, const picojson::object& params)
  {
    if (!EmulationActive())
      return MakeError(id, 1, "no emulation active");

    const std::optional<u64> address = ReadUInt(params, "address");
    if (!address)
      return MakeError(id, -32602, "missing 'address'");
    u64 count = ReadUInt(params, "count").value_or(16);
    if (count == 0 || count > 4096)
      count = 16;

    const Core::CPUThreadGuard guard(m_system);
    auto& debug = m_system.GetPowerPC().GetDebugInterface();

    picojson::array lines;
    for (u64 i = 0; i < count; ++i)
    {
      const u32 addr = static_cast<u32>(*address) + static_cast<u32>(i * 4);
      const u32 op = PowerPC::MMU::HostRead_Instruction(guard, addr);
      picojson::object entry;
      entry["address"] = Num(addr);
      entry["raw"] = Num(op);
      entry["text"] = Str(debug.Disassemble(&guard, addr));
      lines.emplace_back(entry);
    }

    picojson::object r;
    r["instructions"] = picojson::value(lines);
    return MakeResult(id, picojson::value(r));
  }

  picojson::value HandleMemoryRead(const picojson::value& id, const picojson::object& params)
  {
    if (!EmulationActive())
      return MakeError(id, 1, "no emulation active");

    const std::optional<u64> address = ReadUInt(params, "address");
    const std::optional<u64> size = ReadUInt(params, "size");
    if (!address || !size)
      return MakeError(id, -32602, "missing 'address' or 'size'");
    if (*size == 0 || *size > 0x100000)
      return MakeError(id, -32602, "'size' must be between 1 and 0x100000");

    const PowerPC::RequestedAddressSpace space = ParseAddressSpace(params);
    const Core::CPUThreadGuard guard(m_system);

    std::vector<u8> bytes;
    bytes.reserve(*size);
    for (u64 i = 0; i < *size; ++i)
    {
      const u32 addr = static_cast<u32>(*address) + static_cast<u32>(i);
      const auto value = PowerPC::MMU::HostTryRead<u8>(guard, addr, space);
      if (!value)
        return MakeError(id, 2, fmt::format("memory at {:#010x} not accessible", addr));
      bytes.push_back(value->value);
    }

    picojson::object r;
    r["address"] = Num(static_cast<double>(*address));
    r["size"] = Num(static_cast<double>(*size));
    r["data"] = Str(BytesToHex(bytes.data(), bytes.size()));
    return MakeResult(id, picojson::value(r));
  }

  picojson::value HandleMemoryWrite(const picojson::value& id, const picojson::object& params)
  {
    if (!EmulationActive())
      return MakeError(id, 1, "no emulation active");

    const std::optional<u64> address = ReadUInt(params, "address");
    const std::optional<std::string> data = ReadString(params, "data");
    if (!address || !data)
      return MakeError(id, -32602, "missing 'address' or 'data' (hex string)");

    const std::optional<std::vector<u8>> bytes = HexToBytes(*data);
    if (!bytes)
      return MakeError(id, -32602, "'data' is not a valid hex string");

    const PowerPC::RequestedAddressSpace space = ParseAddressSpace(params);
    const Core::CPUThreadGuard guard(m_system);

    for (size_t i = 0; i < bytes->size(); ++i)
    {
      const u32 addr = static_cast<u32>(*address) + static_cast<u32>(i);
      const auto result = PowerPC::MMU::HostTryWrite<u8>(guard, (*bytes)[i], addr, space);
      if (!result)
        return MakeError(id, 2, fmt::format("memory at {:#010x} not writable", addr));
    }

    picojson::object r;
    r["written"] = Num(static_cast<double>(bytes->size()));
    return MakeResult(id, picojson::value(r));
  }

  std::vector<Cheats::MemoryRange> DefaultMemoryRanges() const
  {
    std::vector<Cheats::MemoryRange> ranges;
    auto& memory = m_system.GetMemory();
    ranges.emplace_back(0x80000000, memory.GetRamSizeReal());
    if (memory.GetExRamSizeReal() > 0)
      ranges.emplace_back(0x90000000, memory.GetExRamSizeReal());
    return ranges;
  }

  picojson::value HandleCheatSearchBegin(const picojson::value& id, const picojson::object& params)
  {
    if (!EmulationActive())
      return MakeError(id, 1, "no emulation active");

    const std::string data_type_str = ReadString(params, "dataType").value_or("u32");
    const std::optional<Cheats::DataType> data_type = ParseDataType(data_type_str);
    if (!data_type)
      return MakeError(id, -32602, "invalid 'dataType': " + data_type_str);

    const PowerPC::RequestedAddressSpace space = ParseAddressSpace(params);
    bool aligned = true;
    if (const auto it = params.find("aligned"); it != params.end() && it->second.is<bool>())
      aligned = it->second.get<bool>();

    std::vector<Cheats::MemoryRange> ranges;
    if (const auto it = params.find("ranges"); it != params.end() && it->second.is<picojson::array>())
    {
      for (const auto& entry : it->second.get<picojson::array>())
      {
        if (!entry.is<picojson::object>())
          continue;
        const auto& range_obj = entry.get<picojson::object>();
        const std::optional<u64> start = ReadUInt(range_obj, "start");
        const std::optional<u64> length = ReadUInt(range_obj, "length");
        if (start && length)
          ranges.emplace_back(static_cast<u32>(*start), *length);
      }
    }
    if (ranges.empty())
      ranges = DefaultMemoryRanges();

    auto session = Cheats::MakeSession(std::move(ranges), space, aligned, *data_type);
    if (!session)
      return MakeError(id, 3, "failed to create cheat search session");

    std::lock_guard lock(m_session_mutex);
    const u32 session_id = m_next_session_id++;
    m_sessions[session_id] = std::move(session);

    picojson::object r;
    r["sessionId"] = Num(session_id);
    return MakeResult(id, picojson::value(r));
  }

  picojson::value HandleCheatSearchNext(const picojson::value& id, const picojson::object& params)
  {
    if (!EmulationActive())
      return MakeError(id, 1, "no emulation active");

    const std::optional<u64> session_id = ReadUInt(params, "sessionId");
    if (!session_id)
      return MakeError(id, -32602, "missing 'sessionId'");

    std::lock_guard lock(m_session_mutex);
    const auto it = m_sessions.find(static_cast<u32>(*session_id));
    if (it == m_sessions.end())
      return MakeError(id, 4, "unknown sessionId");
    Cheats::CheatSearchSessionBase& session = *it->second;

    if (const auto compare = ReadString(params, "compareType"))
    {
      const std::optional<Cheats::CompareType> parsed = ParseCompareType(*compare);
      if (!parsed)
        return MakeError(id, -32602, "invalid 'compareType': " + *compare);
      session.SetCompareType(*parsed);
    }

    const std::string filter_str = ReadString(params, "filterType").value_or("value");
    const std::optional<Cheats::FilterType> filter = ParseFilterType(filter_str);
    if (!filter)
      return MakeError(id, -32602, "invalid 'filterType': " + filter_str);
    session.SetFilterType(*filter);

    if (*filter == Cheats::FilterType::CompareAgainstSpecificValue)
    {
      const std::optional<std::string> value = ReadString(params, "value");
      if (!value)
        return MakeError(id, -32602, "filterType 'value' requires a 'value' string");
      bool force_hex = false;
      if (const auto vit = params.find("hex"); vit != params.end() && vit->second.is<bool>())
        force_hex = vit->second.get<bool>();
      if (!session.SetValueFromString(*value, force_hex))
        return MakeError(id, -32602, "could not parse 'value': " + *value);
    }

    const Core::CPUThreadGuard guard(m_system);
    const Cheats::SearchErrorCode error = session.RunSearch(guard);
    if (error != Cheats::SearchErrorCode::Success)
      return MakeError(id, 5, fmt::format("search failed (code {})", static_cast<int>(error)));

    picojson::object r;
    r["resultCount"] = Num(static_cast<double>(session.GetResultCount()));
    return MakeResult(id, picojson::value(r));
  }

  picojson::value HandleCheatSearchResults(const picojson::value& id, const picojson::object& params)
  {
    const std::optional<u64> session_id = ReadUInt(params, "sessionId");
    if (!session_id)
      return MakeError(id, -32602, "missing 'sessionId'");

    std::lock_guard lock(m_session_mutex);
    const auto it = m_sessions.find(static_cast<u32>(*session_id));
    if (it == m_sessions.end())
      return MakeError(id, 4, "unknown sessionId");
    const Cheats::CheatSearchSessionBase& session = *it->second;

    const u64 offset = ReadUInt(params, "offset").value_or(0);
    const u64 limit = ReadUInt(params, "limit").value_or(100);
    const size_t total = session.GetResultCount();

    picojson::array results;
    for (u64 i = offset; i < total && i < offset + limit; ++i)
    {
      picojson::object entry;
      entry["address"] = Num(session.GetResultAddress(i));
      entry["value"] = Str(session.GetResultValueAsString(i, false));
      entry["valueHex"] = Str(session.GetResultValueAsString(i, true));
      results.emplace_back(entry);
    }

    picojson::object r;
    r["total"] = Num(static_cast<double>(total));
    r["results"] = picojson::value(results);
    return MakeResult(id, picojson::value(r));
  }

  picojson::value HandleCheatSearchGenerateAR(const picojson::value& id,
                                              const picojson::object& params)
  {
    const std::optional<u64> session_id = ReadUInt(params, "sessionId");
    const std::optional<u64> index = ReadUInt(params, "index");
    if (!session_id || !index)
      return MakeError(id, -32602, "missing 'sessionId' or 'index'");

    std::lock_guard lock(m_session_mutex);
    const auto it = m_sessions.find(static_cast<u32>(*session_id));
    if (it == m_sessions.end())
      return MakeError(id, 4, "unknown sessionId");

    const auto code = Cheats::GenerateActionReplayCode(*it->second, static_cast<size_t>(*index));
    if (!code)
      return MakeError(id, 6, fmt::format("could not generate AR code (code {})",
                                          static_cast<int>(code.error())));

    return MakeResult(id, ARCodeToJson(*code));
  }

  static picojson::value ARCodeToJson(const ActionReplay::ARCode& code)
  {
    picojson::array ops;
    picojson::array lines;
    for (const ActionReplay::AREntry& op : code.ops)
    {
      picojson::object op_obj;
      op_obj["address"] = Num(op.cmd_addr);
      op_obj["value"] = Num(op.value);
      ops.emplace_back(op_obj);
      lines.emplace_back(fmt::format("{:08X} {:08X}", op.cmd_addr, op.value));
    }
    picojson::object r;
    r["name"] = Str(code.name);
    r["ops"] = picojson::value(ops);
    r["lines"] = picojson::value(lines);
    return picojson::value(r);
  }

  picojson::value HandleCheatSearchEnd(const picojson::value& id, const picojson::object& params)
  {
    const std::optional<u64> session_id = ReadUInt(params, "sessionId");
    if (!session_id)
      return MakeError(id, -32602, "missing 'sessionId'");
    std::lock_guard lock(m_session_mutex);
    m_sessions.erase(static_cast<u32>(*session_id));
    return MakeResult(id, picojson::value(picojson::object{}));
  }

  picojson::value HandleApplyAR(const picojson::value& id, const picojson::object& params)
  {
    if (!EmulationActive())
      return MakeError(id, 1, "no emulation active");

    ActionReplay::ARCode code;
    code.name = ReadString(params, "name").value_or("DebugRPC");
    code.enabled = true;
    code.user_defined = true;

    const auto ops_it = params.find("ops");
    if (ops_it == params.end() || !ops_it->second.is<picojson::array>())
      return MakeError(id, -32602, "missing 'ops' array of {address,value}");

    for (const auto& entry : ops_it->second.get<picojson::array>())
    {
      if (!entry.is<picojson::object>())
        continue;
      const auto& op_obj = entry.get<picojson::object>();
      const std::optional<u64> addr = ReadUInt(op_obj, "address");
      const std::optional<u64> value = ReadUInt(op_obj, "value");
      if (!addr || !value)
        return MakeError(id, -32602, "each op needs 'address' and 'value'");
      code.ops.emplace_back(static_cast<u32>(*addr), static_cast<u32>(*value));
    }

    if (code.ops.empty())
      return MakeError(id, -32602, "'ops' is empty");

    EnsureCheatsEnabled();
    // AddCode appends to the active set, so existing codes are preserved.
    ActionReplay::AddCode(std::move(code));

    picojson::object r;
    r["applied"] = picojson::value(true);
    return MakeResult(id, picojson::value(r));
  }

  // AR/Gecko codes only activate when cheats are enabled in config. Applying a
  // code is an explicit request to enable it, so turn cheats on if needed.
  static void EnsureCheatsEnabled()
  {
    if (!Config::AreCheatsEnabled())
      Config::SetCurrent(Config::MAIN_ENABLE_CHEATS, true);
  }

  picojson::value HandleApplyGecko(const picojson::value& id, const picojson::object& params)
  {
    if (!EmulationActive())
      return MakeError(id, 1, "no emulation active");

    const auto lines_it = params.find("lines");
    if (lines_it == params.end() || !lines_it->second.is<picojson::array>())
      return MakeError(id, -32602, "missing 'lines' array of 'AAAAAAAA DDDDDDDD' strings");

    Gecko::GeckoCode code;
    code.name = ReadString(params, "name").value_or("DebugRPC");
    code.enabled = true;
    code.user_defined = true;

    for (const auto& entry : lines_it->second.get<picojson::array>())
    {
      if (!entry.is<std::string>())
        continue;
      const std::string& line = entry.get<std::string>();
      const size_t space = line.find(' ');
      if (space == std::string::npos)
        return MakeError(id, -32602, "invalid gecko line: " + line);
      try
      {
        Gecko::GeckoCode::Code c;
        c.address = static_cast<u32>(std::stoul(line.substr(0, space), nullptr, 16));
        c.data = static_cast<u32>(std::stoul(line.substr(space + 1), nullptr, 16));
        c.original_line = line;
        code.codes.push_back(c);
      }
      catch (...)
      {
        return MakeError(id, -32602, "invalid gecko line: " + line);
      }
    }

    if (code.codes.empty())
      return MakeError(id, -32602, "'lines' is empty");

    EnsureCheatsEnabled();

    // Merge with the codes already active for this game. A code with the same
    // name is replaced so re-applying updates it rather than duplicating.
    std::vector<Gecko::GeckoCode> codes = Gecko::GetActiveCodes();
    std::erase_if(codes, [&](const Gecko::GeckoCode& c) { return c.name == code.name; });
    codes.push_back(std::move(code));
    Gecko::SetActiveCodes(codes, SConfig::GetInstance().GetGameID(),
                          SConfig::GetInstance().GetRevision());

    picojson::object r;
    r["applied"] = picojson::value(true);
    r["activeCount"] = Num(static_cast<double>(codes.size()));
    return MakeResult(id, picojson::value(r));
  }

  // -- save states ----------------------------------------------------------
  picojson::value HandleStateSave(const picojson::value& id, const picojson::object& params)
  {
    if (!EmulationActive())
      return MakeError(id, 1, "no emulation active");
    if (const auto file = ReadString(params, "file"))
    {
      State::SaveAs(m_system, *file);
      picojson::object r;
      r["saved"] = picojson::value(true);
      r["file"] = Str(*file);
      return MakeResult(id, picojson::value(r));
    }
    const std::optional<u64> slot = ReadUInt(params, "slot");
    if (!slot)
      return MakeError(id, -32602, "provide a 'slot' number or a 'file' path");
    State::Save(m_system, static_cast<int>(*slot));
    picojson::object r;
    r["saved"] = picojson::value(true);
    r["slot"] = Num(static_cast<double>(*slot));
    return MakeResult(id, picojson::value(r));
  }

  picojson::value HandleStateLoad(const picojson::value& id, const picojson::object& params)
  {
    if (!EmulationActive())
      return MakeError(id, 1, "no emulation active");
    if (const auto file = ReadString(params, "file"))
    {
      State::LoadAs(m_system, *file);
      picojson::object r;
      r["loaded"] = picojson::value(true);
      r["file"] = Str(*file);
      return MakeResult(id, picojson::value(r));
    }
    const std::optional<u64> slot = ReadUInt(params, "slot");
    if (!slot)
      return MakeError(id, -32602, "provide a 'slot' number or a 'file' path");
    State::Load(m_system, static_cast<int>(*slot));
    picojson::object r;
    r["loaded"] = picojson::value(true);
    r["slot"] = Num(static_cast<double>(*slot));
    return MakeResult(id, picojson::value(r));
  }

  picojson::value HandleScreenshot(const picojson::value& id, const picojson::object& params)
  {
    if (!EmulationActive())
      return MakeError(id, 1, "no emulation active");
    const std::optional<std::string> name = ReadString(params, "name");
    if (name)
      Core::SaveScreenShot(*name);
    else
      Core::SaveScreenShot();
    picojson::object r;
    r["requested"] = picojson::value(true);
    if (name)
      r["name"] = Str(*name);
    return MakeResult(id, picojson::value(r));
  }

  picojson::value HandleFrameAdvance(const picojson::value& id, const picojson::object& params)
  {
    if (!EmulationActive())
      return MakeError(id, 1, "no emulation active");
    // DoFrameStep advances exactly one frame and pauses.
    Core::DoFrameStep(m_system);
    picojson::object r;
    r["advanced"] = picojson::value(true);
    return MakeResult(id, picojson::value(r));
  }

  // -- symbols --------------------------------------------------------------
  picojson::value SymbolToJson(const picojson::value& id, const Common::Symbol* symbol)
  {
    picojson::object r;
    if (symbol == nullptr)
    {
      r["found"] = picojson::value(false);
      return MakeResult(id, picojson::value(r));
    }
    r["found"] = picojson::value(true);
    r["name"] = Str(symbol->name);
    r["address"] = Num(symbol->address);
    r["size"] = Num(symbol->size);
    return MakeResult(id, picojson::value(r));
  }

  picojson::value HandleSymbolFromAddress(const picojson::value& id, const picojson::object& params)
  {
    if (!EmulationActive())
      return MakeError(id, 1, "no emulation active");
    const std::optional<u64> address = ReadUInt(params, "address");
    if (!address)
      return MakeError(id, -32602, "missing 'address'");
    return SymbolToJson(id, m_system.GetPPCSymbolDB().GetSymbolFromAddr(static_cast<u32>(*address)));
  }

  picojson::value HandleSymbolFromName(const picojson::value& id, const picojson::object& params)
  {
    if (!EmulationActive())
      return MakeError(id, 1, "no emulation active");
    const std::optional<std::string> name = ReadString(params, "name");
    if (!name)
      return MakeError(id, -32602, "missing 'name'");
    return SymbolToJson(id, m_system.GetPPCSymbolDB().GetSymbolFromName(*name));
  }

  // -- code breakpoints -----------------------------------------------------
  // Note: a breakpoint only halts the CPU if Dolphin is running with debugging
  // enabled (e.g. MAIN_ENABLE_DEBUGGING / the interpreter). The breakpoint is
  // always registered regardless.
  picojson::value HandleBreakpointAdd(const picojson::value& id, const picojson::object& params)
  {
    if (!EmulationActive())
      return MakeError(id, 1, "no emulation active");
    const std::optional<u64> address = ReadUInt(params, "address");
    if (!address)
      return MakeError(id, -32602, "missing 'address'");

    TBreakPoint bp;
    bp.address = static_cast<u32>(*address);
    bp.is_enabled = true;
    bp.break_on_hit = ReadBool(params, "break").value_or(true);
    bp.log_on_hit = ReadBool(params, "log").value_or(false);

    const Core::CPUThreadGuard guard(m_system);
    m_system.GetPowerPC().GetBreakPoints().Add(std::move(bp));
    m_system.GetJitInterface().ClearCache(guard);

    picojson::object r;
    r["added"] = picojson::value(true);
    r["address"] = Num(static_cast<double>(*address));
    return MakeResult(id, picojson::value(r));
  }

  picojson::value HandleBreakpointRemove(const picojson::value& id, const picojson::object& params)
  {
    if (!EmulationActive())
      return MakeError(id, 1, "no emulation active");
    const std::optional<u64> address = ReadUInt(params, "address");
    if (!address)
      return MakeError(id, -32602, "missing 'address'");

    const Core::CPUThreadGuard guard(m_system);
    const bool removed = m_system.GetPowerPC().GetBreakPoints().Remove(static_cast<u32>(*address));
    m_system.GetJitInterface().ClearCache(guard);

    picojson::object r;
    r["removed"] = picojson::value(removed);
    return MakeResult(id, picojson::value(r));
  }

  picojson::value HandleBreakpointList(const picojson::value& id)
  {
    if (!EmulationActive())
      return MakeError(id, 1, "no emulation active");
    picojson::array list;
    for (const TBreakPoint& bp : m_system.GetPowerPC().GetBreakPoints().GetBreakPoints())
    {
      picojson::object entry;
      entry["address"] = Num(bp.address);
      entry["enabled"] = picojson::value(bp.is_enabled);
      entry["breakOnHit"] = picojson::value(bp.break_on_hit);
      entry["logOnHit"] = picojson::value(bp.log_on_hit);
      list.emplace_back(entry);
    }
    picojson::object r;
    r["breakpoints"] = picojson::value(list);
    return MakeResult(id, picojson::value(r));
  }

  picojson::value HandleBreakpointClear(const picojson::value& id)
  {
    if (!EmulationActive())
      return MakeError(id, 1, "no emulation active");
    const Core::CPUThreadGuard guard(m_system);
    m_system.GetPowerPC().GetBreakPoints().Clear();
    m_system.GetJitInterface().ClearCache(guard);
    return MakeResult(id, picojson::value(picojson::object{}));
  }

  // -- memory watchpoints (memchecks) ---------------------------------------
  picojson::value HandleMemcheckAdd(const picojson::value& id, const picojson::object& params)
  {
    if (!EmulationActive())
      return MakeError(id, 1, "no emulation active");
    const std::optional<u64> address = ReadUInt(params, "address");
    if (!address)
      return MakeError(id, -32602, "missing 'address'");
    const u32 start = static_cast<u32>(*address);
    const u32 end = static_cast<u32>(ReadUInt(params, "end").value_or(start));

    TMemCheck mc;
    mc.start_address = start;
    mc.end_address = end;
    mc.is_ranged = end != start;
    mc.is_enabled = true;
    mc.is_break_on_read = ReadBool(params, "read").value_or(true);
    mc.is_break_on_write = ReadBool(params, "write").value_or(true);
    mc.break_on_hit = ReadBool(params, "break").value_or(true);
    mc.log_on_hit = ReadBool(params, "log").value_or(false);

    const Core::CPUThreadGuard guard(m_system);
    const DelayedMemCheckUpdate update = m_system.GetPowerPC().GetMemChecks().Add(std::move(mc));
    (void)update;  // applies on destruction, while the guard is still held

    picojson::object r;
    r["added"] = picojson::value(true);
    return MakeResult(id, picojson::value(r));
  }

  picojson::value HandleMemcheckRemove(const picojson::value& id, const picojson::object& params)
  {
    if (!EmulationActive())
      return MakeError(id, 1, "no emulation active");
    const std::optional<u64> address = ReadUInt(params, "address");
    if (!address)
      return MakeError(id, -32602, "missing 'address'");

    const Core::CPUThreadGuard guard(m_system);
    const DelayedMemCheckUpdate update =
        m_system.GetPowerPC().GetMemChecks().Remove(static_cast<u32>(*address));
    (void)update;

    picojson::object r;
    r["removed"] = picojson::value(true);
    return MakeResult(id, picojson::value(r));
  }

  picojson::value HandleMemcheckList(const picojson::value& id)
  {
    if (!EmulationActive())
      return MakeError(id, 1, "no emulation active");
    picojson::array list;
    for (const TMemCheck& mc : m_system.GetPowerPC().GetMemChecks().GetMemChecks())
    {
      picojson::object entry;
      entry["startAddress"] = Num(mc.start_address);
      entry["endAddress"] = Num(mc.end_address);
      entry["ranged"] = picojson::value(mc.is_ranged);
      entry["breakOnRead"] = picojson::value(mc.is_break_on_read);
      entry["breakOnWrite"] = picojson::value(mc.is_break_on_write);
      entry["breakOnHit"] = picojson::value(mc.break_on_hit);
      entry["logOnHit"] = picojson::value(mc.log_on_hit);
      entry["numHits"] = Num(mc.num_hits);
      list.emplace_back(entry);
    }
    picojson::object r;
    r["memchecks"] = picojson::value(list);
    return MakeResult(id, picojson::value(r));
  }

  Core::System& m_system;
  u16 m_port;

  std::optional<Common::SocketContext> m_socket_context;
  SocketHandle m_listen_socket = kInvalidSocket;
  std::thread m_thread;
  std::atomic<bool> m_running{false};

  std::mutex m_session_mutex;
  std::map<u32, std::unique_ptr<Cheats::CheatSearchSessionBase>> m_sessions;
  u32 m_next_session_id = 1;
};

std::unique_ptr<Server> s_server;
}  // namespace

void Init(Core::System& system, u16 port)
{
  if (s_server)
    return;
  auto server = std::make_unique<Server>(system, port);
  if (!server->Start())
    return;
  s_server = std::move(server);
}

void Deinit()
{
  if (!s_server)
    return;
  s_server->Stop();
  s_server.reset();
}

bool IsActive()
{
  return s_server != nullptr;
}
}  // namespace DebugRPC

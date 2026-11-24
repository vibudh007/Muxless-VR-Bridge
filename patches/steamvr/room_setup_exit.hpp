#pragma once

#include <cstddef>
#include <string_view>

namespace steamvrRoomSetup {

inline constexpr std::size_t quitMethodOffset = 0x10de9;
// Pin the IL token and string tables used by the replacement method body.
inline constexpr std::size_t tokenTableOffset = 0x263d0, tokenTableSize = 147132;
inline constexpr std::size_t stringTableOffset = 0x4a28c, stringTableSize = 90944;
inline constexpr std::string_view tokenTableSha256 =
    "1e0ff4638b0e47e6fc5f4bce24d68238ae383c7ee2e932b28c4a3390dc5ac619";
inline constexpr std::string_view stringTableSha256 =
    "b1e0c8e7aedbcea650d04b167dd351417b4882f13b1c37bba88c199324169475";

// Valve.VR.InteractionSystem.Util.Quit, including the tiny method header.
// Replace Process.GetCurrentProcess().Kill() with UnityEngine.Application.Quit().
inline constexpr std::string_view stockQuit{
    "\x2e\x28\x59\x02\x00\x0a\x6f\x5a\x02\x00\x0a\x2a", 12};
inline constexpr std::string_view repairedQuit{
    "\x2e\x28\x8a\x03\x00\x0a\x00\x00\x00\x00\x00\x2a", 12};

}  // namespace steamvrRoomSetup

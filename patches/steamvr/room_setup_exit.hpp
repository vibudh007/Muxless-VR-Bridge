#pragma once

#include <cstddef>
#include <string_view>

namespace steamvrRoomSetup {

inline constexpr std::size_t assemblySize = 445952;
inline constexpr std::size_t quitMethodOffset = 0x10de9;
inline constexpr std::string_view stockSha256 =
    "c717cbc9c459e09a5074afe465acedb2043c9c4eeb7241a5784055cf017f4980";
inline constexpr std::string_view repairedSha256 =
    "8adb45c05f396616819d3433abd49fc2704a96b3ee1bf39d26d2eda4960b2ca2";

// Valve.VR.InteractionSystem.Util.Quit, including the tiny method header.
// Replace Process.GetCurrentProcess().Kill() with UnityEngine.Application.Quit().
inline constexpr std::string_view stockQuit{
    "\x2e\x28\x59\x02\x00\x0a\x6f\x5a\x02\x00\x0a\x2a", 12};
inline constexpr std::string_view repairedQuit{
    "\x2e\x28\x8a\x03\x00\x0a\x00\x00\x00\x00\x00\x2a", 12};

}  // namespace steamvrRoomSetup

#pragma once

#include <cstddef>
#include <string_view>

namespace steamvrPipewire {

inline constexpr std::size_t callbackOffset = 0x206e80;
inline constexpr std::size_t unwindRangeOffset = 0x590e8;
// PipeWire reports property removal as a null value. The matched vrserver callback
// forwards it to the audio default handler, which calls strstr on the value.
// Route the entry through a guard in padding before the next function. Valid
// events resume at the original key check, with the original stack and call.
inline constexpr std::string_view stockCallback{
    "\x85\xf6\x75\x1c\x48\x85\xd2\x48\x89\xd6\x74\x14\x55\x4c\x89\xc2"
    "\x48\x89\xe5\xe8\xd8\xfb\xff\xff\x31\xc0\x5d\xc3\x0f\x1f\x40\x00"
    "\x31\xc0\xc3\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc", 46};
inline constexpr std::string_view repairedCallback{
    "\xeb\x21\x66\x90\x48\x85\xd2\x48\x89\xd6\x74\x14\x55\x4c\x89\xc2"
    "\x48\x89\xe5\xe8\xd8\xfb\xff\xff\x31\xc0\x5d\xc3\x0f\x1f\x40\x00"
    "\x31\xc0\xc3\x85\xf6\x75\xf9\x4d\x85\xc0\x74\xf4\xeb\xd6", 46};

// Extend the existing unwind range to cover the guard. Its last row already
// describes the caller's stack, and the guard itself never adjusts it.
inline constexpr std::string_view stockUnwindRange{"\x23\x00\x00\x00", 4};
inline constexpr std::string_view repairedUnwindRange{"\x2e\x00\x00\x00", 4};

}  // namespace steamvrPipewire

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <time.h>

namespace v3 {

inline uint64_t wsiTime() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return uint64_t(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
}

inline void traceWsi(const char *event, uint64_t id, uint64_t a = 0, uint64_t b = 0,
                     uint64_t c = 0, int result = 0) {
    static const auto file = [] {
        const auto *path = std::getenv("V3_WSI_TRACE");
        const auto close = [](std::FILE *trace) { std::fclose(trace); };
        return std::unique_ptr<std::FILE, decltype(close)>(
            path && *path ? std::fopen(path, "we") : nullptr, close);
    }();
    if (!file)
        return;
    const auto timestamp = wsiTime();
    std::fprintf(file.get(), "%llu,%s,%llu,%llu,%llu,%llu,%d\n",
                 static_cast<unsigned long long>(timestamp), event,
                 static_cast<unsigned long long>(id), static_cast<unsigned long long>(a),
                 static_cast<unsigned long long>(b), static_cast<unsigned long long>(c), result);
}

} // namespace v3

#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>

namespace bridgeTiming {
inline constexpr unsigned maxStreams = 8;
inline constexpr unsigned maxRecords = 65536;
inline constexpr unsigned batchSize = 256;

// Disk format v1 on the x86-64 build. A record is appended on scope exit.
struct Record {
    uint64_t startNs;
    uint64_t durationNs;
    uint64_t childNs;
    uint64_t tag;
    char name[64];
};
static_assert(sizeof(Record) == 96);

inline std::atomic<bool> enabled{false};
inline std::atomic<unsigned> nextStream{0};
inline std::filesystem::path directory;
inline bool pacingOnly = false;

inline void start(const std::filesystem::path& runDirectory, bool pacing = false) noexcept {
    if (runDirectory.empty()) return;
    try {
        directory = runDirectory;
        pacingOnly = pacing;
        enabled.store(true, std::memory_order_release);
    } catch (...) {
        // A diagnostic failure must not affect Vulkan setup.
    }
}

inline uint64_t now() noexcept {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

struct PreserveErrno {
    int value = errno;
    ~PreserveErrno() { errno = value; }
};

struct Scope;
struct Stream {
    int fd = -1;
    unsigned count = 0;
    unsigned pending = 0;
    Scope* parent = nullptr;
    Record records[batchSize];

    Stream() noexcept {
        const unsigned stream = nextStream.fetch_add(1, std::memory_order_relaxed);
        if (stream >= maxStreams) return;
        try {
            const auto path = directory / ("function-timing-v1-" + std::to_string(getpid()) +
                                           "-" + std::to_string(stream) + ".bin");
            fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        } catch (...) {
        }
    }

    void flush() noexcept {
        auto* data = reinterpret_cast<const char*>(records);
        size_t remaining = pending * sizeof(Record);
        while (remaining && fd >= 0) {
            const ssize_t written = write(fd, data, remaining);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) {
                close(fd);
                fd = -1;
                break;
            }
            data += written;
            remaining -= written;
        }
        pending = 0;
    }

    void append(const char* name, uint64_t start, uint64_t duration,
                uint64_t child, uint64_t tag) noexcept {
        if (fd < 0 || count >= maxRecords) return;
        auto& record = records[pending++];
        record = {start, duration, child, tag, {}};
        std::strncpy(record.name, name, sizeof(record.name) - 1);
        ++count;
        if (pending == batchSize || count == maxRecords) flush();
    }

    ~Stream() {
        flush();
        if (fd >= 0) close(fd);
    }
};

struct Scope {
    Stream* stream = nullptr;
    Scope* parent = nullptr;
    const char* name;
    uint64_t startNs = 0;
    uint64_t childNs = 0;
    uint64_t tag = 0;

    explicit Scope(const char* label, bool pacing = false) noexcept : name(label) {
        if (!enabled.load(std::memory_order_acquire) || (pacingOnly && !pacing)) return;
        const PreserveErrno savedErrno;
        thread_local Stream threadStream;
        if (threadStream.fd < 0 || threadStream.count >= maxRecords) return;
        stream = &threadStream;
        parent = stream->parent;
        stream->parent = this;
        startNs = now();
    }

    void event(const char* label, uint64_t monotonicNs, uint64_t id) noexcept {
        if (!stream) return;
        const PreserveErrno savedErrno;
        stream->append(label, monotonicNs, 0, 0, id);
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    ~Scope() {
        if (!stream) return;
        const PreserveErrno savedErrno;
        const auto duration = now() - startNs;
        stream->parent = parent;
        if (parent) parent->childNs += duration;
        stream->append(name, startNs, duration, childNs, tag);
    }
};
}

#define WMR_TIMING_SCOPE(name) bridgeTiming::Scope wmrTimingScope(name)
#define WMR_TIMING_CALL(name, expression) ([&] { WMR_TIMING_SCOPE(name); return (expression); }())

#define WMR_PACING_SCOPE(name) bridgeTiming::Scope wmrTimingScope(name, true)
#define WMR_PACING_CALL(name, expression) ([&] { WMR_PACING_SCOPE(name); return (expression); }())

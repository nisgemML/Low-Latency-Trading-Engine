#pragma once

// Logger — lock-free async logging via SPSC queue.
//
// The matching engine never calls fprintf or any other function that
// might acquire an internal lock (glibc's flockfile).  Instead, it
// formats a log record into the SPSC queue and returns immediately.
// A background I/O thread drains the queue and writes to disk.

#include "core/spsc_queue.hpp"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <thread>
#include <atomic>

namespace engine {

enum class LogLevel : uint8_t { DEBUG = 0, INFO, WARN, ERROR };

struct LogRecord {
    static constexpr int kMsgLen = 192;
    char     msg[kMsgLen];
    uint64_t timestamp_ns;
    LogLevel level;
    uint8_t  _pad[7];
};
static_assert(sizeof(LogRecord) == 208);

class Logger {
public:
    static constexpr std::size_t kQueueDepth = 1 << 14;  // 16384 records

    Logger();
    ~Logger();

    // Hot path: format into queue, no I/O.
    template<typename... Args>
    void log(LogLevel level, const char* fmt, Args&&... args) noexcept {
        LogRecord rec{};
        rec.level        = level;
        rec.timestamp_ns = rdtsc_ns();
        std::snprintf(rec.msg, sizeof(rec.msg), fmt, std::forward<Args>(args)...);
        queue_.try_push(rec);  // if full, drop — logging must not block matching
    }

    void info(const char* fmt, ...) noexcept;
    void warn(const char* fmt, ...) noexcept;
    void error(const char* fmt, ...) noexcept;

    void start(const char* path = nullptr);
    void stop();
    void flush() noexcept;

private:
    [[nodiscard]] static uint64_t rdtsc_ns() noexcept;
    void drain_loop() noexcept;

    SPSCQueue<LogRecord, kQueueDepth> queue_;
    FILE*             out_     = nullptr;
    std::thread       io_thread_;
    std::atomic<bool> running_{false};
};

// Global singleton for convenience.
Logger& global_logger() noexcept;

#define LOG_INFO(fmt, ...)  engine::global_logger().log(engine::LogLevel::INFO,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  engine::global_logger().log(engine::LogLevel::WARN,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) engine::global_logger().log(engine::LogLevel::ERROR, fmt, ##__VA_ARGS__)

} // namespace engine

#include "util/logger.hpp"
#include <time.h>
#include <cstdarg>

namespace engine {

static constexpr const char* level_str[] = { "DEBUG", "INFO ", "WARN ", "ERROR" };

uint64_t Logger::rdtsc_ns() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + ts.tv_nsec;
}

Logger::Logger() = default;

Logger::~Logger() { stop(); }

void Logger::start(const char* path) {
    out_ = path ? std::fopen(path, "a") : stderr;
    running_.store(true, std::memory_order_seq_cst);
    io_thread_ = std::thread([this] { drain_loop(); });
}

void Logger::stop() {
    running_.store(false, std::memory_order_seq_cst);
    if (io_thread_.joinable()) io_thread_.join();
    if (out_ && out_ != stderr) std::fclose(out_);
    out_ = nullptr;
}

void Logger::flush() noexcept {
    if (out_) std::fflush(out_);
}

void Logger::drain_loop() noexcept {
    LogRecord rec;
    while (running_.load(std::memory_order_relaxed)) {
        while (queue_.try_pop(rec)) {
            if (out_) {
                std::fprintf(out_, "[%lu] %s %s\n",
                             rec.timestamp_ns,
                             level_str[static_cast<int>(rec.level)],
                             rec.msg);
            }
        }
        __builtin_ia32_pause();
    }
    // Drain remaining.
    while (queue_.try_pop(rec)) {
        if (out_) {
            std::fprintf(out_, "[%lu] %s %s\n",
                         rec.timestamp_ns,
                         level_str[static_cast<int>(rec.level)],
                         rec.msg);
        }
    }
    flush();
}

Logger& global_logger() noexcept {
    static Logger instance;
    return instance;
}

} // namespace engine

#pragma once
/* Minimal thread-safe logging. */

#include <iostream>
#include <mutex>
#include <string>
#include <utility>

namespace pk {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

extern LogLevel g_log_level;

namespace detail {
extern std::mutex g_log_mtx;
}

template <typename... Args>
void logAt(LogLevel lvl, const char* prefix, Args&&... args)
{
    if (lvl > g_log_level) return;
    std::lock_guard<std::mutex> lk(detail::g_log_mtx);
    std::cout << prefix;
    (std::cout << ... << std::forward<Args>(args)) << '\n';
    std::cout.flush();
}

template <typename... Args> void logError(Args&&... a) { logAt(LogLevel::Error, "[ERROR] ", std::forward<Args>(a)...); }
template <typename... Args> void logWarn (Args&&... a) { logAt(LogLevel::Warn,  "[WARN ] ", std::forward<Args>(a)...); }
template <typename... Args> void logInfo (Args&&... a) { logAt(LogLevel::Info,  "[INFO ] ", std::forward<Args>(a)...); }
template <typename... Args> void logDebug(Args&&... a) { logAt(LogLevel::Debug, "[DEBUG] ", std::forward<Args>(a)...); }

bool setLogLevelByName(const std::string& name);

}  // namespace pk

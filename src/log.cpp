#include "log.h"

#include <string>

namespace pk {

LogLevel g_log_level = LogLevel::Info;

namespace detail {
std::mutex g_log_mtx;
}

bool setLogLevelByName(const std::string& name)
{
    if (name == "error") { g_log_level = LogLevel::Error; return true; }
    if (name == "warn")  { g_log_level = LogLevel::Warn;  return true; }
    if (name == "info")  { g_log_level = LogLevel::Info;  return true; }
    if (name == "debug") { g_log_level = LogLevel::Debug; return true; }
    return false;
}

}  // namespace pk

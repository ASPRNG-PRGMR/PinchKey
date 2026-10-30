#include "config.h"

#include "log.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace pk {

namespace {

std::string trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool parseBool(const std::string& v, bool& out)
{
    if (v == "true"  || v == "yes" || v == "1" || v == "on")  { out = true;  return true; }
    if (v == "false" || v == "no"  || v == "0" || v == "off") { out = false; return true; }
    return false;
}

bool parseDouble(const std::string& v, double& out)
{
    try { size_t n = 0; double d = std::stod(v, &n); if (n != v.size()) return false; out = d; return true; }
    catch (...) { return false; }
}

bool parseInt(const std::string& v, int& out)
{
    try { size_t n = 0; int i = std::stoi(v, &n); if (n != v.size()) return false; out = i; return true; }
    catch (...) { return false; }
}

std::vector<fs::path> candidatePaths()
{
    std::vector<fs::path> out;

    if (const char* env = std::getenv("PINCHKEY_CONFIG"); env && *env)
        out.emplace_back(env);

    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        out.emplace_back(fs::path(xdg) / "pinchkey" / "pinchkey.conf");
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        out.emplace_back(fs::path(home) / ".config" / "pinchkey" / "pinchkey.conf");
    }

    out.emplace_back("/etc/pinchkey.conf");
    return out;
}

}  // namespace

bool Config::setOption(const std::string& key, const std::string& value)
{
    if (key == "trigger") {
        if (value == "alt_wheel")  { trigger = Trigger::AltWheel;  return true; }
        if (value == "side_wheel") { trigger = Trigger::SideWheel; return true; }
        return false;
    }
    if (key == "zoom_step")         return parseDouble(value, zoom_step)         && zoom_step > 1.0;
    if (key == "tick_ms")           return parseInt(value, tick_ms)              && tick_ms >= 1 && tick_ms <= 100;
    if (key == "ease")              return parseDouble(value, ease)              && ease > 0.0 && ease <= 1.0;
    if (key == "start_half_mm_in")  return parseDouble(value, start_half_mm_in)  && start_half_mm_in > 0.0;
    if (key == "start_half_mm_out") return parseDouble(value, start_half_mm_out) && start_half_mm_out > 0.0;
    if (key == "edge_margin_mm")    return parseDouble(value, edge_margin_mm)    && edge_margin_mm >= 0.0;
    if (key == "min_half_mm")       return parseDouble(value, min_half_mm)       && min_half_mm > 0.0;
    if (key == "idle_lift_ms")      return parseInt(value, idle_lift_ms)         && idle_lift_ms >= 0;
    if (key == "grab_mouse")        return parseBool(value, grab_mouse);
    if (key == "grab_keyboard")     return parseBool(value, grab_keyboard);
    if (key == "monitor_touchpad")  return parseBool(value, monitor_touchpad);
    if (key == "log_level") {
        if (!setLogLevelByName(value)) return false;
        log_level = value;
        return true;
    }
    return false;
}

std::string Config::load()
{
    for (const auto& p : candidatePaths()) {
        std::error_code ec;
        if (!fs::exists(p, ec) || ec) continue;

        std::ifstream in(p);
        if (!in) {
            logWarn("Cannot read config ", p.string(), " — skipping");
            continue;
        }

        std::string line;
        int lineno = 0;
        while (std::getline(in, line)) {
            ++lineno;
            /* Strip comments, but only at line start or after whitespace, so a
             * value may legitimately contain '#'. */
            const auto hash = line.find('#');
            if (hash != std::string::npos) line = line.substr(0, hash);

            line = trim(line);
            if (line.empty()) continue;

            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                logWarn(p.string(), ":", lineno, ": expected 'key = value', ignoring");
                continue;
            }
            const std::string key = trim(line.substr(0, eq));
            const std::string val = trim(line.substr(eq + 1));
            if (!setOption(key, val))
                logWarn(p.string(), ":", lineno, ": bad key or value '", key, " = ", val, "', ignoring");
        }
        return p.string();
    }
    return "";
}

void Config::dump() const
{
    logInfo("config: trigger=",
            trigger == Trigger::AltWheel ? "alt_wheel" : "side_wheel",
            " zoom_step=", zoom_step,
            " tick_ms=", tick_ms,
            " ease=", ease);
    logInfo("config: start_half_mm in=", start_half_mm_in, " out=", start_half_mm_out,
            " edge_margin_mm=", edge_margin_mm,
            " min_half_mm=", min_half_mm,
            " idle_lift_ms=", idle_lift_ms);
    logInfo("config: grab_mouse=", grab_mouse,
            " grab_keyboard=", grab_keyboard,
            " monitor_touchpad=", monitor_touchpad);
}

}  // namespace pk

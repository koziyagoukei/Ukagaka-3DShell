#pragma once

#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace uka3d::log {

enum class Level { debug, info, warn, error, fatal };

// Opens the log file (UTF-8, truncated) and configures console output.
// Also captures libmmd's std::cout / std::cerr diagnostics into the log.
void initialize(const std::filesystem::path &logFile, bool debugEnabled);
void shutdown();
bool debugEnabled();

void write(Level level, std::string_view message);

// Plain console output (for --help); not prefixed, not timestamped.
void console(std::string_view message);

template <typename... Args> void debug(std::format_string<Args...> fmt, Args &&...args) {
    if (debugEnabled())
        write(Level::debug, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args> void info(std::format_string<Args...> fmt, Args &&...args) {
    write(Level::info, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args> void warn(std::format_string<Args...> fmt, Args &&...args) {
    write(Level::warn, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args> void error(std::format_string<Args...> fmt, Args &&...args) {
    write(Level::error, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args> void fatal(std::format_string<Args...> fmt, Args &&...args) {
    write(Level::fatal, std::format(fmt, std::forward<Args>(args)...));
}

// Formats an HRESULT as "0x8007000E (message)".
std::string hresultToString(long hr);

} // namespace uka3d::log

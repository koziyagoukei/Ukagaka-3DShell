#include "platform/Log.h"

#include "platform/Utf.h"

#include <windows.h>

#include <fstream>
#include <iostream>
#include <mutex>
#include <streambuf>

namespace uka3d::log {
namespace {

std::mutex g_mutex;
std::ofstream g_file;
bool g_debug = false;

void writeConsoleRaw(std::string_view utf8, bool toStderr) {
    HANDLE handle = GetStdHandle(toStderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
        return;
    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode)) {
        const std::wstring wide = utf8ToWide(utf8);
        DWORD written = 0;
        WriteConsoleW(handle, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr);
    } else {
        // Redirected to a file or pipe: emit UTF-8 bytes.
        DWORD written = 0;
        WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
}

const char *levelName(Level level) {
    switch (level) {
    case Level::debug:
        return "DEBUG";
    case Level::info:
        return "INFO ";
    case Level::warn:
        return "WARN ";
    case Level::error:
        return "ERROR";
    case Level::fatal:
        return "FATAL";
    }
    return "?????";
}

// Line-buffered stream buffer that forwards libmmd's std::cout/std::cerr
// output ("[DEBUG] ...", "[INFO] ...", "[WARN] ...") into our logger.
class LibraryStreamBuf final : public std::streambuf {
  public:
    explicit LibraryStreamBuf(Level defaultLevel) : defaultLevel_(defaultLevel) {}

  protected:
    int_type overflow(int_type ch) override {
        if (ch == traits_type::eof())
            return traits_type::not_eof(ch);
        if (ch == '\n')
            flushLine();
        else
            line_.push_back(static_cast<char>(ch));
        return ch;
    }
    std::streamsize xsputn(const char *s, std::streamsize n) override {
        for (std::streamsize i = 0; i < n; ++i)
            overflow(traits_type::to_int_type(s[i]));
        return n;
    }
    int sync() override {
        return 0;
    }

  private:
    void flushLine() {
        std::string_view text = line_;
        Level level = defaultLevel_;
        const auto strip = [&](std::string_view prefix, Level value) {
            if (text.starts_with(prefix)) {
                text.remove_prefix(prefix.size());
                level = value;
                return true;
            }
            return false;
        };
        strip("[DEBUG] ", Level::debug) || strip("[INFO] ", Level::info) || strip("[WARN] ", Level::warn);
        if (level != Level::debug || g_debug)
            write(level, std::string("libmmd: ") + std::string(text));
        line_.clear();
    }

    Level defaultLevel_;
    std::string line_;
};

LibraryStreamBuf g_coutBuf(Level::info);
LibraryStreamBuf g_cerrBuf(Level::warn);
std::streambuf *g_oldCout = nullptr;
std::streambuf *g_oldCerr = nullptr;

} // namespace

void initialize(const std::filesystem::path &logFile, bool debugEnabled) {
    std::scoped_lock lock(g_mutex);
    g_debug = debugEnabled;
    if (!logFile.empty()) {
        std::error_code ec;
        if (logFile.has_parent_path())
            std::filesystem::create_directories(logFile.parent_path(), ec);
        g_file.open(logFile, std::ios::binary | std::ios::trunc);
    }
    g_oldCout = std::cout.rdbuf(&g_coutBuf);
    g_oldCerr = std::cerr.rdbuf(&g_cerrBuf);
}

void shutdown() {
    std::scoped_lock lock(g_mutex);
    if (g_oldCout != nullptr)
        std::cout.rdbuf(g_oldCout);
    if (g_oldCerr != nullptr)
        std::cerr.rdbuf(g_oldCerr);
    g_oldCout = g_oldCerr = nullptr;
    if (g_file.is_open())
        g_file.close();
}

bool debugEnabled() {
    return g_debug;
}

void write(Level level, std::string_view message) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const std::string line =
        std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [{}] {}\n", now.wYear, now.wMonth, now.wDay, now.wHour,
                    now.wMinute, now.wSecond, now.wMilliseconds, levelName(level), message);
    std::scoped_lock lock(g_mutex);
    if (g_file.is_open()) {
        g_file.write(line.data(), static_cast<std::streamsize>(line.size()));
        g_file.flush();
    }
    writeConsoleRaw(line, level >= Level::warn);
}

void console(std::string_view message) {
    std::scoped_lock lock(g_mutex);
    writeConsoleRaw(message, false);
}

std::string hresultToString(long hr) {
    wchar_t *buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                            FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&buffer), 0,
                                        nullptr);
    std::string text;
    if (length > 0 && buffer != nullptr) {
        text = wideToUtf8(std::wstring_view(buffer, length));
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
            text.pop_back();
    }
    if (buffer != nullptr)
        LocalFree(buffer);
    return text.empty() ? std::format("0x{:08X}", static_cast<unsigned long>(hr))
                        : std::format("0x{:08X} ({})", static_cast<unsigned long>(hr), text);
}

} // namespace uka3d::log

// ukagaka3d - 伺か向け汎用3Dシェル基盤
#include "app/App.h"
#include "app/CommandLine.h"
#include "platform/Log.h"
#include "platform/Utf.h"

#include <windows.h>

#include <format>
#include <vector>

#ifndef UKA3D_VERSION
#ifndef UKA3D_VERSION
#define UKA3D_VERSION "0.1.0-beta"
#endif
#endif

namespace {

std::filesystem::path executableDirectory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return std::filesystem::current_path();
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer).parent_path();
}

// True when this process owns its console, i.e. it was started from Explorer
// (or SSP) rather than from a terminal.
bool ownsConsole() {
    DWORD processes[2]{};
    return GetConsoleProcessList(processes, 2) <= 1;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::vector<std::wstring_view> args;
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);
    auto parsed = uka3d::parseCommandLine(args);

    const bool standalone = ownsConsole();
    if (parsed.ok && parsed.options.help) {
        uka3d::log::console(uka3d::usageText());
        if (standalone)
            MessageBoxW(nullptr, uka3d::utf8ToWide(uka3d::usageText()).c_str(), L"ukagaka3d", MB_OK);
        return uka3d::exitOk;
    }
    if (parsed.ok && parsed.options.version) {
        uka3d::log::console("ukagaka3d " UKA3D_VERSION "\n");
        return uka3d::exitOk;
    }

    // Hide the console window when launched without a terminal, unless debugging.
    if (standalone && !(parsed.ok && parsed.options.debug))
        FreeConsole();

    if (!parsed.ok) {
        const std::string message = std::format("引数エラー: {}\n\n{}", parsed.error, uka3d::usageText());
        uka3d::log::console(message);
        if (standalone)
            MessageBoxW(nullptr, uka3d::utf8ToWide(message).c_str(), L"ukagaka3d", MB_OK | MB_ICONWARNING);
        return uka3d::exitUsage;
    }

    const auto &options = parsed.options;
    const auto logPath = options.logPath.empty() ? executableDirectory() / L"ukagaka3d.log" : options.logPath;
    uka3d::log::initialize(logPath, options.debug);
    uka3d::log::info("ukagaka3d {} 起動 (ログ: {})", UKA3D_VERSION, uka3d::pathToUtf8(logPath));
    uka3d::log::info("オプション: model=\"{}\" motion=\"{}\" mode={} size={}x{} loop={} topmost={} fps={} physics={} "
                     "debug={}",
                     uka3d::pathToUtf8(options.modelPath), uka3d::pathToUtf8(options.motionPath),
                     options.windowMode == uka3d::WindowMode::transparent ? "transparent" : "windowed",
                     options.width, options.height, options.loop, options.topmost,
                     options.fps == 0 ? std::string("unlimited") : std::to_string(options.fps), options.physics,
                     options.debug);

    int exitCode = uka3d::exitUnexpected;
    try {
        // Under the SAORI bridge errors are reported over IPC, not with dialogs.
        exitCode = uka3d::runApp(options, uka3d::RunEnvironment{standalone && options.ipcInstance.empty()});
    } catch (const std::exception &e) {
        uka3d::log::fatal("予期しない例外: {}", e.what());
        if (standalone)
            MessageBoxW(nullptr, uka3d::utf8ToWide(e.what()).c_str(), L"ukagaka3d エラー", MB_OK | MB_ICONERROR);
        exitCode = uka3d::exitUnexpected;
    }
    uka3d::log::info("終了 (終了コード {})", exitCode);
    uka3d::log::shutdown();
    return exitCode;
}

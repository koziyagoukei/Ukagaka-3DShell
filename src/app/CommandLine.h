#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uka3d {

enum class WindowMode { transparent, windowed };

struct Options {
    std::filesystem::path modelPath;
    std::filesystem::path motionPath; // empty: static pose
    WindowMode windowMode{WindowMode::transparent};
    int width{480};
    int height{720};
    bool loop{true};
    int fps{60};          // frame-rate cap; 0 = unlimited
    bool physics{true};   // MMD rigid-body physics
    bool topmost{false};
    bool debug{false};
    bool help{false};
    bool version{false};
    std::filesystem::path logPath; // empty: default next to the executable
    // IPC control (SSP / SAORI bridge, see docs/ipc-protocol.md)
    std::string ipcInstance;                       // empty: no IPC server
    unsigned long parentPid{0};                    // exit when this process ends; only it may connect
    std::vector<std::filesystem::path> allowedRoots; // IPC file paths must be inside these
    // Follow (position + size) a top-level window of the parent process with
    // this class name, e.g. SSP's sakura shell window "Tmainform".
    std::string followWindowClass;
    std::filesystem::path hitRegionsPath;
    // Camera framing: fit the whole model ("all") or only its height ("height").
    bool fitHeight{false};
    float zoom{1.0F}; // >1 = closer
    // Development / automated verification
    double exitAfterSeconds{0.0};       // >0: exit automatically after this many seconds
    std::filesystem::path capturePath; // non-empty: save the last frame as PNG on exit
};

// Parses a --fps value: an integer in [kMinFps, kMaxFps] or "unlimited" (-> 0).
inline constexpr int kMinFps = 10;
inline constexpr int kMaxFps = 240;
bool parseFpsValue(std::wstring_view text, int &fps);

struct ParseResult {
    Options options;
    bool ok{true};
    std::string error; // UTF-8
};

// Parses UTF-16 command-line arguments (argv[0] excluded).
ParseResult parseCommandLine(std::span<const std::wstring_view> args);

std::string usageText();

} // namespace uka3d

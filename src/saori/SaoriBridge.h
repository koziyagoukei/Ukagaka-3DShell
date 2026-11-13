#pragma once

#include "ipc/PipeClient.h"
#include "saori/SaoriProtocol.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace uka3d::saori {

// Settings read from <dll dir>/ukagaka3d.json (all optional).
struct BridgeConfig {
    std::filesystem::path runtimeExe{L"3d\\ukagaka3d.exe"};
    std::filesystem::path model;                       // default model for "launch"
    std::filesystem::path motionDir{L"3d\\motion"};    // where bare motion names are looked up
    std::filesystem::path interaction;
    ipc::Json seat; // optional "seat" object, sent as configure_seat after launch
    std::vector<std::filesystem::path> allowedRoots;   // extra roots besides the ghost folder
    int width{480};
    int height{720};
    bool transparent{true};
    bool topmost{false};
    std::string followWindowClass; // e.g. "Tmainform" (SSP sakura shell)
    std::string fit{"all"};        // "all" / "height"
    double zoom{1.0};
    int fps{60};
    bool physics{true};
    // "face": automatic blink and gaze following (runtime defaults: both on)
    bool blink{true};
    std::string blinkMorph;
    bool gaze{true};
    double gazeRadius{0.0};
    DWORD launchTimeoutMs{5000};
    DWORD requestTimeoutMs{3000};
    DWORD loadTimeoutMs{10000};
    bool shutdownOnUnload{true};
};

// Command dispatcher for the SAORI. All public methods are serialised by an
// internal mutex (YAYA may call from THREADSTART threads).
//
// Result convention (every EXECUTE): Result = "1" success / "0" failure,
// Value0 = message or main value, Value1 = error code (failure) or detail JSON.
class SaoriBridge {
  public:
    explicit SaoriBridge(std::filesystem::path baseDir);
    ~SaoriBridge();

    SaoriResponse execute(const std::vector<std::string> &arguments);
    // Stops the runtime if this SAORI started it (called from unload()).
    void shutdownRuntime();

    [[nodiscard]] const std::string &instanceId() const noexcept {
        return instanceId_;
    }
    [[nodiscard]] const BridgeConfig &config() const noexcept {
        return config_;
    }
    // Overrides the per-SSP-process part of the instance id (tests).
    void setInstanceId(std::string id) {
        instanceId_ = std::move(id);
    }

  private:
    SaoriResponse launch(const std::vector<std::string> &args);
    SaoriResponse status();
    SaoriResponse forward(const std::string &command, const ipc::Json &params, DWORD timeoutMs);
    SaoriResponse shutdown();

    bool processRunning();
    void closeProcess();
    bool ensureConnected(ipc::Error &err);
    std::optional<std::filesystem::path> resolvePath(const std::string &text, bool motion, std::string &error,
                                                     bool allowMissing = false) const;
    void loadConfig();
    void log(const std::string &message);

    std::filesystem::path baseDir_;
    BridgeConfig config_;
    std::vector<std::filesystem::path> roots_;
    std::string instanceId_;
    PROCESS_INFORMATION process_{};
    ipc::PipeClient client_;
    std::recursive_mutex mutex_;
    std::ofstream log_;
};

SaoriResponse ok(std::string value = {}, std::string detail = {});
SaoriResponse fail(std::string message, std::string code);

} // namespace uka3d::saori

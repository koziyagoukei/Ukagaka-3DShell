#pragma once

#include "ipc/Protocol.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace uka3d::ipc {

// Single-client named-pipe server running on its own thread.
//
// * The pipe is created with FILE_FLAG_FIRST_PIPE_INSTANCE (no squatting on an
//   existing name), PIPE_REJECT_REMOTE_CLIENTS and a DACL that only grants the
//   current user access.
// * When `allowedClientPid` is non-zero, connections from any other process
//   are refused (the SAORI runs inside SSP, which is the runtime's parent).
// * Each valid request is passed to the handler on the server thread; the
//   handler returns the full response object.
class PipeServer {
  public:
    using Handler = std::function<Json(const Request &request)>;
    using Logger = std::function<void(const std::string &message)>;

    struct Config {
        std::string instanceId;
        DWORD allowedClientPid{0};
        DWORD writeTimeoutMs{5000};
    };

    PipeServer() = default;
    ~PipeServer();
    PipeServer(const PipeServer &) = delete;
    PipeServer &operator=(const PipeServer &) = delete;

    // Creates the pipe synchronously (so name conflicts are reported) and starts the thread.
    bool start(const Config &config, Handler handler, Logger logger, std::string &error);
    // Waits (up to 2 s) for an in-flight response to be written, then stops.
    void stop();

    [[nodiscard]] unsigned long long requestsHandled() const noexcept {
        return handled_.load();
    }

  private:
    void run();
    void serveClient();
    void log(const std::string &message) const;

    Config config_;
    Handler handler_;
    Logger logger_;
    HANDLE pipe_{INVALID_HANDLE_VALUE};
    HANDLE stopEvent_{nullptr};
    std::thread thread_;
    std::atomic<unsigned long long> handled_{0};
    std::atomic<bool> busy_{false}; // a request is being answered
};

// Builds a SECURITY_ATTRIBUTES whose DACL grants full access to the current
// user only. Returns false on failure. Free `descriptor` with LocalFree.
bool currentUserOnlySecurity(SECURITY_ATTRIBUTES &attributes, PSECURITY_DESCRIPTOR &descriptor);

} // namespace uka3d::ipc

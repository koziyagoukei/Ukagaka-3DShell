#pragma once

#include "ipc/Protocol.h"

#include <optional>
#include <string>

namespace uka3d::ipc {

// Synchronous client with bounded waits (the SAORI runs on SSP's UI thread).
class PipeClient {
  public:
    PipeClient() = default;
    ~PipeClient();
    PipeClient(const PipeClient &) = delete;
    PipeClient &operator=(const PipeClient &) = delete;

    // Connects, retrying until `timeoutMs` elapses (the server may still be
    // starting). When `expectedServerPid` is non-zero the server process must
    // match it, so another process squatting on the pipe name is rejected.
    bool connect(const std::string &instanceId, DWORD timeoutMs, DWORD expectedServerPid, Error &err);
    void close();
    [[nodiscard]] bool connected() const noexcept {
        return pipe_ != INVALID_HANDLE_VALUE;
    }

    // Sends one request and waits for its response. Returns the response
    // object (which may carry success=false), or nullopt with `err` set on
    // transport failure. On timeout the connection is closed because the
    // stream can no longer be matched to requests.
    std::optional<Json> call(const std::string &command, const Json &params, DWORD timeoutMs, Error &err);

    // Sends raw bytes as one frame (tests: malformed payloads).
    std::optional<Json> callRaw(const std::string &payload, DWORD timeoutMs, Error &err);

  private:
    HANDLE pipe_{INVALID_HANDLE_VALUE};
    unsigned long long nextId_{1};
};

} // namespace uka3d::ipc

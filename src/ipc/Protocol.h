#pragma once

// ukagaka3d IPC protocol v1 (see docs/ipc-protocol.md).
//
// Transport : Windows named pipe \\.\pipe\ukagaka3d.<instanceId>, byte mode,
//             local machine only, DACL restricted to the current user.
// Framing   : 4-byte little-endian payload length + UTF-8 JSON payload.
// Messages  : request  {"version":1,"requestId":"..","command":"..","params":{..}}
//             response {"version":1,"requestId":"..","success":true,"result":{..}}
//                      {"version":1,"requestId":"..","success":false,
//                       "error":{"code":"..","message":".."}}
//
// Independent of YAYA/SAORI so other clients (GUI, other SHIORIs) can use it.

#include <windows.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace uka3d::ipc {

using Json = nlohmann::json;

inline constexpr int kProtocolVersion = 1;
inline constexpr std::uint32_t kMaxMessageBytes = 1u << 20; // 1 MiB

// Error codes used in error responses.
namespace error {
inline constexpr const char *invalidJson = "invalid_json";
inline constexpr const char *invalidRequest = "invalid_request";
inline constexpr const char *unsupportedVersion = "unsupported_version";
inline constexpr const char *unknownCommand = "unknown_command";
inline constexpr const char *invalidParams = "invalid_params";
inline constexpr const char *notFound = "not_found";
inline constexpr const char *forbiddenPath = "forbidden_path";
inline constexpr const char *loadFailed = "load_failed";
inline constexpr const char *timeout = "timeout";
inline constexpr const char *notRunning = "not_running";
inline constexpr const char *ioError = "io_error";
inline constexpr const char *internal = "internal";
} // namespace error

struct Error {
    std::string code;
    std::string message; // UTF-8
};

// Instance ids: 1-64 characters of [A-Za-z0-9._-].
bool isValidInstanceId(std::string_view id);
std::wstring pipeName(std::string_view instanceId);

// ---- Framing (pipe handles must be opened with FILE_FLAG_OVERLAPPED) -------
enum class IoResult { ok, timeout, closed, cancelled, tooLarge, error };
const char *toString(IoResult result);

// timeoutMs = INFINITE waits until data, close or `cancelEvent` is signalled.
IoResult writeMessage(HANDLE pipe, std::string_view payload, DWORD timeoutMs, HANDLE cancelEvent = nullptr);
IoResult readMessage(HANDLE pipe, std::string &payload, DWORD timeoutMs, HANDLE cancelEvent = nullptr);

// ---- Messages --------------------------------------------------------------
struct Request {
    std::string requestId;
    std::string command;
    Json params = Json::object();
};

Json makeRequest(const std::string &requestId, const std::string &command, const Json &params);
// Validates the envelope (version, requestId, command, params). On failure
// fills `err` and, if it could be read, `requestId` for the error response.
std::optional<Request> parseRequest(std::string_view text, Error &err, std::string &requestId);
Json makeSuccess(const std::string &requestId, const Json &result = Json::object());
Json makeError(const std::string &requestId, std::string_view code, std::string_view message);

// Serialises with UTF-8 output; invalid UTF-8 in strings is replaced.
std::string dump(const Json &value);

} // namespace uka3d::ipc

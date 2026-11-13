#include "ipc/Protocol.h"

#include "platform/Utf.h"

#include <array>
#include <cstring>

namespace uka3d::ipc {
namespace {

class EventHandle {
  public:
    EventHandle() : handle_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~EventHandle() {
        if (handle_ != nullptr)
            CloseHandle(handle_);
    }
    EventHandle(const EventHandle &) = delete;
    EventHandle &operator=(const EventHandle &) = delete;
    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

  private:
    HANDLE handle_;
};

IoResult mapError(DWORD error) {
    switch (error) {
    case ERROR_BROKEN_PIPE:
    case ERROR_PIPE_NOT_CONNECTED:
    case ERROR_NO_DATA:
    case ERROR_HANDLE_EOF:
        return IoResult::closed;
    case ERROR_OPERATION_ABORTED:
        return IoResult::cancelled;
    default:
        return IoResult::error;
    }
}

// Performs one overlapped ReadFile/WriteFile and waits for it, honouring the
// timeout and the optional cancel event.
IoResult transfer(HANDLE pipe, bool write, void *buffer, DWORD size, DWORD &done, DWORD timeoutMs,
                  HANDLE cancelEvent) {
    EventHandle event;
    if (event.get() == nullptr)
        return IoResult::error;
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    done = 0;
    const BOOL ok = write ? WriteFile(pipe, buffer, size, nullptr, &overlapped)
                          : ReadFile(pipe, buffer, size, nullptr, &overlapped);
    if (!ok) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING && error != ERROR_MORE_DATA)
            return mapError(error);
    }
    HANDLE handles[2] = {event.get(), cancelEvent};
    const DWORD count = cancelEvent != nullptr ? 2 : 1;
    const DWORD wait = WaitForMultipleObjects(count, handles, FALSE, timeoutMs);
    if (wait != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &overlapped);
        GetOverlappedResult(pipe, &overlapped, &done, TRUE); // wait for the cancellation
        if (wait == WAIT_TIMEOUT)
            return IoResult::timeout;
        if (wait == WAIT_OBJECT_0 + 1)
            return IoResult::cancelled;
        return IoResult::error;
    }
    if (!GetOverlappedResult(pipe, &overlapped, &done, FALSE)) {
        const DWORD error = GetLastError();
        if (error != ERROR_MORE_DATA)
            return mapError(error);
    }
    return IoResult::ok;
}

// Transfers exactly `size` bytes. The timeout applies to the whole message.
IoResult transferAll(HANDLE pipe, bool write, char *buffer, DWORD size, DWORD timeoutMs, HANDLE cancelEvent) {
    const ULONGLONG deadline = timeoutMs == INFINITE ? 0 : GetTickCount64() + timeoutMs;
    DWORD total = 0;
    while (total < size) {
        DWORD remaining = INFINITE;
        if (timeoutMs != INFINITE) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline)
                return IoResult::timeout;
            remaining = static_cast<DWORD>(deadline - now);
        }
        DWORD done = 0;
        const IoResult result = transfer(pipe, write, buffer + total, size - total, done, remaining, cancelEvent);
        if (result != IoResult::ok)
            return result;
        if (done == 0)
            return IoResult::closed;
        total += done;
    }
    return IoResult::ok;
}

std::optional<std::string> stringMember(const Json &object, const char *name) {
    const auto it = object.find(name);
    if (it == object.end() || !it->is_string())
        return std::nullopt;
    return it->get<std::string>();
}

} // namespace

bool isValidInstanceId(std::string_view id) {
    if (id.empty() || id.size() > 64)
        return false;
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

std::wstring pipeName(std::string_view instanceId) {
    return L"\\\\.\\pipe\\ukagaka3d." + utf8ToWide(instanceId);
}

const char *toString(IoResult result) {
    switch (result) {
    case IoResult::ok:
        return "ok";
    case IoResult::timeout:
        return "timeout";
    case IoResult::closed:
        return "closed";
    case IoResult::cancelled:
        return "cancelled";
    case IoResult::tooLarge:
        return "too_large";
    case IoResult::error:
        return "error";
    }
    return "?";
}

IoResult writeMessage(HANDLE pipe, std::string_view payload, DWORD timeoutMs, HANDLE cancelEvent) {
    if (payload.size() > kMaxMessageBytes)
        return IoResult::tooLarge;
    std::string frame(4 + payload.size(), '\0');
    const auto size = static_cast<std::uint32_t>(payload.size());
    frame[0] = static_cast<char>(size & 0xFF);
    frame[1] = static_cast<char>((size >> 8) & 0xFF);
    frame[2] = static_cast<char>((size >> 16) & 0xFF);
    frame[3] = static_cast<char>((size >> 24) & 0xFF);
    std::memcpy(frame.data() + 4, payload.data(), payload.size());
    return transferAll(pipe, true, frame.data(), static_cast<DWORD>(frame.size()), timeoutMs, cancelEvent);
}

IoResult readMessage(HANDLE pipe, std::string &payload, DWORD timeoutMs, HANDLE cancelEvent) {
    std::array<unsigned char, 4> header{};
    IoResult result =
        transferAll(pipe, false, reinterpret_cast<char *>(header.data()), 4, timeoutMs, cancelEvent);
    if (result != IoResult::ok)
        return result;
    const std::uint32_t size = static_cast<std::uint32_t>(header[0]) | (static_cast<std::uint32_t>(header[1]) << 8) |
                               (static_cast<std::uint32_t>(header[2]) << 16) |
                               (static_cast<std::uint32_t>(header[3]) << 24);
    if (size > kMaxMessageBytes)
        return IoResult::tooLarge;
    payload.assign(size, '\0');
    if (size == 0)
        return IoResult::ok;
    // The body of a frame must follow promptly even when waiting for the header is unbounded.
    const DWORD bodyTimeout = timeoutMs == INFINITE ? 5000 : timeoutMs;
    return transferAll(pipe, false, payload.data(), size, bodyTimeout, cancelEvent);
}

Json makeRequest(const std::string &requestId, const std::string &command, const Json &params) {
    return Json{{"version", kProtocolVersion}, {"requestId", requestId}, {"command", command}, {"params", params}};
}

std::optional<Request> parseRequest(std::string_view text, Error &err, std::string &requestId) {
    requestId.clear();
    Json value = Json::parse(text, nullptr, false);
    if (value.is_discarded()) {
        err = {error::invalidJson, "メッセージが JSON として解釈できません"};
        return std::nullopt;
    }
    if (!value.is_object()) {
        err = {error::invalidRequest, "メッセージは JSON オブジェクトである必要があります"};
        return std::nullopt;
    }
    if (auto id = stringMember(value, "requestId"))
        requestId = *id;
    const auto version = value.find("version");
    if (version == value.end() || !version->is_number_integer()) {
        err = {error::invalidRequest, "version（整数）がありません"};
        return std::nullopt;
    }
    if (version->get<int>() != kProtocolVersion) {
        err = {error::unsupportedVersion, "未対応のプロトコルバージョンです"};
        return std::nullopt;
    }
    if (requestId.empty() || requestId.size() > 128) {
        err = {error::invalidRequest, "requestId（1〜128 文字の文字列）がありません"};
        return std::nullopt;
    }
    const auto command = stringMember(value, "command");
    if (!command || command->empty() || command->size() > 64) {
        err = {error::invalidRequest, "command（文字列）がありません"};
        return std::nullopt;
    }
    Request request;
    request.requestId = requestId;
    request.command = *command;
    if (const auto params = value.find("params"); params != value.end()) {
        if (!params->is_object()) {
            err = {error::invalidRequest, "params は JSON オブジェクトである必要があります"};
            return std::nullopt;
        }
        request.params = *params;
    }
    return request;
}

Json makeSuccess(const std::string &requestId, const Json &result) {
    return Json{{"version", kProtocolVersion}, {"requestId", requestId}, {"success", true}, {"result", result}};
}

Json makeError(const std::string &requestId, std::string_view code, std::string_view message) {
    return Json{{"version", kProtocolVersion},
                {"requestId", requestId},
                {"success", false},
                {"error", {{"code", std::string(code)}, {"message", std::string(message)}}}};
}

std::string dump(const Json &value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

} // namespace uka3d::ipc

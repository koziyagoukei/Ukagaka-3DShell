#include "ipc/PipeClient.h"

#include <format>

namespace uka3d::ipc {

PipeClient::~PipeClient() {
    close();
}

void PipeClient::close() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
}

bool PipeClient::connect(const std::string &instanceId, DWORD timeoutMs, DWORD expectedServerPid, Error &err) {
    close();
    if (!isValidInstanceId(instanceId)) {
        err = {error::invalidRequest, "インスタンスIDが不正です"};
        return false;
    }
    const std::wstring name = pipeName(instanceId);
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        // SECURITY_IDENTIFICATION: the server may identify but not impersonate us.
        HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            ULONG serverPid = 0;
            GetNamedPipeServerProcessId(pipe, &serverPid);
            if (expectedServerPid != 0 && serverPid != expectedServerPid) {
                CloseHandle(pipe);
                err = {error::ioError, std::format("パイプのサーバーが想定外のプロセスです (pid {})", serverPid)};
                return false;
            }
            pipe_ = pipe;
            return true;
        }
        const DWORD lastError = GetLastError();
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            err = {error::timeout, std::format("ランタイムに接続できませんでした (Win32 {})", lastError)};
            return false;
        }
        const DWORD remaining = static_cast<DWORD>(deadline - now);
        if (lastError == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(name.c_str(), remaining < 100 ? remaining : 100);
        } else if (lastError == ERROR_FILE_NOT_FOUND) {
            Sleep(remaining < 20 ? remaining : 20); // server not created yet
        } else {
            err = {error::ioError, std::format("パイプを開けません (Win32 {})", lastError)};
            return false;
        }
    }
}

std::optional<Json> PipeClient::callRaw(const std::string &payload, DWORD timeoutMs, Error &err) {
    if (!connected()) {
        err = {error::notRunning, "ランタイムに接続していません"};
        return std::nullopt;
    }
    const ULONGLONG start = GetTickCount64();
    IoResult result = writeMessage(pipe_, payload, timeoutMs);
    if (result == IoResult::ok) {
        const ULONGLONG spent = GetTickCount64() - start;
        const DWORD remaining = spent >= timeoutMs ? 1 : static_cast<DWORD>(timeoutMs - spent);
        std::string response;
        result = readMessage(pipe_, response, remaining);
        if (result == IoResult::ok) {
            Json value = Json::parse(response, nullptr, false);
            if (value.is_discarded() || !value.is_object()) {
                close();
                err = {error::ioError, "ランタイムの応答が不正です"};
                return std::nullopt;
            }
            return value;
        }
    }
    close(); // a late response would desynchronise the stream
    if (result == IoResult::timeout)
        err = {error::timeout, std::format("ランタイムの応答がタイムアウトしました ({}ms)", timeoutMs)};
    else if (result == IoResult::closed)
        err = {error::notRunning, "ランタイムとの接続が切れました"};
    else
        err = {error::ioError, std::format("通信エラー ({})", toString(result))};
    return std::nullopt;
}

std::optional<Json> PipeClient::call(const std::string &command, const Json &params, DWORD timeoutMs, Error &err) {
    const std::string requestId = std::to_string(nextId_++);
    auto response = callRaw(dump(makeRequest(requestId, command, params)), timeoutMs, err);
    if (!response)
        return std::nullopt;
    const auto id = response->find("requestId");
    if (id == response->end() || !id->is_string() || id->get<std::string>() != requestId) {
        close();
        err = {error::ioError, "応答の requestId が一致しません"};
        return std::nullopt;
    }
    return response;
}

} // namespace uka3d::ipc

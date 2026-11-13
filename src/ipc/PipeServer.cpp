#include "ipc/PipeServer.h"

#include <sddl.h>

#include <format>
#include <memory>
#include <vector>

namespace uka3d::ipc {

bool currentUserOnlySecurity(SECURITY_ATTRIBUTES &attributes, PSECURITY_DESCRIPTOR &descriptor) {
    descriptor = nullptr;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size);
    const bool gotUser = size > 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    CloseHandle(token);
    if (!gotUser)
        return false;
    wchar_t *sidText = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &sidText))
        return false;
    // Protected DACL: generic-all for the current user, nobody else.
    const std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sidText + L")";
    LocalFree(sidText);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        return false;
    attributes = {};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    return true;
}

PipeServer::~PipeServer() {
    stop();
}

void PipeServer::log(const std::string &message) const {
    if (logger_)
        logger_(message);
}

bool PipeServer::start(const Config &config, Handler handler, Logger logger, std::string &error) {
    if (!isValidInstanceId(config.instanceId)) {
        error = "インスタンスIDが不正です（英数字と . _ - の 1〜64 文字）";
        return false;
    }
    config_ = config;
    handler_ = std::move(handler);
    logger_ = std::move(logger);

    SECURITY_ATTRIBUTES security{};
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!currentUserOnlySecurity(security, descriptor)) {
        error = std::format("パイプのセキュリティ記述子を作成できません (Win32 {})", GetLastError());
        return false;
    }
    const std::wstring name = pipeName(config_.instanceId);
    pipe_ = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64 * 1024,
                             64 * 1024, 0, &security);
    const DWORD createError = GetLastError();
    LocalFree(descriptor);
    if (pipe_ == INVALID_HANDLE_VALUE) {
        error = createError == ERROR_ACCESS_DENIED || createError == ERROR_PIPE_BUSY
                    ? std::format("パイプ名 \"{}\" は既に使われています", config_.instanceId)
                    : std::format("名前付きパイプを作成できません (Win32 {})", createError);
        return false;
    }
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this] { run(); });
    return true;
}

void PipeServer::stop() {
    // Let a response that is being written (e.g. the reply to "shutdown") go out.
    for (int i = 0; i < 200 && busy_.load(); ++i)
        Sleep(10);
    if (stopEvent_ != nullptr)
        SetEvent(stopEvent_);
    if (thread_.joinable())
        thread_.join();
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    if (stopEvent_ != nullptr) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
}

void PipeServer::run() {
    HANDLE connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (;;) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = connectEvent;
        ResetEvent(connectEvent);
        bool connected = false;
        if (ConnectNamedPipe(pipe_, &overlapped)) {
            connected = true;
        } else {
            const DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (err == ERROR_IO_PENDING) {
                HANDLE handles[2] = {connectEvent, stopEvent_};
                const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                if (wait != WAIT_OBJECT_0) {
                    CancelIoEx(pipe_, &overlapped);
                    DWORD ignored = 0;
                    GetOverlappedResult(pipe_, &overlapped, &ignored, TRUE);
                    break; // stop requested
                }
                DWORD ignored = 0;
                connected = GetOverlappedResult(pipe_, &overlapped, &ignored, FALSE) != FALSE;
            } else {
                log(std::format("ConnectNamedPipe 失敗 (Win32 {})", err));
                if (WaitForSingleObject(stopEvent_, 200) == WAIT_OBJECT_0)
                    break;
            }
        }
        if (!connected)
            continue;

        ULONG clientPid = 0;
        GetNamedPipeClientProcessId(pipe_, &clientPid);
        if (config_.allowedClientPid != 0 && clientPid != config_.allowedClientPid) {
            log(std::format("許可されていないクライアントを拒否しました (pid {})", clientPid));
            const std::string response =
                dump(makeError("", error::invalidRequest, "このランタイムに接続できるのは起動元プロセスだけです"));
            writeMessage(pipe_, response, 500, stopEvent_);
            FlushFileBuffers(pipe_);
            DisconnectNamedPipe(pipe_);
            continue;
        }
        log(std::format("IPC クライアント接続 (pid {})", clientPid));
        serveClient();
        // Closing the server handle lets the client finish reading buffered
        // bytes. DisconnectNamedPipe would discard an unread shutdown reply.
        if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0)
            break;
        DisconnectNamedPipe(pipe_);
        log("IPC クライアント切断");
    }
    CloseHandle(connectEvent);
}

void PipeServer::serveClient() {
    std::string payload;
    for (;;) {
        const IoResult read = readMessage(pipe_, payload, INFINITE, stopEvent_);
        if (read == IoResult::tooLarge) {
            writeMessage(pipe_, dump(makeError("", error::invalidRequest, "メッセージが大きすぎます")),
                         config_.writeTimeoutMs, stopEvent_);
            return; // the stream can no longer be resynchronised
        }
        if (read != IoResult::ok)
            return;

        busy_ = true;
        Error err;
        std::string requestId;
        Json response;
        if (auto request = parseRequest(payload, err, requestId)) {
            try {
                response = handler_(*request);
            } catch (const std::exception &e) {
                response = makeError(request->requestId, error::internal, e.what());
            }
        } else {
            response = makeError(requestId, err.code, err.message);
        }
        ++handled_;
        const IoResult written = writeMessage(pipe_, dump(response), config_.writeTimeoutMs, stopEvent_);
        busy_ = false;
        if (written != IoResult::ok)
            return;
    }
}

} // namespace uka3d::ipc
